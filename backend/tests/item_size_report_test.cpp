// SPDX-License-Identifier: GPL-2.0-or-later

// What the KDE plugin tells the daemon about its screen's size (S13.106): SetOutputSize, carrying
// the item's size times its window's device pixel ratio, under the connector name main.qml handed
// it -- once the triggers have stopped for 250 ms, again whenever the size, the scale, the monitor id
// or the daemon changes. UnknownOutput waits for the daemon's ConfigChanged and ServiceUnknown for a
// daemon to arrive; InvalidArgs and UnknownMethod are not sent again and are logged each time they
// happen; any other refusal -- nothing announces its end -- is resent after 2, 4, 8, 8 ... s.
//
// A stub on this run's private bus stands in for the daemon and records every call with its arrival
// time. The item is the plugin's own registered type, parented into a QQuickWindow on the offscreen
// platform, configured here with two screens at 96 and 192 logical DPI: moving the window between
// them is a real scale change with no display. Nothing here renders, so no OpenGL is needed. One
// phase per scenario (WALLPAPERX_TEST_PHASE), each refusal phase on screen names of its own.

#include "tests/pump.h"
#include "tests/report_stub.h"
#include "tests/stderr_capture.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QList>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::phaseSelected;
using wallpaperx::testing::pumpFor;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::ReportStub;
using wallpaperx::testing::StderrCapture;

constexpr const char* kGoals = "S13.106 (the plugin reports its screen's size, when, and what it does with a refusal)";

const QString kSize = QStringLiteral("SetOutputSize");
const QString kRenderFailed = QStringLiteral("io.wallpaperx.Daemon.Error.RenderFailed");

// The plugin's settle interval, written out rather than included, for the reason
// item_refusal_retry_test gives: a check reading the plugin's own constant moves with it.
constexpr qint64 kSettleMs = 250;

// QTimer's default coarse accuracy may fire up to 5% early, and a timer never fires earlier than
// that. This lower bound is therefore reachable only by a plugin that did not wait at all.
constexpr qint64 kSettleFloorMs = 200;

// How long a phase waits for something that must happen, and the span over which "not re-sent" is
// read: long enough that a timer re-send of any plausible interval lands inside it.
constexpr int kArrivalLimitMs = 5000;
constexpr int kQuietSpanMs = 6000;
// After a ConfigChanged has been answered, how long a second re-send is watched for.
constexpr int kAfterNoticeMs = 1000;

// The waits of a run of transient refusals, written out for the reason kSettleMs is: 2 s, doubling,
// capped at 8 s. Four refusals, then the fifth send is accepted.
constexpr qint64 kRunWaitsMs[] = {2000, 4000, 8000, 8000};
constexpr std::size_t kRunRefusals = 4;
// A wait is graded as at least 94% of its nominal length (QTimer's coarse timers fire up to 5% early;
// 88 ms early on 2000 was measured) and at most 150% of it. The ceiling is not a latency budget: it
// sits halfway to the next doubling, so it tells 2 s from 4 s and 8 s from 16 s and nothing finer.
constexpr qint64 waitFloor(qint64 nominalMs) { return nominalMs * 94 / 100; }
constexpr qint64 waitCeiling(qint64 nominalMs) { return nominalMs * 3 / 2; }
// The whole run, plus kArrivalLimitMs of slack.
constexpr int kRunLimitMs = 2000 + 4000 + 8000 + 8000 + kArrivalLimitMs;
// Longer than the longest wait, so a send after the acceptance would land inside it.
constexpr int kAfterAcceptMs = 10000;
// Long enough for a resend at the wrong 8 s wait to arrive, so a slow resend is graded by its gap
// rather than missed.
constexpr int kRestartLimitMs = static_cast<int>(kSettleMs) + 8000 + 3000;
// Long enough for the item's settle timer and a bus round trip, many times over.
constexpr int kTurnMs = 300;

constexpr int kItemWidth = 300;
constexpr int kItemHeight = 170;

quint32 widthOf(const ReportStub::Call& report) { return report.args.value(1).toUInt(); }
quint32 heightOf(const ReportStub::Call& report) { return report.args.value(2).toUInt(); }

// Counts ItemDevicePixelRatioHasChanged, so a scale change the platform never delivered is told apart
// from one the plugin ignored. ItemHasContents because Qt delivers the change only to items with it,
// which the plugin's item also sets.
class RatioProbe : public QQuickItem {
public:
    RatioProbe() { setFlag(ItemHasContents, true); }
    int changes = 0;

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override { return old; }
    void itemChange(ItemChange change, const ItemChangeData& value) override {
        QQuickItem::itemChange(change, value);
        if (change == ItemDevicePixelRatioHasChanged) ++changes;
    }
};

