// SPDX-License-Identifier: GPL-2.0-or-later

// The packaged QML around the renderer, run for real: main.qml, WallpaperSurface.qml and
// ScreenWindows.qml from the source tree, the plugin's own WallpaperXRenderer, and a stub daemon on
// both of the channels main.qml and the item use -- /config over loopback HTTP on the daemon's port,
// and the screen reports on this run's private bus (tests/report_stub.h). Offscreen; nothing renders.
//
// Three phases (WALLPAPERX_TEST_PHASE):
//   taskmanager-missing -- WallpaperSurface.qml still loads when Plasma's task manager module will
//                          not, and the screen is reported as having no window;
//   copies-switch       -- two copies of the plugin on one screen, as Plasma keeps one per activity:
//                          hiding one and showing the other in one call leaves the screen reported
//                          visible;
//   copies-restart      -- with both copies alive, a daemon restart is answered by the shown copy
//                          alone;
//   unloaded-renderer   -- a renderer the Loader has unloaded, not yet deleted, is not reached by
//                          main.qml's monitor-id or screen-name changes;
//   config-reads        -- main.qml reads /config when the screen is named, at each ConfigChanged
//                          and on a rename, and at nothing else;
//   config-owner        -- it reads when the daemon's bus name loses its owner and when it gains one;
//   config-supersede    -- notices during a slow read cause one more read after it, never two at once;
//   config-rename       -- an answer about a name the screen no longer has is never applied;
//   config-stream       -- answers are applied while notices keep arriving faster than reads end;
//   config-named        -- run as `plasma_surface_test config-named`, a process of its own (the entry
//                          plasma_surface_named): a daemon already named when the first change watch
//                          is created causes one more read, from the watch's first owner lookup.
//
// Plasma's task manager and KWindowSystem modules are replaced by stand-ins on the import path
// (tests/qml_standins/), so ScreenWindows.qml loads the same way on any host.

#include "tests/loopback_stub.h"
#include "tests/pump.h"
#include "tests/report_stub.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QGuiApplication>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QPointer>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::phaseSelected;
using wallpaperx::testing::pumpFor;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::ReportStub;

const QString kOutput = QStringLiteral("DP-ACTIVITY");
const QString kVisible = QStringLiteral("SetOutputVisible");
const QString kFocus = QStringLiteral("SetOutputFocus");
const QString kSize = QStringLiteral("SetOutputSize");
constexpr int kArrivalLimitMs = 5000;

const QString kUiDir = QStringLiteral(WALLPAPERX_BACKEND_SOURCE_DIR "/kde/kpackage/contents/ui");

// /config, answering every screen as known, set, and on monitorId (0 unless a phase moves it, or
// names an id for that screen) -- what makes main.qml load its renderer. Records the screen each
// request asked about, and how many were unanswered at once. The answer is fixed when the request
// arrives, so a held request carries the state from before anything the test changed meanwhile.
class ConfigStub {
public:
    ~ConfigStub() { stop(); }
    std::atomic<int> monitorId{0};
    bool start(std::string& why) {
        m_server.Get("/config", [this](const httplib::Request& req, httplib::Response& res) {
            const std::string output = req.get_param_value("output");
            int id = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                const auto named = m_idFor.find(output);
                id = named != m_idFor.end() ? named->second : monitorId.load();
                m_asked.push_back(output);
                m_peak = std::max(m_peak, ++m_inFlight);
                if (m_holding) {
                    const int ticket = ++m_held;
                    m_changed.notify_all();
                    // Bounded, so a phase that never releases cannot wedge the server's stop.
                    m_changed.wait_for(lock, std::chrono::seconds(20), [&] { return !m_holding || m_released >= ticket; });
                }
            }
            res.set_content(R"({"output":"DP-ACTIVITY","known":true,"path":"/w.png","monitor_id":)" + std::to_string(id) + "}",
                            "application/json");
            // Before the reply is written, so a read main.qml sends only after this answer is never
            // counted as overlapping it.
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_inFlight;
        });
        if (!wallpaperx::testing::bindDaemonPort(m_server)) {
            why = "127.0.0.1:18462 could not be bound -- another daemon or test holds main.qml's port";
            return false;
        }
        if (!wallpaperx::testing::serveOnThread(m_server, m_thread)) {
            why = "the /config stub's listener gave up before serving";
            return false;
        }
        return true;
    }
    void stop() {
        unhold();
        wallpaperx::testing::stopServing(m_server, m_thread);
    }

    // Requests so far, and those that asked about `output`.
    int requests() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_asked.size());
    }
    int requestsFor(const QString& output) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(std::count(m_asked.begin(), m_asked.end(), output.toStdString()));
    }

    // The answer for `output` from now on, whatever monitorId says.
    void setIdFor(const QString& output, int id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_idFor[output.toStdString()] = id;
    }

    // Most requests unanswered at once since resetPeak().
    int peak() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_peak;
    }
    void resetPeak() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_peak = m_inFlight;
    }

    // From hold(), the n-th request to arrive is not answered until releaseThrough(n) or unhold().
    // held() counts the requests that have arrived since hold().
    void hold() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_holding = true;
        m_held = 0;
        m_released = 0;
    }
    void releaseThrough(int n) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = n;
        }
        m_changed.notify_all();
    }
    void unhold() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_holding = false;
        }
        m_changed.notify_all();
    }
    int held() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_held;
    }

