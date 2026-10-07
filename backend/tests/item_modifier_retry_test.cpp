// SPDX-License-Identifier: GPL-2.0-or-later

// What the plugin does when the daemon says the layouts in force have CHANGED (S6.61, S6.62).
//
// The two properties pull in opposite directions, which is why one file grades both:
//
//   - A consumer whose report was REFUSED must report again when the announcement arrives. A
//     report is refused for sharing no layout with another consumer, and that is not a verdict
//     the caller can change by editing its own list; if the other consumer later leaves the bus
//     and the daemon's set widens, a consumer that ignores the announcement never asks again and
//     runs unnegotiated for the rest of the session.
//   - A consumer whose report was ACCEPTED must NOT report again on it. Every accepted report is
//     itself a change, so a plugin that answers every announcement with a report turns one
//     departure into an unbounded round of bus calls between every consumer on the session --
//     the per-frame retry the latching design exists to prevent, arrived at by another route.
//
// The daemon is a stub on this run's private bus: it answers GetFrame with a dma_buf-typed
// frame, answers SetImportModifiers with an error or with success on a switch this file owns, and
// emits ImportModifiersChanged on demand. Nothing in the plugin knows a test is running.
//
// WHY THE SERVED LAYOUT IS ONE NO DRIVER PUBLISHES, for item_modifier_report_test's reason: a
// failed import also makes the plugin WITHDRAW the layout that failed, and a withdrawal of a
// layout the plugin's record does not list is dropped before it is sent. With the sentinel in
// force for the whole run, every SetImportModifiers that reaches the stub is a re-report and
// nothing else, so counting calls means what it says.
//
// WHY THE REFUSED HALF RUNS FIRST, AND WHY THE ACCEPTED HALF IS REACHED BY SWITCHING THE STUB
// RATHER THAN BY A SECOND PROCESS. The plugin's latch is process-wide, so a run can only be in
// one of the two states -- and a run that only ever accepts cannot tell "the plugin ignored the
// announcement because its report stands" from "the plugin never subscribed at all", which would
// be a green run for the defect this file exists to catch. Driving refused -> accepted inside one
// process makes the second half rest on an observed retry in the first.
//
// THE ORDERING THIS RESTS ON, because a sleep would be the alternative and would grade the host.
// The stub shares this process's session-bus connection, so its replies and its signals travel
// the same connection in the order it sent them, and Qt runs the handlers in that order. Every
// refusal is therefore processed before the first acceptance, and an announcement emitted after
// an acceptance was sent is processed after every refusal. That is what makes "the plugin is now
// in the accepted state, with no refusal outstanding" a state this file can reach on purpose
// instead of waiting for.

#include "kde/frame_import.h"
#include "tests/qt_frame_reply.h"
#include "tests/private_test_bus.h"
#include "tests/test_report.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QUrl>
#include <QVariant>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::FramePlane;
using wallpaperx::testing::frameReplyFields;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

constexpr const char* kService = "io.wallpaperx.Daemon";
constexpr const char* kPath = "/io/wallpaperx/Daemon";
constexpr const char* kInterface = "io.wallpaperx.Daemon";

constexpr int kFrameWidth = 64;
constexpr int kFrameHeight = 64;

// XRGB8888, the format the daemon exports. The driver has to answer eglQueryDmaBufModifiersEXT
// for it or the plugin declines to report and there is nothing here to grade.
constexpr quint32 kXrgb8888 = 'X' | ('R' << 8) | ('2' << 16) | ('4' << 24);

// A well-formed DRM format modifier no driver publishes: 0x0f is an unassigned vendor byte in
// drm_fourcc.h. See the file header for why every frame carries it.
constexpr quint64 kUnpublishedModifier = 0x0f00000000000001ull;

// How many frames the plugin is shown after its report is refused and BEFORE any announcement is
// emitted. This is what attributes the retry below to the announcement rather than to the passage
// of frames: a plugin that retried per frame would already have done so here.
constexpr int kFramesBeforeAnnouncement = 60;

// Frames served per announcement round. A retry costs one frame -- the plugin reports from the
// pass that imports -- so anything well above one separates "did not retry" from "had no chance".
constexpr int kFramesPerRound = 20;

// How many announcements the refused plugin may be shown before its silence is a failure. More
// than one is needed and the reason is the trap this file's predecessor documented: the refusal
// arrives asynchronously, so an announcement emitted before it lands finds nothing to un-latch.
constexpr int kRetryRounds = 40;

