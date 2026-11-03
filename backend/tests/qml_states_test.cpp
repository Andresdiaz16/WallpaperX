// SPDX-License-Identifier: GPL-2.0-or-later

// Entry point for the Qt Quick Test cases in tests/qml/, which drive the Plasma wallpaper
// plugin's main.qml through the states it can show (S10.10). The checks are QML because the
// thing under test is QML: the states are derived in a binding and rendered by describe(),
// and nothing in C++ can observe either.
//
// Runs headless under the offscreen platform, set by the ctest entry. No Plasma shell is
// needed -- WallpaperItem instantiates on its own -- but no headless platform reports a screen
// name, which is why main.qml takes its connector through an assignable property.
//
// The stub daemon below exists for what the plugin's read decides before main.qml's apply(): the
// body-size bound, and what ConfigRead does with a redirect. Reaching them needs a real HTTP reply,
// so this process answers where the plugin's read asks. The engine refuses every request QML itself
// makes, as the lock screen's does, so a read that went through QML's XMLHttpRequest would fail
// every one of those cases.
//
// ON THE PORT: 18462, the daemon's own, and not a port of this test's choosing: ConfigRead builds
// its URL from the daemon's constants, and a test-only way to point it elsewhere would be one more
// thing in a shipped file. Nothing stops this process binding 18462 in its own network namespace.

#include "tests/loopback_stub.h"

#include "daemon/daemon_bus.h"
#include "kde/config_read.h"
#include "tests/private_test_bus.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QQmlNetworkAccessManagerFactory>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <QTranslator>
#include <QUrl>

#include <httplib.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace {

using wallpaperx::testing::kDaemonPort;

// Printed once, and only when the stub could not be brought up. The ctest entry matches it
// with SKIP_REGULAR_EXPRESSION, so a host where something else holds 18462 reports Skipped
// rather than a green run in which the two refresh()-path cases silently did not happen.
//
// CONTROL, for that wiring: `if (true || !wallpaperx::testing::bindDaemonPort(m_primary))` below,
// i.e. a stub that can never come up -> ctest reported "qml_states .... ***Skipped", the sentinel
// printed once, and "39 passed, 0 failed, 5 skipped". Observed 2026-10-01 by hand, and reverted.
constexpr const char* kNoStubSentinel = "wallpaperx-qml-states: NO STUB DAEMON --";

// A reply the plugin accepts, so that a case which comes out "unreadable" is unreadable for
// the reason that case is about and not because the body was never usable.
std::string goodReplyBody() {
    return R"({"output":"DP-3","known":true,"path":"/home/u/Pictures/beach.png"})";
}

// The same reply, still valid JSON and still describing a wallpaper, padded past the plugin's
// 65536-byte ceiling. Valid on purpose: size is then the only thing that can make it
// unreadable, so a check on the outcome cannot pass because the body was malformed.
std::string oversizedReplyBody() {
    const std::string pad(70000, 'a');
    return std::string(R"({"output":"DP-3","known":true,"path":"/home/u/Pictures/beach.png","pad":")") + pad +
           "\"}";
}

// Answers on 18462 in whichever shape the running QML case asked for, plus a second server on
// an ephemeral loopback port that a redirect can send the plugin to. Two real ports rather
// than one, because the redirect target has to be a genuinely different origin: rewriting
// the host to "localhost" on the same socket would rest on how Qt resolves that name.
class StubDaemon : public QObject {
    Q_OBJECT
public:
    ~StubDaemon() override { stop(); }

