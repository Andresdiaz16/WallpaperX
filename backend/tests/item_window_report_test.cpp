// SPDX-License-Identifier: GPL-2.0-or-later

// What the KDE plugin tells the daemon about the windows on its screen: SetOutputFocus(name,
// focused, maximized), sent from the item's windowFocused and windowMaximized properties (which
// ScreenWindows.qml sets), and when it is sent again. Also the two reports chunk 3d made immediate:
// setting `output` sends visibility and focus at once rather than after the size report's settle.
//
// A stub on this run's private bus stands in for the daemon (tests/report_stub.h) and records every
// report with its arrival time. The item is the plugin's own registered type in a QQuickWindow on
// the offscreen platform; nothing here renders, so no OpenGL is needed. One process, one phase per
// scenario (WALLPAPERX_TEST_PHASE), each on a screen name of its own so the record of one phase
// cannot answer for another; each phase's scene is destroyed before the next begins, so a daemon
// restart staged in one reaches no item from another.

#include "tests/pump.h"
#include "tests/report_stub.h"
#include "tests/stderr_capture.h"
#include "tests/test_phase.h"
#include "tests/private_test_bus.h"
#include "tests/test_report.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::kService;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::phaseSelected;
using wallpaperx::testing::pumpFor;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::ReportStub;
using wallpaperx::testing::StderrCapture;

const QString kSize = QStringLiteral("SetOutputSize");
const QString kVisible = QStringLiteral("SetOutputVisible");
const QString kFocus = QStringLiteral("SetOutputFocus");
const QString kUnknownOutput = QStringLiteral("io.wallpaperx.Daemon.Error.UnknownOutput");
const QString kRenderFailed = QStringLiteral("io.wallpaperx.Daemon.Error.RenderFailed");
// A refusal neither reportSize nor reportWindows retries: what stays refused for good.
const QString kInvalidArgs = QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs");

// The plugin's retry interval, written out rather than included, for the reason
// item_size_report_test gives. A timer never fires early by more than QTimer's coarse 5%.
constexpr qint64 kRetryFloorMs = 1800;
// How long a phase waits for something that must happen, and the span over which "nothing more"
// is read: three retry intervals, so a retry that was going to come has had three chances.
constexpr int kArrivalLimitMs = 5000;
constexpr int kQuietSpanMs = 6000;
// Long enough for the item's zero-delay report timer and a bus round trip, many times over.
constexpr int kTurnMs = 300;
// A refusal run's first two waits, 2 s and 4 s, plus kArrivalLimitMs-worth of slack.
constexpr int kBackoffLimitMs = 2000 + 4000 + 3000;
// Between the base wait (2 s) and the 8 s an item that kept a two-refusal run's pace would use: a
// timer never fires early, so the wrong retry cannot come before 7.6 s, and a correct item misses
// this bound only with its event loop stalled for four seconds.
constexpr qint64 kBaseWaitCeilingMs = 6000;
// Long enough for a retry at the wrong 8 s wait to arrive, so a slow retry is graded by its gap.
constexpr int kRetryArrivalLimitMs = 10000;

// The log line the item writes when the daemon refuses a window report it does not treat as silent.
const QString kWindowRefusalLog = QStringLiteral("refused this screen's window state");

bool focusArgsAre(const ReportStub::Call& call, bool focused, bool maximized) {
    return call.args.size() == 3 && call.args.at(1).toBool() == focused && call.args.at(2).toBool() == maximized;
}

// What the daemon holds for `method` on `out` from the calls after `since`: the last one it
// accepted, since a refused call stores nothing. Read only after `since`, so a value held from
// before a context change cannot answer for one sent after it.
std::optional<ReportStub::Call> heldSince(const ReportStub& stub, const QString& method, const QString& out, std::size_t since) {
    const std::vector<ReportStub::Call> calls = stub.calls(method, out, since);
    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
        if (it->error.isEmpty()) return *it;
    }
    return std::nullopt;
}
bool holdsVisible(const ReportStub& stub, const QString& out, std::size_t since) {
    const std::optional<ReportStub::Call> held = heldSince(stub, kVisible, out, since);
    return held && held->args.value(1).toBool();
}
bool holdsFocus(const ReportStub& stub, const QString& out, std::size_t since, bool focused, bool maximized) {
    const std::optional<ReportStub::Call> held = heldSince(stub, kFocus, out, since);
    return held && focusArgsAre(*held, focused, maximized);
}

// The size report the change causes was refused for good: refused with an error reportSize never
// retries, and never accepted. A phase about the direct sends needs this, or the size report's
// success could be what sent them.
bool sizeRefusedForGood(const ReportStub& stub, const QString& out, std::size_t since) {
    const std::vector<ReportStub::Call> sizes = stub.calls(kSize, out, since);
    if (sizes.empty()) return false;
    for (const ReportStub::Call& call : sizes) {
        if (call.error != kInvalidArgs) return false;
    }
    return true;
}

// One WallpaperXRenderer in its own root Item, parented into the shared window.
struct Scene {
    std::unique_ptr<QObject> root;
    QQuickItem* renderer = nullptr;
    QQuickItem* rootItem() const { return qobject_cast<QQuickItem*>(root.get()); }
};

class Harness {
public:
    Harness(QQmlEngine& engine, QQuickWindow& window, ReportStub& stub) : m_engine(engine), m_window(window), m_stub(stub) {}

    Scene make(const QString& tag) {
        QQmlComponent component(&m_engine);
        component.setData(QByteArrayLiteral("import QtQuick\nimport com.wallpaperx\n"
                                            "Item { width: 300; height: 170; property Item rendererItem: renderer\n"
                                            "  WallpaperXRenderer { id: renderer; anchors.fill: parent; monitorId: 0 } }\n"),
                          QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                              QStringLiteral("/item_window_report_%1.qml").arg(tag)));
        Scene scene;
        scene.root.reset(component.create());
        if (scene.root == nullptr) {
            for (const QQmlError& error : component.errors()) std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
            return scene;
        }
        scene.rootItem()->setParentItem(m_window.contentItem());
        scene.renderer = qvariant_cast<QQuickItem*>(scene.root->property("rendererItem"));
        return scene;
    }

    // A scene on `output` whose first reports have all been answered: the size, and the visibility
    // and window reports that follow it. Every count a phase then takes starts from quiet.
    Scene settled(const QString& output) {
        Scene scene = make(output);
        if (scene.renderer == nullptr) return scene;
        scene.renderer->setProperty("output", output);
        if (!pumpUntil([&] { return !m_stub.calls(kSize, output).empty(); }, kArrivalLimitMs)) {
            scene.renderer = nullptr;
            return scene;
        }
        pumpFor(kTurnMs);
        return scene;
    }

private:
    QQmlEngine& m_engine;
    QQuickWindow& m_window;
    ReportStub& m_stub;
};

// A phase that could not build its scene says so, and every check in it is skipped.
bool ready(const Scene& scene, const char* phase) {
    if (scene.renderer != nullptr) return true;
    notVerified(phase, "the scene could not be built, or its first size report never reached the stub");
    return false;
}

// The log half of a phase: graded only when stderr was really captured, since a capture that failed
// says nothing about what the item logged.
// CONTROL: observed 2026-10-01 by hand, the unknown-output-retry capture pointed into a directory
//       that does not exist -> "NOT VERIFIED -- the unknown-output-retry phase's log clause ...
//       stderr could not be captured", ctest Skipped, no FAIL line.
void checkNotLogged(const StderrCapture& capture, const QString& logged, const char* phase, const char* what) {
    if (!capture.ok()) {
        notVerified(phase, "stderr could not be captured, so whether the item logged is unknown");
        return;
    }
    check(!logged.contains(kWindowRefusalLog), what);
}

// Sets windowFocused and waits for the stub to accept the report it causes.
bool focusAccepted(ReportStub& stub, const Scene& scene, const QString& out) {
    const std::size_t mark = stub.mark();
    scene.renderer->setProperty("windowFocused", true);
    return pumpUntil(
        [&] {
            const std::vector<ReportStub::Call> calls = stub.calls(kFocus, out, mark);
            return !calls.empty() && calls.back().error.isEmpty();
        },
        kArrivalLimitMs);
}

