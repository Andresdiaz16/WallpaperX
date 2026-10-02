// SPDX-License-Identifier: GPL-2.0-or-later

// What the KDE plugin tells the daemon about its screen's size (S13.106): SetOutputSize, carrying
// the item's size times its window's device pixel ratio, under the connector name main.qml handed
// it -- once the triggers have stopped for 250 ms, again whenever the size, the scale, the monitor id
// or the daemon changes, and again after a refusal only when a resend can be answered differently.
//
// A stub on this run's private bus stands in for the daemon and records every call with its arrival
// time. The item is the plugin's own registered type, parented into a QQuickWindow on the offscreen
// platform, configured here with two screens at 96 and 192 logical DPI: moving the window between
// them is a real scale change with no display. Nothing here renders, so no OpenGL is needed.

#include "tests/pump.h"
#include "tests/report_stub.h"
#include "tests/stderr_capture.h"
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
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::pumpFor;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::ReportStub;
using wallpaperx::testing::StderrCapture;

const QString kSize = QStringLiteral("SetOutputSize");

// The plugin's settle and retry intervals, written out rather than included, for the reason
// item_refusal_retry_test gives: a check reading the plugin's own constant moves with it.
constexpr qint64 kSettleMs = 250;
constexpr qint64 kRetryMs = 2000;

// QTimer's default coarse accuracy may fire up to 5% early, and a timer never fires earlier than
// that. These lower bounds are therefore reachable only by a plugin that did not wait at all.
constexpr qint64 kSettleFloorMs = 200;
constexpr qint64 kRetryFloorMs = 1800;