    bool start() {
        m_primary.Get("/config", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastOutput = QString::fromStdString(req.get_param_value("output"));
            }
            ++m_requests;
            Mode now = Mode::Good;
            int status = 0;
            std::string reason;
            {
                // A one-shot shape serves this request and leaves the next one Good.
                std::lock_guard<std::mutex> lock(m_mutex);
                now = m_mode;
                status = m_status;
                reason = m_reason;
                if (m_once) {
                    m_mode = Mode::Good;
                    m_once = false;
                }
            }
            switch (now) {
                case Mode::Status:
                    res.status = status;
                    res.set_content("{\"error\":\"" + reason + "\"}", "application/json");
                    return;
                case Mode::BadGzip:
                    // A 200 that claims gzip and is not: Qt reports the body as undecodable.
                    res.set_header("Content-Encoding", "gzip");
                    res.set_content("this is not gzip data", "application/json");
                    return;
                case Mode::NoStatusLine:
                    // Closed before a byte of the reply: the shutdown ends the client's connection
                    // whatever httplib then tries to write.
                    if (dropConnection(req.local_port, req.remote_port)) ++m_dropped;
                    return;
                case Mode::CutShort:
                    // The status line, then 10 of a declared 1000 bytes, then the connection drops.
                    res.status = status;
                    res.set_content_provider(1000, "application/json",
                                             [](std::size_t offset, std::size_t, httplib::DataSink& sink) {
                                                 if (offset == 0) sink.write("{\"output\":", 10);
                                                 return false;
                                             });
                    return;
                case Mode::Stall: {
                    // Accepts and says nothing; bounded at 12 s, so a case that never releases it
                    // cannot wedge the stub past its own failure.
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_stallReleased.wait_for(lock, std::chrono::seconds(12), [this] { return !m_stalling; });
                    res.status = 503;
                    return;
                }
                case Mode::Endless:
                    // A body that never ends, paced (about 8 MB/s) so a read that never stops cannot
                    // exhaust memory inside a case's wait. The releaser runs once the transfer is over,
                    // which is the plugin closing it, or stop().
                    res.set_chunked_content_provider(
                        "application/json",
                        // Ended by the next reset() too, so a case that failed here does not stream
                        // into every case after it.
                        [this, epoch = m_epoch.load()](std::size_t, httplib::DataSink& sink) {
                            static const std::string chunk(16384, 'a');
                            if (m_stopping.load() || m_epoch.load() != epoch) return false;
                            if (!sink.write(chunk.data(), chunk.size())) return false;
                            m_endlessBytes += static_cast<long long>(chunk.size());
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            return true;
                        },
                        [this](bool) { ++m_endlessEnded; });
                    return;
                default:
                    break;
            }
            switch (now) {
                case Mode::Good:
                    res.set_content(goodReplyBody(), "application/json");
                    return;
                case Mode::Hold: {
                    // Bounded, so a case that never releases cannot wedge the stub past its own failure.
                    // A hold that ran out is harness trouble, never a release: it is answered as a server
                    // failure, counted apart, and said on stderr.
                    std::unique_lock<std::mutex> lock(m_mutex);
                    const bool released =
                        m_holdReleased.wait_for(lock, std::chrono::seconds(20), [this] { return m_released; });
                    lock.unlock();
                    if (!released) {
                        ++m_holdsExpired;
                        std::fprintf(stderr, "wallpaperx-qml-states: a held answer was never released -- "
                                             "answered 503, not counted as released\n");
                        res.status = 503;
                        return;
                    }
                    res.set_content(goodReplyBody(), "application/json");
                    ++m_heldAnswered;
                    return;
                }
                case Mode::Redirect:
                    // 302 to the other server, which answers the good reply: followed, the plugin
                    // would show "set".
                    res.set_redirect("http://127.0.0.1:" + std::to_string(m_altPort) + "/config", 302);
                    return;
                case Mode::OversizedLength:
                    // set_content sets Content-Length.
                    res.set_content(oversizedReplyBody(), "application/json");
                    return;
                case Mode::OversizedChunked:
                    // No Content-Length at all: only the bytes as they arrive can show the size.
                    res.set_chunked_content_provider(
                        "application/json", [body = oversizedReplyBody()](std::size_t, httplib::DataSink& sink) {
                            sink.write(body.data(), body.size());
                            sink.done();
                            return true;
                        });
                    return;
                default:
                    return;
            }
        });
        m_alt.Get("/config", [this](const httplib::Request&, httplib::Response& res) {
            ++m_altRequests;
            res.set_content(goodReplyBody(), "application/json");
        });

        m_altPort = m_alt.bind_to_any_port("127.0.0.1");
        if (m_altPort <= 0) {
            m_why = "no loopback port could be bound for the redirect target";
            return false;
        }
        if (!wallpaperx::testing::bindDaemonPort(m_primary)) {
            m_why = QStringLiteral("127.0.0.1:%1 could not be bound -- a wallpaperx-daemon, or another test, "
                                   "already holds the daemon's port, where the plugin's read asks")
                        .arg(kDaemonPort);
            m_alt.stop();
            return false;
        }

        if (!wallpaperx::testing::serveOnThread(m_alt, m_altThread) ||
            !wallpaperx::testing::serveOnThread(m_primary, m_primaryThread)) {
            m_why = "the stub's listeners gave up before serving";
            stop();
            return false;
        }
        m_available = true;
        return true;
    }

    void stop() {
        m_stopping = true;
        releaseHeld();
        wallpaperx::testing::stopServing(m_primary, m_primaryThread);
        wallpaperx::testing::stopServing(m_alt, m_altThread);
        m_available = false;
    }

    Q_INVOKABLE bool available() const { return m_available; }
    Q_INVOKABLE QString unavailableReason() const { return m_why; }
    Q_INVOKABLE int requests() const { return m_requests.load(); }
    Q_INVOKABLE int altRequests() const { return m_altRequests.load(); }
    Q_INVOKABLE int oversizedBytes() const { return static_cast<int>(oversizedReplyBody().size()); }

    // Counters and mode together: every case sets the shape it wants immediately after, and a
    // case that forgot would otherwise be graded against the previous one's server.
    Q_INVOKABLE void reset() {
        releaseHeld();
        m_heldAnswered = 0;
        m_holdsExpired = 0;
        m_requests = 0;
        m_altRequests = 0;
        ++m_epoch;
        m_endlessBytes = 0;
        m_endlessEnded = 0;
        m_dropped = 0;
        setMode(Mode::Good);
    }
    Q_INVOKABLE void serveGood() { setMode(Mode::Good); }
    // `status` with {"error": reason}; with `once`, only the next request.
    Q_INVOKABLE void serveStatus(int status, const QString& reason, bool once) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = status;
        m_reason = reason.toStdString();
        m_mode = Mode::Status;
        m_once = once;
    }
    Q_INVOKABLE void serveCutShortOnce() { serveCutShortWithStatusOnce(200); }
    Q_INVOKABLE void serveCutShortWithStatusOnce(int status) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = status;
        m_mode = Mode::CutShort;
        m_once = true;
    }
    // Every request until the next shape: Qt may resend a request whose connection closed before
    // any reply, so a one-shot would answer the resend and the read would never see the close.
    Q_INVOKABLE void serveNoStatusLine() { setMode(Mode::NoStatusLine); }
    // Connections the stub closed before writing a byte.
    Q_INVOKABLE int dropped() const { return m_dropped.load(); }
    Q_INVOKABLE void serveBadGzipOnce() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mode = Mode::BadGzip;
        m_once = true;
    }
    Q_INVOKABLE void serveStall() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stalling = true;
        m_mode = Mode::Stall;
    }
    Q_INVOKABLE void serveEndless() { setMode(Mode::Endless); }
    Q_INVOKABLE QString lastOutput() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastOutput;
    }
    // Bytes of the endless body written so far, and endless transfers that have ended.
    Q_INVOKABLE double endlessBytes() const { return static_cast<double>(m_endlessBytes.load()); }
    Q_INVOKABLE int endlessEnded() const { return m_endlessEnded.load(); }
    Q_INVOKABLE void serveFromAnotherOrigin() { setMode(Mode::Redirect); }
    Q_INVOKABLE void serveOversizedWithLength() { setMode(Mode::OversizedLength); }
    Q_INVOKABLE void serveOversizedChunked() { setMode(Mode::OversizedChunked); }

    // Holds every /config request unanswered until releaseHeld(), then answers the good reply.
    Q_INVOKABLE void holdAnswers() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = false;
        m_mode = Mode::Hold;
    }
    Q_INVOKABLE void releaseHeld() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
            m_stalling = false;
            if (m_mode == Mode::Hold || m_mode == Mode::Stall) m_mode = Mode::Good;
        }
        m_holdReleased.notify_all();
        m_stallReleased.notify_all();
    }
    // Held requests the stub answered after a release (server side: written, whether or not anyone
    // read it), and holds that ran out unreleased.
    Q_INVOKABLE int heldAnswered() const { return m_heldAnswered.load(); }
    Q_INVOKABLE int holdsExpired() const { return m_holdsExpired.load(); }