QString qmlSource() {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: %1
    height: %2
    property Item rendererItem: renderer

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
    }
}
)")
        .arg(kItemWidth)
        .arg(kItemHeight);
}

std::unique_ptr<QObject> makeScene(QQmlEngine& engine, const QString& tag) {
    QQmlComponent component(&engine);
    component.setData(qmlSource().toUtf8(), QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                                QStringLiteral("/item_size_report_%1.qml").arg(tag)));
    std::unique_ptr<QObject> root(component.create());
    if (root == nullptr) {
        for (const QQmlError& error : component.errors()) std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
    }
    return root;
}

QQuickItem* rendererOf(const std::unique_ptr<QObject>& root) {
    return root ? qvariant_cast<QQuickItem*>(root->property("rendererItem")) : nullptr;
}

std::string describe(const std::vector<ReportStub::Call>& reports) {
    std::string out;
    for (const ReportStub::Call& r : reports) {
        out += (out.empty() ? "" : ", ") + std::to_string(widthOf(r)) + "x" + std::to_string(heightOf(r)) + " at " +
               std::to_string(r.atMs) + " ms";
    }
    return out.empty() ? "(none)" : out;
}

// A plugin item in the shared window, named `output`. `item` is null when the QML type could not be
// instantiated.
struct Scene {
    std::unique_ptr<QObject> root;
    QQuickItem* item = nullptr;
};

// What every phase shares: the stub, the window both screens are reached through, and the engine.
struct Harness {
    ReportStub& stub;
    QDBusConnection& bus;
    QQmlEngine& engine;
    QQuickWindow& window;
    RatioProbe& probe;
    QList<QScreen*> screens;

    std::vector<ReportStub::Call> reportsFor(const QString& output) const { return stub.calls(kSize, output); }
    std::pair<quint32, quint32> expected(int w, int h) const {
        const qreal ratio = window.effectiveDevicePixelRatio();
        return {static_cast<quint32>(qRound(w * ratio)), static_cast<quint32>(qRound(h * ratio))};
    }
    Scene scene(const QString& output) const {
        Scene s;
        s.root = makeScene(engine, output);
        s.item = rendererOf(s.root);
        if (s.item != nullptr) {
            qobject_cast<QQuickItem*>(s.root.get())->setParentItem(window.contentItem());
            s.item->setProperty("output", output);
        }
        return s;
    }
};

// The log line's tail for a size the stub refused for `output`: the stub's error message.
QString refusalLogFor(const QString& output) { return QStringLiteral("this stub refuses SetOutputSize for %1").arg(output); }

