// SPDX-License-Identifier: GPL-2.0-or-later

// What the plugin does with a REFUSED import-modifier report (S6.57), and with a refused
// withdrawal (S6.55). Two properties that pull in opposite directions, which is why they are
// graded in one place:
//
//   - A refused report is not sent again. Every refusal the daemon can give is a verdict on the
//     list's CONTENTS -- a pixel format it does not export, a length over its cap, no layout in
//     common with another consumer -- so the same list earns the same answer. A plugin that
//     retried would do it once per frame for the life of the session.
//   - A refusal must not dead-end the plugin either. The record keeps the list that WAS sent, so
//     an import that actually fails can still narrow it and send a DIFFERENT list -- a question
//     the daemon has not already answered. Emptying the record on refusal would leave nothing to
//     narrow, and the negotiation would be over.
//
// The refusal is manufactured entirely on the DAEMON side of the wire: a stub service on this
// run's private bus answers SetImportModifiers with an error reply. Nothing in the plugin knows a
// test is running, and no production seam exists for this.
//
// WHY THE STUB SERVES A DMA_BUF FRAME, AND WHY ITS LAYOUT STARTS AS A SENTINEL. The plugin reports
// only for a dma_buf frame -- an opaque fd has no layout to negotiate -- so an opaque reply never
// reaches the code under test. The layout served is at first one no driver publishes, which closes
// the only other route to a SetImportModifiers call: a failed import also makes the plugin
// WITHDRAW the layout that failed, and a withdrawal of a layout the plugin never listed is dropped
// before it is sent. So while the sentinel is in force, any call at all is a re-report -- which is
// what makes "exactly one call" mean the retry loop and nothing else. Switching to a layout the
// driver does publish is then what opens the withdrawal path for the second property.
//
// Rendering goes through QQuickRenderControl rather than a shown window, so the pass that imports
// runs on this thread at a point this file picks. A host with no OpenGL scene graph, or whose
// driver publishes no importable layout for the daemon's format, skips with the reason named:
// the plugin sends nothing at all in that case, by design, and a run that graded it would be
// grading the host.

#include "kde/frame_import.h"
#include "tests/qt_frame_reply.h"
#include "tests/test_report.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QGuiApplication>
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
using wallpaperx::testing::notVerified;

constexpr const char* kService = "io.wallpaperx.Daemon";
constexpr const char* kPath = "/io/wallpaperx/Daemon";
constexpr const char* kInterface = "io.wallpaperx.Daemon";

constexpr int kFrameWidth = 64;
constexpr int kFrameHeight = 64;

// XRGB8888, the format the daemon exports. It has to be a format the driver answers
// eglQueryDmaBufModifiersEXT for, or the plugin declines to report and there is nothing to grade.
constexpr quint32 kXrgb8888 = 'X' | ('R' << 8) | ('2' << 16) | ('4' << 24);

// A well-formed DRM format modifier no driver publishes: 0x0f is an unassigned vendor byte in
// drm_fourcc.h. Served as the frame's layout so that the failed import's withdrawal finds nothing
// to withdraw -- see the file header for why that matters.
constexpr quint64 kUnpublishedModifier = 0x0f00000000000001ull;

// A ceiling on how many frames the stub will serve, not a target. It has to be well above two,
// and this is the trap the first draft of this file fell into: the daemon's refusal arrives
// asynchronously, so the frame immediately after the refused report is usually fetched and
// imported BEFORE the error reply has been delivered, and the plugin is still latched when it
// runs. With the stub stopping at two frames the check went red against correct code. Each failed
// import costs one frame, so the retry that matters is simply a later one; the cap only stops a
// runaway if something else goes wrong, and the loop that drives this is time-bounded anyway.
constexpr int kFramesServed = 200;

// How many frames the plugin must be shown, after its report has been refused, before "it did not
// report again" is worth anything. A plugin that retries does it once per frame, so any figure
// well above one separates the two behaviours; this is asserted rather than assumed, because the
// check it guards is a negative and a run that served nothing would satisfy it vacuously.
constexpr int kFramesAfterRefusal = 60;

