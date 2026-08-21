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
// The stub daemon below exists for the two decisions main.qml makes that apply() cannot
// reach: the origin check and the body-size bound both live in refresh()'s
// onreadystatechange handler, ahead of the apply() call every other case in tst_*.qml drives.
// Reaching them needs a real HTTP reply, so this process answers on the port the plugin's
// daemonOrigin names.
//
// ON THE PORT: 18462, the daemon's own, and not a port of this test's choosing. daemonOrigin
// is a readonly binding on a fixed loopback URL, so the alternative is a writable override in
// main.qml -- a second test-only property in a shipped file, added for a limitation that is
// this harness's rather than the platform's. (connectorOverride is there because no headless
// Qt platform reports a screen name at all; nothing stops this process binding 18462.) The
// cost is that the ctest entry has to be serialised against everything else using that port,
// which is why it now carries the same RESOURCE_LOCK as http_config and daemon_system.

#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <QTranslator>

#include <httplib.h>

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace {

// What main.qml's daemonOrigin names. Not configurable here: a stub on any other port would
// never be asked.
constexpr int kDaemonPort = 18462;

// Printed once, and only when the stub could not be brought up. The ctest entry matches it
// with SKIP_REGULAR_EXPRESSION, so a host where something else holds 18462 reports Skipped
// rather than a green run in which the two refresh()-path cases silently did not happen.
//
// CONTROL, for that wiring: `if (true || !m_primary.bind_to_port(...))` below, i.e. a stub that
// can never come up -> ctest reported "qml_states .... ***Skipped", the sentinel printed once,
// and "21 passed, 0 failed, 3 skipped" with each of the three cases naming the port as its
// reason. Without SKIP_REGULAR_EXPRESSION the same run is a green Passed. Observed 2026-08-19.
// Reverted.
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
// than one, because "not under daemonOrigin" has to be a genuinely different origin: rewriting
// the host to "localhost" on the same socket would rest on how Qt resolves that name.
class StubDaemon : public QObject {
    Q_OBJECT
public:
    ~StubDaemon() override { stop(); }

    bool start() {
        m_primary.Get("/config", [this](const httplib::Request&, httplib::Response& res) {
            ++m_requests;
            switch (mode()) {
                case Mode::Good:
                    res.set_content(goodReplyBody(), "application/json");
                    return;
                case Mode::Redirect:
                    // 302 to the other server. Qt's XMLHttpRequest follows it, so what the
                    // plugin ends up holding is a perfectly good reply from a foreign origin --
                    // which is exactly the shape the origin check exists to refuse.
                    res.set_redirect("http://127.0.0.1:" + std::to_string(m_altPort) + "/config", 302);
                    return;
                case Mode::OversizedLength:
                    // set_content sets Content-Length, so the plugin can refuse it at
                    // HEADERS_RECEIVED without buffering a byte.
                    res.set_content(oversizedReplyBody(), "application/json");
                    return;
                case Mode::OversizedChunked:
                    // No Content-Length at all, so the header bound cannot fire and the
                    // decision falls to the length check made once the body is in.
                    res.set_chunked_content_provider(
                        "application/json", [body = oversizedReplyBody()](std::size_t, httplib::DataSink& sink) {
                            sink.write(body.data(), body.size());
                            sink.done();
                            return true;
                        });
                    return;
            }
        });
        m_alt.Get("/config", [this](const httplib::Request&, httplib::Response& res) {
            ++m_altRequests;
            res.set_content(goodReplyBody(), "application/json");
        });

        // SO_REUSEADDR, as the daemon's own HttpServer sets: httplib's default is SO_REUSEPORT,
        // which does NOT rebind over a socket in TIME_WAIT. Without this the stub failed to
        // bind whenever a daemon from an earlier entry had just released 18462, and the run
        // reported Skipped for a port nothing was actually holding -- measured under
        // `ctest -j4`, where daemon_system finishes moments before this entry starts.
        m_primary.set_socket_options([](socket_t sock) {
            int on = 1;
            ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const void*>(&on),
                         static_cast<socklen_t>(sizeof(on)));
        });

        m_altPort = m_alt.bind_to_any_port("127.0.0.1");
        if (m_altPort <= 0) {
            m_why = "no loopback port could be bound for the foreign-origin server";
            return false;
        }
        if (!m_primary.bind_to_port("127.0.0.1", kDaemonPort)) {
            m_why = QStringLiteral("127.0.0.1:%1 could not be bound -- a wallpaperx-daemon, or another test, "
                                   "already holds the port main.qml's daemonOrigin names")
                        .arg(kDaemonPort);
            m_alt.stop();
            return false;
        }

        m_altThread = std::thread([this] { m_alt.listen_after_bind(); });
        m_primaryThread = std::thread([this] { m_primary.listen_after_bind(); });

        // Bound is not serving: listen_after_bind runs on the threads above, and a request made
        // before either is accepting would be refused and read as "the daemon is not running".
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_primary.is_running() && m_alt.is_running()) {
                m_available = true;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        m_why = "the stub's listener threads did not start within five seconds";
        stop();
        return false;
    }

    void stop() {
        m_primary.stop();
        m_alt.stop();
        if (m_primaryThread.joinable()) m_primaryThread.join();
        if (m_altThread.joinable()) m_altThread.join();
        m_available = false;
    }

    Q_INVOKABLE bool available() const { return m_available; }
    Q_INVOKABLE QString unavailableReason() const { return m_why; }
    Q_INVOKABLE int altPort() const { return m_altPort; }
    Q_INVOKABLE int requests() const { return m_requests.load(); }
    Q_INVOKABLE int altRequests() const { return m_altRequests.load(); }
    Q_INVOKABLE int oversizedBytes() const { return static_cast<int>(oversizedReplyBody().size()); }

    // Counters and mode together: every case sets the shape it wants immediately after, and a
    // case that forgot would otherwise be graded against the previous one's server.
    Q_INVOKABLE void reset() {
        m_requests = 0;
        m_altRequests = 0;
        setMode(Mode::Good);
    }
    Q_INVOKABLE void serveGood() { setMode(Mode::Good); }
    Q_INVOKABLE void serveFromAnotherOrigin() { setMode(Mode::Redirect); }
    Q_INVOKABLE void serveOversizedWithLength() { setMode(Mode::OversizedLength); }
    Q_INVOKABLE void serveOversizedChunked() { setMode(Mode::OversizedChunked); }

private:
    enum class Mode { Good, Redirect, OversizedLength, OversizedChunked };

    // Written from the QML thread, read on httplib's worker threads.
    Mode mode() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_mode;
    }
    void setMode(Mode next) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mode = next;
    }

    mutable std::mutex m_mutex;
    Mode m_mode = Mode::Good;

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

class Setup : public QObject {
    Q_OBJECT

public slots:
    void applicationAvailable() {
        if (!m_stub.start()) {
            // Loud, and on stderr, which ctest captures unbuffered: a stub that did not come up
            // makes two cases skip, and a silent waiver there is a green run that established
            // nothing about either decision.
            std::fprintf(stderr, "%s %s\n", kNoStubSentinel, m_stub.unavailableReason().toUtf8().constData());
            std::fflush(stderr);
        }
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("stubDaemon"), &m_stub);
        m_translation.setEngine(engine);
        engine->rootContext()->setContextProperty(QStringLiteral("translationProbe"), &m_translation);
    }

    // Belt and braces: a case that failed between installing and removing would otherwise leave
    // every later string in this process rewritten.
    void cleanupTestCase() {
        m_translation.stopTranslating();
        m_stub.stop();
    }

private:
    StubDaemon m_stub;
    TranslationProbe m_translation;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(wallpaperx_qml_states, Setup)

#include "qml_states_test.moc"