private:
    enum class Mode { Good, Hold, Redirect, OversizedLength, OversizedChunked, Status, CutShort, Stall, Endless, BadGzip,
                      NoStatusLine };

    // httplib exposes no socket to a handler, so the accepted one is found by its two ports: this
    // process's own client end has them the other way round.
    static bool dropConnection(int localPort, int remotePort) {
        for (int fd = 0; fd < 4096; ++fd) {
            sockaddr_in local{};
            sockaddr_in peer{};
            socklen_t localSize = sizeof local;
            socklen_t peerSize = sizeof peer;
            if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localSize) != 0 || local.sin_family != AF_INET ||
                getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peerSize) != 0) {
                continue;
            }
            if (ntohs(local.sin_port) == localPort && ntohs(peer.sin_port) == remotePort) {
                return shutdown(fd, SHUT_RDWR) == 0;
            }
        }
        return false;
    }
    std::atomic<int> m_dropped{0};

    // Written from the QML thread, read on httplib's worker threads. Any other shape change also
    // ends a stall.
    void setMode(Mode next) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_mode = next;
            m_once = false;
            m_stalling = false;
        }
        m_stallReleased.notify_all();
    }

    mutable std::mutex m_mutex;
    Mode m_mode = Mode::Good;
    bool m_once = false;
    int m_status = 0;
    std::string m_reason;
    QString m_lastOutput;
    bool m_stalling = false;
    std::condition_variable m_stallReleased;
    std::atomic<bool> m_stopping{false};
    std::atomic<int> m_epoch{0};
    std::atomic<long long> m_endlessBytes{0};
    std::atomic<int> m_endlessEnded{0};
    std::condition_variable m_holdReleased;
    bool m_released = true;
    std::atomic<int> m_heldAnswered{0};
    std::atomic<int> m_holdsExpired{0};

    std::atomic<int> m_requests{0};
    std::atomic<int> m_altRequests{0};

    httplib::Server m_primary;
    httplib::Server m_alt;
    std::thread m_primaryThread;
    std::thread m_altThread;
    int m_altPort = 0;
    bool m_available = false;
    QString m_why = QStringLiteral("the stub daemon was never started");
};

