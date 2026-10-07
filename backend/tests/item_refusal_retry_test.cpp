// SPDX-License-Identifier: GPL-2.0-or-later

// What the KDE plugin does when the daemon REFUSES a hand-over rather than failing one.
//
// The daemon keeps one image per monitor free to draw into and bounds what one consumer may
// hold, and it takes nothing back: every bound is a refusal, answered as
// io.wallpaperx.Daemon.Error.NoFreeImage. A refusal is temporary, and the daemon announces a
// frame whenever an image comes back -- but that announcement is produced by some consumer
// RELEASING, and a consumer that has stopped releasing sends none. So an item that has nothing on
// screen and is refused cannot simply wait: it would show the plain backdrop with no message and
// never ask again.
//
// Two answers, graded against each other in one process, because the difference between them is
// the whole property:
//
//   * NoFreeImage with nothing on screen -- say so, and ask again on a timer.
//   * NoFrame, the ordinary state before the user has ever set a wallpaper -- say so, and do NOT
//     ask again. Nothing is coming until the user acts, and a plugin that polled here would ask
//     the daemon forever on a fresh install.
//
// One process, two scenes, because "it re-asks" and "it does not re-ask" are the same measurement
// with opposite expectations: read in separate runs, a wait that was silently too short would
// make the second pass and only the first fail, and the pair would still look like coverage.
//
// A WINDOW WITH NO SCENE GRAPH RUNNING, which is an exact requirement of this file and not a
// convenience. The item fetches only while it is on the screen -- in a window and visible -- so an
// item parented into nothing never reaches the bus at all and every check here would grade a
// silence the plugin is right to keep. The window is a QQuickRenderControl one that is never
// initialised and never rendered into, so joining it costs exactly ONE fetch (the in-flight latch
// coalesces the scene-change re-ask into the visibility one) and nothing afterwards can produce a
// request but the retry timer this file is about. No OpenGL is needed and the run skips on nothing
// but a missing bus.

#include "tests/bus_monitor.h"
#include "tests/qt_frame_reply.h"
#include "tests/stream_capture.h"
#include "tests/test_report.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <dbus/dbus.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

namespace {

// The plugin's owner lookups as they leave this process: GetNameOwner for the daemon's name with a
// timeout of its own (QtDBus's own lookups pass -1, the default). Each one's timeout is recorded while
// g_recordLookups is set, and while g_redirectLookupsTo names a connection the lookup is sent there
// instead of to the bus -- a stand-in bus driver that answers as late as a test chooses.
std::mutex g_lookupLock;
std::vector<int> g_lookupTimeouts;   // guarded by g_lookupLock
std::string g_redirectLookupsTo;     // guarded by g_lookupLock
std::atomic<bool> g_recordLookups{false};

bool isDaemonOwnerLookup(DBusMessage* message) {
    if (message == nullptr || !dbus_message_is_method_call(message, "org.freedesktop.DBus", "GetNameOwner")) {
        return false;
    }
    DBusMessageIter args;
    if (!dbus_message_iter_init(message, &args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) {
        return false;
    }
    const char* name = nullptr;
    dbus_message_iter_get_basic(&args, &name);
    return name != nullptr && std::strcmp(name, "io.wallpaperx.Daemon") == 0;
}

}  // namespace

extern "C" dbus_bool_t dbus_connection_send_with_reply(DBusConnection* connection, DBusMessage* message,
                                                       DBusPendingCall** pending, int timeout) {
    using Fn = dbus_bool_t (*)(DBusConnection*, DBusMessage*, DBusPendingCall**, int);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "dbus_connection_send_with_reply"));
    if (g_recordLookups.load() && timeout > 0 && isDaemonOwnerLookup(message)) {
        std::lock_guard<std::mutex> lock(g_lookupLock);
        g_lookupTimeouts.push_back(timeout);
        if (!g_redirectLookupsTo.empty()) dbus_message_set_destination(message, g_redirectLookupsTo.c_str());
    }
    return real(connection, message, pending, timeout);
}

namespace {

std::vector<int> lookupTimeouts() {
    std::lock_guard<std::mutex> lock(g_lookupLock);
    return g_lookupTimeouts;
}

std::string listed(const std::vector<int>& values) {
    std::string out;
    for (const int v : values) out += (out.empty() ? "" : " ") + std::to_string(v);
    return "[" + out + "]";
}

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;

constexpr const char* kService = "io.wallpaperx.Daemon";
constexpr const char* kPath = "/io/wallpaperx/Daemon";
constexpr const char* kInterface = "io.wallpaperx.Daemon";

// The plugin's own constant for how long a refused item with nothing on screen waits. Written out
// rather than included from the plugin: comparing the plugin against its own number would make
// "it asks again within a couple of seconds" unfalsifiable, since a change to it would move both
// sides of every wait below at once.
constexpr int kExpectedRetryMs = 2000;

// Long enough for at least one retry to have happened and been observed, with margin for a busy
// machine. Used for BOTH the "it re-asked" wait and the "it did not re-ask" span, so the two
// answers are read over the same window and the negative one cannot pass by being asked sooner.
constexpr int kRetryWindowMs = 3 * kExpectedRetryMs;

// Answers every GetFrame with one chosen error, and counts the asking. No frame is ever served:
// what is under test is entirely what the item does with a refusal.
class RefusingStub : public QDBusVirtualObject {
public:
    explicit RefusingStub(QString errorName) : m_errorName(std::move(errorName)) {}

    void answerWith(QString errorName) { m_errorName = std::move(errorName); }

    QString introspect(const QString&) const override { return QString(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String(kInterface)) return false;
        if (message.member() != QLatin1String("GetFrame")) return false;
        ++m_requests;
        m_requestTimesMs.push_back(m_clock.elapsed());
        return connection.send(message.createErrorReply(
            m_errorName, QStringLiteral("this stub answers every hand-over with the same refusal")));
    }

    int requests() const { return m_requests; }
    // When each request arrived, and now, in milliseconds on this stub's own clock.
    const std::vector<qint64>& requestTimesMs() const { return m_requestTimesMs; }
    qint64 nowMs() const { return m_clock.elapsed(); }

private:
    QString m_errorName;
    int m_requests = 0;
    std::vector<qint64> m_requestTimesMs;
    QElapsedTimer m_clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
};

// Pumps the event loop until `done`, or gives up. Returns whether it happened -- every caller
// branches on it, so an expiry can never be read as the thing having occurred.
bool pumpUntil(const std::function<bool()>& done, int limitMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > limitMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

// Pumps for a fixed span, waiting for nothing. Separate from pumpUntil so no call site can
// discard a "it never happened" answer by accident.
void pumpFor(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// publishedReason is a BINDING, not a read: it updates only if the item emits
// unavailableReasonChanged, so a property that changed silently would leave it stale.
QString qmlSource() {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: 64
    height: 64
    property string publishedReason: renderer.unavailableReason
    property bool rendererDrawing: renderer.frameShown

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
    }
}
)");
}

// One item, parented into `window` so that it counts as on the screen and therefore fetches at
// all. Returned rather than built inline so each phase gets a fresh one: the retry timer belongs
// to the item, and a phase reusing the previous phase's item would be reading a timer armed under
// the other answer.
std::unique_ptr<QObject> makeItem(QQmlEngine& engine, QQuickWindow& window, const char* fileName) {
    QQmlComponent component(&engine);
    component.setData(qmlSource().toUtf8(),
                      QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + QLatin1Char('/') +
                                          QString::fromUtf8(fileName)));
    std::unique_ptr<QObject> root(component.create());
    if (root == nullptr) {
        for (const QQmlError& error : component.errors()) {
            std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
        }
        return root;
    }
    auto* rootItem = qobject_cast<QQuickItem*>(root.get());
    if (rootItem == nullptr) {
        wallpaperx::testing::harnessFailure("the QML root is not a QQuickItem, so it could not be put in a window");
        return {};
    }
    rootItem->setParentItem(window.contentItem());
    return root;
}