private:
    httplib::Server m_server;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::string> m_asked;
    std::map<std::string, int> m_idFor;
    int m_inFlight = 0;
    int m_peak = 0;
    bool m_holding = false;
    int m_held = 0;
    int m_released = 0;
};

// Counts a signal of `target`'s, by signature, from the moment it is made.
class SignalCount : public QObject {
    Q_OBJECT
public:
    SignalCount(QObject* target, const char* signature) {
        const int index = target != nullptr ? target->metaObject()->indexOfSignal(signature) : -1;
        m_connected = index >= 0 && QMetaObject::connect(target, index, this, staticMetaObject.indexOfSlot("hit()"));
    }
    bool connected() const { return m_connected; }
    int count = 0;
public slots:
    void hit() { ++count; }

private:
    bool m_connected = false;
};

// Every value a property of `target`'s takes, through its notify signal.
class PropertyTrail : public QObject {
    Q_OBJECT
public:
    PropertyTrail(QObject* target, const char* name) : m_target(target), m_name(name) {
        const QMetaObject* meta = target->metaObject();
        const QMetaProperty property = meta->property(meta->indexOfProperty(name));
        m_connected = property.hasNotifySignal() &&
                      QMetaObject::connect(target, property.notifySignalIndex(), this, staticMetaObject.indexOfSlot("record()"));
    }
    bool connected() const { return m_connected; }
    bool took(int value) const { return std::find(values.begin(), values.end(), value) != values.end(); }
    std::vector<int> values;
public slots:
    void record() { values.push_back(m_target->property(m_name).toInt()); }

private:
    QObject* m_target;
    const char* m_name;
    bool m_connected = false;
};

std::string joined(const std::vector<int>& values) {
    std::string out;
    for (int v : values) out += (out.empty() ? "" : ", ") + std::to_string(v);
    return out.empty() ? "(none)" : out;
}

// Makes Plasma's task manager module unloadable in one engine, whatever the host has installed.
class BlockTaskManager : public QQmlAbstractUrlInterceptor {
public:
    QUrl intercept(const QUrl& path, DataType type) override {
        if (type != QmldirFile || !path.toString().contains(QLatin1String("org/kde/taskmanager"))) return path;
        ++blocked;
        return QUrl::fromLocalFile(QStringLiteral("/nonexistent/wallpaperx-taskmanager-blocked-by-this-test/qmldir"));
    }
    int blocked = 0;
};

// One copy of the wallpaper plugin, as Plasma holds one per activity: main.qml in a parent Item that
// is shown or hidden the way Plasma shows or hides a containment.
struct Copy {
    std::unique_ptr<QQmlComponent> component;
    std::unique_ptr<QObject> holder;
    QQuickItem* parentItem() const { return qobject_cast<QQuickItem*>(holder.get()); }
    QObject* plugin() const { return holder ? qvariant_cast<QObject*>(holder->property("plugin")) : nullptr; }
    QObject* surfaceLoader() const {
        QObject* main = plugin();
        return main != nullptr ? main->findChild<QObject*>(QStringLiteral("wallpaperxSurface")) : nullptr;
    }
    QObject* renderer() const {
        QObject* loader = surfaceLoader();
        return loader != nullptr ? qvariant_cast<QObject*>(loader->property("item")) : nullptr;
    }
    // The change watch main.qml loaded, whose configChanged is what makes it read.
    QObject* configWatch() const {
        QObject* main = plugin();
        QObject* loader = main != nullptr ? main->findChild<QObject*>(QStringLiteral("wallpaperxConfigWatch")) : nullptr;
        return loader != nullptr ? qvariant_cast<QObject*>(loader->property("item")) : nullptr;
    }
};

Copy makeCopy(QQmlEngine& engine, QQuickWindow& window, bool shown) {
    Copy copy;
    copy.component = std::make_unique<QQmlComponent>(&engine);
    const QByteArray qml = "import QtQuick\nItem { width: 400; height: 300\n"
                           "  property alias plugin: loader.item\n"
                           "  Loader { id: loader; anchors.fill: parent; source: \"" +
                           QUrl::fromLocalFile(kUiDir + QStringLiteral("/main.qml")).toString().toUtf8() + "\" } }\n";
    copy.component->setData(qml, QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + QStringLiteral("/copy.qml")));
    copy.holder.reset(copy.component->create());
    if (copy.holder == nullptr || copy.plugin() == nullptr) {
        for (const QQmlError& error : copy.component->errors()) std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
        copy.holder.reset();
        return copy;
    }
    copy.parentItem()->setVisible(shown);
    copy.parentItem()->setParentItem(window.contentItem());
    // Naming the screen is what makes main.qml ask /config; nothing here asks for it.
    copy.plugin()->setProperty("connectorOverride", kOutput);
    return copy;
}

int monitorIdOf(const Copy& copy) { return copy.plugin()->property("monitorId").toInt(); }

std::vector<bool> visibilitySince(const ReportStub& stub, std::size_t mark) {
    std::vector<bool> out;
    for (const ReportStub::Call& call : stub.calls(kVisible, kOutput, mark)) out.push_back(call.args.value(1).toBool());
    return out;
}