// Lets a check tell a translated string from a hardcoded one.
//
// It cannot be read off the text: qsTr() with no translation loaded returns its own source
// string, so a pane that ships `text: "..."` and one that ships `text: qsTr("...")` render
// character-identical output and no runtime comparison distinguishes them. What does
// distinguish them is what happens when a translation arrives -- the qsTr binding
// re-evaluates and the literal does not. So this installs a translator that rewrites every
// source string, asks the engine to retranslate, and lets the check look again.
class TranslationProbe : public QObject {
    Q_OBJECT
public:
    // Prefixed rather than replaced, so a failure message still shows which string was being
    // translated instead of just a marker.
    static QString marker() { return QStringLiteral("[xlat]"); }

    void setEngine(QQmlEngine* engine) { m_engine = engine; }

    Q_INVOKABLE bool translateEverything() {
        if (m_engine == nullptr) return false;
        if (!m_installed) {
            QCoreApplication::installTranslator(&m_translator);
            m_installed = true;
        }
        m_engine->retranslate();
        return true;
    }

    // Must be called by every case that translates, before it asserts anything: a translator
    // left installed would rewrite every other case's strings in this same process.
    Q_INVOKABLE void stopTranslating() {
        if (!m_installed) return;
        QCoreApplication::removeTranslator(&m_translator);
        m_installed = false;
        if (m_engine != nullptr) m_engine->retranslate();
    }

    Q_INVOKABLE QString translationMarker() const { return marker(); }

private:
    class RewritingTranslator : public QTranslator {
    public:
        // Non-empty, or QCoreApplication treats an installed translator as carrying nothing.
        bool isEmpty() const override { return false; }
        QString translate(const char*, const char* sourceText, const char*, int) const override {
            return marker() + QString::fromUtf8(sourceText);
        }
    };

    RewritingTranslator m_translator;
    QQmlEngine* m_engine = nullptr;
    bool m_installed = false;
};