void reportingPhase(Harness& harness) {
    ReportStub& stub = harness.stub;
    QDBusConnection& bus = harness.bus;
    QQuickWindow& window = harness.window;
    RatioProbe& probe = harness.probe;
    const QList<QScreen*>& screens = harness.screens;

    const QString sized = QStringLiteral("DP-SIZE");
    Scene scene = harness.scene(sized);
    QQuickItem* renderer = scene.item;
    check(renderer != nullptr, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");
    if (renderer == nullptr) {
        notVerified(kGoals, "the plugin's QML type could not be instantiated, so nothing was ever reported");
        return;
    }
    qint64 lastTriggerMs = stub.elapsedMs();

    // GOAL: S13.106 -- the size reported is the item's size in device pixels, under the connector
    //       name it was given, and not before the triggers have settled.
    // WHY-THIS-SHAPE: the settle is graded as a lower bound on the gap from the last trigger to the
    //       arrival, never as an upper one: a timer cannot fire early, so a lower bound holds on any
    //       machine, while "within 300 ms" would grade the scheduler.
    // CONTROL: `m_sizeReportTimer.start(kSizeReportSettleMs)` in WallpaperXItem::scheduleSizeReport
    //       (backend/kde/wallpaperx_item.cpp) replaced by a direct `reportSize();` -> "FAIL: the first
    //       size report waits for the triggers to settle", printing 0 ms, and the burst check below red
    //       with five reports. Reverted.
    // CONTROL: `* ratio` dropped from both axes in WallpaperXItem::reportSize -> only the scale check
    //       below red, printing 350x170 against 700x340; this one stays green at ratio 1. Reverted.
    const bool first = pumpUntil([&] { return !harness.reportsFor(sized).empty(); }, kArrivalLimitMs);
    check(first, "the item reports its size once it is in a window with a connector name");
    if (!first) {
        notVerified(kGoals, "no size report ever arrived, so none of the reporting phase's timing could be read");
        return;
    }
    {
        const ReportStub::Call report = harness.reportsFor(sized).front();
        const auto [w, h] = harness.expected(kItemWidth, kItemHeight);
        check(widthOf(report) == w && heightOf(report) == h, "the first report is the item's size in device pixels");
        check(report.atMs - lastTriggerMs >= kSettleFloorMs, "the first size report waits for the triggers to settle");
        if (widthOf(report) != w || heightOf(report) != h || report.atMs - lastTriggerMs < kSettleFloorMs) {
            std::fprintf(stderr, "  reported %ux%u %lld ms after the last trigger; expected %ux%u no sooner than %lld ms\n",
                         widthOf(report), heightOf(report), static_cast<long long>(report.atMs - lastTriggerMs), w, h,
                         static_cast<long long>(kSettleFloorMs));
        }
    }
    bool wellFormed = true;
    for (const ReportStub::Call& report : stub.allCalls(kSize)) wellFormed = wellFormed && report.args.size() == 3;
    check(wellFormed, "every SetOutputSize carried exactly a name, a width and a height");

    // GOAL: S13.106 -- a burst of geometry changes costs the daemon one report, of the final size.
    // WHY-THIS-SHAPE: the five changes are made without returning to the event loop, so no timer can
    //       fire between them on any machine and "exactly one" is not a claim about scheduling. The
    //       unchanged-size half is read over a span twice the settle.
    //       The unchanged-size half makes a real geometry change of 0.2 px, which rounds to the same
    //       device pixels: without a trigger, a plugin with no latch at all would also send nothing.
    // CONTROL: the `if (newGeometry.size() != oldGeometry.size())` trigger deleted from
    //       WallpaperXItem::geometryChange (backend/kde/wallpaperx_item.cpp) -> only "FAIL: a burst of
    //       size changes is reported once, at the final size", printing no new report. Reverted.
    // CONTROL: `|| m_sizeReported == report` removed from reportSize's early return -> only "FAIL: a
    //       trigger that leaves the size in device pixels unchanged sends nothing". Reverted.
    {
        QQuickItem* top = qobject_cast<QQuickItem*>(scene.root.get());
        // A real geometry change, so the report is triggered, that rounds to the same device pixels.
        top->setWidth(kItemWidth + 0.2);
        pumpFor(2 * static_cast<int>(kSettleMs));
        const size_t before = harness.reportsFor(sized).size();
        check(before == 1, "a trigger that leaves the size in device pixels unchanged sends nothing");
        for (int step = 1; step <= 5; ++step) top->setWidth(kItemWidth + 10 * step);
        lastTriggerMs = stub.elapsedMs();
        const bool arrived = pumpUntil([&] { return harness.reportsFor(sized).size() > before; }, kArrivalLimitMs);
        pumpFor(2 * static_cast<int>(kSettleMs));
        const std::vector<ReportStub::Call> after = harness.reportsFor(sized);
        const auto [w, h] = harness.expected(kItemWidth + 50, kItemHeight);
        const bool once = arrived && after.size() == before + 1 && widthOf(after.back()) == w &&
                          heightOf(after.back()) == h && after.back().atMs - lastTriggerMs >= kSettleFloorMs;
        check(once, "a burst of size changes is reported once, at the final size, after the settle");
        if (!once) std::fprintf(stderr, "  reports so far: %s\n", describe(after).c_str());
    }

    // GOAL: S13.106 -- a scale change is reported, at the new ratio.
    // WHY-THIS-SHAPE: the probe item beside the plugin counts ItemDevicePixelRatioHasChanged, so a
    //       platform that never delivered the change skips instead of blaming the plugin. Moving the
    //       window between the two configured screens is the only scale change the offscreen platform
    //       can make.
    // CONTROL: the ItemDevicePixelRatioHasChanged branch deleted from WallpaperXItem::itemChange
    //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: a scale change is reported at the new ratio",
    //       and the monitor-id check after it red too, since the size it re-sends is then 700x340 against
    //       a last report of 350x170. Reverted.
    {
        const size_t before = harness.reportsFor(sized).size();
        const int probeBefore = probe.changes;
        window.setScreen(screens.at(1));
        const bool delivered = pumpUntil([&] { return probe.changes > probeBefore; }, kArrivalLimitMs);
        if (!delivered || window.effectiveDevicePixelRatio() != 2.0) {
            notVerified("S13.106's scale-change clause",
                        "moving the window to the 192 dpi offscreen screen did not change its device pixel ratio to 2 "
                        "(it is " + std::to_string(window.effectiveDevicePixelRatio()) + ")");
        } else {
            const auto [w, h] = harness.expected(kItemWidth + 50, kItemHeight);
            const bool arrived = pumpUntil(
                [&] {
                    const std::vector<ReportStub::Call> now = harness.reportsFor(sized);
                    return now.size() > before && widthOf(now.back()) == w && heightOf(now.back()) == h;
                },
                kArrivalLimitMs);
            check(arrived, "a scale change is reported at the new ratio");
            if (!arrived) std::fprintf(stderr, "  wanted %ux%u; reports so far: %s\n", w, h, describe(harness.reportsFor(sized)).c_str());
        }
        pumpFor(2 * static_cast<int>(kSettleMs));
    }

    // GOAL: S13.106 -- a monitor-id change sends the unchanged size again. The daemon may have
    //       unbound and rebound the name in between, and the plugin cannot tell.
    // CONTROL: `m_sizeReported.reset();` deleted from WallpaperXItem::setMonitorId
    //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: a monitor-id change reports the unchanged
    //       size again". Reverted.
    {
        const size_t before = harness.reportsFor(sized).size();
        const ReportStub::Call last = harness.reportsFor(sized).back();
        renderer->setProperty("monitorId", 1);
        const bool again = pumpUntil([&] { return harness.reportsFor(sized).size() > before; }, kArrivalLimitMs);
        check(again && widthOf(harness.reportsFor(sized).back()) == widthOf(last) &&
                  heightOf(harness.reportsFor(sized).back()) == heightOf(last),
              "a monitor-id change reports the unchanged size again");
        pumpFor(2 * static_cast<int>(kSettleMs));
    }

    // GOAL: S13.106 -- the daemon's bus name changing owner sends the unchanged size again: a
    //       restarted daemon has only what its config file kept.
    // WHY-THIS-SHAPE: the stub gives the name up and takes it back, which is a real
    //       NameOwnerChanged pair on the bus the plugin watches -- not a call into the plugin.
    // CONTROL: `m_sizeReported.reset();` deleted from the DaemonWatch::replaced handler in
    //       WallpaperXItem::connectToDaemon (backend/kde/wallpaperx_item.cpp) -> "FAIL: a daemon
    //       restart reports the unchanged size again". Re-run 2026-10-05 via control_batch.sh. Reverted.
    {
        const size_t before = harness.reportsFor(sized).size();
        if (!wallpaperx::testing::restartDaemonName(bus)) {
            notVerified("S13.106's daemon-restart clause", "the stub could not give up and retake the daemon's bus name");
        } else {
            const bool again = pumpUntil([&] { return harness.reportsFor(sized).size() > before; }, kArrivalLimitMs);
            check(again, "a daemon restart reports the unchanged size again");
        }
    }
}

void eventRefusalsPhase(Harness& h) {
    // GOAL: S13.106 -- a size refused for a reason a later event ends, or no resend can cure, is never
    //       resent on a timer: over a 6 s span each of four refusals is sent once. At the daemon's next
    //       ConfigChanged the UnknownOutput one (the screen not yet named) is sent once more; InvalidArgs,
    //       and UnknownMethod from an older daemon, are not sent again and are logged once -- and logged
    //       again when a monitor-id change sends them again and they are refused again. ServiceUnknown
    //       (no daemon) is graded only before the notice: its cure is the owner watch, and a notice
    //       resending it is neither required nor forbidden.
    // WHY-THIS-SHAPE: four items at once, read over one span and answered by one notice, so the curable
    //       and the kept refusals are graded against the same events: a notice that never reached the
    //       items cannot pass the "not sent again at the notice" half, because UnknownOutput must have
    //       answered that same notice first. Every refusal is refuseAlways, so a resend is refused
    //       again and a plugin that re-armed anything on it would show a third send. The second
    //       refusal of a permanent one is the same size, so only a run-style count could silence it.
    // CONTROL: in backend/kde/wallpaperx_item.cpp, via control_run.sh 2026-10-08, --phase
    //       '^event-refusals$' through control_batch.sh, each reverted. In classifyRefusal: UnknownOutput
    //       returning `Refusal::Retried` -> UnknownOutput's "not re-sent before any notice" alone (sent at
    //       242 and 2342 ms); ServiceUnknown returning `Retried` -> ServiceUnknown's "not re-sent" alone;
    //       InvalidArgs dropped from the Permanent test -> InvalidArgs's "not re-sent", "not sent again, a
    //       ConfigChanged included" and "logged again".
    //       `watchConfigChanged(this, SLOT(onConfigChanged()));` deleted from connectToDaemon ->
    //       UnknownOutput's "sent once more at ConfigChanged" alone. In reportSize's reply handler, `times`
    //       counted and the run recorded for a permanent refusal too -> both "logged again" lines alone.
    // COVERAGE-ASSERT: every item's first report must have arrived and been refused before the span,
    //       and the notice must have been sent, or the phase is not verified; the second refusal must
    //       have arrived before its log is read.
    enum class AtNotice { SentOnceMore, NotSent, NotGraded };
    struct Case {
        QString output;
        QString error;
        AtNotice atNotice;
        Scene scene;
    };
    std::vector<Case> cases;
    cases.push_back({QStringLiteral("DP-UNKNOWN"), QStringLiteral("io.wallpaperx.Daemon.Error.UnknownOutput"),
                     AtNotice::SentOnceMore, {}});
    cases.push_back({QStringLiteral("DP-NODAEMON"), QStringLiteral("org.freedesktop.DBus.Error.ServiceUnknown"),
                     AtNotice::NotGraded, {}});
    cases.push_back({QStringLiteral("DP-INVALIDARGS"), QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                     AtNotice::NotSent, {}});
    cases.push_back({QStringLiteral("DP-OLDDAEMON"), QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"),
                     AtNotice::NotSent, {}});

    StderrCapture capture;
    bool allRefused = capture.ok();
    if (allRefused) {
        for (Case& c : cases) {
            h.stub.refuseAlways(kSize, c.output, c.error);
            c.scene = h.scene(c.output);
            if (c.scene.item == nullptr) {
                allRefused = false;
                break;
            }
        }
    }
    if (allRefused) {
        allRefused = pumpUntil(
            [&] {
                for (const Case& c : cases) {
                    if (h.reportsFor(c.output).empty()) return false;
                }
                return true;
            },
            kArrivalLimitMs);
    }
    // Before the notice: what each item sent over the quiet span.
    std::vector<std::size_t> beforeNotice;
    bool noticed = false;
    bool curedAnswered = false;
    if (allRefused) {
        pumpFor(kQuietSpanMs);
        for (const Case& c : cases) beforeNotice.push_back(h.reportsFor(c.output).size());
        noticed = wallpaperx::testing::announceConfigChanged(h.bus);
        if (noticed) {
            curedAnswered = pumpUntil(
                [&] {
                    for (std::size_t i = 0; i < cases.size(); ++i) {
                        if (cases[i].atNotice == AtNotice::SentOnceMore &&
                            h.reportsFor(cases[i].output).size() <= beforeNotice[i]) {
                            return false;
                        }
                    }
                    return true;
                },
                kArrivalLimitMs);
            pumpFor(kAfterNoticeMs);
        }
    }
    capture.restore();

    if (!allRefused || !noticed) {
        notVerified("S13.106's event-refusal clauses", !capture.ok()   ? "stderr could not be captured"
                                                       : !allRefused ? "not every refused item reached the stub"
                                                                     : "the stub could not send ConfigChanged");
        return;
    }
    const QString logged = capture.text();
    // GOAL (a property of this harness): the only refusals in the span are the size refusals
    //       this phase stages, so the counts below are about them. False when the stub declines
    //       the item's visibility or window report and Qt's error is logged beside them.
    // CONTROL: via control_batch.sh, --phase '^event-refusals$', 2026-10-11, each alone, in
    //       backend/tests/report_stub.h, reverted: SetOutputVisible dropped from the methods it answers ->
    //       this line red (the visibility half); SetOutputFocusState answered InvalidArgs, a refusal the item
    //       logs -> this line red (the window half). Dropping the focus method instead cannot work: the item
    //       reads UnknownMethod as an older daemon and logs nothing.
    const int otherRefusals = logged.count(QStringLiteral("refused this screen's visibility")) +
                              logged.count(QStringLiteral("refused this screen's window state"));
    check(otherRefusals == 0, "the item logged no refusal of a report this stub does not refuse");
    if (otherRefusals != 0) std::fprintf(stderr, "  %d such line(s) logged\n", otherRefusals);
    if (!curedAnswered) std::fprintf(stderr, "  the UnknownOutput refusal was not sent again within 5 s of the notice\n");
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        const std::vector<ReportStub::Call> reports = h.reportsFor(c.output);
        const std::string name = c.error.toStdString();
        check(beforeNotice[i] == 1, ("a size refused with " + name + " is not re-sent before any notice").c_str());
        if (c.atNotice == AtNotice::SentOnceMore) {
            check(reports.size() == beforeNotice[i] + 1,
                  ("a size refused with " + name + " is sent once more at ConfigChanged").c_str());
        } else if (c.atNotice == AtNotice::NotSent) {
            check(reports.size() == 1, ("a size refused with " + name + " is not sent again, a ConfigChanged included").c_str());
            const int times = static_cast<int>(logged.count(refusalLogFor(c.output)));
            check(times == 1, ("a size refused with " + name + " is logged once").c_str());
            if (times != 1) std::fprintf(stderr, "  logged %d time(s)\n", times);
        }
        std::fprintf(stderr, "  %s: %zu before the notice; all: %s\n", qPrintable(c.output), beforeNotice[i],
                     describe(reports).c_str());
    }

    // The permanent refusals once more: a monitor-id change sends the same size again.
    StderrCapture again;
    std::vector<std::size_t> sentBefore;
    for (const Case& c : cases) sentBefore.push_back(h.reportsFor(c.output).size());
    for (Case& c : cases) {
        if (c.atNotice == AtNotice::NotSent) c.scene.item->setProperty("monitorId", 1);
    }
    const bool resent = pumpUntil(
        [&] {
            for (std::size_t i = 0; i < cases.size(); ++i) {
                if (cases[i].atNotice == AtNotice::NotSent && h.reportsFor(cases[i].output).size() <= sentBefore[i]) {
                    return false;
                }
            }
            return true;
        },
        kArrivalLimitMs);
    pumpFor(kTurnMs);
    const QString secondLog = again.text();
    if (!again.ok() || !resent) {
        notVerified("S13.106's every-refusal-logged clause", !again.ok() ? "stderr could not be captured"
                                                                         : "a monitor-id change did not send the "
                                                                           "permanently refused sizes again");
        return;
    }
    for (const Case& c : cases) {
        if (c.atNotice != AtNotice::NotSent) continue;
        const int times = static_cast<int>(secondLog.count(refusalLogFor(c.output)));
        check(times == 1, ("a size refused with " + c.error.toStdString() + " is logged again when it is refused again")
                              .c_str());
        if (times != 1) std::fprintf(stderr, "  %s: logged %d time(s) at its second refusal\n", qPrintable(c.output), times);
    }
}