std::string describe(const std::vector<bool>& values) {
    std::string out;
    for (bool v : values) out += (out.empty() ? "" : ", ") + std::string(v ? "true" : "false");
    return out.empty() ? "(none)" : out;
}

void addImportPaths(QQmlEngine& engine) {
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_STANDIN_QML_PATH));
}

// GOAL: S15.389 -- a daemon that already owns its bus name when the first change watch in the
//       process is created still causes one more /config read, from that watch's first owner lookup:
//       main.qml's first read may have gone before the daemon's port answered, and nothing else
//       would ever announce that daemon. False if the screen is left on its first answer.
// WHY-THIS-SHAPE: a process of its own (the entry plasma_surface_named), because the lookup answers
//       once per process; in the main entry the config phases spend it before they count. Exactly two
//       reads, the second within 5 s and nothing more over 1.5 s: the first is main.qml's own at
//       naming, and nothing here announces anything.
// CONTROL: the `connect(watch, &DaemonWatch::named, ...)` block deleted from ConfigWatch's constructor
//       (backend/kde/wallpaperx_item.cpp) -> "a daemon already named when the first change watch is
//       created causes one more /config read" red, with 1 read. Via control_run.sh 2026-10-08. Reverted.
// COVERAGE-ASSERT: the name is owned before main.qml is created (main() claims it first), and the
//       change watch must have loaded, or the phase is not verified.
void configNamedPhase() {
    constexpr const char* kClause = "S15.389's first-lookup clause";
    ConfigStub config;
    std::string why;
    if (!config.start(why)) {
        notVerified(kClause, why);
        return;
    }
    QQmlEngine engine;
    addImportPaths(engine);
    QQuickWindow window;
    window.setGeometry(0, 0, 400, 300);
    window.show();
    Copy a = makeCopy(engine, window, true);
    if (!a.holder) {
        notVerified(kClause, "main.qml could not be loaded");
        return;
    }
    const bool first = pumpUntil([&] { return config.requests() >= 1; }, kArrivalLimitMs);
    const bool second = first && pumpUntil([&] { return config.requests() >= 2; }, kArrivalLimitMs);
    pumpFor(1500);
    if (a.configWatch() == nullptr) {
        notVerified(kClause, "main.qml's change watch did not load, so no lookup could answer through it");
        return;
    }
    check(first, "a named screen reads /config");
    check(second && config.requests() == 2,
          "a daemon already named when the first change watch is created causes one more /config read");
    std::fprintf(stderr, "  reads: %d\n", config.requests());
}