// How many announcement rounds the plugin gets to stop re-reporting once the stub accepts. Two is
// the most correct code can need (see the header's ordering argument); this is slack, not a
// target, and exhausting it is itself the S6.62 failure.
constexpr int kSettleRounds = 8;

// Announcements emitted, after the plugin has settled on an ACCEPTED report, that must produce no
// report at all. This is the whole strength of that negative, so it is asserted rather than
// assumed.
constexpr int kQuietRounds = 10;

// A ceiling on frames, not a target. Every round below is bounded by a frame count, so a full run
// needs about 1 220; this leaves headroom and only stops a runaway. Reaching it is reported as a
// harness fault, because a stub with no frames left silently stops grading anything.
constexpr int kFramesServed = 6000;

// The stall guard on a single round: reaching it means neither a report NOR the round's frames
// arrived, which is a fault in its own right and is reported as one -- never treated as "no report
// followed this announcement".
constexpr int kRoundMs = 5000;

// Answers GetFrame with a dma_buf-typed frame, and SetImportModifiers with an error or with
// success. A virtual object rather than an adaptor, for item_import_failure_test's reason: the
// reply's shape is part of what is under test, and an adaptor would put Qt's marshalling of a C++
// signature between this file and the wire.
class StubDaemon : public QDBusVirtualObject {
public:
    QString introspect(const QString&) const override { return QString(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String(kInterface)) return false;

        if (message.member() == QLatin1String("SetImportModifiers")) {
            // Recorded before the answer, so "the plugin reported" and "this stub refused" stay
            // separate verdicts.
            wallpaperx::testing::ModifierReportArgs args = wallpaperx::testing::readModifierReport(message.arguments());
            Report report;
            report.malformed = !args.ok;
            report.drmFormat = args.drmFormat;
            report.modifiers = std::move(args.modifiers);
            report.refused = m_refusing;
            m_reports.push_back(std::move(report));
            if (!m_refusing) return connection.send(message.createReply());
            return connection.send(message.createErrorReply(
                QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                QStringLiteral("this stub refuses import modifier reports while it is in refusing mode")));
        }

        if (message.member() != QLatin1String("GetFrame")) return false;

        if (m_served >= kFramesServed) {
            // Every check here is about what the plugin does across frames, so a run that hits
            // this stops establishing them silently -- the plugin cannot report on a frame it
            // was never given. Recorded, and reported as a harness fault at the end.
            m_capped = true;
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.NoFrame"),
                                                            QStringLiteral("this stub has served its frames")));
        }

        // A real, mappable fd: a bad one is refused by the reply parse long before the import.
        const int fd = ::memfd_create("wallpaperx-stub-frame", 0);
        if (fd < 0) {
            m_fdError = true;
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.ExportFailed"),
                                                            QStringLiteral("memfd_create failed in the stub")));
        }
        const quint64 memorySize = static_cast<quint64>(kFrameWidth) * kFrameHeight * 4;
        if (::ftruncate(fd, static_cast<off_t>(memorySize)) != 0) {
            m_fdError = true;
            ::close(fd);
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.ExportFailed"),
                                                            QStringLiteral("ftruncate failed in the stub")));
        }

        QDBusUnixFileDescriptor handle(fd);
        ::close(fd);  // QDBusUnixFileDescriptor took its own duplicate.

        QList<FramePlane> planes;
        planes.append(FramePlane{0, static_cast<quint64>(kFrameWidth) * 4});

        ++m_served;
        return connection.send(message.createReply(
            frameReplyFields({handle}, kFrameWidth, kFrameHeight, memorySize, /*handleType=*/1,
                             /*generation=*/static_cast<quint64>(m_served), kXrgb8888, kUnpublishedModifier, planes,
                             // Not this device, and it does not have to be: a dma_buf is not
                             // device bound, and this import is expected to fail regardless.
                             QByteArray(16, '\0'), /*bufferId=*/static_cast<quint64>(m_served),
                             /*handover=*/static_cast<quint64>(m_served))));
    }

    struct Report {
        quint32 drmFormat = 0;
        std::vector<quint64> modifiers;
        bool malformed = false;
        bool refused = false;
    };

    int served() const { return m_served; }
    bool fdError() const { return m_fdError; }
    bool capped() const { return m_capped; }
    const std::vector<Report>& reports() const { return m_reports; }

    // The one lever this file stages the two halves with. Moved only after a condition the test
    // has observed, never after a delay.
    void stopRefusing() { m_refusing = false; }