// Makes `com.wallpaperx` unloadable in this process, whatever is installed on the machine.
//
// Three of the cases below rest on the renderer module NOT loading -- the one that grades what
// main.qml does when the Loader reaches Loader.Error, and two that read the backdrop while the
// Loader is not Ready. That used to be true by accident, because a test binary's import path does
// not include the module it is built beside. It stopped being true the moment the plugin was
// installed system-wide for a live desktop test: Qt searches /usr/lib/qt6/qml, the import
// succeeded, and the Loader.Error case failed with "Actual 1, Expected 3" on a build whose
// behaviour was correct. A check whose verdict depends on what is installed on the developer's
// machine grades the machine.
//
// Rewriting the qmldir URL is what makes it deterministic in the direction the cases need. The
// obvious alternative -- trimming /usr/lib/qt6/qml out of the engine's import path -- cannot be
// used: QtQuick, QtTest and the Plasma modules all live there, so the whole file would stop
// loading.
class BlockRendererModule : public QObject, public QQmlAbstractUrlInterceptor {
    Q_OBJECT
public:
    QUrl intercept(const QUrl& path, DataType type) override {
        if (type != QmldirFile || !path.toString().contains(QLatin1String("com/wallpaperx"))) return path;
        ++m_blocked;
        return QUrl::fromLocalFile(QStringLiteral("/nonexistent/wallpaperx-module-blocked-by-this-test/qmldir"));
    }

    // Read by the Loader.Error case: a run where no candidate qmldir was ever rewritten is one
    // where this class did nothing, and its verdict would be about the machine again.
    Q_INVOKABLE int blocked() const { return m_blocked; }

private:
    int m_blocked = 0;
};

// The lock screen's setup: kscreenlocker_greet gives its QML engine a network manager that refuses
// every request. This one sends each request to a scheme nothing serves, so QML's XMLHttpRequest
// finishes with status 0 and nothing leaves the process; refused() counts them, so a case can show
// the refusal applied rather than assume it.
class RefusingNetwork : public QObject, public QQmlNetworkAccessManagerFactory {
    Q_OBJECT
public:
    QNetworkAccessManager* create(QObject* parent) override { return new Manager(parent, &m_refused); }
    Q_INVOKABLE int refused() const { return m_refused.load(); }

private:
    class Manager : public QNetworkAccessManager {
    public:
        Manager(QObject* parent, std::atomic<int>* refused) : QNetworkAccessManager(parent), m_refused(refused) {}

    protected:
        QNetworkReply* createRequest(Operation op, const QNetworkRequest&, QIODevice* data) override {
            ++*m_refused;
            return QNetworkAccessManager::createRequest(
                op, QNetworkRequest(QUrl(QStringLiteral("refused-by-this-test:nothing"))), data);
        }

    private:
        std::atomic<int>* m_refused;
    };

    std::atomic<int> m_refused{0};
};

// Hands main.qml the plugin's real reads: each stand-in change watch owns a real ConfigWatch
// (watchFor), and its fetch() calls that watch's fetch(), so what the HTTP cases grade is the shipped
// ConfigRead with the lifetime it has in plasmashell -- tied to its plugin instance. fetch() reads
// through one watch of the probe's own. Every read is counted as made, finished and destroyed, from
// C++ through destroyed(), so a case can count live reads without relying on garbage collection.
class ReadProbe : public QObject {
    Q_OBJECT
public:
    void start() { m_watch = new wallpaperx::ConfigWatch(this); }

    // The read's own bound, so a fixture is measured against the value the read applies.
    Q_INVOKABLE int maxBodyBytes() const { return static_cast<int>(wallpaperx::ConfigRead::kMaxBodyBytes); }
    Q_INVOKABLE int timeoutMs() const { return wallpaperx::ConfigRead::kTimeoutMs; }

    // A real watch that lives and dies with `owner`, as the loaded watch does with its plugin.
    Q_INVOKABLE QObject* watchFor(QObject* owner) {
        if (owner == nullptr) return nullptr;
        auto* watch = new wallpaperx::ConfigWatch(owner);
        QQmlEngine::setObjectOwnership(watch, QQmlEngine::CppOwnership);
        connect(watch, &QObject::destroyed, this, [this] { ++m_watchesDestroyed; });
        return watch;
    }

    Q_INVOKABLE QObject* fetch(const QString& output) { return fetchFrom(m_watch, output); }

