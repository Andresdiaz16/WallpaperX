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
//                          main.qml's monitor-id or screen-name changes.
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

#include <atomic>
#include <cstdio>
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

// /config, answering every screen as known, set, and on monitorId (0 unless a phase moves it) --
// what makes main.qml load its renderer.
class ConfigStub {
public:
    ~ConfigStub() { stop(); }
    std::atomic<int> monitorId{0};
    bool start(std::string& why) {
        m_server.Get("/config", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"output":"DP-ACTIVITY","known":true,"path":"/w.png","monitor_id":)" +
                                std::to_string(monitorId.load()) + "}",
                            "application/json");
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
    void stop() { wallpaperx::testing::stopServing(m_server, m_thread); }

private:
    httplib::Server m_server;
    std::thread m_thread;
};

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
    copy.plugin()->setProperty("connectorOverride", kOutput);
    QMetaObject::invokeMethod(copy.plugin(), "refresh");
    return copy;
}

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

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "plasma_surface_test";
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    QDBusConnection bus = wallpaperx::testing::stubConnection();
    ReportStub stub;
    if (!wallpaperx::testing::claimDaemonName(bus, stub)) {
        notVerified("every goal in this file", "no session bus this run could own io.wallpaperx.Daemon on: " +
                                                   bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
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
    if ((copiesSelected || restartSelected || unloadedSelected) && !config.start(why)) {
        notVerified("the main.qml phases", why);
    } else if (copiesSelected || restartSelected || unloadedSelected) {
        QQmlEngine engine;
        engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
        engine.addImportPath(QString::fromUtf8(WALLPAPERX_STANDIN_QML_PATH));
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
            //       `output` -> both lines red (backend/kde/kpackage/contents/ui/main.qml).
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
                    const bool taken = pumpUntil([&] { return a.plugin()->property("monitorId").toInt() == 1; }, kArrivalLimitMs);
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
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