// Creates a change watch of the plugin's own type and waits for its first owner lookup to answer:
// that answer comes once per process, so no main.qml copy made afterwards reads /config because of it.
bool spendFirstLookup() {
    QQmlEngine engine;
    addImportPaths(engine);
    QQmlComponent component(&engine);
    component.setData("import com.wallpaperx\nWallpaperXConfigWatch {}\n",
                      QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + QStringLiteral("/first_lookup.qml")));
    std::unique_ptr<QObject> watch(component.create());
    SignalCount heard(watch.get(), "configChanged()");
    return heard.connected() && pumpUntil([&] { return heard.count > 0; }, kArrivalLimitMs);
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "plasma_surface_test";
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const bool namedRun = argc > 1 && std::strcmp(argv[1], "config-named") == 0;
    QGuiApplication app(argc, argv);

    QDBusConnection bus = wallpaperx::testing::stubConnection();
    ReportStub stub;
    if (!wallpaperx::testing::claimDaemonName(bus, stub)) {
        notVerified("every goal in this file", "no session bus this run could own io.wallpaperx.Daemon on: " +
                                                   bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    if (namedRun) {
        if (phaseSelected("config-named")) configNamedPhase();
        if (wallpaperx::testing::reportCheckFailures()) return 1;
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    const bool readsSelected = phaseSelected("config-reads");
    const bool ownerSelected = phaseSelected("config-owner");
    const bool supersedeSelected = phaseSelected("config-supersede");
    const bool renameSelected = phaseSelected("config-rename");
    const bool streamSelected = phaseSelected("config-stream");
    const bool anyConfigPhase = readsSelected || ownerSelected || supersedeSelected || renameSelected || streamSelected;
    // GOAL (a property of this harness): the config phases count reads with the first owner lookup's
    //       extra read already spent, so no count depends on which phases ran before. False when a
    //       phase run alone counts that read as one main.qml made for its own reasons.
    // WHY-THIS-SHAPE: spent first, before any phase makes a watch or an item, and waited for on the
    //       watch's own signal rather than for a fixed time. If it never answers, the config phases are
    //       not verified: a lookup still due could land inside any count.
    // CONTROL: config-reads run alone without this call -> "a named screen reads /config once" red with
    //       2 reads, the defect the confirming review found. By hand 2026-10-08, restored.
    bool lookupSpent = false;
    if (anyConfigPhase) {
        lookupSpent = spendFirstLookup();
        if (!lookupSpent) {
            notVerified("the config phases", "the change watch's first owner lookup did not answer within 5 s, so it "
                                             "could still add a read inside any count");
        }
    }

    if (phaseSelected("taskmanager-missing")) {
        // GOAL: WallpaperSurface.qml loads, and its renderer reports, when Plasma's task manager
        //       module cannot be imported: the window watcher sits behind a Loader of its own, so a
        //       failure there loses the two window facts and nothing else -- not the wallpaper.
        // WHY-THIS-SHAPE: the module is made unloadable by this test rather than assumed absent (the
        //       host has it installed), and the watcher's Loader is required to be in Error, so a run
        //       in which the block did nothing cannot pass.
        // CONTROL: `import org.kde.taskmanager as TaskManager` and a `TaskManager.TasksModel {}` added
        //       to WallpaperSurface.qml (backend/kde/kpackage/contents/ui/) -> "FAIL: WallpaperSurface.qml
        //       loads without the task manager module". This file's controls observed 2026-10-01
        //       through control_batch.sh, each phase alone, and reverted.
        // COVERAGE-ASSERT: the interceptor must have rewritten a task manager qmldir.
        QQmlEngine engine;
        BlockTaskManager block;
        engine.addUrlInterceptor(&block);
        engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
        QQuickWindow window;
        window.setGeometry(0, 0, 400, 300);
        window.show();
        QQmlComponent component(&engine, QUrl::fromLocalFile(kUiDir + QStringLiteral("/WallpaperSurface.qml")));
        std::unique_ptr<QObject> surface(component.create());
        check(surface != nullptr, "WallpaperSurface.qml loads without the task manager module");
        if (surface == nullptr) {
            for (const QQmlError& error : component.errors()) std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
        } else {
            qobject_cast<QQuickItem*>(surface.get())->setParentItem(window.contentItem());
            QObject* windows = surface->findChild<QObject*>(QStringLiteral("wallpaperxWindows"));
            check(block.blocked > 0, "this test made org.kde.taskmanager unloadable, rather than hoping it was absent");
            check(windows != nullptr && windows->property("status").toInt() == 3,
                  "and the window watcher's own Loader is the one in error");
            const std::size_t mark = stub.mark();
            surface->setProperty("monitorId", 0);
            surface->setProperty("output", kOutput);
            const bool reported = pumpUntil([&] { return !stub.calls(kFocus, kOutput, mark).empty(); }, kArrivalLimitMs);
            check(reported && !stub.calls(kFocus, kOutput, mark).front().args.value(1).toBool() &&
                      !stub.calls(kFocus, kOutput, mark).front().args.value(2).toBool(),
                  "and the screen is reported as having no focused or maximized window");
        }
    }

    // The main.qml phases need its port and the stand-in window modules.
    ConfigStub config;
    std::string why;
    const bool copiesSelected = phaseSelected("copies-switch");
    const bool restartSelected = phaseSelected("copies-restart");
    const bool unloadedSelected = phaseSelected("unloaded-renderer");
    const bool configPhases = anyConfigPhase && lookupSpent;
    const bool anyMainPhase = copiesSelected || restartSelected || unloadedSelected || configPhases;
    if (anyMainPhase && !config.start(why)) {
        notVerified("the main.qml phases", why);
    } else if (anyMainPhase) {
        QQmlEngine engine;
        addImportPaths(engine);
        QQuickWindow window;
        window.setGeometry(0, 0, 400, 300);
        window.show();

        if (copiesSelected) {
            // GOAL: hiding one copy and showing another in one call -- what Plasma does on an activity
            //       switch -- leaves the screen reported visible: no "not visible" from the copy going
            //       away arrives after the "visible" from the copy arriving.
            // WHY-THIS-SHAPE: both copies in one process, so their reports share one connection and
            //       arrive in the order they were sent; across processes nothing orders them and the
            //       check would grade the scheduler. Graded on the sequence after the switch: none of
            //       its values may be false once one is true, and the last must be true.
            // CONTROL: in WallpaperXItem::updateOnScreen's off-screen branch (backend/kde/wallpaperx_item.cpp),
            //       `reportVisible();` deferred with `QTimer::singleShot(0, this, [this] { reportVisible(); });`
            //       -> "FAIL: an activity switch leaves the screen reported visible". Unbroken, the sequence
            //       after the switch reads "false, true".
            Copy a = makeCopy(engine, window, true);
            Copy b = makeCopy(engine, window, false);
            const bool up = a.holder && b.holder &&
                            pumpUntil([&] { return a.renderer() != nullptr && !stub.calls(kSize, kOutput).empty(); },
                                      kArrivalLimitMs);
            if (!up) {
                notVerified("the copies-switch phase", "the shown copy never loaded its renderer and reported its size");
            } else {
                pumpFor(300);
                check(b.renderer() == nullptr, "the hidden copy has no renderer before the switch");
                const std::size_t mark = stub.mark();
                a.parentItem()->setVisible(false);
                b.parentItem()->setVisible(true);
                pumpFor(1000);
                const std::vector<bool> seen = visibilitySince(stub, mark);
                bool falseAfterTrue = false, sawTrue = false;
                for (bool v : seen) {
                    if (v) sawTrue = true;
                    if (!v && sawTrue) falseAfterTrue = true;
                }
                check(!seen.empty() && seen.back() && !falseAfterTrue, "an activity switch leaves the screen reported visible");
                std::fprintf(stderr, "  SetOutputVisible after the switch: %s\n", describe(seen).c_str());
                check(b.renderer() != nullptr && a.renderer() == nullptr,
                      "and the renderer is the shown copy's alone after it");
            }
        }

        if (restartSelected) {
            // GOAL: when every copy is asked to report again -- a daemon restart -- only the copy on
            //       the screen answers: one SetOutputVisible, true, and one SetOutputFocus. A hidden
            //       copy that still had a renderer would send "not visible" under the same name, and
            //       whichever arrived last would decide whether the user's video plays.
            // WHY-THIS-SHAPE: counted, not ordered: with two answers their order is a race, measured
            //       wrong in 5 of 10 restarts before the change, so only the count can say there was
            //       nothing to race. Both copies have been shown once before the restart, so the hidden
            //       one has had a renderer.
            //       Renderers unloaded earlier are still alive here (pumpFor never runs deleteLater),
            //       so the count also grades their silence.
            // CONTROL: `root.visible &&` dropped from the surface Loader's `active` in main.qml
            //       (backend/kde/kpackage/contents/ui/main.qml) -> "FAIL: a daemon restart is answered by
            //       the shown copy alone". `if (m_onScreen) reportVisible();` made `reportVisible();` in
            //       the DaemonWatch::replaced handler of WallpaperXItem::connectToDaemon
            //       (backend/kde/wallpaperx_item.cpp) -> the same line red: the unloaded renderers each
            //       send "not visible" after the shown copy's "visible". The second re-run 2026-10-05 via
            //       control_batch.sh, --phase '^copies-restart$'.
            Copy a = makeCopy(engine, window, true);
            Copy b = makeCopy(engine, window, false);
            const bool up = a.holder && b.holder && pumpUntil([&] { return a.renderer() != nullptr; }, kArrivalLimitMs);
            if (!up) {
                notVerified("the copies-restart phase", "the shown copy never loaded its renderer");
            } else {
                a.parentItem()->setVisible(false);
                b.parentItem()->setVisible(true);
                pumpFor(500);
                b.parentItem()->setVisible(false);
                a.parentItem()->setVisible(true);
                pumpFor(800);
                const std::size_t mark = stub.mark();
                if (!wallpaperx::testing::restartDaemonName(bus)) {
                    notVerified("the copies-restart phase", "the stub could not give up and retake the bus name");
                } else {
                    const bool sized = pumpUntil([&] { return !stub.calls(kSize, kOutput, mark).empty(); }, kArrivalLimitMs);
                    pumpFor(1000);
                    const std::vector<bool> seen = visibilitySince(stub, mark);
                    const std::size_t focus = stub.calls(kFocus, kOutput, mark).size();
                    check(sized && seen.size() == 1 && seen.front(), "a daemon restart is answered by the shown copy alone");
                    check(sized && focus == 1, "and with one window report");
                    std::fprintf(stderr, "  after the restart: SetOutputVisible %s; %zu SetOutputFocus\n",
                                 describe(seen).c_str(), focus);
                }
            }
        }

        if (unloadedSelected) {
            // GOAL: a renderer the Loader has unloaded but not yet deleted is not told main.qml's new
            //       monitor id or screen name. setMonitorId and setOutput send visibility without the
            //       on-screen gate, so a renderer reached off the screen would send "not visible" under
            //       the name while another copy shows it.
            // WHY-THIS-SHAPE: one copy, hidden, so its unloaded renderer is the only thing that could
            //       send; the renderer is held by QPointer and must still be alive (pumpFor never runs
            //       deleteLater), and main.qml must have taken the new id, or the phase proves nothing.
            //       Graded on the wire and on the renderer's own properties.
            // CONTROL: main.qml given `property QtObject lastRenderer: null`, set in the Loader's
            //       onLoaded, and onMonitorIdChanged writing `root.lastRenderer.monitorId` instead of
            //       `surface.item.monitorId` -> both lines red; the same done to onConnectorChanged for
            //       `output` -> both lines red (backend/kde/kpackage/contents/ui/main.qml). The monitor-id
            //       one re-run 2026-10-07 via control_batch.sh once the stub announced ConfigChanged.
            const QString moved = QStringLiteral("DP-ACTIVITY-MOVED");
            const std::size_t start = stub.mark();
            Copy a = makeCopy(engine, window, true);
            const bool up = a.holder && pumpUntil([&] { return a.renderer() != nullptr && !stub.calls(kSize, kOutput, start).empty(); },
                                                  kArrivalLimitMs);
            if (!up) {
                notVerified("the unloaded-renderer phase", "the copy never loaded its renderer and reported its size");
            } else {
                pumpFor(300);
                QPointer<QObject> unloaded = a.renderer();
                a.parentItem()->setVisible(false);
                pumpFor(300);
                if (unloaded.isNull() || a.renderer() != nullptr) {
                    notVerified("the unloaded-renderer phase", "the Loader kept its renderer, or the renderer was deleted at "
                                                               "once, so there is no unloaded renderer to test");
                } else {
                    const std::size_t mark = stub.mark();
                    config.monitorId = 1;
                    // As the daemon does when the app moves a screen: main.qml reads /config only when told.
                    const bool taken = wallpaperx::testing::announceConfigChanged(bus) &&
                                       pumpUntil([&] { return monitorIdOf(a) == 1; }, kArrivalLimitMs);
                    a.plugin()->setProperty("connectorOverride", moved);
                    pumpFor(1500);
                    if (!taken || unloaded.isNull()) {
                        notVerified("the unloaded-renderer phase", "main.qml never took the new monitor id from /config, or "
                                                                   "the unloaded renderer was deleted meanwhile");
                    } else {
                        const std::size_t sent = stub.callsFor(kOutput, mark).size() + stub.callsFor(moved, mark).size();
                        check(sent == 0, "an unloaded renderer sends nothing when main.qml's monitor id and screen name change");
                        check(unloaded->property("monitorId").toInt() == 0 && unloaded->property("output").toString() == kOutput,
                              "and its monitor id and screen name stay as they were");
                        std::fprintf(stderr, "  after the changes: %s | %s\n",
                                     ReportStub::describe(stub.callsFor(kOutput, mark)).c_str(),
                                     ReportStub::describe(stub.callsFor(moved, mark)).c_str());
                    }
                }
            }
            config.monitorId = 0;
        }

        // A copy shown, named, and holding its first answer: the start every config phase counts from.
        // Earlier phases asked about the same screen, so the first answer is counted from here.
        const auto answered = [&](const char* phase) {
            const int askedBefore = config.requestsFor(kOutput);
            Copy a = makeCopy(engine, window, true);
            const bool up = a.holder && pumpUntil(
                                            [&] {
                                                return config.requestsFor(kOutput) > askedBefore &&
                                                       a.plugin()->property("screenState").toString() == QLatin1String("set");
                                            },
                                            kArrivalLimitMs);
            if (!up) {
                notVerified(phase, "main.qml never asked /config about its screen and showed the answer");
                a.holder.reset();
            } else {
                pumpFor(300);
            }
            return a;
        };

        if (readsSelected && lookupSpent) {
            // GOAL: S15.389 -- main.qml reads /config once when its screen is named and then not at all
            //       while nothing is announced; each ConfigChanged causes exactly one read, whose answer
            //       is applied; and a rename reads once, about the new name.
            // WHY-THIS-SHAPE: counted on the stub's own record of requests, over a quiet span of 3.5 s,
            //       long enough for any periodic re-read every 1 s or faster to ask at least three times.
            //       Exactly one, in any phase order, because main() spent the change watch's first owner
            //       lookup before any config phase (config-named grades that extra read on its own). The
            //       answer is graded by the monitor id it carries, which only the notice's read can bring.
            // CONTROL: in backend/kde/kpackage/contents/ui/main.qml, a `Timer { interval: 1000;
            //       repeat: true; running: true; triggeredOnStart: true; onTriggered: root.refresh() }`
            //       added -> "a named screen reads /config once, and not again with nothing announced" red,
            //       and the exactly-one-read line; re-run with the lookup spent first, via control_run.sh
            //       2026-10-08, --phase '^config-reads$'. The body of the `onConfigChanged` handler emptied -> "a
            //       ConfigChanged causes exactly one read, and its answer is applied" alone. The root
            //       `onConnectorChanged: root.refresh()` deleted -> "a screen being named makes main.qml read
            //       /config", the quiet-span line and "a renamed screen reads once" red.
            // COVERAGE-ASSERT: the change watch must have loaded, so a run in which no notice could ever
            //       arrive does not pass the quiet half on that alone.
            // Not answered(): here the first read is graded, not assumed -- a main.qml that never asks at
            // naming fails this phase rather than skipping it.
            const int base = config.requests();
            Copy a = makeCopy(engine, window, true);
            const bool firstRead =
                a.holder && pumpUntil(
                                [&] {
                                    return config.requests() > base &&
                                           a.plugin()->property("screenState").toString() == QLatin1String("set");
                                },
                                kArrivalLimitMs);
            if (!a.holder) notVerified("the config-reads phase", "main.qml could not be loaded");
            if (a.holder) {
                check(firstRead, "a screen being named makes main.qml read /config, and the answer is shown");
                QObject* watch = a.plugin()->findChild<QObject*>(QStringLiteral("wallpaperxConfigWatch"));
                check(watch != nullptr && watch->property("status").toInt() == 1,
                      "main.qml's change watch loads from the build tree's module");
                pumpFor(3500);
                const int quiet = config.requests();
                check(quiet == base + 1, "a named screen reads /config once, and not again with nothing announced");

                config.monitorId = 3;
                const bool sent = wallpaperx::testing::announceConfigChanged(bus);
                const bool applied = sent && pumpUntil([&] { return monitorIdOf(a) == 3; }, kArrivalLimitMs);
                pumpFor(500);
                check(applied && config.requests() == quiet + 1, "a ConfigChanged causes exactly one read, and its answer is applied");

                const QString renamed = QStringLiteral("DP-ACTIVITY-RENAMED");
                const int beforeRename = config.requests();
                a.plugin()->setProperty("connectorOverride", renamed);
                pumpUntil([&] { return config.requestsFor(renamed) >= 1; }, kArrivalLimitMs);
                pumpFor(500);
                check(config.requestsFor(renamed) == 1 && config.requests() == beforeRename + 1,
                      "a renamed screen reads once, about its new name");
                std::fprintf(stderr, "  requests: %d after naming and the quiet span, %d in all; monitor id %d\n",
                             quiet - base, config.requests() - base, monitorIdOf(a));
            }
            config.monitorId = 0;
        }

        if (ownerSelected && lookupSpent) {
            // GOAL: S15.390 -- main.qml reads /config when the daemon's bus name loses its owner and again
            //       when it gains one, with nothing else announced.
            // WHY-THIS-SHAPE: the name is released and the departure's read waited for before the name
            //       is taken again, so the two reads are told apart; restartDaemonName's fixed gap would
            //       let one read stand for both.
            // CONTROL: `connect(watch, &DaemonWatch::replaced, this, &ConfigWatch::configChanged);` deleted
            //       from ConfigWatch's constructor (backend/kde/wallpaperx_item.cpp) -> both lines red, no read
            //       at all. Re-run 2026-10-08 via control_run.sh, --phase '^config-owner$'. Reverted.
            Copy a = answered("the config-owner phase");
            if (a.holder) {
                const int before = config.requests();
                if (!bus.unregisterService(QString::fromUtf8(wallpaperx::testing::kService))) {
                    notVerified("the config-owner phase", "the stub could not give up the daemon's bus name");
                } else {
                    const bool onDeparture = pumpUntil([&] { return config.requests() > before; }, kArrivalLimitMs);
                    pumpFor(300);
                    const int departed = config.requests() - before;
                    const bool back = bus.registerService(QString::fromUtf8(wallpaperx::testing::kService));
                    const bool onArrival =
                        back && pumpUntil([&] { return config.requests() > before + departed; }, kArrivalLimitMs);
                    check(onDeparture, "the daemon's bus name losing its owner makes main.qml read /config");
                    if (!back) {
                        notVerified("the config-owner phase's arrival half", "the stub could not take the name back");
                    } else {
                        check(onArrival, "and gaining one makes it read again");
                    }
                    std::fprintf(stderr, "  reads on the departure: %d; in all since: %d\n", departed,
                                 config.requests() - before);
                }
            }
        }

        if (supersedeSelected && lookupSpent) {
            // GOAL: S15.391 -- ConfigChanged notices that arrive while a read is unanswered cause exactly
            //       one more read, sent only once that read is answered, and the screen ends on the state
            //       from after the notices. False if notices pile reads up (one per notice, or two out at
            //       once), or if the queued read never happens and the screen keeps the slow answer.
            // WHY-THIS-SHAPE: the stub holds the first read until the change watch has emitted for every
            //       notice, so the whole burst provably landed while it was out -- a fixed delay would let a
            //       slow machine finish the read mid-burst and make a third read legitimate. It stays held
            //       1 s more, the span the overlap line is read over: a read sent at a notice reaches the
            //       stub some turns of the event loop later, and releasing as soon as the notices were heard
            //       let such reads arrive after the release, one at a time, and pass that line. The held
            //       read carries 4, fixed when it arrived; the stub says 5 from then on, so only a read sent
            //       after the notices can bring 5.
            // CONTROL: in refresh() (backend/kde/kpackage/contents/ui/main.qml), via control_run.sh
            //       2026-10-08, --phase '^config-supersede$', each reverted. `root.askAgain = true;`
            //       deleted -> "notices during a slow read cause exactly one more read after it" and "the
            //       screen ends on the state from after the notices" red. The askAgain block replaced by
            //       aborting the pending request and sending a new one -> the exactly-one and "never more
            //       than one read out at a time" lines red, 21 reads and 6 out at once.
            // COVERAGE-ASSERT: all kBurst notices must have reached main.qml's watch while the first read
            //       was held, or the phase is not verified.
            constexpr int kBurst = 20;
            constexpr int kHeldSpanMs = 1000;
            Copy a = answered("the config-supersede phase");
            SignalCount notices(a.holder ? a.configWatch() : nullptr, "configChanged()");
            if (a.holder && !notices.connected()) notVerified("the config-supersede phase", "main.qml's change watch was not found");
            if (a.holder && notices.connected()) {
                const int before = config.requests();
                config.resetPeak();
                config.hold();
                config.monitorId = 4;
                const bool slowOut = wallpaperx::testing::announceConfigChanged(bus) &&
                                     pumpUntil([&] { return config.held() >= 1; }, kArrivalLimitMs);
                config.monitorId = 5;
                bool heard = false;
                if (slowOut) {
                    for (int i = 0; i < kBurst; ++i) wallpaperx::testing::announceConfigChanged(bus);
                    heard = pumpUntil([&] { return notices.count >= 1 + kBurst; }, kArrivalLimitMs);
                    // Still held, long enough for any read the notices sent to reach the stub.
                    if (heard) pumpFor(kHeldSpanMs);
                }
                const int heldMeanwhile = config.held();
                config.unhold();
                if (!slowOut || !heard) {
                    notVerified("the config-supersede phase", !slowOut ? "the first read never reached the stub"
                                                                       : "main.qml's watch did not hear every notice while "
                                                                         "the first read was held");
                } else {
                    const bool settled = pumpUntil(
                        [&] { return config.requests() >= before + 2 && monitorIdOf(a) == 5; }, kArrivalLimitMs);
                    pumpFor(1000);
                    const int reads = config.requests() - before;
                    check(settled && reads == 2, "notices during a slow read cause exactly one more read after it");
                    check(config.peak() == 1, "and never more than one read is out at a time");
                    check(monitorIdOf(a) == 5, "and the screen ends on the state from after the notices");
                    std::fprintf(stderr, "  %d notices: %d reads, %d of them while the first was held; at most %d out at "
                                         "once; monitor id %d\n",
                                 notices.count, reads, heldMeanwhile, config.peak(), monitorIdOf(a));
                }
            }
            config.monitorId = 0;
        }

        if (renameSelected && lookupSpent) {
            // GOAL: S15.400 -- a /config answer about a name the screen no longer has is never applied: renamed
            //       while the old name's read is out, the screen takes the new name's answer only. False
            //       if the old name's monitor id is applied, even briefly -- the renderer would ask for
            //       another screen's frames.
            // WHY-THIS-SHAPE: the old name's read is held at the stub across the rename, so it is
            //       answered after it on any machine. Every value monitorId takes is recorded through its
            //       notify signal, so a value overwritten within one turn of the event loop still shows;
            //       sampling the property would miss it.
            // CONTROL: `asked === root.connector` made `true` in refresh()'s handler
            //       (backend/kde/kpackage/contents/ui/main.qml) -> "a read about the old name, answered
            //       after a rename, is never applied" red, monitor id 7 then 8. Via control_run.sh
            //       2026-10-08, --phase '^config-rename$'. Reverted.
            // COVERAGE-ASSERT: the held read must have asked about the old name, and the trail must be
            //       connected, or the phase is not verified.
            const QString renamed = QStringLiteral("DP-ACTIVITY-RENAMED-WHILE-READING");
            Copy a = answered("the config-rename phase");
            if (a.holder) {
                PropertyTrail ids(a.plugin(), "monitorId");
                const int oldAsked = config.requestsFor(kOutput);
                config.hold();
                config.monitorId = 7;
                config.setIdFor(renamed, 8);
                const bool oldOut = ids.connected() && wallpaperx::testing::announceConfigChanged(bus) &&
                                    pumpUntil([&] { return config.held() >= 1; }, kArrivalLimitMs) &&
                                    config.requestsFor(kOutput) == oldAsked + 1;
                if (oldOut) a.plugin()->setProperty("connectorOverride", renamed);
                config.unhold();
                if (!oldOut) {
                    notVerified("the config-rename phase", "no read about the old name was held at the stub, or the id "
                                                           "trail could not be connected");
                } else {
                    const bool took = pumpUntil([&] { return monitorIdOf(a) == 8; }, kArrivalLimitMs);
                    pumpFor(500);
                    check(took && !ids.took(7), "a read about the old name, answered after a rename, is never applied");
                    check(took && config.requestsFor(renamed) == 1, "and the new name is read once, and its answer applied");
                    std::fprintf(stderr, "  monitor id took: %s\n", joined(ids.values).c_str());
                }
            }
            config.monitorId = 0;
        }

        if (streamSelected && lookupSpent) {
            // GOAL: S15.401 -- while notices keep arriving faster than reads are answered, each answer is still
            //       applied: the screen follows the daemon during the stream, not only once it stops.
            //       False if an answer with another read queued behind it is dropped -- a steady stream
            //       of notices would then leave the screen on its first answer.
            // WHY-THIS-SHAPE: each read is held until a notice has been heard while it was out, so every
            //       answer has a read queued behind it on any machine; notices merely sent faster than
            //       reads finish would grade the scheduler. Each read carries its own monitor id, fixed
            //       when it arrived, and is released only after the next id is set, so the id applied
            //       names the read.
            // CONTROL: `&& !root.askAgain` added to refresh()'s `asked === root.connector` test
            //       (backend/kde/kpackage/contents/ui/main.qml) -> "with a notice queued behind every
            //       read, each answer is still applied" red, 0 of 5 applied. Via control_run.sh
            //       2026-10-08, --phase '^config-stream$'. Reverted.
            // COVERAGE-ASSERT: the check requires kStreamReads answers, each with a notice heard during
            //       its read, so a stream that never formed cannot pass.
            constexpr int kStreamReads = 5;
            Copy a = answered("the config-stream phase");
            SignalCount notices(a.holder ? a.configWatch() : nullptr, "configChanged()");
            if (a.holder && !notices.connected()) notVerified("the config-stream phase", "main.qml's change watch was not found");
            if (a.holder && notices.connected()) {
                int next = 100;
                config.hold();
                config.monitorId = next;
                bool formed = wallpaperx::testing::announceConfigChanged(bus);
                int applied = 0;
                for (int k = 1; formed && k <= kStreamReads; ++k) {
                    const int heard = notices.count;
                    formed = pumpUntil([&] { return config.held() >= k; }, kArrivalLimitMs) &&
                             wallpaperx::testing::announceConfigChanged(bus) &&
                             pumpUntil([&] { return notices.count > heard; }, kArrivalLimitMs);
                    if (!formed) break;
                    const int carried = next;
                    config.monitorId = ++next;
                    config.releaseThrough(k);
                    if (!pumpUntil([&] { return monitorIdOf(a) == carried; }, kArrivalLimitMs)) break;
                    ++applied;
                }
                config.unhold();
                if (!formed) {
                    notVerified("the config-stream phase", "read " + std::to_string(applied + 1) +
                                                               " never reached the stub, or no notice was heard while "
                                                               "it was held");
                } else {
                    check(applied == kStreamReads, "with a notice queued behind every read, each answer is still applied");
                    std::fprintf(stderr, "  %d of %d answers applied during the stream; monitor id %d\n", applied,
                                 kStreamReads, monitorIdOf(a));
                }
                pumpFor(500);
            }
            config.monitorId = 0;
        }
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

#include "plasma_surface_test.moc"