// Sets windowFocused and has the stub refuse it twice as RenderFailed, the second answer handled:
// the item then holds a retry due in 4 s, and the next refusal of the same run would wait 8 s.
bool backedOff(ReportStub& stub, const Scene& scene, const QString& out) {
    stub.refuseNext(kFocus, out, kRenderFailed);
    stub.refuseNext(kFocus, out, kRenderFailed);
    const std::size_t mark = stub.mark();
    scene.renderer->setProperty("windowFocused", true);
    const bool two = pumpUntil([&] { return stub.calls(kFocus, out, mark).size() >= 2; }, kArrivalLimitMs);
    pumpFor(kTurnMs);
    const std::vector<ReportStub::Call> calls = stub.calls(kFocus, out, mark);
    return two && calls.size() == 2 && calls[0].error == kRenderFailed && calls[1].error == kRenderFailed;
}

// GOAL: S15.402 -- an item created with no session bus says, once, that it cannot hear config changes ("could
//       not subscribe to WallpaperX config changes"). False if it says nothing -- the screen then
//       never learns of a new wallpaper and nothing explains why -- or says it more than once.
// WHY-THIS-SHAPE: a process of its own (the entry item_window_report_no_bus), because QtDBus makes
//       its session connection once per process; the address names a socket that does not exist, set
//       before Qt connects. It still runs only under isolated_run.sh's private session.
// CONTROL: in WallpaperXItem::connectToDaemon (backend/kde/wallpaperx_item.cpp),
//       `watchConfigChanged(this, SLOT(onConfigChanged()));` replaced by the same
//       QDBusConnection::sessionBus().connect(...) call made directly -> this line red, logged 0 times.
//       Via control_run.sh 2026-10-08. Reverted.
// COVERAGE-ASSERT: the session bus must be disconnected and the item instantiated, or not verified.
int noBusRun(int argc, char** argv) {
    constexpr const char* kClause = "the item's failed config-change subscription is reported";
    if (const std::optional<int> refused = wallpaperx::testing::refuseOffPrivateTestBus()) return *refused;
    if (!phaseSelected("no-bus")) return wallpaperx::testing::reportNotVerifiedOrPassed();
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified(kClause, "no scratch directory for the absent bus's address");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + QFile::encodeName(scratch.path()) + "/no-bus");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (QDBusConnection::sessionBus().isConnected()) {
        notVerified(kClause, "the session bus connected although its address names no socket");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    QQuickWindow window;
    window.setGeometry(0, 0, 400, 300);
    window.show();
    ReportStub unused;
    Harness harness(engine, window, unused);
    StderrCapture capture;
    Scene scene = harness.make(QStringLiteral("no-bus"));
    if (scene.renderer != nullptr) scene.renderer->setProperty("output", QStringLiteral("DP-NO-BUS"));
    pumpFor(kTurnMs * 2);
    const QString logged = capture.text();
    if (!capture.ok() || scene.renderer == nullptr) {
        notVerified(kClause, !capture.ok() ? "stderr could not be captured" : "the item could not be instantiated");
    } else {
        const int lines = static_cast<int>(logged.count(QStringLiteral("could not subscribe to WallpaperX config changes")));
        check(lines == 1, "an item created with no bus says once that it cannot hear config changes");
        std::fprintf(stderr, "  logged %d time(s)\n", lines);
    }
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_window_report_test";
    if (argc > 1 && std::strcmp(argv[1], "no-bus") == 0) return noBusRun(argc, argv);
    // X.34: nothing owned or sent off the private bus.
    if (const std::optional<int> refused = wallpaperx::testing::refuseOffPrivateTestBus()) return *refused;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    QDBusConnection bus = wallpaperx::testing::stubConnection();
    ReportStub stub;
    if (!wallpaperx::testing::claimDaemonName(bus, stub)) {
        notVerified("every goal in this file", "no session bus this run could own io.wallpaperx.Daemon on: " +
                                                   bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    QQuickWindow window;
    window.setGeometry(0, 0, 400, 300);
    window.show();
    Harness harness(engine, window, stub);

    if (phaseSelected("argument-order")) {
        const QString out = QStringLiteral("DP-ORDER");
        Scene scene = harness.settled(out);
        if (ready(scene, "the argument-order phase")) {
            // GOAL: a window focused on this screen is reported as SetOutputFocus(name, true, false),
            //       and one maximized as (name, false, true): the daemon reads the fields by position.
            // WHY-THIS-SHAPE: one fact at a time, so a swapped pair cannot pass by symmetry.
            // CONTROL: report.windows.focused and report.windows.maximized swapped in
            //       WallpaperXItem::reportWindows's setArguments (backend/kde/wallpaperx_item.cpp) ->
            //       both lines red. All controls in this file observed 2026-10-01 through
            //       control_batch.sh, phase by phase, and reverted.
            std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            const bool sent = pumpUntil([&] { return !stub.calls(kFocus, out, mark).empty(); }, kArrivalLimitMs);
            const bool rightOrder = sent && focusArgsAre(stub.calls(kFocus, out, mark).front(), true, false);
            check(rightOrder, "a focused window is reported as SetOutputFocus(name, true, false)");
            if (!rightOrder) std::fprintf(stderr, "  saw %s\n", ReportStub::describe(stub.calls(kFocus, out, mark)).c_str());

            scene.renderer->setProperty("windowFocused", false);
            pumpFor(kTurnMs);
            mark = stub.mark();
            scene.renderer->setProperty("windowMaximized", true);
            const bool sentMax = pumpUntil([&] { return !stub.calls(kFocus, out, mark).empty(); }, kArrivalLimitMs);
            check(sentMax && focusArgsAre(stub.calls(kFocus, out, mark).front(), false, true),
                  "a maximized window is reported as SetOutputFocus(name, false, true)");
        }
    }

    if (phaseSelected("one-call")) {
        const QString out = QStringLiteral("DP-BURST");
        Scene scene = harness.settled(out);
        if (ready(scene, "the one-call phase")) {
            // GOAL: both facts changing in one turn of the event loop -- what one QML update does when
            //       a window is focused and maximized together -- cost the daemon one call, carrying both.
            // WHY-THIS-SHAPE: the two properties are set without returning to the event loop, so no
            //       timer can fire between them on any machine and "exactly one" is not a claim about
            //       scheduling.
            // CONTROL: `m_windowReportTimer.start(0);` replaced by `reportWindows();` in both setters
            //       (backend/kde/wallpaperx_item.cpp) -> this line red.
            // COVERAGE-ASSERT: the one call must have arrived; zero calls fails as surely as two.
            const std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            scene.renderer->setProperty("windowMaximized", true);
            pumpFor(kTurnMs);
            const std::vector<ReportStub::Call> calls = stub.calls(kFocus, out, mark);
            check(calls.size() == 1 && focusArgsAre(calls.front(), true, true),
                  "two facts changed in one turn are one SetOutputFocus carrying both");
            if (calls.size() != 1) std::fprintf(stderr, "  saw %s\n", ReportStub::describe(calls).c_str());
        }
    }

    if (phaseSelected("unchanged")) {
        const QString out = QStringLiteral("DP-SAME");
        Scene scene = harness.settled(out);
        if (ready(scene, "the unchanged phase")) {
            // GOAL: a state equal to the last one the daemon accepted is not sent again: a window
            //       focused and unfocused inside one update costs nothing.
            // WHY-THIS-SHAPE: a real change and its undo in one turn, so the report timer fires on a
            //       state that matches the accepted one; with nothing changed at all the setters'
            //       own equality test would send nothing whatever the item remembered.
            // CONTROL: `|| m_windowsReported == report` removed from reportWindows's early return ->
            //       the first line red.
            // COVERAGE-ASSERT: the same item then reports a real change, so a silence here is not an
            //       item that has stopped reporting.
            std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            scene.renderer->setProperty("windowFocused", false);
            pumpFor(kTurnMs);
            check(stub.calls(kFocus, out, mark).empty(),
                  "a state equal to the last one accepted is not sent again");
            if (!stub.calls(kFocus, out, mark).empty()) {
                std::fprintf(stderr, "  saw %s\n", ReportStub::describe(stub.calls(kFocus, out, mark)).c_str());
            }
            mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            check(pumpUntil([&] { return !stub.calls(kFocus, out, mark).empty(); }, kArrivalLimitMs),
                  "and the same item still reports a real change");
        }
    }

    if (phaseSelected("daemon-restart")) {
        const QString out = QStringLiteral("DP-RESTART");
        Scene scene = harness.settled(out);
        if (ready(scene, "the daemon-restart phase")) {
            // GOAL: a daemon that takes the bus name over is told the current window state, though it
            //       has not changed: a restarted daemon starts with no report and would play a video
            //       under a maximized window.
            // WHY-THIS-SHAPE: the stub gives the name up and takes it back, a real NameOwnerChanged
            //       pair on the bus the item watches. The size report succeeds here, so either the
            //       direct send at the arrival or the size report's success can satisfy this line;
            //       the direct send alone is graded in restart-size-refused.
            // CONTROL: `m_windowsReported.reset();` deleted from the DaemonWatch::replaced handler in
            //       WallpaperXItem::connectToDaemon (backend/kde/wallpaperx_item.cpp) -> this line red:
            //       both paths then see the state as already sent. Re-run 2026-10-05 via control_batch.sh,
            //       --phase '^daemon-restart$'. Reverted.
            std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            const bool accepted = pumpUntil([&] { return !stub.calls(kFocus, out, mark).empty(); }, kArrivalLimitMs);
            mark = stub.mark();
            if (!accepted || !wallpaperx::testing::restartDaemonName(bus)) {
                notVerified("the daemon-restart phase", "the first report never arrived, or the stub could not give "
                                                        "up and retake the bus name");
            } else {
                const bool again = pumpUntil([&] { return !stub.calls(kFocus, out, mark).empty(); }, kArrivalLimitMs);
                check(again && focusArgsAre(stub.calls(kFocus, out, mark).front(), true, false),
                      "a new daemon owner is told the unchanged window state");
            }
        }
    }

    if (phaseSelected("immediate")) {
        // GOAL: setting `output` on an item on the screen sends SetOutputVisible(name, true) and
        //       SetOutputFocus(name, ...) before the size report, which waits to settle: a video stays
        //       stopped until the daemon hears its screen is visible. Properties set while `output`
        //       was empty are what that first SetOutputFocus carries.
        // WHY-THIS-SHAPE: graded by order against SetOutputSize, not by a time bound: the size report
        //       cannot leave before its settle, so "before it" holds on any machine and an upper bound
        //       on the arrival time would grade the scheduler.
        // CONTROL: the `reportVisible();` and `reportWindows();` calls deleted from
        //       WallpaperXItem::setOutput, the `restartWindowRetry();` between them kept -> the two
        //       order checks and the rename check red. The "carries the state" check is controlled by
        //       deleting the reportWindows() call in setOutput AND in reportSize's success branch --
        //       either alone leaves the report going out.
        const QString out = QStringLiteral("DP-LATE");
        Scene scene = harness.make(out);
        if (ready(scene, "the immediate phase")) {
            const std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            scene.renderer->setProperty("windowMaximized", true);
            pumpFor(kTurnMs);
            // GOAL: an item with no screen name sends no window report. The daemon has nowhere to file
            //       it, and an empty name is one it refuses as invalid.
            // CONTROL: `m_output.isEmpty() ||` removed from reportWindows's early return -> this line red.
            check(stub.calls(kFocus, QString(), mark).empty(), "an item with no screen name sends no window report");

            scene.renderer->setProperty("output", out);
            const bool sized = pumpUntil([&] { return !stub.calls(kSize, out, mark).empty(); }, kArrivalLimitMs);
            pumpFor(kTurnMs);
            const std::vector<ReportStub::Call> calls = stub.callsFor(out, mark);
            int sizeAt = -1, visibleAt = -1, focusAt = -1;
            for (int i = 0; i < static_cast<int>(calls.size()); ++i) {
                if (sizeAt < 0 && calls[static_cast<size_t>(i)].method == kSize) sizeAt = i;
                if (visibleAt < 0 && calls[static_cast<size_t>(i)].method == kVisible) visibleAt = i;
                if (focusAt < 0 && calls[static_cast<size_t>(i)].method == kFocus) focusAt = i;
            }
            if (!sized) {
                notVerified("the immediate phase's order", "no size report arrived to order the others against");
            } else {
                check(visibleAt >= 0 && visibleAt < sizeAt && calls[static_cast<size_t>(visibleAt)].args.value(1).toBool(),
                      "setting the screen name sends SetOutputVisible(name, true) before the size report");
                check(focusAt >= 0 && focusAt < sizeAt, "and SetOutputFocus before the size report");
            }
            check(focusAt >= 0 && focusArgsAre(calls[static_cast<size_t>(focusAt)], true, true),
                  "the window state set before the screen had a name is reported once it has one");
            std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(calls).c_str());

            // GOAL: renaming a shown screen sends both reports under the new name at once, as the
            //       first name did.
            // CONTROL: the same deletion from setOutput -> this line red too.
            const QString renamed = QStringLiteral("DP-LATE-RENAMED");
            const std::size_t renameMark = stub.mark();
            scene.renderer->setProperty("output", renamed);
            const bool renamedSized =
                pumpUntil([&] { return !stub.calls(kSize, renamed, renameMark).empty(); }, kArrivalLimitMs);
            const std::vector<ReportStub::Call> after = stub.callsFor(renamed, renameMark);
            bool focusFirst = false;
            for (const ReportStub::Call& call : after) {
                if (call.method == kSize) break;
                if (call.method == kFocus && focusArgsAre(call, true, true)) focusFirst = true;
            }
            check(renamedSized && focusFirst, "a renamed screen's window state is sent under the new name before its size");
        }
    }

    if (phaseSelected("immediate-refusal")) {
        // GOAL: an immediate report the daemon refuses as UnknownOutput -- the name not bound yet -- is
        //       sent again once the size report succeeds, with nothing having changed. The size
        //       report's success is the only signal that the daemon now knows the name.
        // CONTROL: `reportVisible();` deleted from reportSize's success branch -> the visibility line
        //       red; `reportWindows();` deleted from the same branch -> the focus line red.
        const QString out = QStringLiteral("DP-EARLY");
        stub.refuseNext(kVisible, out, kUnknownOutput);
        stub.refuseNext(kFocus, out, kUnknownOutput);
        Scene scene = harness.make(out);
        if (ready(scene, "the immediate-refusal phase")) {
            const std::size_t mark = stub.mark();
            scene.renderer->setProperty("output", out);
            const bool resent = pumpUntil(
                [&] { return stub.calls(kVisible, out, mark).size() >= 2 && stub.calls(kFocus, out, mark).size() >= 2; },
                kArrivalLimitMs);
            const std::vector<ReportStub::Call> calls = stub.callsFor(out, mark);
            const auto resentAfterSize = [&](const QString& method) {
                bool refused = false, sized = false;
                for (const ReportStub::Call& call : calls) {
                    if (call.method == method && !call.error.isEmpty()) refused = true;
                    if (refused && call.method == kSize && call.error.isEmpty()) sized = true;
                    if (sized && call.method == method && call.error.isEmpty()) return true;
                }
                return false;
            };
            check(resentAfterSize(kVisible), "a visibility report refused as UnknownOutput is sent again after the size is accepted");
            check(resentAfterSize(kFocus), "a window report refused as UnknownOutput is sent again after the size is accepted");
            if (!resent) std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(calls).c_str());
        }
    }

    // A report refused as UnknownOutput after the screen was known -- the daemon lost the name -- waits
    // for the ConfigChanged naming the screen sends, then goes again behind the size report.
    // `refused` stages the refusal; `method` is what it refused.
    const auto unknownOutputWaitsForNotice = [&](const char* phase, const QString& out, const QString& method,
                                                 const std::function<void(const Scene&)>& refused) {
        Scene scene = harness.settled(out);
        if (!ready(scene, phase)) return;
        StderrCapture capture;
        stub.refuseNext(method, out, kUnknownOutput);
        const std::size_t mark = stub.mark();
        refused(scene);
        const bool sent = pumpUntil([&] { return !stub.calls(method, out, mark).empty(); }, kArrivalLimitMs);
        pumpFor(kQuietSpanMs);
        const std::vector<ReportStub::Call> quiet = stub.callsFor(out, mark);
        if (!sent || quiet.front().method != method || quiet.front().error != kUnknownOutput) {
            notVerified(phase, "the staged report was never sent and refused as UnknownOutput (" +
                                   ReportStub::describe(quiet) + ")");
            return;
        }
        const std::string name = method.toStdString();
        check(quiet.size() == 1, (name + " refused as UnknownOutput sends nothing more without a ConfigChanged").c_str());
        const std::size_t noticeMark = stub.mark();
        if (!wallpaperx::testing::announceConfigChanged(bus)) {
            notVerified(phase, "the stub could not send ConfigChanged");
            return;
        }
        pumpUntil(
            [&] {
                const std::vector<ReportStub::Call> calls = stub.calls(method, out, noticeMark);
                return !calls.empty() && calls.back().error.isEmpty();
            },
            kArrivalLimitMs);
        pumpFor(kTurnMs);
        const QString logged = capture.text();
        const std::vector<ReportStub::Call> after = stub.callsFor(out, noticeMark);
        // The size first, accepted, then the refused report, accepted.
        bool sized = false, again = false;
        for (const ReportStub::Call& call : after) {
            if (call.method == kSize && call.error.isEmpty()) sized = true;
            if (sized && call.method == method && call.error.isEmpty()) again = true;
        }
        check(sized && again, (name + " refused as UnknownOutput goes again at ConfigChanged, after the size").c_str());
        checkNotLogged(capture, logged, phase, "and the refusal is not logged");
        std::fprintf(stderr, "  %s: %s | after the notice: %s\n", qPrintable(out), ReportStub::describe(quiet).c_str(),
                     ReportStub::describe(after).c_str());
    };

    if (phaseSelected("unknown-output-notice")) {
        // GOAL: S15.190 -- a window report refused as UnknownOutput after the screen was known sends
        //       nothing more -- neither the report nor the size -- over a 6 s span, and at the daemon's
        //       next ConfigChanged sends the size and then the window report, both accepted; nothing is
        //       logged.
        // WHY-THIS-SHAPE: refused once the size report has already succeeded, because a refusal of the
        //       immediate report is cured by the size report's first success anyway. Ordered on the
        //       stub's record, not timed: the size's success is what re-sends the report, so "size,
        //       then report" is the property and any gap is the scheduler's.
        // CONTROL: `if (!m_sizeReportTimer.isActive()) m_sizeReportTimer.start(kRefusedRetryMs);` put
        //       back after `m_sizeReported.reset();` in reportWindows's UnknownOutput branch
        //       (backend/kde/wallpaperx_item.cpp) -> "sends nothing more without a ConfigChanged" red, and the
        //       "goes again" line with it (the size went before the notice).
        // CONTROL: `m_sizeReported.reset();` deleted from the same branch -> "goes again at
        //       ConfigChanged, after the size" red.
        unknownOutputWaitsForNotice("the unknown-output-notice phase", QStringLiteral("DP-LOST"), kFocus,
                                    [](const Scene& scene) { scene.renderer->setProperty("windowFocused", true); });
    }

    if (phaseSelected("visible-unknown-notice")) {
        // GOAL: S15.388 -- the same for a visibility report: hidden, refused as UnknownOutput, it sends
        //       nothing more until ConfigChanged, then the size and the visibility report, both accepted.
        // WHY-THIS-SHAPE: as the phase above; hidden through the scene's root, which is the change
        //       that sends visibility.
        // CONTROL: `m_sizeReported.reset();` deleted from reportVisible's UnknownOutput branch
        //       (backend/kde/wallpaperx_item.cpp) -> "SetOutputVisible refused as UnknownOutput goes again
        //       at ConfigChanged, after the size" red.
        unknownOutputWaitsForNotice("the visible-unknown-notice phase", QStringLiteral("DP-LOST-VISIBLE"), kVisible,
                                    [](const Scene& scene) { scene.rootItem()->setVisible(false); });
    }

    if (phaseSelected("silent-refusals")) {
        // GOAL: a window report a daemon older than the method refuses (UnknownMethod) is not logged
        //       and not sent again; one sent while no daemon is on the bus (ServiceUnknown) is not
        //       logged, and the daemon that arrives next is told the state. This follows every focus
        //       change, so a log line or a retry here repeats for the life of the session.
        // WHY-THIS-SHAPE: the older daemon starts refusing only once the scene has settled with its
        //       reports accepted, so the refusal under test is the first of a run: refused from the
        //       start, the reports at setOutput already make a run whose backed-off retry falls after
        //       the span read here. "Not sent again" is read over three retry intervals. The
        //       no-daemon half claims no "not retried": a retry with no daemon never reaches the stub,
        //       and the next owner's re-report replaces it, so nothing here could see one.
        // CONTROL: in reportWindows's reply handler (backend/kde/wallpaperx_item.cpp), via control_run.sh
        //       2026-10-08, --phase '^silent-refusals$', each reverted. The early `return` for
        //       `Refusal::Absent || ... UnknownMethod` deleted -> both log lines red. UnknownMethod dropped
        //       from that return and from classifyRefusal's Permanent test, so it is retried -> "is not
        //       sent again" red, and the older daemon's log line with it.
        // COVERAGE-ASSERT: the one report must have arrived and been refused with UnknownMethod, so
        //       a staging that did not take cannot pass the silence.
        const QString old = QStringLiteral("DP-OLD-DAEMON");
        const QString unknownMethod = QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod");
        Scene oldScene = harness.settled(old);
        const QString gone = QStringLiteral("DP-NO-DAEMON");
        Scene goneScene = harness.settled(gone);
        if (ready(oldScene, "the silent-refusals phase") && ready(goneScene, "the silent-refusals phase")) {
            stub.refuseAlways(kFocus, old, unknownMethod);
            StderrCapture capture;
            const std::size_t mark = stub.mark();
            oldScene.renderer->setProperty("windowFocused", true);
            pumpFor(kQuietSpanMs);
            // Read, and stderr given back, before anything is checked: a FAIL line written while
            // captured would land in the file instead of the run's output.
            const QString oldLog = capture.text();
            const std::vector<ReportStub::Call> oldCalls = stub.calls(kFocus, old, mark);
            check(!oldCalls.empty() && oldCalls.front().error == unknownMethod,
                  "the older daemon's refusal was staged: the report arrived and was refused as UnknownMethod");
            check(oldCalls.size() == 1, "a window report an older daemon does not know is not sent again");
            if (oldCalls.size() != 1) std::fprintf(stderr, "  %s: %s\n", qPrintable(old), ReportStub::describe(oldCalls).c_str());
            checkNotLogged(capture, oldLog, "the silent-refusals phase's older-daemon log clause",
                           "and an older daemon's refusal is not logged");

            // The no-daemon half destroys the older-daemon scene first: the restart that ends it would
            // otherwise send that scene's report into the span being read.
            oldScene.root.reset();
            StderrCapture goneCapture;
            const bool unregistered = bus.unregisterService(QString::fromUtf8(kService));
            pumpFor(kTurnMs);
            goneScene.renderer->setProperty("windowFocused", true);
            pumpFor(kTurnMs * 3);
            const QString goneLog = goneCapture.text();
            const std::size_t back = stub.mark();
            const bool reregistered = bus.registerService(QString::fromUtf8(kService));
            if (!unregistered || !reregistered) {
                notVerified("the no-daemon half of the silent-refusals phase", "the stub could not give up and retake the name");
            } else {
                checkNotLogged(goneCapture, goneLog, "the silent-refusals phase's no-daemon log clause",
                               "a window report sent while no daemon is on the bus is not logged");
                // And the state reaches the next daemon, so silence lost nothing.
                const bool reported = pumpUntil([&] { return !stub.calls(kFocus, gone, back).empty(); }, kArrivalLimitMs);
                check(reported && focusArgsAre(stub.calls(kFocus, gone, back).front(), true, false),
                      "and the daemon that arrives next is told that state");
            }
        }
    }

    if (phaseSelected("stale-refusal")) {
        const QString out = QStringLiteral("DP-STALE");
        Scene scene = harness.settled(out);
        if (ready(scene, "the stale-refusal phase")) {
            // GOAL: a refusal that arrives for a report a newer one has already replaced changes
            //       nothing: the newer report, accepted, stands; nothing is logged or sent again.
            // WHY-THIS-SHAPE: the stub holds the first report's answer until the second has been
            //       accepted, which is the only way to put the two answers in that order on any
            //       machine. The refusal is RenderFailed, one the item retries, so a wrongly honoured
            //       refusal is visible as a resend as well as a log line.
            // CONTROL: `if (m_windowsReported != report) return;` deleted from reportWindows's reply
            //       handler -> both lines red.
            stub.holdNext(kFocus);
            const std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            const bool held = pumpUntil([&] { return stub.isHolding(); }, kArrivalLimitMs);
            scene.renderer->setProperty("windowMaximized", true);
            const bool newer = pumpUntil([&] { return stub.calls(kFocus, out, mark).size() >= 2; }, kArrivalLimitMs);
            if (!held || !newer) {
                notVerified("the stale-refusal phase", "the first report was not held, or the second never arrived");
                stub.releaseHeld(QString());
            } else {
                StderrCapture capture;
                const std::size_t afterBoth = stub.mark();
                stub.releaseHeld(kRenderFailed);
                pumpFor(kQuietSpanMs / 2);
                const QString logged = capture.text();
                check(stub.calls(kFocus, out, afterBoth).empty(),
                      "a refusal of a report already replaced sends nothing again");
                checkNotLogged(capture, logged, "the stale-refusal phase's log clause", "and is not logged");
                std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(stub.callsFor(out, mark)).c_str());
            }
        }
    }

    if (phaseSelected("off-screen")) {
        const QString out = QStringLiteral("DP-HIDDEN");
        Scene scene = harness.settled(out);
        if (ready(scene, "the off-screen phase")) {
            // GOAL: an item off the screen sends no window report, whatever its properties do, and
            //       sends the state it has when it comes back. Plasma keeps a hidden copy of the
            //       plugin per activity under the same screen name, and a hidden copy's report would
            //       stand for the screen the user is looking at.
            // WHY-THIS-SHAPE: hidden through its parent, as Plasma hides a containment. The silence is
            //       read over a span many times the item's zero-delay report timer.
            // CONTROL: `!m_onScreen ||` removed from reportWindows's early return -> both lines red (the
            //       hidden item's report is then the last one, so showing it sends nothing new).
            //       `reportWindows();` deleted from updateOnScreen's on-screen branch -> the second red.
            scene.rootItem()->setVisible(false);
            pumpFor(kTurnMs);
            const std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            pumpFor(kTurnMs * 2);
            check(stub.calls(kFocus, out, mark).empty(), "an item off the screen sends no window report");
            const std::size_t shown = stub.mark();
            scene.rootItem()->setVisible(true);
            const bool reported = pumpUntil([&] { return !stub.calls(kFocus, out, shown).empty(); }, kArrivalLimitMs);
            check(reported && focusArgsAre(stub.calls(kFocus, out, shown).front(), true, false),
                  "and coming back on the screen sends the state it has");
            std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(stub.callsFor(out, mark)).c_str());
        }
    }

    if (phaseSelected("back-on-screen")) {
        const QString out = QStringLiteral("DP-RETURN");
        Scene scene = harness.settled(out);
        if (ready(scene, "the back-on-screen phase")) {
            // GOAL: an item coming back on the screen sends its window state even when that state is
            //       unchanged since it last reported: while it was off the screen another copy for
            //       the same name may have reported over it.
            // WHY-THIS-SHAPE: nothing changes while hidden, unlike the off-screen phase, whose change
            //       makes the re-send happen whatever the item remembered. The state is a focused
            //       window, not the default, so the report counted is this one.
            // CONTROL: `m_windowsReported.reset();` deleted from updateOnScreen's off-screen branch
            //       (backend/kde/wallpaperx_item.cpp) -> this line red.
            if (!focusAccepted(stub, scene, out)) {
                notVerified("the back-on-screen phase", "the focused state was never accepted before hiding");
            } else {
                scene.rootItem()->setVisible(false);
                pumpFor(kTurnMs);
                const std::size_t shown = stub.mark();
                scene.rootItem()->setVisible(true);
                pumpUntil([&] { return !stub.calls(kFocus, out, shown).empty(); }, kArrivalLimitMs);
                pumpFor(kTurnMs);
                const std::vector<ReportStub::Call> calls = stub.calls(kFocus, out, shown);
                check(calls.size() == 1 && focusArgsAre(calls.front(), true, false),
                      "coming back on the screen sends the unchanged window state, once");
                std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(calls).c_str());
            }
        }
    }

    if (phaseSelected("monitor-change")) {
        const QString out = QStringLiteral("DP-REBOUND");
        Scene scene = harness.settled(out);
        if (ready(scene, "the monitor-change phase")) {
            // GOAL: after a new monitor id, the daemon holds SetOutputVisible(name, true) and the
            //       unchanged window state again. The app rebinds the name before the plugin hears the
            //       new id, and the rebinding erases both, so a maximized window would otherwise play
            //       the video.
            // WHY-THIS-SHAPE: graded on what the daemon ends up holding from the calls after the
            //       change, read once the size report has arrived, not on order against the size
            //       report: the daemon's erase happens before the plugin learns the id, so a report
            //       sent at the change is as good as one sent after the size. The state is a focused
            //       window, so the report counted cannot be a default the daemon assumes anyway.
            // CONTROL: in WallpaperXItem::setMonitorId (backend/kde/wallpaperx_item.cpp),
            //       `m_windowsReported.reset();` deleted -> the window line red; `m_visibleReported.reset();`
            //       deleted -> the visibility line red: the direct send and the size report's success
            //       both see the value as already sent.
            if (!focusAccepted(stub, scene, out)) {
                notVerified("the monitor-change phase", "the focused state was never accepted before the change");
            } else {
                const std::size_t mark = stub.mark();
                scene.renderer->setProperty("monitorId", 1);
                const bool sized = pumpUntil([&] { return !stub.calls(kSize, out, mark).empty(); }, kArrivalLimitMs);
                pumpFor(kTurnMs);
                if (!sized) {
                    notVerified("the monitor-change phase", "no size report followed the change, so what the item "
                                                            "sends because of it may not all have arrived");
                } else {
                    check(holdsVisible(stub, out, mark), "after a new monitor id the daemon holds SetOutputVisible(name, true) again");
                    check(holdsFocus(stub, out, mark, true, false), "and the unchanged window state again");
                }
                std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(stub.callsFor(out, mark)).c_str());
            }
        }
    }

    if (phaseSelected("transient-backoff")) {
        const QString out = QStringLiteral("DP-BUSY");
        Scene scene = harness.settled(out);
        if (ready(scene, "the transient-backoff phase")) {
            // GOAL: a window report the daemon keeps refusing as RenderFailed is retried with waits that
            //       double (2 s, then 4 s), never through the size report, and the run is logged once.
            //       A refusal that does not clear otherwise costs a call, a size report and a log line
            //       every two seconds for the session.
            // WHY-THIS-SHAPE: the stub starts refusing once the scene has settled, so the run begins
            //       at a change and its first wait is the base one. The waits are lower bounds -- a
            //       timer never fires early -- so the doubling is read as "the second wait is at least
            //       two retry floors", which no fixed interval reaches.
            // CONTROL: in reportWindows's reply handler (backend/kde/wallpaperx_item.cpp), via
            //       control_run.sh 2026-10-08, --phase '^transient-backoff$', each reverted. The line
            //       doubling `m_windowRetryMs` deleted -> the doubling line red, and the log-count line
            //       (the wait never leaves the base the log condition reads). `m_sizeReported.reset();`
            //       and a `m_sizeReportTimer.start(kRefusedRetryMs)` added to the `Refusal::Retried`
            //       branch -> the size line red, and the doubling line. The log condition made
            //       `if (true)` -> the log-count line alone.
            // COVERAGE-ASSERT: three refused calls must have arrived, so two waits were measured.
            stub.refuseAlways(kFocus, out, kRenderFailed);
            StderrCapture capture;
            const std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            const bool three = pumpUntil([&] { return stub.calls(kFocus, out, mark).size() >= 3; }, kBackoffLimitMs);
            const QString logged = capture.text();
            const std::vector<ReportStub::Call> calls = stub.calls(kFocus, out, mark);
            const std::vector<ReportStub::Call> sizes = stub.calls(kSize, out, mark);
            check(three, "a window report refused as RenderFailed is retried while the refusal lasts");
            if (three) {
                const qint64 first = calls[1].atMs - calls[0].atMs;
                const qint64 second = calls[2].atMs - calls[1].atMs;
                check(first >= kRetryFloorMs && second >= 2 * kRetryFloorMs,
                      "each retry of a refusal that does not clear waits twice as long as the one before");
                std::fprintf(stderr, "  waits: %lld ms, then %lld ms\n", static_cast<long long>(first),
                             static_cast<long long>(second));
            }
            check(sizes.empty(), "and none of the retries goes through the size report");
            if (!capture.ok()) {
                notVerified("the transient-backoff phase's log clause", "stderr could not be captured");
            } else {
                const int lines = static_cast<int>(logged.count(kWindowRefusalLog));
                check(lines == 1, "and the run of refusals is logged once");
                if (lines != 1) std::fprintf(stderr, "  logged %d time(s)\n", lines);
            }
            std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(stub.callsFor(out, mark)).c_str());
        }
    }

    if (phaseSelected("transient-recovery")) {
        const QString out = QStringLiteral("DP-BLIP");
        Scene scene = harness.settled(out);
        if (ready(scene, "the transient-recovery phase")) {
            // GOAL: a window report refused once as RenderFailed is sent again on its own retry, with
            //       nothing changing, and the daemon ends up holding it.
            // CONTROL: `m_windowReportTimer.start(m_windowRetryMs);` deleted from reportWindows's
            //       `Refusal::Retried` branch (backend/kde/wallpaperx_item.cpp) -> this line red. Via
            //       control_run.sh 2026-10-08, --phase '^transient-recovery$'. Reverted.
            stub.refuseNext(kFocus, out, kRenderFailed);
            const std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            const bool resent = pumpUntil([&] { return stub.calls(kFocus, out, mark).size() >= 2; }, kArrivalLimitMs);
            const std::vector<ReportStub::Call> calls = stub.calls(kFocus, out, mark);
            check(resent && calls[0].error == kRenderFailed && calls[1].error.isEmpty() && focusArgsAre(calls[1], true, false) &&
                      calls[1].atMs - calls[0].atMs >= kRetryFloorMs,
                  "a window report refused once as RenderFailed is sent again after the retry interval and accepted");
            std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(calls).c_str());
        }
    }

    if (phaseSelected("refusal-kinds")) {
        // GOAL: S15.403 -- a window report refused for any reason but its arguments or an unknown method is resent
        //       after about 2 s and accepted -- LimitsExceeded, NoMemory, AccessDenied, Disconnected and an
        //       error name the plugin does not know among them -- while one refused as InvalidArgs is not
        //       resent. False if a busy or short-of-memory daemon leaves a screen wrongly paused or playing
        //       until the next window change, or if a refusal no resend can cure is resent forever.
        // WHY-THIS-SHAPE: one screen per error, all at once, each refused once (InvalidArgs always), so
        //       the resend is graded between the 1.8 s floor and kBaseWaitCeilingMs. "Not resent" is read
        //       over kQuietSpanMs, three retry intervals.
        // CONTROL: in classifyRefusal (backend/kde/wallpaperx_item.cpp), via control_run.sh 2026-10-08,
        //       --phase '^refusal-kinds$', each reverted. The default made `Refusal::Permanent` -> the five
        //       retried kinds' lines red, InvalidArgs green. InvalidArgs dropped from the Permanent test ->
        //       InvalidArgs's line alone.
        // COVERAGE-ASSERT: every report must have arrived and been refused with its staged error, or the
        //       phase is not verified.
        struct Kind {
            QString output;
            QString error;
            bool retried;
            Scene scene;
        };
        std::vector<Kind> kinds;
        kinds.push_back({QStringLiteral("DP-W-LIMITS"), QStringLiteral("org.freedesktop.DBus.Error.LimitsExceeded"), true, {}});
        kinds.push_back({QStringLiteral("DP-W-NOMEMORY"), QStringLiteral("org.freedesktop.DBus.Error.NoMemory"), true, {}});
        kinds.push_back({QStringLiteral("DP-W-ACCESS"), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"), true, {}});
        kinds.push_back({QStringLiteral("DP-W-DISCONNECTED"), QStringLiteral("org.freedesktop.DBus.Error.Disconnected"), true, {}});
        kinds.push_back({QStringLiteral("DP-W-UNKNOWN-ERROR"), QStringLiteral("io.wallpaperx.Daemon.Error.SomethingNew"), true, {}});
        kinds.push_back({QStringLiteral("DP-W-INVALIDARGS"), kInvalidArgs, false, {}});
        bool built = true;
        for (Kind& k : kinds) {
            k.scene = harness.settled(k.output);
            built = built && k.scene.renderer != nullptr;
        }
        if (!built) {
            notVerified("the refusal-kinds phase", "a scene could not be built, or its first size report never arrived");
        } else {
            const std::size_t mark = stub.mark();
            for (Kind& k : kinds) {
                if (k.retried) {
                    stub.refuseNext(kFocus, k.output, k.error);
                } else {
                    stub.refuseAlways(kFocus, k.output, k.error);
                }
                k.scene.renderer->setProperty("windowFocused", true);
            }
            pumpFor(kQuietSpanMs);
            bool staged = true;
            for (const Kind& k : kinds) {
                const std::vector<ReportStub::Call> calls = stub.calls(kFocus, k.output, mark);
                staged = staged && !calls.empty() && calls.front().error == k.error;
            }
            if (!staged) {
                notVerified("the refusal-kinds phase", "not every report arrived and was refused with its staged error");
            } else {
                for (const Kind& k : kinds) {
                    const std::vector<ReportStub::Call> calls = stub.calls(kFocus, k.output, mark);
                    const std::string name = k.error.toStdString();
                    if (k.retried) {
                        const qint64 gap = calls.size() >= 2 ? calls[1].atMs - calls[0].atMs : -1;
                        check(calls.size() == 2 && calls[1].error.isEmpty() && focusArgsAre(calls[1], true, false) &&
                                  gap >= kRetryFloorMs && gap <= kBaseWaitCeilingMs,
                              ("a window report refused with " + name + " is resent after about 2 s and accepted").c_str());
                    } else {
                        check(calls.size() == 1, ("a window report refused with " + name + " is not resent").c_str());
                    }
                    std::fprintf(stderr, "  %s: %s\n", qPrintable(k.output), ReportStub::describe(calls).c_str());
                }
            }
        }
    }

    if (phaseSelected("retry-reset")) {
        const QString out = QStringLiteral("DP-STREAK");
        Scene scene = harness.settled(out);
        if (ready(scene, "the retry-reset phase")) {
            // GOAL: once a report is accepted, the next refusal is retried after the base wait again,
            //       not after the longer wait the previous run of refusals had reached.
            // WHY-THIS-SHAPE: two refusals then an acceptance leave a forgetful item at an 8 s wait, a
            //       correct one at 2 s; kBaseWaitCeilingMs sits between the two.
            // CONTROL: `m_windowRetryMs = kRefusedRetryMs;` deleted from reportWindows's success branch
            //       (backend/kde/wallpaperx_item.cpp) -> this line red.
            stub.refuseNext(kFocus, out, kRenderFailed);
            stub.refuseNext(kFocus, out, kRenderFailed);
            std::size_t mark = stub.mark();
            scene.renderer->setProperty("windowFocused", true);
            const bool ended = pumpUntil([&] { return stub.calls(kFocus, out, mark).size() >= 3; }, kBackoffLimitMs);
            const std::vector<ReportStub::Call> streak = stub.calls(kFocus, out, mark);
            if (!ended || streak[0].error != kRenderFailed || streak[1].error != kRenderFailed ||
                !streak[2].error.isEmpty()) {
                notVerified("the retry-reset phase", "the run of two refusals never ended in an accepted report (" +
                                                         ReportStub::describe(streak) + ")");
            } else {
                stub.refuseNext(kFocus, out, kRenderFailed);
                mark = stub.mark();
                scene.renderer->setProperty("windowMaximized", true);
                const bool retried =
                    pumpUntil([&] { return stub.calls(kFocus, out, mark).size() >= 2; }, kRetryArrivalLimitMs);
                const std::vector<ReportStub::Call> calls = stub.calls(kFocus, out, mark);
                check(retried && calls[0].error == kRenderFailed && calls[1].error.isEmpty() &&
                          calls[1].atMs - calls[0].atMs < kBaseWaitCeilingMs,
                      "after an accepted report, the next refusal is retried after the base wait again");
                std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(calls).c_str());
            }
        }
    }

    // GOAL: a run of window-report refusals that begins in a new reporting context -- a new daemon
    //       owner, a new monitor id, the item back on the screen, a new screen name -- is retried
    //       after the base wait and logged, whatever wait the previous context's run had reached.
    //       Otherwise the new context's refusal waits up to a minute with nothing in the log.
    // WHY-THIS-SHAPE: two refusals before the change leave a pending retry at 4 s and the next wait
    //       at 8 s, so a carried-over pace is both slow (kBaseWaitCeilingMs) and silent (the item logs
    //       only a run's first refusal). The gap is read between the new context's refusal and its
    //       retry, both stub arrival times, so the change's own settle is not in it.
    // CONTROL: `restartWindowRetry();` deleted at each site in backend/kde/wallpaperx_item.cpp, one
    //       control per site with its phase -> for updateOnScreen's off-screen branch, that phase's
    //       wait line and log line red (retry about 8 s after the refusal); for the owner-change
    //       handler, setMonitorId and setOutput the log line alone, since each sends at once and the
    //       size report's success re-sends the refused state 0.25 s later whatever the timer holds.
    //       The wait line at the owner-change and setMonitorId sites is controlled by deleting
    //       `restartWindowRetry();` there together with `reportWindows();` in reportSize's success
    //       branch -> that phase's wait line red.
    const auto freshRun = [&](const char* phase, const char* site, const QString& out, const QString& target,
                              bool waitGraded, const std::function<bool(const Scene&)>& change) {
        Scene scene = harness.settled(out);
        if (!ready(scene, phase)) return;
        if (!backedOff(stub, scene, out)) {
            notVerified(phase, "the run of two RenderFailed refusals before the change was not staged (" +
                                   ReportStub::describe(stub.callsFor(out, 0)) + ")");
            return;
        }
        StderrCapture capture;
        stub.refuseNext(kFocus, target, kRenderFailed);
        const std::size_t mark = stub.mark();
        if (!change(scene)) {
            notVerified(phase, "the context change could not be made: the property was not set, or the stub "
                               "could not give up and retake the bus name");
            return;
        }
        const bool retried = pumpUntil([&] { return stub.calls(kFocus, target, mark).size() >= 2; }, kRetryArrivalLimitMs);
        const QString logged = capture.text();
        const std::vector<ReportStub::Call> calls = stub.calls(kFocus, target, mark);
        const bool refusedFirst = !calls.empty() && calls[0].error == kRenderFailed;
        if (waitGraded) {
            check(retried && refusedFirst && calls[1].error.isEmpty() && calls[1].atMs - calls[0].atMs < kBaseWaitCeilingMs,
                  ("after " + std::string(site) + ", a new refusal is retried after the base wait").c_str());
        }
        if (!refusedFirst) {
            notVerified(phase, "the change sent no window report for the stub to refuse");
        } else if (!capture.ok()) {
            notVerified(phase, "stderr could not be captured, so whether the item logged is unknown");
        } else {
            check(logged.contains(kWindowRefusalLog), ("and after " + std::string(site) + ", that refusal is logged").c_str());
        }
        std::fprintf(stderr, "  %s: %s\n", qPrintable(target), ReportStub::describe(stub.callsFor(target, mark)).c_str());
    };

    if (phaseSelected("fresh-run-restart")) {
        freshRun("the fresh-run-restart phase", "a daemon restart", QStringLiteral("DP-FRESH-OWNER"),
                 QStringLiteral("DP-FRESH-OWNER"), true,
                 [&](const Scene&) { return wallpaperx::testing::restartDaemonName(bus); });
    }

    if (phaseSelected("fresh-run-monitor")) {
        freshRun("the fresh-run-monitor phase", "a new monitor id", QStringLiteral("DP-FRESH-MONITOR"),
                 QStringLiteral("DP-FRESH-MONITOR"), true,
                 [](const Scene& scene) { return scene.renderer->setProperty("monitorId", 1); });
    }

    if (phaseSelected("fresh-run-screen")) {
        freshRun("the fresh-run-screen phase", "the item leaves the screen and comes back",
                 QStringLiteral("DP-FRESH-SHOWN"), QStringLiteral("DP-FRESH-SHOWN"), true, [](const Scene& scene) {
                     scene.rootItem()->setVisible(false);
                     pumpFor(kTurnMs);
                     scene.rootItem()->setVisible(true);
                     return true;
                 });
    }

    if (phaseSelected("fresh-run-output")) {
        // WHY-THIS-SHAPE: the wait is not graded for a new screen name: the size report's success
        //       re-sends the state about 0.25 s after the refusal whatever the retry timer holds, so
        //       an item that carried the old pace over is caught by its missing log line alone.
        // CONTROL: `restartWindowRetry();` deleted from WallpaperXItem::setOutput -> the log line
        //       red, and only it.
        freshRun("the fresh-run-output phase", "a new screen name", QStringLiteral("DP-FRESH-NAME"),
                 QStringLiteral("DP-FRESH-NAME-2"), false,
                 [](const Scene& scene) { return scene.renderer->setProperty("output", QStringLiteral("DP-FRESH-NAME-2")); });
    }

    if (phaseSelected("queued-report-rebind")) {
        const QString out = QStringLiteral("DP-QUEUED");
        Scene scene = harness.settled(out);
        if (ready(scene, "the queued-report-rebind phase")) {
            // GOAL: a window state changed in the same turn as a new monitor id is what the daemon
            //       ends up holding, with the size report succeeding: the change's queued report is
            //       stopped by setMonitorId, so something after the stop must send the current state.
            // WHY-THIS-SHAPE: the change queues the item's zero-delay report timer, and the new id
            //       is set before the event loop runs, so that timer is provably pending at the
            //       change on any machine. Graded on the state held after the change, not on order
            //       against the size report (see monitor-change). monitor-size-refused grades the same
            //       turn with the size report refused, where the direct send is the only path.
            // CONTROL: `reportWindows();` deleted from WallpaperXItem::setMonitorId and from
            //       reportSize's success branch (backend/kde/wallpaperx_item.cpp) -> this line red.
            //       Deleting `m_windowReportTimer.stop();` from restartWindowRetry no longer turns it
            //       red: the direct send reads the current state, so the timer left running finds it
            //       already sent and sends nothing.
            if (!focusAccepted(stub, scene, out)) {
                notVerified("the queued-report-rebind phase", "the focused state was never accepted before the change");
            } else {
                const std::size_t mark = stub.mark();
                scene.renderer->setProperty("windowMaximized", true);
                scene.renderer->setProperty("monitorId", 1);
                const bool sized = pumpUntil([&] { return !stub.calls(kSize, out, mark).empty(); }, kArrivalLimitMs);
                pumpFor(kTurnMs);
                if (!sized) {
                    notVerified("the queued-report-rebind phase", "no size report followed the change, so what the "
                                                                  "item sends because of it may not all have arrived");
                } else {
                    check(holdsFocus(stub, out, mark, true, true),
                          "a window state changed with the monitor id is what the daemon holds after the change");
                }
                std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(stub.callsFor(out, mark)).c_str());
            }
        }
    }

    if (phaseSelected("monitor-size-refused")) {
        const QString out = QStringLiteral("DP-MOVED-NOSIZE");
        Scene scene = harness.settled(out);
        if (ready(scene, "the monitor-size-refused phase")) {
            // GOAL: after a new monitor id whose size report is refused for good, the daemon holds
            //       SetOutputVisible(name, true) and the current window state, including a change made
            //       in the same turn as the new id. The size report has failures it never retries,
            //       and the rebinding has erased both reports.
            // WHY-THIS-SHAPE: refused with InvalidArgs, which reportSize neither retries nor answers
            //       with a re-send, so the size report's success cannot be what sent them; the phase is
            //       not verified unless that refusal is what happened. The change is queued on the
            //       report timer before the id is set, so setMonitorId's stop drops it on any machine.
            // CONTROL: in WallpaperXItem::setMonitorId (backend/kde/wallpaperx_item.cpp), the
            //       `reportVisible();` after `scheduleSizeReport();` deleted -> the visibility line red;
            //       the `reportWindows();` after it deleted -> the window line red.
            if (!focusAccepted(stub, scene, out)) {
                notVerified("the monitor-size-refused phase", "the focused state was never accepted before the change");
            } else {
                stub.refuseNext(kSize, out, kInvalidArgs);
                const std::size_t mark = stub.mark();
                scene.renderer->setProperty("windowMaximized", true);
                scene.renderer->setProperty("monitorId", 1);
                const bool sized = pumpUntil([&] { return !stub.calls(kSize, out, mark).empty(); }, kArrivalLimitMs);
                pumpFor(kTurnMs);
                if (!sized || !sizeRefusedForGood(stub, out, mark)) {
                    notVerified("the monitor-size-refused phase", "the size report after the change was not refused for "
                                                                  "good as staged (" +
                                                                      ReportStub::describe(stub.callsFor(out, mark)) + ")");
                } else {
                    check(holdsVisible(stub, out, mark),
                          "after a new monitor id whose size report is refused, the daemon holds SetOutputVisible(name, true)");
                    check(holdsFocus(stub, out, mark, true, true),
                          "and the window state changed in the same turn as the id");
                }
                std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(stub.callsFor(out, mark)).c_str());
            }
        }
    }

    if (phaseSelected("monitor-second-chance")) {
        const QString out = QStringLiteral("DP-MOVED-AGAIN");
        Scene scene = harness.settled(out);
        if (ready(scene, "the monitor-second-chance phase")) {
            // GOAL: a window report sent at a monitor-id change and refused is sent again when the size
            //       report succeeds, so the daemon ends up holding the state.
            // WHY-THIS-SHAPE: the direct send is refused with InvalidArgs, a refusal reportWindows
            //       does not retry on its own, so the size report's success is the only way the state
            //       can come back; a RenderFailed refusal would be re-sent by the window retry timer
            //       too, and the check would need a timing bound to tell the two apart.
            // CONTROL: `reportWindows();` deleted from reportSize's success branch
            //       (backend/kde/wallpaperx_item.cpp) -> this line red.
            if (!focusAccepted(stub, scene, out)) {
                notVerified("the monitor-second-chance phase", "the focused state was never accepted before the change");
            } else {
                stub.refuseNext(kFocus, out, kInvalidArgs);
                const std::size_t mark = stub.mark();
                scene.renderer->setProperty("monitorId", 1);
                const bool sized = pumpUntil([&] { return !stub.calls(kSize, out, mark).empty(); }, kArrivalLimitMs);
                pumpUntil([&] { return holdsFocus(stub, out, mark, true, false); }, kArrivalLimitMs);
                pumpFor(kTurnMs);
                const std::vector<ReportStub::Call> focus = stub.calls(kFocus, out, mark);
                const std::vector<ReportStub::Call> sizes = stub.calls(kSize, out, mark);
                if (!sized || sizes.front().error != QString() || focus.empty() || focus.front().error != kInvalidArgs ||
                    focus.front().atMs > sizes.front().atMs) {
                    notVerified("the monitor-second-chance phase", "the direct report was not refused before an accepted "
                                                                   "size report, as staged (" +
                                                                       ReportStub::describe(stub.callsFor(out, mark)) + ")");
                } else {
                    check(holdsFocus(stub, out, mark, true, false),
                          "a window report refused at a monitor change is sent again when the size report succeeds");
                }
                std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(stub.callsFor(out, mark)).c_str());
            }
        }
    }

    if (phaseSelected("restart-size-refused")) {
        const QString out = QStringLiteral("DP-RESTART-NOSIZE");
        Scene scene = harness.settled(out);
        if (ready(scene, "the restart-size-refused phase")) {
            // GOAL: a daemon that takes the bus name over and refuses the size report for good still
            //       ends up holding SetOutputVisible(name, true) and the current window state: the
            //       pause modes otherwise act on the new daemon's defaults until the next window change.
            // WHY-THIS-SHAPE: as monitor-size-refused: InvalidArgs is never retried, so the size
            //       report's success cannot be what sent them, and the phase is not verified unless
            //       that refusal is what happened. daemon-restart cannot tell the two paths apart.
            // CONTROL: in the DaemonWatch::replaced handler of WallpaperXItem::connectToDaemon
            //       (backend/kde/wallpaperx_item.cpp), `if (m_onScreen) reportVisible();` deleted -> the
            //       visibility line red; the `reportWindows();` after it deleted -> the window line red;
            //       both deleted -> both red, the size report's refusal the only call after the restart.
            //       The first two re-run 2026-10-05 via control_batch.sh, --phase '^restart-size-refused$'.
            if (!focusAccepted(stub, scene, out)) {
                notVerified("the restart-size-refused phase", "the focused state was never accepted before the restart");
            } else {
                stub.refuseNext(kSize, out, kInvalidArgs);
                const std::size_t mark = stub.mark();
                if (!wallpaperx::testing::restartDaemonName(bus)) {
                    notVerified("the restart-size-refused phase", "the stub could not give up and retake the bus name");
                } else {
                    const bool sized = pumpUntil([&] { return !stub.calls(kSize, out, mark).empty(); }, kArrivalLimitMs);
                    pumpFor(kTurnMs);
                    if (!sized || !sizeRefusedForGood(stub, out, mark)) {
                        notVerified("the restart-size-refused phase", "the size report after the restart was not refused "
                                                                      "for good as staged (" +
                                                                          ReportStub::describe(stub.callsFor(out, mark)) + ")");
                    } else {
                        check(holdsVisible(stub, out, mark),
                              "a new daemon owner that refuses the size report holds SetOutputVisible(name, true)");
                        check(holdsFocus(stub, out, mark, true, false), "and the current window state");
                    }
                }
                std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(stub.callsFor(out, mark)).c_str());
            }
        }
    }

    if (phaseSelected("restart-unknown")) {
        const QString out = QStringLiteral("DP-RESTART-UNKNOWN");
        Scene scene = harness.settled(out);
        if (ready(scene, "the restart-unknown phase")) {
            // GOAL: a new daemon that does not know the name yet -- every report refused as
            //       UnknownOutput, the size report once -- ends up holding SetOutputVisible(name, true)
            //       and the current window state once it announces ConfigChanged (the screen named) and
            //       the size report that sends is accepted.
            // WHY-THIS-SHAPE: the first visibility, window and size reports after the restart are
            //       all refused, so neither direct send can satisfy the lines; only the size report sent
            //       at the notice and its success's re-send can. The phase is not verified unless the
            //       direct sends were what the stub refused.
            // CONTROL: in reportSize's success branch (backend/kde/wallpaperx_item.cpp),
            //       `reportVisible();` deleted -> the visibility line red. `|| refusal == Refusal::Unnamed`
            //       deleted from reportSize's `if (refusal == Refusal::Absent || ...)` reset-and-return
            //       -> both lines red. Via control_run.sh 2026-10-08, --phase '^restart-unknown$'.
            //       Deleting `m_sizeReported.reset();` from either reply handler's UnknownOutput branch
            //       leaves it green: the size report's own refusal clears the same record here.
            if (!focusAccepted(stub, scene, out)) {
                notVerified("the restart-unknown phase", "the focused state was never accepted before the restart");
            } else {
                stub.refuseNext(kVisible, out, kUnknownOutput);
                stub.refuseNext(kFocus, out, kUnknownOutput);
                stub.refuseNext(kSize, out, kUnknownOutput);
                const std::size_t mark = stub.mark();
                if (!wallpaperx::testing::restartDaemonName(bus)) {
                    notVerified("the restart-unknown phase", "the stub could not give up and retake the bus name");
                } else {
                    // The three refusals first, then the notice that says the name is known again.
                    pumpUntil(
                        [&] {
                            return !stub.calls(kVisible, out, mark).empty() && !stub.calls(kFocus, out, mark).empty() &&
                                   !stub.calls(kSize, out, mark).empty();
                        },
                        kArrivalLimitMs);
                    pumpFor(kTurnMs);
                    const bool noticed = wallpaperx::testing::announceConfigChanged(bus);
                    if (noticed) {
                        pumpUntil([&] { return holdsVisible(stub, out, mark) && holdsFocus(stub, out, mark, true, false); },
                                  kArrivalLimitMs);
                    }
                    pumpFor(kTurnMs);
                    const std::vector<ReportStub::Call> visible = stub.calls(kVisible, out, mark);
                    const std::vector<ReportStub::Call> focus = stub.calls(kFocus, out, mark);
                    const std::vector<ReportStub::Call> sizes = stub.calls(kSize, out, mark);
                    if (!noticed) {
                        notVerified("the restart-unknown phase", "the stub could not send ConfigChanged");
                    } else if (visible.empty() || visible.front().error != kUnknownOutput || focus.empty() ||
                               focus.front().error != kUnknownOutput || sizes.empty() ||
                               sizes.front().error != kUnknownOutput) {
                        notVerified("the restart-unknown phase", "the first reports after the restart were not refused as "
                                                                 "UnknownOutput as staged (" +
                                                                     ReportStub::describe(stub.callsFor(out, mark)) + ")");
                    } else {
                        check(holdsVisible(stub, out, mark),
                              "a new daemon that did not know the name holds SetOutputVisible(name, true) once it does");
                        check(holdsFocus(stub, out, mark, true, false), "and the current window state");
                    }
                }
                std::fprintf(stderr, "  %s: %s\n", qPrintable(out), ReportStub::describe(stub.callsFor(out, mark)).c_str());
            }
        }
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