    Q_INVOKABLE QObject* fetchFrom(QObject* watch, const QString& output) {
        auto* real = qobject_cast<wallpaperx::ConfigWatch*>(watch);
        if (real == nullptr) return nullptr;
        QObject* read = real->fetch(output);
        ++m_made;
        connect(read, SIGNAL(finished()), this, SLOT(noteFinished()));
        connect(read, &QObject::destroyed, this, [this] { ++m_destroyed; });
        return read;
    }

    Q_INVOKABLE int finishedReads() const { return m_finished; }
    Q_INVOKABLE int madeReads() const { return m_made; }
    Q_INVOKABLE int destroyedReads() const { return m_destroyed; }
    Q_INVOKABLE int liveReads() const { return m_made - m_destroyed; }
    Q_INVOKABLE int destroyedWatches() const { return m_watchesDestroyed; }

private slots:
    void noteFinished() { ++m_finished; }

private:
    wallpaperx::ConfigWatch* m_watch = nullptr;
    int m_finished = 0;
    int m_made = 0;
    int m_destroyed = 0;
    int m_watchesDestroyed = 0;
};

// The daemon's bus name on the private bus, owned by this process so a case can make it go and come
// back, as a daemon stopping and starting does. Never owned off the private bus.
class DaemonName : public QObject {
    Q_OBJECT
public:
    void takeAtStart() {
        std::string why;
        m_onPrivateBus = wallpaperx::testing::onPrivateTestBus(why);
        if (m_onPrivateBus && !take()) {
            std::fprintf(stderr, "wallpaperx-qml-states: could not own %s on the private bus -- %s\n",
                         wallpaperx::kDaemonBusName,
                         QDBusConnection::sessionBus().lastError().message().toUtf8().constData());
        }
    }
    Q_INVOKABLE bool owned() const { return m_owned; }
    Q_INVOKABLE bool release() {
        if (!m_owned) return false;
        m_owned = !QDBusConnection::sessionBus().unregisterService(QString::fromUtf8(wallpaperx::kDaemonBusName));
        return !m_owned;
    }
    Q_INVOKABLE bool take() {
        if (!m_onPrivateBus) return false;
        if (!m_owned) m_owned = QDBusConnection::sessionBus().registerService(QString::fromUtf8(wallpaperx::kDaemonBusName));
        return m_owned;
    }

private:
    bool m_onPrivateBus = false;
    bool m_owned = false;
};

class Setup : public QObject {
    Q_OBJECT

public slots:
    void applicationAvailable() {
        // On the private bus isolated_run.sh starts, the daemon's name is owned here before the first
        // change watch asks who owns it: a name with no owner is a stopped daemon, and main.qml rightly
        // retries nothing then, so the retry cases would grade that instead of the retry.
        m_name.takeAtStart();
        m_reads.start();
        if (!m_stub.start()) {
            // Loud, and on stderr, which ctest captures unbuffered: a stub that did not come up
            // makes two cases skip, and a silent waiver there is a green run that established
            // nothing about either decision.
            std::fprintf(stderr, "%s %s\n", kNoStubSentinel, m_stub.unavailableReason().toUtf8().constData());
            std::fflush(stderr);
        }
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->addUrlInterceptor(&m_blockRenderer);
        engine->setNetworkAccessManagerFactory(&m_refusing);
        engine->rootContext()->setContextProperty(QStringLiteral("refusingNetwork"), &m_refusing);
        engine->rootContext()->setContextProperty(QStringLiteral("readProbe"), &m_reads);
        engine->rootContext()->setContextProperty(QStringLiteral("stubDaemon"), &m_stub);
        engine->rootContext()->setContextProperty(QStringLiteral("daemonName"), &m_name);
        m_translation.setEngine(engine);
        engine->rootContext()->setContextProperty(QStringLiteral("translationProbe"), &m_translation);
        engine->rootContext()->setContextProperty(QStringLiteral("moduleBlock"), &m_blockRenderer);
    }

    // Belt and braces: a case that failed between installing and removing would otherwise leave
    // every later string in this process rewritten.
    void cleanupTestCase() {
        m_translation.stopTranslating();
        m_stub.stop();
    }

private:
    DaemonName m_name;
    StubDaemon m_stub;
    RefusingNetwork m_refusing;
    ReadProbe m_reads;
    TranslationProbe m_translation;
    BlockRendererModule m_blockRenderer;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(wallpaperx_qml_states, Setup)

#include "qml_states_test.moc"