private:
    int m_served = 0;
    bool m_fdError = false;
    bool m_capped = false;
    bool m_refusing = true;
    std::vector<Report> m_reports;
};

// Counts ImportModifiersChanged as the BUS delivers it, subscribed exactly the way the plugin
// subscribes. It exists to keep one failure off the plugin's record: if this process cannot get
// its own announcement back, the plugin was never asked to do anything, and reporting that as a
// plugin defect would be the harness lying about the code under test.
class AnnouncementWitness : public QObject {
    Q_OBJECT
public:
    int count() const { return m_count; }

public slots:
    void onAnnouncement() { ++m_count; }

private:
    int m_count = 0;
};

// Pumps the event loop until `done`, or gives up. Every caller branches on the answer, so a stall
// can never be read as data.
bool pumpUntil(const std::function<bool()>& done, int limitMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > limitMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

QString qmlSource() {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: 64
    height: 64
    property Item rendererItem: renderer

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
    }
}
)");
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_modifier_retry_test";
    // X.34: nothing owned or sent off the private bus.
    if (const std::optional<int> refused = wallpaperx::testing::refuseOffPrivateTestBus()) return *refused;

    wallpaperx::testing::registerFrameReplyTypes();

    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);

    QDBusConnection bus = QDBusConnection::sessionBus();
    StubDaemon stub;
    if (!bus.isConnected() || !bus.registerService(QString::fromUtf8(kService)) ||
        !bus.registerVirtualObject(QString::fromUtf8(kPath), &stub)) {
        notVerified("S6.61 and S6.62", "no session bus this run could own io.wallpaperx.Daemon on: " +
                                           bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    AnnouncementWitness witness;
    if (!bus.connect(QString::fromUtf8(kService), QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                     QStringLiteral("ImportModifiersChanged"), &witness, SLOT(onAnnouncement()))) {
        harnessFailure("this test could not subscribe to its own ImportModifiersChanged signal");
        notVerified("S6.61 and S6.62",
                    "without a subscription of its own this run cannot tell a plugin that ignored the "
                    "announcement from a bus that never delivered it");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    const auto announce = [&bus] {
        return bus.send(QDBusMessage::createSignal(QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                                                   QStringLiteral("ImportModifiersChanged")));
    };

    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.setGeometry(0, 0, kFrameWidth, kFrameHeight);

    // The scene graph makes its own OpenGL context here. Whether it can is a property of the
    // host's Qt platform -- the offscreen one forces the software adaptation, which refuses the
    // RHI outright -- so a failure is a named skip, not a verdict on the plugin.
    if (!control.initialize() || window.rendererInterface() == nullptr ||
        window.rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
        notVerified("S6.61 and S6.62", std::string("no OpenGL scene graph could be brought up under the ") +
                                           QGuiApplication::platformName().toStdString() +
                                           " platform, so the plugin's import path never runs");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // The same call the plugin makes, asked with the scene graph's own context current. A driver
    // that publishes nothing makes the plugin send nothing, by design, and a run that graded that
    // would be grading the host.
    control.beginFrame();
    std::string whyNotImportable;
    const std::vector<uint64_t> importable = wallpaperx::FrameImport::importableModifiers(kXrgb8888, &whyNotImportable);
    control.endFrame();
    if (importable.empty()) {
        notVerified("S6.61 and S6.62",
                    "this host publishes no importable DRM format modifier for the format the daemon exports, so "
                    "the plugin never sends a report and there is no refusal to recover from: " +
                        whyNotImportable);
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    QQmlComponent component(&engine);
    component.setData(qmlSource().toUtf8(), QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                                QStringLiteral("/item_modifier_retry.qml")));
    std::unique_ptr<QObject> root(component.create());
    auto* rootItem = qobject_cast<QQuickItem*>(root.get());
    if (rootItem == nullptr) {
        for (const QQmlError& error : component.errors()) {
            std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
        }
    }
    // The module and the type are the plugin's own, so a load failure here is production's, not
    // this harness's -- red, and every check below is gated on it.
    check(rootItem != nullptr, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");

    if (rootItem != nullptr) {
        rootItem->setParentItem(window.contentItem());

        // sync() is the pass that calls updatePaintNode, which is where the plugin reports and
        // then imports. render() is left out: this window has no render target, and drawing
        // establishes nothing these checks read.
        const auto syncOnce = [&control] {
            control.polishItems();
            control.beginFrame();
            control.sync();
            control.endFrame();
        };
        syncOnce();

        // The plugin reports at all. Not one of this file's goals, but both are meaningless
        // without it: a plugin that never reported would show "no retry" for a second reason.
        const bool reportedOnce = pumpUntil(
            [&] {
                syncOnce();
                return !stub.reports().empty();
            },
            kRoundMs);
        check(reportedOnce, "the plugin reports its importable layouts for a dma_buf frame");
        if (stub.fdError()) {
            harnessFailure("the stub could not allocate the frame fd it answers with");
        }


        // GOAL: the retry graded further down is caused by the ANNOUNCEMENT and not by the
        //       passage of frames. Without this, a plugin that re-reported once per frame would
        //       satisfy the retry check while violating everything the latch exists for.
        // WHY-THIS-SHAPE: frames are served with no announcement emitted at all, and the count of
        //       them is asserted rather than assumed -- "no second report arrived" is trivially
        //       true of a run that served nothing, which is exactly the shape a stalled fetch
        //       loop produces. Reaching the time bound ABANDONS the phase with a printed reason
        //       rather than falling through into the announcement rounds.
        // COVERAGE-ASSERT: kFramesBeforeAnnouncement frames must have been served and imported
        //       before this is graded.
        // CONTROL: `g_modifiersReported.store(false);` added beside `g_reportRefused.store(true)`
        //       in the watcher's error path in sendImportModifiers (backend/kde/
        //       wallpaperx_item.cpp) -- the per-frame retry loop the latch replaced. Observed
        //       red, the only failure in that run: "the stub was sent 61 report(s) across 60
        //       frame(s), none of them announced". Reverted.
        const int framesBeforeAnnouncing = reportedOnce ? stub.served() : 0;
        const bool gaveItChances =
            reportedOnce && pumpUntil(
                                [&] {
                                    syncOnce();
                                    return stub.served() >= framesBeforeAnnouncing + kFramesBeforeAnnouncement;
                                },
                                20000);
        if (reportedOnce && !gaveItChances) {
            check(false, "the plugin keeps fetching frames after its report was refused");
            std::fprintf(stderr,
                         "  only %d frame(s) were served after the refusal; %d were needed before the checks below "
                         "mean anything\n",
                         stub.served() - framesBeforeAnnouncing, kFramesBeforeAnnouncement);
        }

        bool retried = false;
        if (gaveItChances) {
            check(stub.reports().size() == 1,
                  "with no announcement, a refused report is not sent again on the frames that follow");
            if (stub.reports().size() != 1) {
                std::fprintf(stderr, "  the stub was sent %zu report(s) across %d frame(s), none of them announced\n",
                             stub.reports().size(), stub.served() - framesBeforeAnnouncing);
            }

            // GOAL: S6.61 -- a consumer whose report the daemon REFUSED reports again once the
            //       daemon announces that the layouts in force have changed. False leaves it
            //       latched for the session: the refusal was a verdict about ANOTHER consumer's
            //       list, so nothing this plugin does on its own will ever change the answer.
            // WHY-THIS-SHAPE: announcements are emitted repeatedly rather than once, and that is
            //       not slack. The refusal reply is asynchronous, so an announcement emitted
            //       before it lands finds nothing latched and correctly does nothing; a single
            //       emission would go red against correct code. Repetition cannot manufacture a
            //       pass either -- the only other thing that clears the latch is the daemon's bus
            //       name changing owner, and it does not change here.
            // COVERAGE-ASSERT: the announcements are counted by a subscriber of this file's own,
            //       and a run where none of them came back is reported as a harness fault rather
            //       than as the plugin's silence.
            // CONTROL: two, in backend/kde/wallpaperx_item.cpp, each observed red here and the
            //       only failure in its run. A bare `return;` at the top of
            //       WallpaperXItem::onImportModifiersChanged, and separately the
            //       `subscribeToImportModifiersChanged();` call deleted from
            //       WallpaperXItem::connectToDaemon. Both printed "40 announcement(s) were
            //       emitted and 40 came back to this process ... and the plugin sent no second
            //       report", which is what the witness is for: the harness had delivered every
            //       one, so the silence is the plugin's. Both reverted.
            int announcements = 0;
            bool retryStalled = false;
            for (int round = 0; round < kRetryRounds && !retried && !retryStalled; ++round) {
                if (!announce()) {
                    harnessFailure("this test could not send its own ImportModifiersChanged signal");
                    break;
                }
                ++announcements;
                // Bounded in FRAMES, not in wall-clock time, and that is not a style choice: at
                // roughly a millisecond a frame a five-second round burns thousands of them, and
                // a stub that has served its last frame can no longer give the plugin anything to
                // report on -- so a clock-bounded round silently disarms every later round.
                // Measured: with a time bound this loop exhausted the stub's frames in round one.
                const int framesAtRoundStart = stub.served();
                const bool roundCompleted = pumpUntil(
                    [&] {
                        syncOnce();
                        return stub.reports().size() > 1 || stub.served() >= framesAtRoundStart + kFramesPerRound;
                    },
                    kRoundMs);
                retryStalled = !roundCompleted;
                retried = stub.reports().size() > 1;
            }
            if (retryStalled) {
                harnessFailure("the stub stopped serving frames while the retry rounds were running");
            }
            if (!retried && witness.count() == 0) {
                harnessFailure("none of the ImportModifiersChanged signals this test emitted came back to this "
                               "process, so the plugin was never asked to retry");
                notVerified("S6.61 and S6.62",
                            "the announcement could not be delivered on this run's bus, so nothing was established "
                            "about what the plugin does with one");
            } else {
                check(retried, "a consumer whose report was refused reports again when the daemon announces a change");
                if (!retried) {
                    std::fprintf(stderr,
                                 "  %d announcement(s) were emitted and %d came back to this process across %d served "
                                 "frame(s), and the plugin sent no second report\n",
                                 announcements, witness.count(), stub.served());
                }
            }
        }

        if (retried) {
            // From here the stub says yes, so the report the plugin is left holding is an
            // ACCEPTED one -- the state S6.62 is about.
            stub.stopRefusing();

            // GOAL: S6.62 -- a consumer whose report was ACCEPTED does not report again when the
            //       daemon announces a change. False turns every change into a report from every
            //       consumer, and every accepted report is itself a change.
            // WHY-THIS-SHAPE: two stages, and the first is why this is not just "emit and count".
            //       The plugin is not in the accepted state the moment the stub stops refusing:
            //       refusals already in flight still arm the retry, and each one legitimately
            //       earns one more report. So the phase first drives it to quiescence, one
            //       announcement per round, each emitted only after the previous round's report
            //       was answered -- which by this connection's own message ordering is processed
            //       after every refusal, so correct code needs at most two such rounds. Running
            //       out of rounds is NOT a setup failure to skip on: it is the property being
            //       false, and it is graded as one with the counts printed.
            // CONTROL: `if (!g_reportRefused.exchange(false)) return;` in
            //       WallpaperXItem::onImportModifiersChanged (backend/kde/wallpaperx_item.cpp)
            //       replaced by `g_reportRefused.store(false);`, so every announcement un-latches
            //       whether or not this process was refused. Observed red, the only failure in
            //       that run: "8 announcement round(s) after the daemon started accepting
            //       produced a report each; 10 report(s) in total". Reverted.
            int settleRounds = 0;
            bool settled = false;
            bool stalled = false;
            // A quiet round only counts once the round BEFORE it produced a report, and that
            // condition is doing real work rather than being conservative. Refusals still in
            // flight when the stub switched are processed in the order the stub sent them, all
            // of them before its first acceptance -- so a quiet round reached without any
            // acceptance yet can be followed by a refusal landing and arming the retry for real.
            // A quiet round that follows an accepted report cannot: the announcement that opens
            // it is emitted after that acceptance was sent, and therefore processed after every
            // refusal there was.
            bool previousRoundReported = false;
            for (; settleRounds < kSettleRounds && !settled && !stalled; ++settleRounds) {
                if (!announce()) {
                    harnessFailure("this test could not send its own ImportModifiersChanged signal");
                    break;
                }
                const size_t beforeRound = stub.reports().size();
                const int framesAtRoundStart = stub.served();
                const bool roundCompleted = pumpUntil(
                    [&] {
                        syncOnce();
                        return stub.reports().size() > beforeRound ||
                               stub.served() >= framesAtRoundStart + kFramesPerRound;
                    },
                    kRoundMs);
                // Neither outcome reached inside the bound means frames stopped arriving, which
                // is a different fault from "it re-reported" and must not be graded as either.
                stalled = !roundCompleted;
                const bool roundReported = stub.reports().size() > beforeRound;
                settled = roundCompleted && !roundReported && previousRoundReported;
                previousRoundReported = roundReported;
            }
            if (stalled) {
                check(false, "the plugin keeps fetching frames while the daemon announces changes");
                std::fprintf(stderr,
                             "  a round served fewer than %d frame(s) and produced no report inside %d ms, so the "
                             "accepted-report check below was abandoned\n",
                             kFramesPerRound, kRoundMs);
            } else if (!settled) {
                check(false, "an accepted report stops earning a re-report once the refusals have been answered");
                std::fprintf(stderr,
                             "  %d announcement round(s) after the daemon started accepting produced a report each; "
                             "%zu report(s) in total\n",
                             settleRounds, stub.reports().size());
            }

            if (settled) {
                const size_t settledReports = stub.reports().size();
                const int framesAtQuietStart = stub.served();
                int quietAnnouncements = 0;
                for (int round = 0; round < kQuietRounds; ++round) {
                    if (!announce()) {
                        harnessFailure("this test could not send its own ImportModifiersChanged signal");
                        break;
                    }
                    ++quietAnnouncements;
                    const int framesAtRoundStart = stub.served();
                    pumpUntil(
                        [&] {
                            syncOnce();
                            return stub.served() >= framesAtRoundStart + kFramesPerRound;
                        },
                        kRoundMs);
                }
                const int quietFrames = stub.served() - framesAtQuietStart;

                // CONTROL for the negative below, and it is deliberately artificial because every
                // realistic mutation reddens the settle loop first and never reaches this line:
                // `static int nth = 0; if (++nth % 2 == 1) return;` before the flag test in
                // WallpaperXItem::onImportModifiersChanged (backend/kde/wallpaperx_item.cpp), so
                // the plugin un-latches on every second announcement. That settles and then
                // re-reports. Observed red, the only failure in that run: "5 further report(s)
                // reached the stub across 10 announcement(s) and 200 frame(s)". Reverted.
                //
                // COVERAGE-ASSERT: the announcements emitted and the frames served are both
                //       counted and asserted before the negative below is allowed to mean
                //       anything. A negative check whose opportunities were never served is
                //       satisfied by a dead fetch loop, and that is the one failure this file
                //       would otherwise be blind to.
                const bool hadChances =
                    quietAnnouncements == kQuietRounds && quietFrames >= kQuietRounds * kFramesPerRound;
                if (!hadChances) {
                    check(false, "the plugin was shown every announcement the accepted-report check grades on");
                    std::fprintf(stderr,
                                 "  %d of %d announcement(s) were emitted across %d frame(s); %d were needed, so the "
                                 "silence that follows establishes nothing\n",
                                 quietAnnouncements, kQuietRounds, quietFrames, kQuietRounds * kFramesPerRound);
                } else {
                    check(stub.reports().size() == settledReports,
                          "a consumer whose report was accepted does not report again when a change is announced");
                    if (stub.reports().size() != settledReports) {
                        std::fprintf(stderr,
                                     "  %zu further report(s) reached the stub across %d announcement(s) and %d "
                                     "frame(s), on a report the daemon had accepted\n",
                                     stub.reports().size() - settledReports, quietAnnouncements, quietFrames);
                    }
                }

                // Not a check: the settle loop cannot reach this line unless the round before the
                // quiet one produced a report while the stub was accepting, so a refused last
                // report here would mean the stub's own mode lever failed. Reported as a harness
                // fault so it cannot be read as a verdict on the plugin.
                if (!stub.reports().empty() && stub.reports().back().refused) {
                    harnessFailure("the stub was still refusing when the accepted-report check ran");
                }
            }
        }
    }

    // The node owns any import and freeing it needs the context, but destroying the item only
    // QUEUES the node -- and QQuickRenderControl makes the context current only between
    // beginFrame() and endFrame().
    root.reset();
    control.beginFrame();
    control.sync();
    control.endFrame();
    control.invalidate();

    // A run that ran out of frames stopped grading everything after that point, and it would look
    // identical to a clean one. Attributed here rather than as a check: it is this file's ceiling
    // that was reached, not anything the plugin did.
    if (stub.capped()) {
        harnessFailure("the stub served its whole frame ceiling, so the checks after that point had nothing to "
                       "show the plugin");
    }

    const bool checksRed = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
    if (checksRed || harnessRed) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

#include "item_modifier_retry_test.moc"