// The owner lookup failing, in a process of its own: the plugin's watch of the daemon's name is one per
// process, and this entry runs on a private bus whose policy refuses GetNameOwner
// (backend/tests/deny_owner_lookup.conf.in), with the stub owning the name all the while.
int runLookupFailed() {
    const char* kGoal = "S15.349 (an owner lookup that fails says the daemon could not be reached and is asked again, "
                        "later each time)";
    QDBusConnection bus = QDBusConnection::sessionBus();
    RefusingStub stub(QStringLiteral("io.wallpaperx.Daemon.Error.NoFrame"));
    if (!bus.isConnected() || !bus.registerService(QString::fromUtf8(kService)) ||
        !bus.registerVirtualObject(QString::fromUtf8(kPath), &stub)) {
        notVerified(kGoal, "no session bus this run could own io.wallpaperx.Daemon on: " +
                               bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // The premise, asked of the bus before the monitor starts so it is not counted: the lookup fails
    // with an error other than "nobody owns it".
    QDBusMessage probe = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                        QStringLiteral("/org/freedesktop/DBus"),
                                                        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    probe.setArguments({QString::fromUtf8(kService)});
    const QDBusMessage answer = bus.call(probe, QDBus::Block, 3000);
    if (answer.type() != QDBusMessage::ErrorMessage ||
        answer.errorName() == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")) {
        wallpaperx::testing::harnessFailure("this entry's bus did not refuse GetNameOwner (answered " +
                                            answer.errorName().toStdString() + "), so it is not the bus it needs");
        (void)wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
        return 1;
    }
    wallpaperx::testing::BusMonitor monitor;
    if (!monitor.ok()) {
        notVerified(kGoal, "the private bus would not accept a monitor, so the lookups cannot be timed: " +
                               monitor.why());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // From here on: the probe above was this test's own lookup.
    g_recordLookups.store(true);

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.setGeometry(0, 0, 64, 64);
    std::unique_ptr<QObject> root = makeItem(engine, window, "item_refusal_retry_lookup.qml");
    if (root == nullptr) {
        check(false, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const auto publishedReason = [&root] { return root->property("publishedReason").toString(); };

    // GOAL: S15.349 -- when asking the bus who owns the daemon's name fails (any error but "nobody owns
    //       it"), the item says "WallpaperX could not be reached." rather than nothing, and asks nothing
    //       of a daemon it cannot address.
    // WHY-THIS-SHAPE: the exact words, as the other phases compare theirs; the daemon is on the bus all
    //       the while, so "is not running" would be a wrong answer and "no reason" the silence the goal
    //       forbids.
    // CONTROL: `m_lookupFailed = true;` deleted from DaemonWatch::askWhoOwns
    //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: an item whose owner lookup failed says the daemon
    //       could not be reached and asks it nothing (published [], 0 request(s))".
    const QString kUnreachable = QStringLiteral("WallpaperX could not be reached.");
    const bool said = pumpUntil([&] { return publishedReason() == kUnreachable; }, 5000);
    check(said && stub.requests() == 0,
          ("an item whose owner lookup failed says the daemon could not be reached and asks it nothing (published [" +
           publishedReason().toStdString() + "], " + std::to_string(stub.requests()) + " request(s))")
              .c_str());

    // GOAL: S15.349 -- the lookup is asked again, later each time, and the wait stops growing at 8 s:
    //       gaps of 2, 4, 8 and 8 s, so a bus that keeps refusing is neither polled at a fixed rate nor
    //       left for a minute while no screen can be drawn.
    // WHY-THIS-SHAPE: timed off the bus monitor, which sees each GetNameOwner as the bus routes it. QtDBus
    //       asks the same question for its own bookkeeping when the item subscribes, in a burst with the
    //       plugin's first ask, so only gaps over half a second are the plugin's retries. Each gap is
    //       bounded below at 90% of its wait, since Qt's coarse timers fire up to 5% early (1899 ms has
    //       been seen for 2 s); the fourth also above, at 12 s, where the next doubling would be 16 s.
    // CONTROL: in DaemonWatch::askWhoOwns (backend/kde/wallpaperx_item.cpp), (a) `++m_failedAsks;`
    //       deleted -> "FAIL: a failed owner lookup is asked again after 2, 4, 8 and 8 s (gaps 2099 2059
    //       1999 1999 ms)"; (b) the retry's cap `kLookupRetryMaxMs` made `kWindowRetryMaxMs` -> the same
    //       line, "(gaps 1914 4000 7999 16000 ms)".
    // COVERAGE-ASSERT: four retry gaps must be seen within 45 s (22 s are due); fewer is a FAIL, since
    //       the monitor is shown working -- only a monitor that stopped is reported not verified.
    const auto retryGaps = [&monitor] {
        const std::vector<std::chrono::steady_clock::time_point> times = monitor.timesOf("GetNameOwner");
        std::vector<long long> gaps;
        for (size_t i = 1; i < times.size(); ++i) {
            const long long ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(times[i] - times[i - 1]).count();
            if (ms > 500) gaps.push_back(ms);
        }
        return gaps;
    };
    (void)pumpUntil([&] { return retryGaps().size() >= 4 || !monitor.ok(); }, 45000);
    const std::vector<long long> gaps = retryGaps();
    if (!monitor.ok()) {
        notVerified(kGoal, "the bus monitor stopped, so how the gaps grow cannot be read: " + monitor.why());
    } else {
        std::string seen;
        for (const long long gap : gaps) seen += (seen.empty() ? "" : " ") + std::to_string(gap);
        check(gaps.size() >= 4 && gaps[0] >= 1800 && gaps[1] >= 3600 && gaps[2] >= 7200 && gaps[3] >= 7200 &&
                  gaps[3] < 12000,
              ("a failed owner lookup is asked again after 2, 4, 8 and 8 s (gaps " + seen + " ms)").c_str());
    }

    // GOAL: S15.366 -- each lookup may take longer than the one before it: 2, 4, 8, 16, then 25 s
    //       (QtDBus's own default) and no longer, so a bus slower than the first ask allows is still
    //       answered in the end.
    // WHY-THIS-SHAPE: read as the plugin passes it to libdbus, interposed in this binary; this bus answers
    //       every lookup at once (refused), so the timeouts are the plugin's numbers and nothing else.
    // CONTROL: in DaemonWatch::askWhoOwns (backend/kde/wallpaperx_item.cpp), (a) the call's timeout made
    //       `kRefusedRetryMs` -> "FAIL: the owner lookups' timeouts grow 2, 4, 8, 16 and 25 s ([2000 2000 2000
    //       2000 2000])"; (b) its cap made `2 * kFetchPatienceMs` -> the same line, the fifth 32000.
    // COVERAGE-ASSERT: five timeouts are required; none seen at all means the interposer did not take
    //       effect, which is reported not verified.
    const std::vector<int> timeouts = lookupTimeouts();
    if (timeouts.empty()) {
        notVerified("S15.366", "none of the plugin's owner lookups was seen leaving this process");
    } else {
        check(timeouts.size() >= 5 &&
                  std::vector<int>(timeouts.begin(), timeouts.begin() + 5) == std::vector<int>{2000, 4000, 8000, 16000, 25000},
              ("the owner lookups' timeouts grow 2, 4, 8, 16 and 25 s (" + listed(timeouts) + ")").c_str());
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// ---------------------------------------------------------------- //
// The scripted stub, and the runs that need one.
// ---------------------------------------------------------------- //

constexpr const char* kNoFreeImage = "io.wallpaperx.Daemon.Error.NoFreeImage";
constexpr const char* kTooManyDescriptors = "io.wallpaperx.Daemon.Error.TooManyDescriptors";
constexpr const char* kNoFrame = "io.wallpaperx.Daemon.Error.NoFrame";

// The earliest the plugin's 2 s wait can end: Qt's coarse timers may fire up to 5% early, so only an
// ask before 90% of it is sooner than the wait.
constexpr int kEarliestBackoffEndMs = kExpectedRetryMs * 9 / 10;

// A stub daemon on a bus connection of its own, answering each GetFrame by a script kept per monitor:
// an error, a request held for the test to answer later, or a frame the item reads and claims but never
// draws (no render pass runs here). It answers at once, so a request's arrival on its clock is also when
// the answer before it left. Its own connection, not the item's: QtDBus answers a call made to its own
// connection locally, taking whatever reply the handler created, so nothing could be held there.
class ScriptedStub : public QDBusVirtualObject {
public:
    enum class Kind { Error, Hold, Frame };
    struct Answer {
        Kind kind = Kind::Error;
        QString error;
        QString message;
    };
    static Answer refuse(const char* name, const char* message = "this stub refuses this request") {
        return {Kind::Error, QString::fromUtf8(name), QString::fromUtf8(message)};
    }
    static Answer hold() { return {Kind::Hold, {}, {}}; }
    static Answer frame() { return {Kind::Frame, {}, {}}; }
    // What a monitor's script answers once it has run out: no frame yet, which the item neither retries
    // nor backs off from, so nothing after a script asks again on its own.
    static Answer nothingMore() { return refuse(kNoFrame, "this stub has nothing more for this screen"); }

    explicit ScriptedStub(QDBusConnection bus) : m_bus(std::move(bus)) { m_clock.start(); }

    void script(int monitor, const std::vector<Answer>& answers) {
        m_scripts[monitor].assign(answers.begin(), answers.end());
    }
    std::vector<qint64> times(int monitor) const {
        const auto it = m_times.find(monitor);
        return it == m_times.end() ? std::vector<qint64>{} : it->second;
    }
    int requests(int monitor) const { return static_cast<int>(times(monitor).size()); }
    // Request `index`'s arrival to the next one's, or -1 when that next one has not come.
    qint64 gapAfter(int monitor, size_t index) const {
        const std::vector<qint64> t = times(monitor);
        return index + 1 < t.size() ? t[index + 1] - t[index] : -1;
    }
    int held(int monitor) const {
        const auto it = m_held.find(monitor);
        return it == m_held.end() ? 0 : static_cast<int>(it->second.size());
    }
    quint64 generation(int monitor) const {
        const auto it = m_generations.find(monitor);
        return it == m_generations.end() ? 0 : it->second;
    }
    int malformed() const { return m_malformed; }
    qint64 nowMs() const { return m_clock.elapsed(); }

    // Answers the oldest request held for `monitor`; false when none is held.
    bool answerHeld(int monitor, const Answer& answer) {
        const auto it = m_held.find(monitor);
        if (it == m_held.end() || it->second.empty()) return false;
        const QDBusMessage request = it->second.front();
        it->second.pop_front();
        return m_bus.send(replyTo(request, monitor, answer));
    }

    // FrameReady for `monitor` naming `generation`, as the daemon announces a frame.
    bool announce(int monitor, quint64 generation) {
        QDBusMessage ready = QDBusMessage::createSignal(QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                                                        QStringLiteral("FrameReady"));
        ready.setArguments({monitor, static_cast<qulonglong>(generation)});
        return m_bus.send(ready);
    }

    QString introspect(const QString&) const override { return QString(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String(kInterface)) return false;
        if (message.member() == QLatin1String("ReleaseBuffer")) return connection.send(message.createReply());
        if (message.member() != QLatin1String("GetFrame")) return false;
        const wallpaperx::testing::FrameRequestArgs request = wallpaperx::testing::readFrameRequest(message.arguments());
        if (!request.ok) {
            ++m_malformed;
            return connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                                            QStringLiteral("this stub could not read the request")));
        }
        m_times[request.monitorId].push_back(m_clock.elapsed());
        std::deque<Answer>& left = m_scripts[request.monitorId];
        const Answer answer = left.empty() ? nothingMore() : left.front();
        if (!left.empty()) left.pop_front();
        if (answer.kind == Kind::Hold) {
            message.setDelayedReply(true);
            m_held[request.monitorId].push_back(message);
            return true;
        }
        return connection.send(replyTo(message, request.monitorId, answer));
    }

private:
    QDBusMessage replyTo(const QDBusMessage& request, int monitor, const Answer& answer) {
        if (answer.kind != Kind::Frame) return request.createErrorReply(answer.error, answer.message);
        // The daemon's reply shape around a memfd: read and claimed like any frame, never imported here.
        constexpr quint32 kSide = 16;
        constexpr quint64 kBytes = kSide * kSide * 4;
        const int fd = ::memfd_create("wallpaperx-stub-frame", MFD_CLOEXEC);
        if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(kBytes)) != 0) {
            if (fd >= 0) ::close(fd);
            return request.createErrorReply(QStringLiteral("io.wallpaperx.Daemon.Error.ExportFailed"),
                                            QStringLiteral("the stub could not make its frame's memory"));
        }
        const QDBusUnixFileDescriptor memory(fd);
        ::close(fd);  // QDBusUnixFileDescriptor took its own duplicate.
        const quint64 generation = ++m_generations[monitor];
        return request.createReply(wallpaperx::testing::frameReplyFields(
            {memory}, kSide, kSide, kBytes, /*handleType=*/1, generation, /*DRM_FORMAT_ARGB8888=*/0x34325241,
            /*DRM_FORMAT_MOD_LINEAR=*/0, {wallpaperx::testing::FramePlane{0, kSide * 4}}, QByteArray(16, '\0'),
            /*bufferId=*/1, ++m_handover));
    }

    QDBusConnection m_bus;
    QElapsedTimer m_clock;
    std::map<int, std::deque<Answer>> m_scripts;
    std::map<int, std::vector<qint64>> m_times;
    std::map<int, std::deque<QDBusMessage>> m_held;
    std::map<int, quint64> m_generations;
    quint64 m_handover = 0;
    int m_malformed = 0;
};

// An item on `monitor`, with every reason it published kept in order (`reasonsSeen`): a reason that
// is replaced within one turn of the event loop is still recorded.
QString scriptedQml(int monitor) {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: 64
    height: 64
    property Item rendererItem: renderer
    property string publishedReason: renderer.unavailableReason
    property string reasonsSeen: ""
    onPublishedReasonChanged: reasonsSeen += "[" + publishedReason + "]"

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: %1
    }
}
)")
        .arg(monitor);
}