// Answers GetFrameFd with a dma_buf-typed frame, and SetImportModifiers with an error.
//
// A virtual object rather than an adaptor, for item_import_failure_test's reason: the reply's
// shape is part of what is under test, and an adaptor would put Qt's marshalling of a C++
// signature between this file and the wire.
class StubDaemon : public QDBusVirtualObject {
public:
    QString introspect(const QString&) const override { return QString(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String(kInterface)) return false;

        if (message.member() == QLatin1String("SetImportModifiers")) {
            // Recorded before the answer, so "the plugin reported" and "this stub refused" stay
            // separate verdicts.
            const QList<QVariant> args = message.arguments();
            Report report;
            if (args.size() >= 2) {
                report.drmFormat = args.at(0).toUInt();
                const QDBusArgument list = args.at(1).value<QDBusArgument>();
                list.beginArray();
                while (!list.atEnd()) {
                    quint64 modifier = 0;
                    list >> modifier;
                    report.modifiers.push_back(modifier);
                }
                list.endArray();
            } else {
                report.malformed = true;
            }
            m_reports.push_back(std::move(report));
            return connection.send(message.createErrorReply(
                QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                QStringLiteral("this stub refuses every import modifier report")));
        }

        if (message.member() != QLatin1String("GetFrameFd")) return false;

        ++m_requests;
        if (m_served >= kFramesServed) {
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
            frameReplyFields(handle, kFrameWidth, kFrameHeight, memorySize, /*handleType=*/1,
                             /*generation=*/static_cast<quint64>(m_served), kXrgb8888, m_servedModifier, planes,
                             // Not this device, and it does not have to be: a dma_buf is not
                             // device bound, and this import is expected to fail regardless.
                             QByteArray(16, '\0'), /*bufferId=*/static_cast<quint64>(m_served))));
    }

    struct Report {
        quint32 drmFormat = 0;
        std::vector<quint64> modifiers;
        bool malformed = false;
    };

    int requests() const { return m_requests; }
    int served() const { return m_served; }
    bool fdError() const { return m_fdError; }
    const std::vector<Report>& reports() const { return m_reports; }

    // The layout served with every frame from now on. This is the one lever the two halves of this
    // file are staged with, and it is moved only after a condition the test has observed, never
    // after a delay: while it names a layout no driver publishes, a failed import produces no
    // withdrawal, so any SetImportModifiers that arrives is a re-report and nothing else.
    void serveModifier(quint64 modifier) { m_servedModifier = modifier; }

private:
    int m_requests = 0;
    int m_served = 0;
    bool m_fdError = false;
    quint64 m_servedModifier = kUnpublishedModifier;
    std::vector<Report> m_reports;
};