// How long a phase waits for something that must happen, and the span over which "no retry" is
// read -- the same span in which the transient refusals are required to have retried.
constexpr int kArrivalLimitMs = 5000;
constexpr int kRetryWindowMs = 3 * static_cast<int>(kRetryMs);

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

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_size_report_test";
    const char* kGoals = "S13.106 (the plugin reports its screen's size, when, and what it does with a refusal)";

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

    const QString sized = QStringLiteral("DP-SIZE");
    std::unique_ptr<QObject> root = makeScene(engine, QStringLiteral("sized"));
    QQuickItem* renderer = rendererOf(root);
    check(renderer != nullptr, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");
    if (renderer == nullptr) {
        notVerified(kGoals, "the plugin's QML type could not be instantiated, so nothing was ever reported");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    qobject_cast<QQuickItem*>(root.get())->setParentItem(window.contentItem());
    renderer->setProperty("output", sized);
    qint64 lastTriggerMs = stub.elapsedMs();
    const auto reportsFor = [&stub](const QString& output) { return stub.calls(kSize, output); };

    const auto expected = [&window](int w, int h) {
        const qreal ratio = window.effectiveDevicePixelRatio();
        return std::pair<quint32, quint32>(static_cast<quint32>(qRound(w * ratio)),
                                           static_cast<quint32>(qRound(h * ratio)));
    };

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
    const bool first = pumpUntil([&] { return !reportsFor(sized).empty(); }, kArrivalLimitMs);
    check(first, "the item reports its size once it is in a window with a connector name");
    if (!first) {
        notVerified(kGoals, "no size report ever arrived, so none of the timing or refusal behaviour could be read");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    {
        const ReportStub::Call report = reportsFor(sized).front();
        const auto [w, h] = expected(kItemWidth, kItemHeight);
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
        QQuickItem* scene = qobject_cast<QQuickItem*>(root.get());
        // A real geometry change, so the report is triggered, that rounds to the same device pixels.
        scene->setWidth(kItemWidth + 0.2);
        pumpFor(2 * static_cast<int>(kSettleMs));
        const size_t before = reportsFor(sized).size();
        check(before == 1, "a trigger that leaves the size in device pixels unchanged sends nothing");
        for (int step = 1; step <= 5; ++step) scene->setWidth(kItemWidth + 10 * step);
        lastTriggerMs = stub.elapsedMs();
        const bool arrived = pumpUntil([&] { return reportsFor(sized).size() > before; }, kArrivalLimitMs);
        pumpFor(2 * static_cast<int>(kSettleMs));
        const std::vector<ReportStub::Call> after = reportsFor(sized);
        const auto [w, h] = expected(kItemWidth + 50, kItemHeight);
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
        const size_t before = reportsFor(sized).size();
        const int probeBefore = probe.changes;
        window.setScreen(screens.at(1));
        const bool delivered = pumpUntil([&] { return probe.changes > probeBefore; }, kArrivalLimitMs);
        if (!delivered || window.effectiveDevicePixelRatio() != 2.0) {
            notVerified("S13.106's scale-change clause",
                        "moving the window to the 192 dpi offscreen screen did not change its device pixel ratio to 2 "
                        "(it is " + std::to_string(window.effectiveDevicePixelRatio()) + ")");
        } else {
            const auto [w, h] = expected(kItemWidth + 50, kItemHeight);
            const bool arrived = pumpUntil(
                [&] {
                    const std::vector<ReportStub::Call> now = reportsFor(sized);
                    return now.size() > before && widthOf(now.back()) == w && heightOf(now.back()) == h;
                },
                kArrivalLimitMs);
            check(arrived, "a scale change is reported at the new ratio");
            if (!arrived) std::fprintf(stderr, "  wanted %ux%u; reports so far: %s\n", w, h, describe(reportsFor(sized)).c_str());
        }
        pumpFor(2 * static_cast<int>(kSettleMs));
    }

    // GOAL: S13.106 -- a monitor-id change sends the unchanged size again. The daemon may have
    //       unbound and rebound the name in between, and the plugin cannot tell.
    // CONTROL: `m_sizeReported.reset();` deleted from WallpaperXItem::setMonitorId
    //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: a monitor-id change reports the unchanged
    //       size again". Reverted.
    {
        const size_t before = reportsFor(sized).size();
        const ReportStub::Call last = reportsFor(sized).back();
        renderer->setProperty("monitorId", 1);
        const bool again = pumpUntil([&] { return reportsFor(sized).size() > before; }, kArrivalLimitMs);
        check(again && widthOf(reportsFor(sized).back()) == widthOf(last) &&
                  heightOf(reportsFor(sized).back()) == heightOf(last),
              "a monitor-id change reports the unchanged size again");
        pumpFor(2 * static_cast<int>(kSettleMs));
    }

    // GOAL: S13.106 -- the daemon's bus name changing owner sends the unchanged size again: a
    //       restarted daemon has only what its config file kept.
    // WHY-THIS-SHAPE: the stub gives the name up and takes it back, which is a real
    //       NameOwnerChanged pair on the bus the plugin watches -- not a call into the plugin.
    // CONTROL: `m_sizeReported.reset();` deleted from the serviceOwnerChanged handler in
    //       WallpaperXItem::connectToDaemon (backend/kde/wallpaperx_item.cpp) -> "FAIL: a daemon
    //       restart reports the unchanged size again". Reverted.
    {
        const size_t before = reportsFor(sized).size();
        if (!wallpaperx::testing::restartDaemonName(bus)) {
            notVerified("S13.106's daemon-restart clause", "the stub could not give up and retake the daemon's bus name");
        } else {
            const bool again = pumpUntil([&] { return reportsFor(sized).size() > before; }, kArrivalLimitMs);
            check(again, "a daemon restart reports the unchanged size again");
        }
    }

    // GOAL: S13.106 -- a refused report is resent after about two seconds for exactly the four
    //       refusals a resend can cure (UnknownOutput, RenderFailed, NoReply, Timeout), and for any
    //       other -- InvalidArgs, or UnknownMethod from a daemon older than the method -- is logged
    //       once and never resent.
    // WHY-THIS-SHAPE: six items at once, one refusal each, read over one span, so "not resent" is
    //       measured in the same window in which the four transient ones are required to have been
    //       resent -- a span too short to see a retry cannot pass the silent half. The resend gap is
    //       a lower bound for the settle's reason.
    // CONTROL: `m_sizeReportTimer.start(kRefusedRetryMs)` deleted from reportSize's reply handler
    //       (backend/kde/wallpaperx_item.cpp) -> four "is resent" lines red. Reverted.
    // CONTROL: `transient` forced true in the same handler -> both "is not resent" lines red, and both
    //       "is logged once" lines (each resend is refused and logged again). Reverted.
    // CONTROL: the `NoReply` comparison deleted from `transient` -> "FAIL: a size refused with
    //       org.freedesktop.DBus.Error.NoReply is resent" alone. Reverted.
    // CONTROL: the std::fprintf in that handler deleted -> both "is logged once" lines red, printing 0.
    //       Reverted.
    // COVERAGE-ASSERT: every item's first report is required to have arrived and been refused before
    //       the span is read, so a phase whose items never reached the stub cannot pass the silent half.
    {
        struct Case {
            QString output;
            QString error;
            bool transient;
            std::unique_ptr<QObject> root;
        };
        std::vector<Case> cases;
        cases.push_back({QStringLiteral("DP-UNKNOWN"), QStringLiteral("io.wallpaperx.Daemon.Error.UnknownOutput"), true, {}});
        cases.push_back({QStringLiteral("DP-RENDERFAILED"), QStringLiteral("io.wallpaperx.Daemon.Error.RenderFailed"), true, {}});
        cases.push_back({QStringLiteral("DP-NOREPLY"), QStringLiteral("org.freedesktop.DBus.Error.NoReply"), true, {}});
        cases.push_back({QStringLiteral("DP-TIMEOUT"), QStringLiteral("org.freedesktop.DBus.Error.Timeout"), true, {}});
        cases.push_back({QStringLiteral("DP-INVALIDARGS"), QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), false, {}});
        cases.push_back({QStringLiteral("DP-OLDDAEMON"), QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"), false, {}});

        StderrCapture capture(scratch.path() + QStringLiteral("/stderr.txt"));
        bool allRefused = capture.ok();
        if (allRefused) {
            for (Case& c : cases) {
                stub.refuseAlways(kSize, c.output, c.error);
                c.root = makeScene(engine, c.output);
                QQuickItem* item = rendererOf(c.root);
                if (item == nullptr) {
                    allRefused = false;
                    break;
                }
                qobject_cast<QQuickItem*>(c.root.get())->setParentItem(window.contentItem());
                item->setProperty("output", c.output);
            }
        }
        if (allRefused) {
            allRefused = pumpUntil(
                [&] {
                    for (const Case& c : cases) {
                        if (reportsFor(c.output).empty()) return false;
                    }
                    return true;
                },
                kArrivalLimitMs);
        }
        if (allRefused) pumpFor(kRetryWindowMs);
        capture.restore();

        if (!allRefused) {
            notVerified("S13.106's refusal clauses", capture.ok() ? "not every refused item reached the stub"
                                                                   : "stderr could not be captured");
        } else {
            const QString logged = capture.text();
            // GOAL (a property of this harness): the only refusals in the span are the size refusals
            //       this phase stages, so the counts below are about them. False when the stub declines
            //       the item's visibility or window report and Qt's error is logged beside them.
            // CONTROL: SetOutputVisible and SetOutputFocus dropped from the methods ReportStub answers
            //       (tests/report_stub.h) -> this line red. Observed 2026-10-01. Reverted.
            const int otherRefusals = logged.count(QStringLiteral("refused this screen's visibility")) +
                                      logged.count(QStringLiteral("refused this screen's window state"));
            check(otherRefusals == 0, "the item logged no refusal of a report this stub does not refuse");
            if (otherRefusals != 0) std::fprintf(stderr, "  %d such line(s) logged\n", otherRefusals);
            for (const Case& c : cases) {
                const std::vector<ReportStub::Call> reports = reportsFor(c.output);
                const std::string name = c.error.toStdString();
                if (c.transient) {
                    const bool resent = reports.size() >= 2 && reports[1].atMs - reports[0].atMs >= kRetryFloorMs;
                    check(resent, ("a size refused with " + name + " is resent, after the retry interval").c_str());
                    if (!resent) std::fprintf(stderr, "  %s: %s\n", qPrintable(c.output), describe(reports).c_str());
                } else {
                    check(reports.size() == 1, ("a size refused with " + name + " is not resent").c_str());
                    if (reports.size() != 1) std::fprintf(stderr, "  %s: %s\n", qPrintable(c.output), describe(reports).c_str());
                    const int times = logged.count(QStringLiteral("this stub refuses SetOutputSize for %1").arg(c.output));
                    check(times == 1, ("a size refused with " + name + " is logged once").c_str());
                    if (times != 1) std::fprintf(stderr, "  logged %d time(s)\n", times);
                }
            }
        }
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