void transientBackoffPhase(Harness& h) {
    constexpr const char* kClause = "S13.106's retried-refusal clauses";
    // GOAL: S13.106 -- a size refused for any reason but a missing daemon, an unnamed screen, its
    //       arguments or an unknown method -- nothing announces the end of any of them -- is resent
    //       after about 2, 4, 8 and 8 s, stops at the first acceptance, and the run is logged once;
    //       after that acceptance the same size refused again starts back at 2 s. Graded for
    //       RenderFailed, NoReply, Timeout, LimitsExceeded, NoMemory, AccessDenied, Disconnected and an
    //       error name the plugin does not know. False if the plugin waits for a notice that never
    //       comes (the screen stays drawn at the wrong size), resends at a fixed pace or past the cap,
    //       keeps sending once accepted, logs every resend, or carries a finished run's count on.
    // WHY-THIS-SHAPE: each wait is graded between 94% and 150% of nominal, because a floor alone cannot
    //       tell the 8 s cap from no cap (16 s) and the coarse timer fires up to 5% early. The stub
    //       refuses exactly four sends (refuseNext), so "stops" is read over 10 s, longer than the
    //       longest wait. The same size is sent again through a monitor-id change, because a geometry
    //       change would be a new size, which restarts at 2 s whether or not the run was cleared.
    // CONTROL: in backend/kde/wallpaperx_item.cpp, via control_run.sh 2026-10-08, --phase
    //       '^transient-backoff$' through control_batch.sh, each reverted, the lines named red for all
    //       eight errors. classifyRefusal's default made `Refusal::Permanent` -> every line except the two
    //       "logged once" lines (one send, then one more at the monitor-id change). The retry's
    //       `m_sizeReportTimer.start(doubledWait(...))` block deleted -> every line except "a run of ...
    //       refusals is logged once". The wait made a flat `kRefusedRetryMs` -> "resent after about 2, 4,
    //       8 and 8 s" alone; kSizeRetryMaxMs 16000 -> the same alone. The success branch's
    //       `m_sizeRefusals.reset();` deleted -> "the same size refused again ... after about 2 s" and
    //       "that new run ... logged once". The log condition `if (times == 0)` made `if (true)` -> "a run
    //       of ... refusals is logged once" alone. Test-side, by hand, earlier
    //       2026-10-08: a fifth refuseNext staged -> "not sent again once accepted" (6 sent) and the
    //       waits line, whose fifth send must be the accepted one.
    // COVERAGE-ASSERT: the waits line requires five sends, the first four refused with the staged error
    //       and the fifth accepted, so a run that never happened cannot read as paced.
    struct Run {
        QString output;
        QString error;
        Scene scene;
    };
    std::vector<Run> runs;
    runs.push_back({QStringLiteral("DP-RENDERFAILED"), kRenderFailed, {}});
    runs.push_back({QStringLiteral("DP-NOREPLY"), QStringLiteral("org.freedesktop.DBus.Error.NoReply"), {}});
    runs.push_back({QStringLiteral("DP-TIMEOUT"), QStringLiteral("org.freedesktop.DBus.Error.Timeout"), {}});
    runs.push_back({QStringLiteral("DP-LIMITS"), QStringLiteral("org.freedesktop.DBus.Error.LimitsExceeded"), {}});
    runs.push_back({QStringLiteral("DP-NOMEMORY"), QStringLiteral("org.freedesktop.DBus.Error.NoMemory"), {}});
    runs.push_back({QStringLiteral("DP-ACCESSDENIED"), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"), {}});
    runs.push_back({QStringLiteral("DP-DISCONNECTED"), QStringLiteral("org.freedesktop.DBus.Error.Disconnected"), {}});
    runs.push_back({QStringLiteral("DP-UNKNOWN-ERROR"), QStringLiteral("io.wallpaperx.Daemon.Error.SomethingNew"), {}});

    std::vector<std::size_t> afterAccept;
    QString firstLog;
    {
        StderrCapture capture;
        if (!capture.ok()) {
            notVerified(kClause, "stderr could not be captured");
            return;
        }
        for (Run& r : runs) {
            for (std::size_t i = 0; i < kRunRefusals; ++i) h.stub.refuseNext(kSize, r.output, r.error);
            r.scene = h.scene(r.output);
            if (r.scene.item == nullptr) {
                capture.restore();
                notVerified(kClause, "the plugin's QML type could not be instantiated");
                return;
            }
        }
        pumpUntil(
            [&] {
                for (const Run& r : runs) {
                    if (h.reportsFor(r.output).size() < kRunRefusals + 1) return false;
                }
                return true;
            },
            kRunLimitMs);
        pumpFor(kAfterAcceptMs);
        for (const Run& r : runs) afterAccept.push_back(h.reportsFor(r.output).size());
        firstLog = capture.text();
    }

    // The same size refused once more, reached again through a monitor-id change.
    const std::size_t mark = h.stub.mark();
    QString secondLog;
    {
        StderrCapture capture;
        if (!capture.ok()) {
            notVerified(kClause, "stderr could not be captured for the second run");
            return;
        }
        for (Run& r : runs) {
            h.stub.refuseNext(kSize, r.output, r.error);
            r.scene.item->setProperty("monitorId", 1);
        }
        pumpUntil(
            [&] {
                for (const Run& r : runs) {
                    if (h.stub.calls(kSize, r.output, mark).size() < 2) return false;
                }
                return true;
            },
            kRestartLimitMs);
        secondLog = capture.text();
    }

    for (std::size_t i = 0; i < runs.size(); ++i) {
        const Run& r = runs[i];
        const std::string name = r.error.toStdString();
        const std::vector<ReportStub::Call> reports = h.reportsFor(r.output);
        bool shaped = reports.size() >= kRunRefusals + 1;
        for (std::size_t k = 0; shaped && k < kRunRefusals; ++k) shaped = reports[k].error == r.error;
        shaped = shaped && reports[kRunRefusals].error.isEmpty();
        bool paced = shaped;
        std::string gaps;
        for (std::size_t k = 0; shaped && k < kRunRefusals; ++k) {
            const qint64 gap = reports[k + 1].atMs - reports[k].atMs;
            paced = paced && gap >= waitFloor(kRunWaitsMs[k]) && gap <= waitCeiling(kRunWaitsMs[k]);
            gaps += (gaps.empty() ? "" : ", ") + std::to_string(gap) + " ms";
        }
        check(paced, ("a size refused with " + name + " is resent after about 2, 4, 8 and 8 s").c_str());
        check(shaped && afterAccept[i] == kRunRefusals + 1,
              ("a size refused with " + name + " is not sent again once accepted").c_str());
        const int firstLines = static_cast<int>(firstLog.count(refusalLogFor(r.output)));
        check(firstLines == 1, ("a run of " + name + " refusals is logged once").c_str());

        const std::vector<ReportStub::Call> again = h.stub.calls(kSize, r.output, mark);
        const qint64 restartGap = again.size() >= 2 ? again[1].atMs - again[0].atMs : -1;
        const bool restarted = again.size() >= 2 && again[0].error == r.error && again[1].error.isEmpty() &&
                               restartGap >= waitFloor(kRunWaitsMs[0]) && restartGap <= waitCeiling(kRunWaitsMs[0]);
        check(restarted, ("after an accepted resend, the same size refused again with " + name +
                          " is resent after about 2 s").c_str());
        const int secondLines = static_cast<int>(secondLog.count(refusalLogFor(r.output)));
        check(secondLines == 1, ("and that new run of " + name + " refusals is logged once").c_str());
        std::fprintf(stderr, "  %s: waits %s; %zu sent by the end of the quiet span; logged %d then %d time(s)\n",
                     qPrintable(r.output), gaps.empty() ? "(not read)" : gaps.c_str(), afterAccept[i], firstLines,
                     secondLines);
        std::fprintf(stderr, "    first run: %s\n    after the monitor-id change: %s\n",
                     ReportStub::describe(reports).c_str(), ReportStub::describe(again).c_str());
    }
}

void newSizeBackoffPhase(Harness& h) {
    constexpr const char* kClause = "S13.106's new-size clause";
    // GOAL: S13.106 -- a run of transient refusals belongs to the size it refused: a new size refused
    //       the same way is resent after about 2 s, not at the wait the old size's run had reached.
    //       False if one count runs across sizes, so a screen resized during a run waits up to 8 s for
    //       no reason.
    // WHY-THIS-SHAPE: the old size is refused twice first, so a carried-over run would be at its third
    //       wait (8 s), well past the 3 s ceiling; a floor alone cannot see it, since carrying over
    //       only ever makes the wait longer. The resize comes kTurnMs after the second refusal
    //       arrived, so the item has heard that refusal and recorded the run.
    // CONTROL: `m_sizeRefusals->report == report` dropped from the `times` condition in reportSize's
    //       reply handler (backend/kde/wallpaperx_item.cpp) -> "a new size refused during a run of
    //       refusals is resent after about 2 s" red, resent after 7962 ms. Via control_run.sh
    //       2026-10-08. Reverted.
    // COVERAGE-ASSERT: the old size must have been refused twice before the resize, or not verified.
    const QString out = QStringLiteral("DP-RESIZED");
    h.stub.refuseAlways(kSize, out, kRenderFailed);
    Scene scene = h.scene(out);
    if (scene.item == nullptr) {
        notVerified(kClause, "the plugin's QML type could not be instantiated");
        return;
    }
    const bool twice = pumpUntil([&] { return h.reportsFor(out).size() >= 2; }, kArrivalLimitMs);
    if (!twice) {
        notVerified(kClause, "the old size was not sent and resent within 5 s, so no run was under way to carry over");
        return;
    }
    pumpFor(kTurnMs);
    qobject_cast<QQuickItem*>(scene.root.get())->setWidth(kItemWidth + 20);
    const auto [w, ht] = h.expected(kItemWidth + 20, kItemHeight);
    const auto newSize = [&] {
        std::vector<ReportStub::Call> out2;
        for (const ReportStub::Call& c : h.reportsFor(out)) {
            if (widthOf(c) == w && heightOf(c) == ht) out2.push_back(c);
        }
        return out2;
    };
    pumpUntil([&] { return newSize().size() >= 2; }, kRestartLimitMs);
    const std::vector<ReportStub::Call> sent = newSize();
    const qint64 gap = sent.size() >= 2 ? sent[1].atMs - sent[0].atMs : -1;
    check(sent.size() >= 2 && gap >= waitFloor(kRunWaitsMs[0]) && gap <= waitCeiling(kRunWaitsMs[0]),
          "a new size refused during a run of refusals is resent after about 2 s");
    std::fprintf(stderr, "  %s: new size %ux%u resent after %lld ms; all: %s\n", qPrintable(out), w, ht,
                 static_cast<long long>(gap), ReportStub::describe(h.reportsFor(out)).c_str());
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_size_report_test";

    QTemporaryDir scratch;
    const QString screensFile = scratch.path() + QStringLiteral("/screens.json");
    {
        QFile out(screensFile);
        const QByteArray json = R"({"screens":[
            {"name":"WPX-1X","x":0,"y":0,"width":1024,"height":768,"logicalDpi":96,"logicalBaseDpi":96},
            {"name":"WPX-2X","x":1024,"y":0,"width":1024,"height":768,"logicalDpi":192,"logicalBaseDpi":96}]})";
        if (!scratch.isValid() || !out.open(QIODevice::WriteOnly) || out.write(json) != json.size()) {
            notVerified(kGoals, "the offscreen platform's screen configuration could not be written");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
    }
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen:configfile=") + QFile::encodeName(screensFile));

    QGuiApplication app(argc, argv);

    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.size() != 2) {
        notVerified(kGoals, "the offscreen platform did not come up with the two configured screens (it has " +
                                std::to_string(screens.size()) + ")");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // Every arrival time below, and every trigger time compared with one, is read on the stub's clock.
    QDBusConnection bus = wallpaperx::testing::stubConnection();
    ReportStub stub;
    if (!wallpaperx::testing::claimDaemonName(bus, stub)) {
        notVerified(kGoals, "no session bus this run could own io.wallpaperx.Daemon on: " +
                                bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));

    QQuickWindow window;
    window.setScreen(screens.at(0));
    window.setGeometry(0, 0, 400, 300);
    window.show();

    RatioProbe probe;
    probe.setParentItem(window.contentItem());

    Harness harness{stub, bus, engine, window, probe, screens};
    if (phaseSelected("reporting")) reportingPhase(harness);
    if (phaseSelected("event-refusals")) eventRefusalsPhase(harness);
    if (phaseSelected("transient-backoff")) transientBackoffPhase(harness);
    if (phaseSelected("new-size-backoff")) newSizeBackoffPhase(harness);

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