// The stub on its own connection owning the daemon's name, and the window items join.
struct ScriptedRun {
    QString connectionName;
    QDBusConnection bus;
    ScriptedStub stub;
    QQmlEngine engine;
    QQuickRenderControl control;
    QQuickWindow window;
    bool ok = false;

    explicit ScriptedRun(const char* name)
        : connectionName(QString::fromUtf8(name)),
          bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName)),
          stub(bus),
          window(&control) {
        wallpaperx::testing::registerFrameReplyTypes();
        ok = bus.isConnected() && bus.registerService(QString::fromUtf8(kService)) &&
             bus.registerVirtualObject(QString::fromUtf8(kPath), &stub);
        engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
        window.setGeometry(0, 0, 64, 64);
    }
    ~ScriptedRun() {
        bus.unregisterObject(QString::fromUtf8(kPath));
        (void)bus.unregisterService(QString::fromUtf8(kService));
        QDBusConnection::disconnectFromBus(connectionName);
    }
    ScriptedRun(const ScriptedRun&) = delete;
    ScriptedRun& operator=(const ScriptedRun&) = delete;

    // A fresh item on `monitor`, in the window; null, reported, when the plugin's type will not load.
    std::unique_ptr<QObject> item(int monitor) {
        QQmlComponent component(&engine);
        component.setData(scriptedQml(monitor).toUtf8(),
                          QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                              QStringLiteral("/item_refusal_retry_m%1.qml").arg(monitor)));
        std::unique_ptr<QObject> root(component.create());
        auto* rootItem = qobject_cast<QQuickItem*>(root.get());
        if (rootItem == nullptr) {
            for (const QQmlError& error : component.errors()) {
                std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
            }
            check(false, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");
            return {};
        }
        rootItem->setParentItem(window.contentItem());
        return root;
    }

    // Returns once the items' connection has read everything the stub sent it and this thread has
    // handled it: a blocking Ping from the stub's connection to the items', which the bus delivers after
    // the stub's earlier messages and QtDBus answers on its own thread, then the event loop run over what
    // that queued. False when the Ping went unanswered.
    bool itemsCaughtUp() {
        const QDBusMessage ping =
            QDBusMessage::createMethodCall(QDBusConnection::sessionBus().baseService(), QStringLiteral("/"),
                                           QStringLiteral("org.freedesktop.DBus.Peer"), QStringLiteral("Ping"));
        const QDBusMessage pong = bus.call(ping, QDBus::Block, 3000);
        pumpFor(20);
        return pong.type() == QDBusMessage::ReplyMessage;
    }

    // The daemon replaced: the name dropped and taken again, which the plugin hears as a departure and
    // an arrival. False when the name could not be taken again.
    bool replaceDaemon() {
        const bool dropped = bus.unregisterService(QString::fromUtf8(kService));
        pumpFor(100);
        return dropped && bus.registerService(QString::fromUtf8(kService));
    }
};

QQuickItem* rendererOf(const std::unique_ptr<QObject>& root) {
    return qobject_cast<QQuickItem*>(root->property("rendererItem").value<QObject*>());
}

QString reasonsOf(const std::unique_ptr<QObject>& root) { return root->property("reasonsSeen").toString(); }

// Whether a measured wait was the plugin's first, 2 s one: never under 90% of it (coarse timers), and
// short of 3 s, where the next doubling is 4 s and any wait grown from an earlier refusal is longer.
bool firstWait(qint64 gapMs) { return gapMs >= kEarliestBackoffEndMs && gapMs < 3000; }