// Pumps the event loop until `done`, or gives up. Every caller branches on the answer, so a
// stall can never be read as data.
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
    property string publishedReason: renderer.unavailableReason

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
    wallpaperx::testing::g_testName = "item_modifier_report_test";

    wallpaperx::testing::registerFrameReplyTypes();

    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);

    QDBusConnection bus = QDBusConnection::sessionBus();
    StubDaemon stub;
    if (!bus.isConnected() || !bus.registerService(QString::fromUtf8(kService)) ||
        !bus.registerVirtualObject(QString::fromUtf8(kPath), &stub)) {
        notVerified("S6.55 and S6.57","no session bus this run could own io.wallpaperx.Daemon on: " +
                                 bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.setGeometry(0, 0, kFrameWidth, kFrameHeight);

    // The scene graph makes its own OpenGL context here. Whether it can is a property of the
    // host's Qt platform -- the offscreen one forces the software adaptation, which refuses the
    // RHI outright -- so a failure is a named skip, not a verdict on the plugin.
    if (!control.initialize() || window.rendererInterface() == nullptr ||
        window.rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
        notVerified("S6.55 and S6.57",std::string("no OpenGL scene graph could be brought up under the ") +
                                 QGuiApplication::platformName().toStdString() +
                                 " platform, so the plugin's import path never runs");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // Asked with the scene graph's own context current -- eglQueryDmaBufModifiersEXT answers for
    // the current display and nothing else. This is the same call the plugin makes, so it is both
    // the gate (a driver that publishes nothing makes the plugin send nothing, by design, and a
    // run that graded that would be grading the host) and the expected value the reports below are
    // compared against.
    control.beginFrame();
    std::string whyNotImportable;
    const std::vector<uint64_t> expected = wallpaperx::FrameImport::importableModifiers(kXrgb8888, &whyNotImportable);
    control.endFrame();
    if (expected.empty()) {
        notVerified("S6.55 and S6.57","this host publishes no importable DRM format modifier for the format the daemon "
                             "exports, so the plugin never sends a report and there is no refusal to recover from: " +
                                 whyNotImportable);
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // Printed because it is what the checks below are compared against, and it varies by driver:
    // a run whose reports disagree with it is unreadable without knowing what "it" was.
    std::printf("item_modifier_report_test: this host's EGL publishes %zu importable layout(s) for XRGB8888\n",
                expected.size());
    std::fflush(stdout);

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    QQmlComponent component(&engine);
    component.setData(qmlSource().toUtf8(), QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                                QStringLiteral("/item_modifier_report.qml")));
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

        // GOAL: the plugin reports at all. Not S6.55, but S6.55 is meaningless without it -- a
        //       plugin that never reported would show zero calls for a second reason.
        // WHY-THIS-SHAPE: bounded, with an explicit failure branch, and everything below gated on
        //       it. Falling through on the timeout would let a plugin that never reports be graded
        //       as one that reported once and gave up, which is the defect under test.
        // CONTROL: the `if (incoming->request.handleType == FrameImport::HandleType::DmaBuf)`
        //       guard around the reportImportModifiersOnce call in
        //       WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) inverted to
        //       `!=`. Observed red, the only failure in that run. Reverted.
        const bool reportedOnce = pumpUntil(
            [&] {
                syncOnce();
                return !stub.reports().empty();
            },
            5000);
        check(reportedOnce, "the plugin reports its importable layouts for a dma_buf frame");
        if (stub.fdError()) {
            wallpaperx::testing::harnessFailure("the stub could not allocate the frame fd it answers with");
        }

        if (reportedOnce) {
            const std::vector<quint64> driverList(expected.begin(), expected.end());
            // Graded on the FIRST call alone, and here rather than at the end: from the moment the
            // served layout changes below, later calls are withdrawals and carry a shorter list on
            // purpose. A loop over every call would have to be loosened to accommodate them, which
            // is how a check stops being able to fail.
            const StubDaemon::Report& firstCall = stub.reports().front();
            check(!firstCall.malformed && firstCall.drmFormat == kXrgb8888 && firstCall.modifiers == driverList,
                  "the report carries the daemon's format and this driver's whole importable list");
            if (firstCall.modifiers != driverList) {
                std::fprintf(stderr, "  the report carried format %u and %zu layout(s); this host's EGL publishes "
                                     "%zu for format %u\n",
                             firstCall.drmFormat, firstCall.modifiers.size(), expected.size(), kXrgb8888);
            }

            // GOAL: S6.57's first half -- a refused report is not sent again. Every refusal is a
            //       verdict on the list's contents, so re-sending the same list gets the same
            //       answer; a plugin that retries does it once per frame for the life of the
            //       session.
            // WHY-THIS-SHAPE: a negative, so its whole strength is in how many chances it gave the
            //       plugin to violate it -- hence the frame count is asserted rather than assumed.
            //       Reaching the time bound without those frames ABANDONS the check with a printed
            //       reason instead of falling through: "no second report arrived" is trivially
            //       true of a run where nothing was served, and that is precisely the shape a
            //       stalled fetch loop would produce. The layout served throughout is one no
            //       driver publishes, so a failed import produces no withdrawal and any second
            //       call would be a re-report and nothing else.
            // COVERAGE-ASSERT: kFramesAfterRefusal frames are asserted to have been served and
            //       imported after the refusal, so an edit that stops the fetch loop fails here
            //       rather than making this check pass on an empty run.
            // CONTROL: `g_modifiersReported.store(false);` restored to the watcher's error path in
            //       sendImportModifiers (backend/kde/wallpaperx_item.cpp) -- the per-frame retry
            //       loop. Observed red: "the stub was sent 61 report(s) across 60 frame(s)", one
            //       per frame. It also reddens the withdrawal-contents check below, because with
            //       retries reinstated the calls after the first are no longer all withdrawals;
            //       2 failures in that run. Reverted.
            const int framesBefore = stub.served();
            const bool gaveItChances = pumpUntil(
                [&] {
                    syncOnce();
                    return stub.served() >= framesBefore + kFramesAfterRefusal;
                },
                20000);
            if (!gaveItChances) {
                check(false, "the plugin keeps fetching frames after its report was refused");
                std::fprintf(stderr,
                             "  only %d frame(s) were served after the refusal; %d were needed before 'it did not "
                             "report again' means anything\n",
                             stub.served() - framesBefore, kFramesAfterRefusal);
            } else {
                check(stub.reports().size() == 1, "a refused report is not sent again on the frames that follow");
                if (stub.reports().size() != 1) {
                    std::fprintf(stderr, "  the stub was sent %zu report(s) across %d frame(s); a refusal is a verdict "
                                         "on the list, so one is all this list can earn\n",
                                 stub.reports().size(), stub.served() - framesBefore);
                }
            }

            // S6.57's second half needs a WITHDRAWAL, which the plugin declines to send when its
            // record holds a single layout -- there would be nothing left to report. A driver
            // publishing one layout therefore cannot reach it, and neither can it reach S6.55.
            if (expected.size() < 2) {
                notVerified("S6.57's second half and S6.55",
                            "this host's EGL publishes one importable layout, and the plugin declines to withdraw "
                            "its last one -- so no withdrawal can be produced here, and a withdrawal is the only "
                            "thing that reveals what record the plugin kept");
            } else {
                // GOAL: S6.57's second half -- a refused report does not dead-end the plugin. The
                //       record keeps the list that was sent, so an import that actually fails can
                //       still narrow it and ask the daemon a question it has not answered.
                // WHY-THIS-SHAPE: the served layout changes to one the driver DOES publish, which
                //       is what makes a withdrawal possible at all -- the plugin drops a withdrawal
                //       for a layout its record does not list. Nothing else changes, and the switch
                //       happens after the frames above, so the refusal is long since delivered and
                //       "the record was emptied" cannot pass as "the reply has not landed yet".
                //       Asserting that the plugin still holds SOMETHING is not available directly:
                //       the record is a file-local static with no accessor, and X.6 forbids adding
                //       one.
                // CONTROL: the second argument at the sendImportModifiers call in
                //       WallpaperXItem::reportImportModifiersOnce (backend/kde/wallpaperx_item.cpp)
                //       changed from `modifiers` to `{}`, so a refusal empties the record.
                //       Observed red, the only failure in that run: "no further call reached the
                //       stub across 200 served frame(s); the plugin has nothing left to withdraw
                //       from". Reverted.
                const quint64 servedLayout = expected.front();
                stub.serveModifier(servedLayout);
                const size_t beforeWithdrawals = stub.reports().size();
                const bool narrowed = pumpUntil(
                    [&] {
                        syncOnce();
                        return stub.reports().size() > beforeWithdrawals;
                    },
                    5000);
                check(narrowed, "a refused report still leaves the plugin able to narrow what it reported");
                if (!narrowed) {
                    std::fprintf(stderr,
                                 "  no further call reached the stub across %d served frame(s); the plugin has "
                                 "nothing left to withdraw from\n",
                                 stub.served());
                }

                if (narrowed) {
                    // GOAL: S6.55's first clause -- when the daemon refuses a WITHDRAWAL, the
                    //       plugin puts its own record back to what the daemon still holds, so the
                    //       next withdrawal narrows that record rather than one already narrowed.
                    // WHY-THIS-SHAPE: every frame from here carries the SAME layout, and that is
                    //       what makes the observable binary instead of a size comparison. The
                    //       plugin drops a withdrawal for a layout its record no longer lists, so
                    //       a record left narrowed sends exactly ONE withdrawal ever, while a
                    //       record put back sends one per frame. Serving DIFFERENT layouts instead
                    //       -- the obvious form -- would compare list lengths, and those cannot be
                    //       asserted here: the refusal is delivered asynchronously, so a
                    //       withdrawal issued before the previous refusal was processed legitimately
                    //       starts from the narrowed record. Under the same-layout form that case
                    //       sends nothing at all rather than something shorter, so it costs a
                    //       count and never a wrong verdict.
                    // COVERAGE-ASSERT: every withdrawal's list is compared against the driver list
                    //       minus the served layout, so a plugin that restores to the WRONG value
                    //       fails here rather than passing on the count alone.
                    // CONTROL: `if (g_reportedModifiers == modifiers) g_reportedModifiers =
                    //       restoreOnFailure;` deleted from the watcher's finished handler in
                    //       sendImportModifiers (backend/kde/wallpaperx_item.cpp). Observed red,
                    //       the only failure in that run: "1 withdrawal(s) reached the stub across
                    //       200 served frame(s)". The list check below has a control of its own --
                    //       a second `g_reportedModifiers.pop_back()` after the erase in
                    //       WallpaperXItem::withdrawImportModifier -- observed red alone,
                    //       reporting 10 layouts where 11 were expected. Both reverted.
                    const size_t firstWithdrawal = beforeWithdrawals;
                    const bool withdrewTwice = pumpUntil(
                        [&] {
                            syncOnce();
                            return stub.reports().size() >= firstWithdrawal + 2;
                        },
                        5000);
                    check(withdrewTwice,
                          "a refused withdrawal leaves the record able to withdraw the same layout again");
                    if (!withdrewTwice) {
                        std::fprintf(stderr,
                                     "  %zu withdrawal(s) reached the stub across %d served frame(s); a plugin that "
                                     "kept its narrowed record can only send one\n",
                                     stub.reports().size() - firstWithdrawal, stub.served());
                    }

                    std::vector<quint64> withoutServed;
                    for (uint64_t modifier : expected) {
                        if (modifier != servedLayout) withoutServed.push_back(modifier);
                    }
                    bool everyWithdrawalIsCorrect = true;
                    for (size_t i = firstWithdrawal; i < stub.reports().size(); ++i) {
                        if (stub.reports().at(i).modifiers != withoutServed) {
                            everyWithdrawalIsCorrect = false;
                            std::fprintf(stderr, "  withdrawal %zu carried %zu layout(s); the driver list minus the "
                                                 "one that failed to import has %zu\n",
                                         i - firstWithdrawal, stub.reports().at(i).modifiers.size(),
                                         withoutServed.size());
                        }
                    }
                    check(everyWithdrawalIsCorrect,
                          "every withdrawal drops only the layout that failed, from the whole reported list");
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

    const bool checksRed = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
    if (checksRed || harnessRed) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