// A replaced daemon's error, of every kind the item branches on.
int runStaleRefusal() {
    const char* kGoal = "S15.359 (a replaced daemon's error starts no back-off, arms no retry and labels no screen)";
    ScriptedRun run("wallpaperx-refusal-stale");
    if (!run.ok) {
        notVerified(kGoal, "no bus connection could own io.wallpaperx.Daemon: " + run.bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    struct Case {
        int monitor;
        const char* error;
        const char* message;
        const char* forbidden;  // a reason the stale error would publish
    };
    const Case cases[] = {
        {0, kTooManyDescriptors, "the replaced daemon refuses descriptors",
         "WallpaperX has refused this desktop more frames for now."},
        {1, kNoFreeImage, "the replaced daemon has no free image", "WallpaperX has no image free for this screen yet."},
        {2, "org.freedesktop.DBus.Error.Failed", "the replaced daemon's bus error", "WallpaperX could not be reached."},
        {3, "io.wallpaperx.Daemon.Error.RenderFailed", "the replaced daemon's own words",
         "the replaced daemon's own words"},
    };
    std::vector<std::unique_ptr<QObject>> items;
    for (const Case& c : cases) {
        run.stub.script(c.monitor, {ScriptedStub::hold()});
        items.push_back(run.item(c.monitor));
        if (items.back() == nullptr) return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const auto allHeld = [&] {
        for (const Case& c : cases) {
            if (run.stub.held(c.monitor) != 1) return false;
        }
        return true;
    };
    if (!pumpUntil(allHeld, 5000) || !run.replaceDaemon()) {
        wallpaperx::testing::harnessFailure("the four requests were not all held, or the daemon's name could not be "
                                            "dropped and taken again");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    pumpFor(300);
    for (const Case& c : cases) {
        if (run.stub.requests(c.monitor) != 1) {
            notVerified(kGoal, "an item asked again while its first request was still out, so its fetch for the new "
                               "daemon is not the one the stale answer releases");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
    }
    const qint64 refusedAt = run.stub.nowMs();
    for (const Case& c : cases) (void)run.stub.answerHeld(c.monitor, ScriptedStub::refuse(c.error, c.message));
    const auto allAsked = [&] {
        for (const Case& c : cases) {
            if (run.stub.requests(c.monitor) < 2) return false;
        }
        return true;
    };
    (void)pumpUntil(allAsked, 3 * kExpectedRetryMs);
    // Long enough for a retry a stale NoFreeImage armed, or a back-off a stale refusal started, to ask.
    pumpFor(3 * kExpectedRetryMs);

    // GOAL: S15.359 -- a TooManyDescriptors refusal that arrives from a daemon already replaced starts no
    //       back-off: the new daemon is asked at once, before the old back-off could have ended.
    // WHY-THIS-SHAPE: the request is held by the stub until the name has been dropped and taken again, so
    //       the refusal is provably the old daemon's when it lands; the forced fetch for the new daemon is
    //       queued behind it and leaves as it lands. "At once" is before 90% of the 2 s back-off.
    // CONTROL: in WallpaperXItem::fetchFrame's reply handler (backend/kde/wallpaperx_item.cpp), the
    //       `&& asked.lifetime == BufferClaims::instance().currentDaemon().lifetime` clause deleted from
    //       `backOff` -> "FAIL: a TooManyDescriptors refusal from a replaced daemon starts no back-off (the new
    //       daemon asked 1962 ms after it)".
    const std::vector<qint64> first = run.stub.times(0);
    const qint64 askedAfter = first.size() >= 2 ? first[1] - refusedAt : -1;
    check(askedAfter >= 0 && askedAfter < kEarliestBackoffEndMs,
          ("a TooManyDescriptors refusal from a replaced daemon starts no back-off (the new daemon asked " +
           std::to_string(askedAfter) + " ms after it)")
              .c_str());

    // GOAL: S15.359 -- a NoFreeImage refusal from a replaced daemon arms no retry: after the new daemon has
    //       answered (no frame yet, which is not retried), nothing more is asked.
    // CONTROL: in WallpaperXItem::takeFrameReply (backend/kde/wallpaperx_item.cpp), the
    //       `if (from.lifetime != BufferClaims::instance().currentDaemon().lifetime) return;` deleted ->
    //       "FAIL: a NoFreeImage refusal from a replaced daemon arms no retry (3 request(s) where 2 were due)"
    //       and the reason lines below.
    check(run.stub.requests(1) == 2, ("a NoFreeImage refusal from a replaced daemon arms no retry (" +
                                      std::to_string(run.stub.requests(1)) + " request(s) where 2 were due)")
                                         .c_str());

    // GOAL: S15.359 -- no error from a replaced daemon is put on the screen: not a refusal's words, not
    //       "could not be reached" for a bus error, not the daemon's own message.
    // WHY-THIS-SHAPE: every reason the item published is kept by the QML in order, so one replaced within
    //       the same turn of the event loop by the new daemon's answer is still seen.
    // CONTROL: the stale return in WallpaperXItem::takeFrameReply deleted, as above -> "FAIL: an error from a
    //       replaced daemon is never put on the screen (monitor 0: ...[WallpaperX has refused this desktop
    //       more frames for now.]...)" for each of the four.
    for (size_t i = 0; i < std::size(cases); ++i) {
        const QString seen = reasonsOf(items[i]);
        check(!seen.contains(QString::fromUtf8(cases[i].forbidden)),
              ("an error from a replaced daemon is never put on the screen (monitor " +
               std::to_string(cases[i].monitor) + ": " + seen.toStdString() + ")")
                  .c_str());
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// A fetch whose answer outlives the item's patience, then comes.
int runPatience() {
    const char* kGoal = "S15.360 (a fetch the daemon answers after the item stopped waiting is asked again)";
    ScriptedRun run("wallpaperx-refusal-patience");
    if (!run.ok) {
        notVerified(kGoal, "no bus connection could own io.wallpaperx.Daemon: " + run.bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    constexpr int kBlank = 0;
    constexpr int kOlder = 1;
    run.stub.script(kBlank, {ScriptedStub::hold()});
    run.stub.script(kOlder, {ScriptedStub::frame(), ScriptedStub::hold()});
    std::unique_ptr<QObject> blank = run.item(kBlank);
    std::unique_ptr<QObject> older = run.item(kOlder);
    if (blank == nullptr || older == nullptr) return wallpaperx::testing::reportNotVerifiedOrPassed();
    // The older item has a frame's reply in hand before its newer fetch is held.
    if (!pumpUntil([&] { return run.stub.requests(kOlder) == 1 && run.stub.held(kBlank) == 1; }, 5000) ||
        !run.stub.announce(kOlder, run.stub.generation(kOlder) + 1) ||
        !pumpUntil([&] { return run.stub.held(kOlder) == 1; }, 5000)) {
        wallpaperx::testing::harnessFailure("the two fetches were not both held, the older item's after a frame");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const QString kUnreachable = QStringLiteral("WallpaperX could not be reached.");
    const auto reasonOf = [](const std::unique_ptr<QObject>& root) { return root->property("publishedReason").toString(); };
    const bool gaveUp =
        pumpUntil([&] { return reasonOf(blank) == kUnreachable && reasonOf(older) == kUnreachable; }, 30000);
    check(gaveUp, ("a fetch unanswered for 25 s says the daemon could not be reached (blank [" +
                   reasonOf(blank).toStdString() + "], older [" + reasonOf(older).toStdString() + "])")
                      .c_str());
    if (!gaveUp) return wallpaperx::testing::reportNotVerifiedOrPassed();
    pumpFor(500);
    const int blankWhileStuck = run.stub.requests(kBlank);
    const int olderWhileStuck = run.stub.requests(kOlder);
    (void)run.stub.answerHeld(kBlank, ScriptedStub::nothingMore());
    (void)run.stub.answerHeld(kOlder, ScriptedStub::nothingMore());
    (void)pumpUntil([&] { return run.stub.requests(kBlank) > blankWhileStuck && run.stub.requests(kOlder) > olderWhileStuck; },
                    3000);

    // GOAL: S15.360 -- once an unanswered fetch's late answer comes, the item asks again: an item with
    //       nothing on screen, and an item holding an older frame whose fetch for a newer one timed out.
    //       And while the call is unanswered it asks nothing more of that daemon.
    // WHY-THIS-SHAPE: two items, because the obvious repair -- ensureFrameRequested() after the late answer
    //       -- serves only the first: it asks only an item that has fetched nothing, and the second has a
    //       frame's reply in hand. Each item's first fetch is held by the stub past the 25 s patience.
    // CONTROL: in WallpaperXItem::fetchFrame (backend/kde/wallpaperx_item.cpp), (a) the patience timeout's
    //       `if (requestedMonitor == m_monitorId) queueFetch(generation);` deleted -> "FAIL: once a late
    //       answer comes, the item asks again (blank: 1 then 1; older frame: 2 then 2)"; (b) the same line
    //       deleted and the late reply's drain made `ensureFrameRequested();` -> the same line, "blank: 1 then
    //       2; older frame: 2 then 2"; (c) the patience timeout's `answered(...)` call deleted -> "FAIL: a fetch
    //       unanswered for 25 s says the daemon could not be reached".
    // COVERAGE-ASSERT: the counts while unanswered must be one held fetch each, or a re-ask could be
    //       something other than the late answer's.
    check(blankWhileStuck == 1 && olderWhileStuck == 2 && run.stub.requests(kBlank) > blankWhileStuck &&
              run.stub.requests(kOlder) > olderWhileStuck,
          ("once a late answer comes, the item asks again, and nothing while it waits (blank: " +
           std::to_string(blankWhileStuck) + " then " + std::to_string(run.stub.requests(kBlank)) + "; older frame: " +
           std::to_string(olderWhileStuck) + " then " + std::to_string(run.stub.requests(kOlder)) + ")")
              .c_str());
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// The waits after refusals, several items at once, one per monitor.
int runRefusalRuns() {
    const char* kGoal = "S15.362 and S15.363 (the waits after refusals grow per kind and start over)";
    ScriptedRun run("wallpaperx-refusal-runs");
    if (!run.ok) {
        notVerified(kGoal, "no bus connection could own io.wallpaperx.Daemon: " + run.bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    using S = ScriptedStub;
    const S::Answer N = S::refuse(kNoFreeImage);
    const S::Answer D = S::refuse(kTooManyDescriptors);
    enum : int { kGrowNoImage, kGrowDescriptors, kNoImageThenDescriptors, kDescriptorsThenNoImage, kLeaveNoImage,
                 kLeaveDescriptors, kServedDescriptors, kServedNoImage, kMonitors };
    run.stub.script(kGrowNoImage, {N, N, N, N, N, N});
    run.stub.script(kGrowDescriptors, {D, D, D, D, D, D});
    run.stub.script(kNoImageThenDescriptors, {N, N, D});
    run.stub.script(kDescriptorsThenNoImage, {D, D, N});
    run.stub.script(kLeaveNoImage, {N, N, N});
    run.stub.script(kLeaveDescriptors, {D, D, D});
    run.stub.script(kServedDescriptors, {D, D, S::frame(), D});
    run.stub.script(kServedNoImage, {N, N, S::frame(), N});
    // The requests each monitor's grading needs, the last one answered by "nothing more".
    const int due[kMonitors] = {6, 6, 4, 4, 4, 4, 5, 5};
    std::vector<std::unique_ptr<QObject>> items;
    for (int m = 0; m < kMonitors; ++m) {
        items.push_back(run.item(m));
        if (items.back() == nullptr) return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // Driven as the requests arrive: off the screen after the second refusal, and back; an announcement
    // once a frame has been served.
    qint64 hiddenAt[kMonitors] = {};
    bool shownAgain[kMonitors] = {};
    bool announced[kMonitors] = {};
    bool caughtUp = true;
    const auto drive = [&] {
        for (const int m : {int(kLeaveNoImage), int(kLeaveDescriptors)}) {
            // Off for 200 ms with the NoFreeImage retry armed; through the whole 4 s back-off for descriptors,
            // so it ends while the item is away. Only once the item has taken the second refusal, or it would
            // take it off the screen and start a run there.
            const qint64 away = m == kLeaveNoImage ? 200 : 4600;
            if (run.stub.requests(m) == 2 && hiddenAt[m] == 0) {
                caughtUp = run.itemsCaughtUp() && caughtUp;
                rendererOf(items[m])->setVisible(false);
                hiddenAt[m] = run.stub.nowMs();
            } else if (hiddenAt[m] != 0 && !shownAgain[m] && run.stub.nowMs() - hiddenAt[m] >= away) {
                rendererOf(items[m])->setVisible(true);
                shownAgain[m] = true;
            }
        }
        for (const int m : {int(kServedDescriptors), int(kServedNoImage)}) {
            if (run.stub.requests(m) == 3 && !announced[m]) {
                announced[m] = run.stub.announce(m, run.stub.generation(m) + 1);
            }
        }
        for (int m = 0; m < kMonitors; ++m) {
            if (run.stub.requests(m) < due[m]) return false;
        }
        return true;
    };
    // 2 + 4 + 8 + 16 + 16 s for the two growth runs, with margin; each wait that does not end in time is
    // read below as the failure it is.
    (void)pumpUntil(drive, 60000);
    if (!caughtUp) {
        wallpaperx::testing::harnessFailure("the items' connection did not answer the stub's Ping, so an item may have "
                                            "left the screen before taking its second refusal");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const auto gaps = [&](int m) {
        std::string out;
        for (size_t i = 0; i + 1 < run.stub.times(m).size(); ++i) {
            out += (out.empty() ? "" : " ") + std::to_string(run.stub.gapAfter(m, i));
        }
        return "[" + out + "]";
    };

    // GOAL: S15.362 -- refused over and over with nothing on screen, the wait before each new ask doubles
    //       from 2 s and stops at 16 s: 2, 4, 8, 16, 16 -- for NoFreeImage and for TooManyDescriptors alike.
    // WHY-THIS-SHAPE: gaps on the stub's clock, which answers at once; each bounded below at 90% of its
    //       wait (coarse timers) and the fifth above at 24 s, where an uncapped one would be 32 s.
    // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) WallpaperXItem::retryWhileNothingIsShown's
    //       `m_noImageRefusals++` made `m_noImageRefusals` -> "FAIL: NoFreeImage refusals are asked again after
    //       2, 4, 8, 16 and 16 s ([1900 1964 2000 2000 2000 2000] ms)"; (b) the back-off's
    //       `m_descriptorRefusals++` made `m_descriptorRefusals` -> the TooManyDescriptors line; (c)
    //       kRefusedRetryMaxMs made 32000 -> both lines, the fifth gap never measured ([1933 4000 8000 16000]).
    // COVERAGE-ASSERT: all five gaps must have been measured; a run that never asked again fails here.
    const auto grew = [&](int m) {
        const qint64 want[] = {2000, 4000, 8000, 16000, 16000};
        for (size_t i = 0; i < 5; ++i) {
            const qint64 gap = run.stub.gapAfter(m, i);
            if (gap < want[i] * 9 / 10) return false;
        }
        return run.stub.gapAfter(m, 4) < 24000;
    };
    check(grew(kGrowNoImage),
          ("NoFreeImage refusals are asked again after 2, 4, 8, 16 and 16 s (" + gaps(kGrowNoImage) + " ms)").c_str());
    check(grew(kGrowDescriptors), ("TooManyDescriptors refusals are asked again after 2, 4, 8, 16 and 16 s (" +
                                   gaps(kGrowDescriptors) + " ms)")
                                      .c_str());

    // GOAL: S15.363 -- each kind of refusal counts its own run: NoFreeImage refusals do not lengthen the
    //       first TooManyDescriptors back-off, nor the reverse.
    // WHY-THIS-SHAPE: two refusals of one kind (waits 2 and 4 s) then one of the other, whose wait must be
    //       the first, 2 s; counted from the other kind's run it would be 8 s.
    // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) the back-off's delay counted from `m_noImageRefusals++`
    //       -> "FAIL: two NoFreeImage refusals do not lengthen the TooManyDescriptors back-off after them ([1900
    //       3963 8000] ms)"; (b) retryWhileNothingIsShown's delay counted from `m_descriptorRefusals++` -> the
    //       reverse line. Each break shares one counter, so each reddens both lines.
    check(firstWait(run.stub.gapAfter(kNoImageThenDescriptors, 2)),
          ("two NoFreeImage refusals do not lengthen the TooManyDescriptors back-off after them (" +
           gaps(kNoImageThenDescriptors) + " ms)")
              .c_str());
    check(firstWait(run.stub.gapAfter(kDescriptorsThenNoImage, 2)),
          ("two TooManyDescriptors refusals do not lengthen the NoFreeImage wait after them (" +
           gaps(kDescriptorsThenNoImage) + " ms)")
              .c_str());

    // GOAL: S15.363 -- leaving the screen ends both runs: the first wait after the item comes back is 2 s,
    //       for NoFreeImage (whose 4 s retry was armed when it left) and for TooManyDescriptors.
    // WHY-THIS-SHAPE: the NoFreeImage item leaves with its 4 s retry armed and is back 200 ms later, so a
    //       retry left running would end the next wait at about 3.8 s; the descriptor item stays away through
    //       its back-off. Each wait graded is the one after the first refusal on return.
    // CONTROL: in WallpaperXItem::updateOnScreen (backend/kde/wallpaperx_item.cpp), (a)
    //       `m_noImageRetryTimer.stop();` deleted -> "FAIL: after leaving the screen and coming back, the first
    //       NoFreeImage wait is 2 s ([1929 220 3780] ms)"; (b) `endRefusalRuns();` deleted -> that line
    //       ([1935 220 7779] ms) and the TooManyDescriptors one ([1935 4640 8359] ms).
    check(firstWait(run.stub.gapAfter(kLeaveNoImage, 2)),
          ("after leaving the screen and coming back, the first NoFreeImage wait is 2 s (" + gaps(kLeaveNoImage) +
           " ms)")
              .c_str());
    check(firstWait(run.stub.gapAfter(kLeaveDescriptors, 2)),
          ("after leaving the screen and coming back, the first TooManyDescriptors back-off is 2 s (" +
           gaps(kLeaveDescriptors) + " ms)")
              .c_str());

    // GOAL: S15.363 -- a frame served ends both runs: the next refusal of either kind waits 2 s.
    // CONTROL: in WallpaperXItem::fetchFrame's reply handler (backend/kde/wallpaperx_item.cpp), the
    //       `endRefusalRuns();` for a ReplyMessage deleted -> "FAIL: after a frame is served, the next
    //       TooManyDescriptors back-off is 2 s (... 8000 ...)" and the NoFreeImage line.
    check(firstWait(run.stub.gapAfter(kServedDescriptors, 3)),
          ("after a frame is served, the next TooManyDescriptors back-off is 2 s (" + gaps(kServedDescriptors) + " ms)")
              .c_str());
    check(firstWait(run.stub.gapAfter(kServedNoImage, 3)),
          ("after a frame is served, the next NoFreeImage wait is 2 s (" + gaps(kServedNoImage) + " ms)").c_str());
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// Both runs started over by a change of daemon.
int runRefusalReplaced() {
    const char* kGoal = "S15.363 (a change of daemon ends both refusal runs)";
    ScriptedRun run("wallpaperx-refusal-replaced");
    if (!run.ok) {
        notVerified(kGoal, "no bus connection could own io.wallpaperx.Daemon: " + run.bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    using S = ScriptedStub;
    constexpr int kDescriptors = 0;
    constexpr int kNoImage = 1;
    // Descriptors: backing off 8 s when the daemon is replaced. NoFreeImage: two refusals, then an answer
    // that arms nothing, so no retry is running when the daemon is replaced.
    run.stub.script(kDescriptors, {S::refuse(kTooManyDescriptors), S::refuse(kTooManyDescriptors),
                                   S::refuse(kTooManyDescriptors), S::refuse(kTooManyDescriptors)});
    run.stub.script(kNoImage, {S::refuse(kNoFreeImage), S::refuse(kNoFreeImage), S::nothingMore(),
                               S::refuse(kNoFreeImage)});
    std::unique_ptr<QObject> descriptors = run.item(kDescriptors);
    std::unique_ptr<QObject> noImage = run.item(kNoImage);
    if (descriptors == nullptr || noImage == nullptr) return wallpaperx::testing::reportNotVerifiedOrPassed();
    if (!pumpUntil([&] { return run.stub.requests(kDescriptors) == 3 && run.stub.requests(kNoImage) == 3; }, 15000)) {
        check(false, ("refused items ask again on their own (" + std::to_string(run.stub.requests(kDescriptors)) +
                      " and " + std::to_string(run.stub.requests(kNoImage)) + " request(s) where 3 each were due)")
                         .c_str());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // Both third answers taken before the daemon changes, so neither lands on the new one.
    if (!run.itemsCaughtUp() || !run.replaceDaemon()) {
        wallpaperx::testing::harnessFailure("the items did not take the stub's answers, or the daemon's name could not "
                                            "be dropped and taken again");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    (void)pumpUntil([&] { return run.stub.requests(kDescriptors) >= 5 && run.stub.requests(kNoImage) >= 5; }, 20000);
    const auto gaps = [&](int m) {
        std::string out;
        for (size_t i = 0; i + 1 < run.stub.times(m).size(); ++i) {
            out += (out.empty() ? "" : " ") + std::to_string(run.stub.gapAfter(m, i));
        }
        return "[" + out + "]";
    };

    // GOAL: S15.363 -- a change of daemon ends both refusal runs: the new daemon's first refusal of either
    //       kind waits 2 s, however long the old daemon's had grown.
    // WHY-THIS-SHAPE: the descriptor item is backing off 8 s when the daemon changes, the NoFreeImage item has
    //       a run of two with no retry left running (one still running would govern the wait, which is a
    //       separate matter); the wait graded is after each one's first refusal from the new daemon.
    // CONTROL: `endRefusalRuns();` deleted from the `replaced` handler in WallpaperXItem::connectToDaemon
    //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: after the daemon is replaced, the first TooManyDescriptors
    //       back-off is 2 s ([1933 4000 120 15880] ms)" and the NoFreeImage line ([... 7880] ms).
    check(firstWait(run.stub.gapAfter(kDescriptors, 3)),
          ("after the daemon is replaced, the first TooManyDescriptors back-off is 2 s (" + gaps(kDescriptors) + " ms)")
              .c_str());
    check(firstWait(run.stub.gapAfter(kNoImage, 3)),
          ("after the daemon is replaced, the first NoFreeImage wait is 2 s (" + gaps(kNoImage) + " ms)").c_str());
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// A refusal that was asked before the item left the screen and answered while it was away.
int runRefusalAway() {
    const char* kGoal = "S15.363 (a refusal answered while the item is away starts no run)";
    ScriptedRun run("wallpaperx-refusal-away");
    if (!run.ok) {
        notVerified(kGoal, "no bus connection could own io.wallpaperx.Daemon: " + run.bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    using S = ScriptedStub;
    constexpr int kDescriptors = 0;
    constexpr int kNoImage = 1;
    run.stub.script(kDescriptors, {S::refuse(kTooManyDescriptors), S::hold(), S::refuse(kTooManyDescriptors)});
    run.stub.script(kNoImage, {S::refuse(kNoFreeImage), S::hold(), S::refuse(kNoFreeImage)});
    std::unique_ptr<QObject> descriptors = run.item(kDescriptors);
    std::unique_ptr<QObject> noImage = run.item(kNoImage);
    if (descriptors == nullptr || noImage == nullptr) return wallpaperx::testing::reportNotVerifiedOrPassed();
    const bool bothHeld = pumpUntil([&] { return run.stub.held(kDescriptors) == 1 && run.stub.held(kNoImage) == 1; },
                                    10000);
    if (!bothHeld || !run.itemsCaughtUp()) {
        wallpaperx::testing::harnessFailure("the second fetch of each item was not held, or the items did not take "
                                            "the first refusals");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    rendererOf(descriptors)->setVisible(false);
    rendererOf(noImage)->setVisible(false);
    pumpFor(100);
    const bool answered = run.stub.answerHeld(kDescriptors, S::refuse(kTooManyDescriptors)) &&
                          run.stub.answerHeld(kNoImage, S::refuse(kNoFreeImage));
    // Longer than any wait the refusals just answered could arm, so whatever they armed has run out.
    pumpFor(3000);
    rendererOf(descriptors)->setVisible(true);
    rendererOf(noImage)->setVisible(true);
    (void)pumpUntil([&] { return run.stub.requests(kDescriptors) >= 4 && run.stub.requests(kNoImage) >= 4; }, 15000);
    const auto gaps = [&](int m) {
        std::string out;
        for (size_t i = 0; i + 1 < run.stub.times(m).size(); ++i) {
            out += (out.empty() ? "" : " ") + std::to_string(run.stub.gapAfter(m, i));
        }
        return "[" + out + "]";
    };
    if (!answered) {
        wallpaperx::testing::harnessFailure("the held fetches could not be answered while the items were away");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // GOAL: S15.363 -- leaving the screen ends both refusal runs, also for a fetch the item asked before
    //       it left and the daemon refused while it was away: the first wait after it comes back is 2 s.
    // WHY-THIS-SHAPE: the second fetch of each item is held by the stub until both items have left, then
    //       refused, so the refusal provably lands off the screen; the items stay away 3 s, past every
    //       wait it could arm. The wait graded is the one after the first refusal on return.
    // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) `&& m_onScreen` removed from `backOff` in
    //       WallpaperXItem::fetchFrame's reply handler -> "FAIL: after a refusal answered while the item was
    //       away, the first TooManyDescriptors back-off on its return is 2 s ([1907 3120 3880] ms)"; (b)
    //       `!m_onScreen ||` removed from WallpaperXItem::retryWhileNothingIsShown -> "FAIL: ... the first
    //       NoFreeImage wait on its return is 2 s ([2100 3120 4012] ms)". Each break reddens only its line.
    check(firstWait(run.stub.gapAfter(kDescriptors, 2)),
          ("after a refusal answered while the item was away, the first TooManyDescriptors back-off on its return is "
           "2 s (" + gaps(kDescriptors) + " ms)")
              .c_str());
    check(firstWait(run.stub.gapAfter(kNoImage, 2)),
          ("after a refusal answered while the item was away, the first NoFreeImage wait on its return is 2 s (" +
           gaps(kNoImage) + " ms)")
              .c_str());
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// A NoFreeImage retry running when the daemon is replaced.
int runRefusalReplacedRetry() {
    const char* kGoal = "S15.374 (a replaced daemon's NoFreeImage retry does not outlive it)";
    ScriptedRun run("wallpaperx-refusal-replaced-retry");
    if (!run.ok) {
        notVerified(kGoal, "no bus connection could own io.wallpaperx.Daemon: " + run.bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    using S = ScriptedStub;
    constexpr int kNoImage = 0;
    // Two refusals from the old daemon (the second arms the 4 s retry), then one from the new one, whose
    // wait is graded; the fourth answer arms nothing.
    run.stub.script(kNoImage, {S::refuse(kNoFreeImage), S::refuse(kNoFreeImage), S::refuse(kNoFreeImage),
                               S::nothingMore()});
    std::unique_ptr<QObject> item = run.item(kNoImage);
    if (item == nullptr) return wallpaperx::testing::reportNotVerifiedOrPassed();
    if (!pumpUntil([&] { return run.stub.requests(kNoImage) == 2; }, 6000)) {
        check(false, ("a refused item asks again on its own (" + std::to_string(run.stub.requests(kNoImage)) +
                      " request(s) where 2 were due)")
                         .c_str());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // The second refusal taken, so its 4 s retry is armed before the daemon changes.
    if (!run.itemsCaughtUp() || !run.replaceDaemon()) {
        wallpaperx::testing::harnessFailure("the item did not take the stub's second refusal, or the daemon's name "
                                            "could not be dropped and taken again");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const qint64 replacedAfterMs = run.stub.nowMs() - run.stub.times(kNoImage)[1];
    (void)pumpUntil([&] { return run.stub.requests(kNoImage) >= 4; }, 10000);
    std::string gaps;
    for (size_t i = 0; i + 1 < run.stub.times(kNoImage).size(); ++i) {
        gaps += (gaps.empty() ? "" : " ") + std::to_string(run.stub.gapAfter(kNoImage, i));
    }

    // GOAL: S15.374 -- a NoFreeImage retry armed under the old daemon does not survive its replacement:
    //       the wait after the new daemon's first refusal is the first one, 2 s.
    // WHY-THIS-SHAPE: refusal-replaced deliberately leaves no retry running, so it cannot see a timer that
    //       outlives the daemon. Here the 4 s retry is armed when the daemon changes; left running, it makes
    //       retryWhileNothingIsShown return early and ends the graded wait at about 3.9 s instead of 2 s.
    // CONTROL: `m_noImageRetryTimer.stop();` deleted from the `replaced` handler in
    //       WallpaperXItem::connectToDaemon (backend/kde/wallpaperx_item.cpp) -> "FAIL: with a 4 s NoFreeImage
    //       retry running when the daemon is replaced, the first wait after the new daemon's refusal is 2 s
    //       ([2099 121 3966] ms)".
    // COVERAGE-ASSERT: the new daemon must be asked within 1 s of the second refusal; asked later, what was
    //       left of the old 4 s timer would itself fall under 3 s and the check could not tell them apart.
    //       A replacement this harness made late is its own fault; an ask late after a prompt one is the item's.
    // CONTROL: test-side, `pumpFor(1500);` before the replacement -> "FAIL (harness): the daemon was replaced
    //       1621 ms after the second refusal, too late ... ([1900 1621 1899] ms)" -- a run in which the graded
    //       wait alone would have passed. The replaced handler's closing `fetchFrame(0);` deleted
    //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: after the daemon is replaced, the item asks the new one
    //       within 1 s ([2099] ms)".
    if (replacedAfterMs >= 800) {
        wallpaperx::testing::harnessFailure("the daemon was replaced " + std::to_string(replacedAfterMs) +
                                            " ms after the second refusal, too late for the old retry to be told "
                                            "apart from a new one ([" + gaps + "] ms)");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const qint64 untilNewDaemonAsked = run.stub.gapAfter(kNoImage, 1);
    if (untilNewDaemonAsked < 0 || untilNewDaemonAsked >= 1000) {
        check(false, ("after the daemon is replaced, the item asks the new one within 1 s ([" + gaps + "] ms)").c_str());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    check(firstWait(run.stub.gapAfter(kNoImage, 2)),
          ("with a 4 s NoFreeImage retry running when the daemon is replaced, the first wait after the new daemon's "
           "refusal is 2 s ([" + gaps + "] ms)")
              .c_str());
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// The stderr line a TooManyDescriptors refusal writes, against the wait that follows it.
int runRefusalLog() {
    const char* kGoal = "S15.364 (the logged descriptor wait is the wait that follows)";
    ScriptedRun run("wallpaperx-refusal-log");
    if (!run.ok) {
        notVerified(kGoal, "no bus connection could own io.wallpaperx.Daemon: " + run.bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    using S = ScriptedStub;
    constexpr int kServed = 0;
    constexpr int kRefused = 1;
    run.stub.script(kServed, {S::frame()});
    run.stub.script(kRefused, {S::refuse(kTooManyDescriptors), S::refuse(kTooManyDescriptors),
                               S::refuse(kTooManyDescriptors)});
    wallpaperx::testing::StreamCapture capture(STDERR_FILENO);
    if (!capture.ok()) {
        wallpaperx::testing::harnessFailure("stderr could not be captured");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    std::unique_ptr<QObject> refused = run.item(kRefused);
    if (refused == nullptr) return wallpaperx::testing::reportNotVerifiedOrPassed();
    // The second item is served only after the first one's second refusal, which reopens the one-line
    // latch while the first one's run has grown: its next refusal logs a wait other than 2 s.
    std::unique_ptr<QObject> served;
    (void)pumpUntil(
        [&] {
            if (run.stub.requests(kRefused) == 2 && served == nullptr) served = run.item(kServed);
            return run.stub.requests(kRefused) >= 4;
        },
        25000);
    capture.restore();
    std::vector<int> logged;
    const std::string text = capture.text();
    const std::regex line("refusing this process frames, asking again after ([0-9]+) ms");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), line); it != std::sregex_iterator(); ++it) {
        logged.push_back(std::stoi((*it)[1].str()));
    }
    const qint64 waitAfterSecondLine = run.stub.gapAfter(kRefused, 2);

    // GOAL: S15.364 -- the wait the TooManyDescriptors line on stderr names is the wait that follows it,
    //       also when it is not the first 2 s.
    // WHY-THIS-SHAPE: two items, because with one the line can only ever name 2 s: the latch that allows a
    //       line reopens on a served frame, which also ends that item's run. Here a second item's frame
    //       reopens it while the first item's run has grown to 8 s. The wait is measured on the stub's clock
    //       and must lie within 10% below and 1.5 s above the number logged.
    // CONTROL: in WallpaperXItem::fetchFrame's reply handler (backend/kde/wallpaperx_item.cpp), the line's
    //       `delay` argument made `kRefusedRetryMs` -> "FAIL: the TooManyDescriptors line names the wait that
    //       follows it (logged [2000 2000], the second followed by 8000 ms)".
    // COVERAGE-ASSERT: two lines must have been logged and the second one's wait measured.
    const bool measured = logged.size() == 2 && waitAfterSecondLine >= 0;
    check(measured && logged[1] > kExpectedRetryMs && waitAfterSecondLine >= logged[1] * 9 / 10 &&
              waitAfterSecondLine < logged[1] + 1500,
          ("the TooManyDescriptors line names the wait that follows it (logged " + listed(logged) +
           ", the second followed by " + std::to_string(waitAfterSecondLine) + " ms)")
              .c_str());
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// A stand-in bus driver for the daemon's owner lookup, on the stub's connection: it answers each
// GetNameOwner it is sent after `delayMs`, naming the stub.
class SlowOwnerLookup : public QDBusVirtualObject {
public:
    SlowOwnerLookup(QDBusConnection bus, int delayMs) : m_bus(std::move(bus)), m_delayMs(delayMs) {}
    QString introspect(const QString&) const override { return QString(); }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection&) override {
        if (message.member() != QLatin1String("GetNameOwner")) return false;
        message.setDelayedReply(true);
        ++m_asked;
        QTimer::singleShot(m_delayMs, [bus = m_bus, message]() mutable {
            bus.send(message.createReply(QVariant(bus.baseService())));
        });
        return true;
    }
    int asked() const { return m_asked; }

private:
    QDBusConnection m_bus;
    int m_delayMs;
    int m_asked = 0;
};

int runSlowLookup() {
    const char* kGoal = "S15.366 (a lookup the bus answers late gets through)";
    ScriptedRun run("wallpaperx-refusal-slow-lookup");
    constexpr int kDelayMs = 3000;
    SlowOwnerLookup driver(run.bus, kDelayMs);
    if (!run.ok || !run.bus.registerVirtualObject(QStringLiteral("/org/freedesktop/DBus"), &driver)) {
        notVerified(kGoal, "no bus connection could own io.wallpaperx.Daemon: " + run.bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    {
        std::lock_guard<std::mutex> lock(g_lookupLock);
        g_redirectLookupsTo = run.bus.baseService().toStdString();
    }
    g_recordLookups.store(true);
    run.stub.script(0, {});
    std::unique_ptr<QObject> root = run.item(0);
    if (root == nullptr) return wallpaperx::testing::reportNotVerifiedOrPassed();
    const bool through = pumpUntil([&] { return run.stub.requests(0) > 0; }, 20000);
    g_recordLookups.store(false);
    run.bus.unregisterObject(QStringLiteral("/org/freedesktop/DBus"));
    const std::vector<int> timeouts = lookupTimeouts();

    // GOAL: S15.366 -- a bus that answers the owner lookup later than the first ask allows (3 s, against
    //       2 s) is still answered in the end: the next ask may take longer, so the item reaches the daemon.
    // WHY-THIS-SHAPE: the plugin's lookups, told apart from QtDBus's own by the timeout they carry, are sent
    //       by this binary's libdbus interposer to a stand-in driver on the stub's connection that answers
    //       3 s late; QtDBus drops the first answer as timed out, exactly as from a slow bus. The item
    //       reaching the stub at all is the verdict; the timeouts say why.
    // CONTROL: in DaemonWatch::askWhoOwns (backend/kde/wallpaperx_item.cpp), the call's timeout made
    //       `kRefusedRetryMs` -> "FAIL: an owner lookup the bus answers 3 s late gets through in the end (no
    //       fetch in 20 s; timeouts [2000 2000 2000 2000])".
    // COVERAGE-ASSERT: the stand-in must have been asked at least once, or the redirect never took.
    if (driver.asked() < 1) {
        wallpaperx::testing::harnessFailure("no owner lookup reached the stand-in bus driver, so the redirect never took");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    check(through && timeouts.size() >= 2 && timeouts[0] == 2000 && timeouts[1] == 4000,
          ("an owner lookup the bus answers 3 s late gets through in the end (" +
           std::string(through ? "fetched" : "no fetch in 20 s") + "; timeouts " + listed(timeouts) + ")")
              .c_str());
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_refusal_retry_test";

    QGuiApplication app(argc, argv);
    // Before anything owns a name or asks the bus a thing: this binary claims io.wallpaperx.Daemon.
    // GOAL: X.34 -- run anywhere but on the private bus isolated_run.sh starts, this binary claims no
    //       name and sends nothing.
    // CONTROL: test-side: run with WALLPAPERX_CALLER_NET_NS unset by `env -u` inside the entry's private
    //       bus -> "FAIL (harness): this test runs only on the private bus isolated_run.sh starts: this
    //       process was not started by isolated_run.sh ...", exit 1, and no GetFrame on the bus.
    if (std::string whyNotPrivate; !wallpaperx::testing::onPrivateTestBus(whyNotPrivate)) {
        wallpaperx::testing::harnessFailure("this test runs only on the private bus isolated_run.sh starts: " +
                                            whyNotPrivate);
        (void)wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
        return 1;
    }
    if (argc > 1) {
        const std::string run = argv[1];
        if (run == "lookup-failed") return runLookupFailed();
        if (run == "slow-lookup") return runSlowLookup();
        if (run == "stale-refusal") return runStaleRefusal();
        if (run == "patience") return runPatience();
        if (run == "refusal-runs") return runRefusalRuns();
        if (run == "refusal-replaced") return runRefusalReplaced();
        if (run == "refusal-log") return runRefusalLog();
        if (run == "refusal-away") return runRefusalAway();
        if (run == "refusal-replaced-retry") return runRefusalReplacedRetry();
    }
    if (argc > 1) {
        wallpaperx::testing::harnessFailure(std::string("unknown run '") + argv[1] + "'");
        (void)wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
        return 1;
    }

    const char* kGoals = "the refused-hand-over goals (a refusal with nothing on screen is reported and asked again, "
                         "and an ordinary no-frame answer is reported and not polled)";

    QDBusConnection bus = QDBusConnection::sessionBus();
    RefusingStub stub(QStringLiteral("io.wallpaperx.Daemon.Error.NoFreeImage"));
    if (!bus.isConnected() || !bus.registerService(QString::fromUtf8(kService)) ||
        !bus.registerVirtualObject(QString::fromUtf8(kPath), &stub)) {
        notVerified(kGoals, "no session bus this run could own io.wallpaperx.Daemon on: " +
                                bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));

    // A window the items live in, so that they count as being on the screen and fetch at all. Under
    // a render control that is never initialised, so no scene-graph pass ever runs: the item asks
    // for one on every hand-over and the answer must stay "nothing was drawn", or it would be
    // driving its paint watch instead of its retry timer.
    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.setGeometry(0, 0, 64, 64);

    // How many times the item asked while it was being refused, carried out of the first phase so
    // the second phase's "it did not ask again" can be read against a number that is known to be
    // reachable. Without it the second phase's span could be too short and nobody would know.
    int refusedRetries = 0;

    // ---------------------------------------------------------------- //
    // NoFreeImage, with nothing on screen.
    // ---------------------------------------------------------------- //
    {
        std::unique_ptr<QObject> root = makeItem(engine, window, "item_refusal_retry_refused.qml");
        // The module and the type are the plugin's own, so a load failure here is production's,
        // not this harness's -- and every check below is gated on it.
        // CONTROL: the import path replaced with a nonexistent directory -> `qml: ... module
        //       "com.wallpaperx" is not installed` and "FAIL: the com.wallpaperx module loads and
        //       WallpaperXRenderer instantiates". Reverted.
        check(root != nullptr, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");
        if (root == nullptr) {
            notVerified(kGoals, "the plugin's QML type could not be instantiated, so no item was ever refused");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }

        const auto publishedReason = [&root] { return root->property("publishedReason").toString(); };

        // GOAL: an item with nothing on screen that is refused SAYS SO. False for a plugin that
        //       reports nothing, which is the right answer only while a wallpaper is up: with the
        //       backdrop showing, silence is a blank desktop that explains nothing.
        // WHY-THIS-SHAPE: the reason is required to be non-empty and the item is required to have
        //       actually asked, in that order, so a run where the item never reached the bus
        //       reports the asking rather than the reason. The string itself is not compared:
        //       it is translated user-facing prose, and pinning it here would make every wording
        //       change a test failure while establishing nothing more.
        // CONTROL: the `else` arm of `if (m_frameShown)` in WallpaperXItem::takeFrameReply
        //       (backend/kde/wallpaperx_item.cpp) changed to `setFetchReason(QString());` ->
        //       "FAIL: an item with nothing on screen publishes a reason when it is refused",
        //       printing "the item asked 3 time(s) and published []". Reverted.
        const bool refusedAndReported =
            pumpUntil([&] { return stub.requests() > 0 && !publishedReason().isEmpty(); }, 5000);
        check(refusedAndReported, "an item with nothing on screen publishes a reason when it is refused");
        if (!refusedAndReported) {
            std::fprintf(stderr, "  the item asked %d time(s) and published [%s]\n", stub.requests(),
                         publishedReason().toStdString().c_str());
            notVerified(kGoals, "the item was never refused with a reason, so the retry below had nothing under it");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        check(!root->property("rendererDrawing").toBool(),
              "and it is not claiming a frame is on screen, which is the case the retry is for");

        // GOAL: and it asks again on its own. Nothing else will make it: the daemon's
        //       announcement of a freed image is sent when a consumer RELEASES one, and the
        //       consumer holding the ring is by definition one that has stopped releasing.
        //       Without this the screen stays on the backdrop until the user sets a wallpaper.
        // WHY-THIS-SHAPE: no FrameReady is ever sent on this bus, and the window the item lives in
        //       never runs a scene-graph pass, so nothing but the item's own timer can produce a
        //       second request. The obvious shape -- send a FrameReady and check it re-fetches --
        //       grades the announcement path, which already has coverage, and would pass for a
        //       plugin with no timer at all.
        // COVERAGE-ASSERT: the count at the moment the refusal was published is asserted to be
        //       exactly one, so a harness that made the item ask repeatedly on its own -- by
        //       rendering the scene, or by joining and leaving the window -- fails here rather
        //       than crediting those asks to the timer.
        // CONTROL: `retryWhileNothingIsShown();` deleted from the NoFreeImage branch of
        //       WallpaperXItem::takeFrameReply (backend/kde/wallpaperx_item.cpp) -> "FAIL: and it
        //       asks again on its own, with nothing having told it to", printing that it stayed
        //       at 1 request. Reverted.
        const int askedWhenReported = stub.requests();
        check(askedWhenReported == 1, "joining the window cost exactly one fetch, so a second is the timer's");
        if (askedWhenReported != 1) {
            std::fprintf(stderr, "  the item had asked %d time(s) before the retry window opened; something other "
                                 "than the timer is driving it and this phase cannot attribute the next ask\n",
                         askedWhenReported);
        }
        const bool askedAgain = pumpUntil([&] { return stub.requests() > askedWhenReported; }, kRetryWindowMs);
        check(askedAgain, "and it asks again on its own, with nothing having told it to");
        if (!askedAgain) {
            std::fprintf(stderr, "  it stayed at %d request(s) for %d ms after being refused, with no announcement "
                                 "coming\n",
                         stub.requests(), kRetryWindowMs);
        }

        // COVERAGE-ASSERT: how many retries actually landed in the window, carried into the
        // second phase. That phase's claim is that a DIFFERENT answer produces none over the same
        // span, and it is worth nothing unless this one produced some.
        refusedRetries = stub.requests() - askedWhenReported;
    }

    // ---------------------------------------------------------------- //
    // NoFrame, the ordinary state before a wallpaper has ever been set.
    // ---------------------------------------------------------------- //
    stub.answerWith(QStringLiteral("io.wallpaperx.Daemon.Error.NoFrame"));
    {
        // Read BEFORE the item exists, because the item's first fetch can complete during
        // construction: subscribing to FrameReady is a blocking call to the bus daemon, and Qt
        // dispatches incoming messages while one is outstanding. Measured -- taken afterwards,
        // this count already included the request and the check below read zero.
        const int askedBefore = stub.requests();
        std::unique_ptr<QObject> root = makeItem(engine, window, "item_refusal_retry_noframe.qml");
        if (root == nullptr) {
            wallpaperx::testing::harnessFailure("the plugin's QML type instantiated once and then would not again");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        const auto publishedReason = [&root] { return root->property("publishedReason").toString(); };

        const bool reported = pumpUntil(
            [&] { return stub.requests() > askedBefore && !publishedReason().isEmpty(); }, 5000);
        check(reported, "an item told there is no frame yet publishes a reason too");
        if (!reported) {
            std::fprintf(stderr, "  the item asked %d time(s) since the answer changed and published [%s]\n",
                         stub.requests() - askedBefore, publishedReason().toStdString().c_str());
            notVerified(kGoals, "the second item never reached the bus, so the no-polling check had nothing under it");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }

        // GOAL: and it does NOT ask again. "No frame yet" is answered by the user setting a
        //       wallpaper, which arrives as a FrameReady; a plugin that treated it like a
        //       refusal would ask the daemon every couple of seconds for the life of a fresh
        //       install, on every screen.
        // WHY-THIS-SHAPE: measured over the same span as the retry above, in the same process,
        //       against a count carried out of it -- so "it did not ask again" is read beside a
        //       demonstration that the span is long enough for asking again to be seen. A span
        //       checked on its own could be too short and this check would pass for a plugin
        //       that polls everything.
        // CONTROL: `retryWhileNothingIsShown();` moved out of the NoFreeImage branch and into
        //       the general error path of WallpaperXItem::takeFrameReply (backend/kde/
        //       wallpaperx_item.cpp), just before its `return` -> "FAIL: and it does not ask
        //       again -- a frame that has never existed is not something to poll for", printing
        //       the extra requests. Reverted.
        // COVERAGE-ASSERT: the retry count from the first phase is asserted non-zero here, so a
        //       run in which nothing ever retried cannot report this silence as established.
        check(refusedRetries > 0, "the same span produced at least one retry when the answer was a refusal");
        if (refusedRetries <= 0) {
            std::fprintf(stderr, "  the refusal phase saw no retry, so %d ms of silence here says nothing\n",
                         kRetryWindowMs);
        } else {
            const int askedWhenReported = stub.requests();
            pumpFor(kRetryWindowMs);
            const int extra = stub.requests() - askedWhenReported;
            check(extra == 0, "and it does not ask again -- a frame that has never existed is not something to poll "
                              "for");
            if (extra != 0) {
                std::fprintf(stderr, "  it asked %d more time(s) in %d ms, where a refusal produced %d\n", extra,
                             kRetryWindowMs, refusedRetries);
            }
        }
    }

    // ---------------------------------------------------------------- //
    // TooManyDescriptors, with nothing on screen.
    // ---------------------------------------------------------------- //
    stub.answerWith(QStringLiteral("io.wallpaperx.Daemon.Error.TooManyDescriptors"));
    {
        const int askedBefore = stub.requests();
        std::unique_ptr<QObject> root = makeItem(engine, window, "item_refusal_retry_descriptors.qml");
        if (root == nullptr) {
            wallpaperx::testing::harnessFailure("the plugin's QML type instantiated once and then would not again");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        const auto publishedReason = [&root] { return root->property("publishedReason").toString(); };

        // GOAL: S15.272 -- an item showing nothing that the daemon refuses for holding too many
        //       descriptors says so in those words, and asks again on its own once its back-off ends.
        //       False for an item that stays blank and silent: only this process can end the refusal,
        //       so the daemon announces nothing that would make it ask.
        // WHY-THIS-SHAPE: the exact words are compared, as item_import_cache_test compares its blank
        //       reason: another refusal's text would satisfy "some reason". The re-ask is read over the
        //       same window as the NoFreeImage retry above, with no FrameReady on this bus and no
        //       scene-graph pass, so only the item's own back-off can produce it.
        // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) WallpaperXItem::takeFrameReply's
        //       TooManyDescriptors arm's reason made `QString()` -> "FAIL: an item with nothing on screen
        //       refused for descriptors says so (published [] after 2 request(s))"; (b)
        //       WallpaperXItem::reopenFetchAfterBackoff reduced to reopening the latch, its fetch of what is
        //       queued deleted -> "FAIL: and it asks again once its back-off ends, with nothing having told it to".
        const QString kDescriptorRefusal = QStringLiteral("WallpaperX has refused this desktop more frames for now.");
        const bool reported = pumpUntil(
            [&] { return stub.requests() > askedBefore && publishedReason() == kDescriptorRefusal; }, 5000);
        check(reported, ("an item with nothing on screen refused for descriptors says so (published [" +
                         publishedReason().toStdString() + "] after " + std::to_string(stub.requests() - askedBefore) +
                         " request(s))")
                            .c_str());
        check(!root->property("rendererDrawing").toBool(), "and it is not claiming a frame is on screen");
        const int askedWhenReported = stub.requests();
        const bool askedAgain = pumpUntil([&] { return stub.requests() > askedWhenReported; }, kRetryWindowMs);
        check(askedAgain, "and it asks again once its back-off ends, with nothing having told it to");
    }

    // ---------------------------------------------------------------- //
    // TooManyDescriptors, then the daemon replaced during the back-off.
    // ---------------------------------------------------------------- //
    {
        const int askedBefore = stub.requests();
        std::unique_ptr<QObject> root = makeItem(engine, window, "item_refusal_retry_replaced.qml");
        if (root == nullptr) {
            wallpaperx::testing::harnessFailure("the plugin's QML type instantiated once and then would not again");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        const auto publishedReason = [&root] { return root->property("publishedReason").toString(); };

        // GOAL: S15.348 -- a refusal for descriptors is the daemon's that made it: when that daemon is
        //       replaced while the item backs off, the new one is asked at once, not when the old
        //       back-off would have ended.
        // WHY-THIS-SHAPE: "at once" is graded as "before the old back-off could have ended", measured
        //       from the refusal on the stub's own clock. The back-off is a Qt coarse timer, which may fire
        //       up to 5% early, so only an ask before 90% of it counts as at once; a replacement that took
        //       most of that itself leaves no margin, and is reported not verified. The daemon is replaced by
        //       dropping and re-taking the name, which the item hears as a departure and an arrival.
        // CONTROL: the `if (m_fetch == Fetch::BackingOff) {...}` reset deleted from the `replaced`
        //       handler in WallpaperXItem::connectToDaemon (backend/kde/wallpaperx_item.cpp) -> "FAIL: a
        //       daemon that replaced the one refusing descriptors is asked at once, before the old back-off
        //       ends (asked 1999 ms after the refusal, replaced at 100 ms)".
        const QString kDescriptorRefusal = QStringLiteral("WallpaperX has refused this desktop more frames for now.");
        const bool refused = pumpUntil(
            [&] { return stub.requests() > askedBefore && publishedReason() == kDescriptorRefusal; }, 5000);
        if (!refused) {
            check(false, "an item with nothing on screen refused for descriptors says so before the replacement");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        const qint64 refusedAt = stub.requestTimesMs().back();
        const int askedAtRefusal = stub.requests();
        const bool dropped = bus.unregisterService(QString::fromUtf8(kService));
        pumpFor(100);
        const bool retaken = dropped && bus.registerService(QString::fromUtf8(kService));
        const qint64 replacedAt = stub.nowMs();
        if (!retaken) {
            wallpaperx::testing::harnessFailure("the daemon's name could not be dropped and taken again");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        if (replacedAt - refusedAt > kEarliestBackoffEndMs - 500 || stub.requests() != askedAtRefusal) {
            notVerified("S15.348", "the replacement took " + std::to_string(replacedAt - refusedAt) +
                                       " ms after the refusal, or the item asked meanwhile, so the old back-off left no "
                                       "margin to tell its end from an ask at once");
        } else {
            const bool asked = pumpUntil([&] { return stub.requests() > askedAtRefusal; }, 3 * kExpectedRetryMs);
            const qint64 askedAt = asked ? stub.requestTimesMs()[static_cast<size_t>(askedAtRefusal)] : -1;
            check(asked && askedAt - refusedAt < kEarliestBackoffEndMs,
                  ("a daemon that replaced the one refusing descriptors is asked at once, before the old back-off "
                   "ends (asked " +
                   std::to_string(asked ? askedAt - refusedAt : -1) + " ms after the refusal, replaced at " +
                   std::to_string(replacedAt - refusedAt) + " ms)")
                      .c_str());
        }
    }

    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
