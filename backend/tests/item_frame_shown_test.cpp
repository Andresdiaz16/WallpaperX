// SPDX-License-Identifier: GPL-2.0-or-later

// S12.7: what the plugin TELLS the desktop about whether a wallpaper is on screen must match what
// is actually on screen -- in both directions, because they are opposite failures.
//
//   * When the scene graph takes the item's resources away, the node and its texture go with them
//     and nothing is drawn any more. If the plugin keeps claiming a frame is shown, main.qml keeps
//     the message panel hidden and the user gets a black desktop that explains nothing. That is
//     the defect that shipped.
//   * When the item's GENERATION MARKS are invalidated -- a daemon restart, or this screen being
//     given a different monitor id -- the node keeps drawing the frame it already has. If the
//     plugin drops its claim there, an opaque grey panel covers a wallpaper that is perfectly
//     visible. That is the defect a fix for the first one introduced.
//
// The claim is the item's `frameShown` property; main.qml mirrors it into `rendererDrawing` and
// hides the backdrop only while it is true. This file grades the C++ end, where the node actually
// dies, on a REAL imported frame -- a stand-in renderer cannot make Qt call releaseResources().
// The QML end, where the panel comes back, is graded in tests/qml/tst_wallpaper_states.qml by
// test_a_renderer_that_stops_drawing_brings_the_message_back; neither half establishes the other,
// so both exist.
//
// Why a separate binary from item_import_release_test, which stands up the same scene: grading the
// first direction means detaching the item from its window, which destroys the very node and
// texture that file's whole S12.4 phase is about. Sharing one scene would make each goal's
// mutation reachable only in the other's teardown.
//
// This needs a frame the importer ACCEPTS, so the stub exports a real Vulkan image, and the run
// reports NOT VERIFIED where this host cannot pair a GL context with the Vulkan device that
// produced it: an opaque fd is importable only by the driver that made it.

#include "kde/frame_import.h"
#include "support/validation_grade.h"
// For vulkanDeviceUuid only; this test brings up its own GL through QQuickRenderControl.
#include "tests/gl_import_harness.h"
#include "tests/item_stub_scene.h"
#include "tests/test_report.h"
#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QUrl>
#include <QVariant>

// The only way to ask whether the item still has a paint node, which is the "something is drawn"
// half of every check here. Qt's private header, not a seam added to this project: WallpaperXItem
// exposes nothing about its node and must not start to. If this include ever stops resolving, the
// answer is to drop the test with its coverage stated, never to publish the node from the plugin.
#include <QtQuick/private/qquickitem_p.h>
#include <QtQuick/qsgtexture_platform.h>

#include <EGL/egl.h>
#include <GL/gl.h>

#include <unistd.h>

#include <array>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <string>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::claimsFrameShown;
using wallpaperx::testing::ExportingStub;
using wallpaperx::testing::glDeviceUuid;
using wallpaperx::testing::itemPaintNode;
using wallpaperx::testing::itemTexture;
using wallpaperx::testing::kFirstMonitor;
using wallpaperx::testing::kHeight;
using wallpaperx::testing::kPath;
using wallpaperx::testing::kSecondMonitor;
using wallpaperx::testing::kService;
using wallpaperx::testing::kWidth;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::qmlSource;
using wallpaperx::testing::vulkanDeviceUuid;


constexpr const char* kGoal = "S12.7 (the plugin's claim that a frame is on screen follows what is actually drawn: "
                              "withdrawn when the scene graph takes the node away, kept while the node still holds "
                              "a frame)";

// Nonzero when the validation layer was absent. Its own counter, because charging that to
// failures turns every deliberate skip below into exit 1: a host without the layer package would
// report "not verified" in its output and FAIL to ctest, blaming the plugin for a package.
int g_ungradedValidation = 0;

bool runFrameShownChecks() {
    return wallpaperx::testing::gradedRun("item_frame_shown_test", [](wallpaperx::testing::ValidationGrade& grade) {
        grade.reportUngradedTo(g_ungradedValidation);
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);
        // Created but never rendered into. Nothing here reads a pixel: what matters is that the
        // export is a REAL one, so the item takes the success path and a node with a live texture
        // ends up on screen. frame_import_probe is what grades that the contents survive the trip.
        const wallpaperx::OffscreenTarget& target = targets.create(kFirstMonitor, kWidth, kHeight);

        QDBusConnection bus = QDBusConnection::sessionBus();
        ExportingStub stub(device, target);
        if (!bus.isConnected() || !bus.registerService(QString::fromUtf8(kService)) ||
            !bus.registerVirtualObject(QString::fromUtf8(kPath), &stub)) {
            notVerified(kGoal, "no session bus this run could own io.wallpaperx.Daemon on: " +
                                   bus.lastError().message().toStdString());
            return true;
        }

        QQuickRenderControl control;
        QQuickWindow window(&control);
        window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));

        if (!control.initialize() || window.rendererInterface() == nullptr ||
            window.rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
            notVerified(kGoal, std::string("no OpenGL scene graph could be brought up under the ") +
                                   QGuiApplication::platformName().toStdString() +
                                   " platform, so no frame could be put on screen");
            return true;
        }

        // One synchronisation pass, with the GL context dropped again at the end of it, for the
        // reason item_import_release_test measures: QQuickRenderControl leaves the context current
        // on the thread that called endFrame(), which no real compositor's GUI thread ever has.
        const auto renderPass = [&control](const std::function<void()>& withContext) {
            control.polishItems();
            control.beginFrame();
            control.sync();
            if (withContext) withContext();
            control.endFrame();
            if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();
        };

        // An opaque fd is importable only by the driver that produced it, so a GL context on a
        // different device says nothing at all here -- the item would never reach the state both
        // directions start from. Asked, not assumed: a headless EGL context on this machine comes
        // up on llvmpipe even where Vulkan resolves to NVIDIA.
        bool sameDevice = false;
        bool uuidQueryable = false;
        std::array<uint8_t, 16> glUuid{};
        std::string glRenderer = "(unknown)";
        renderPass([&] {
            const GLubyte* name = glGetString(GL_RENDERER);
            if (name != nullptr) glRenderer = reinterpret_cast<const char*>(name);
            uuidQueryable = glDeviceUuid(glUuid);
            sameDevice = uuidQueryable && glUuid == vulkanDeviceUuid(device.physicalDevice());
        });
        // Only the opaque path needs the match: a dma_buf is importable across devices, so
        // refusing to run for a UUID mismatch would skip every AMD and Intel host with a reason
        // untrue of them. Asked of a real export rather than of the stub, which does not know its
        // handle type until it has served a frame -- and that is after this gate.
        bool needsSameDevice = true;
        {
            wallpaperx::ExportedFrame probe = wallpaperx::exportFrame(device, target);
            needsSameDevice = probe.type != wallpaperx::ExportedFrame::HandleType::DmaBuf;
            if (probe.fd >= 0) ::close(probe.fd);
        }

        // Two causes, named apart: a context on another GPU, and a context that cannot answer
        // which GPU it is on. Both mean this run establishes nothing, but reporting the second as
        // the first sends the reader hunting for a device mismatch that may not exist.
        if (needsSameDevice && !uuidQueryable) {
            notVerified(kGoal, "this GL context does not answer GL_DEVICE_UUID_EXT (GL reports " + glRenderer +
                                   "), so whether it is on the exporting device could not be established");
            control.invalidate();
            return true;
        }
        if (needsSameDevice && !sameDevice) {
            notVerified(kGoal, "the scene graph's GL context is not on the device the Vulkan export came from (GL "
                               "reports " + glRenderer + "), so the frame is not importable here at all");
            control.invalidate();
            return true;
        }

        QQmlEngine engine;
        engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
        QQmlComponent component(&engine);
        component.setData(qmlSource().toUtf8(), QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                                     QStringLiteral("/item_frame_shown.qml")));
        std::unique_ptr<QObject> root(component.create());
        auto* rootItem = qobject_cast<QQuickItem*>(root.get());
        if (rootItem == nullptr) {
            for (const QQmlError& error : component.errors()) {
                std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
            }
            // The module and the type are the plugin's own, so this is production's failure.
            check(false, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");
            control.invalidate();
            return true;
        }
        rootItem->setParentItem(window.contentItem());
        auto* item = qobject_cast<QQuickItem*>(root->property("rendererItem").value<QObject*>());
        if (item == nullptr) {
            // Checked, or a QML wiring fault spins the pump for its full deadline and then reports
            // "the item never imported the frame" -- blaming the import path for a property this
            // file failed to resolve.
            wallpaperx::testing::harnessFailure(
                "the root object exposed no rendererItem, so there was no WallpaperXRenderer to drive");
            control.invalidate();
            return true;
        }

        // Bounded, with a failure branch: everything below is about an item that HAS a frame on
        // screen, so a run that never imported one must not fall through into it.
        const bool imported = pumpUntil(
            [&] {
                renderPass(nullptr);
                return itemTexture(item) != nullptr;
            },
            10000);
        if (!imported) {
            const std::string detail = std::string("it asked ") + std::to_string(stub.requests()) +
                                       " time(s), the stub served " + std::to_string(stub.served()) +
                                       ", handle type " + std::to_string(stub.handleType()) + ", export error [" +
                                       stub.exportError() + "], the item reports [" +
                                       root->property("publishedReason").toString().toStdString() + "]";
            std::fprintf(stderr, "  %s\n", detail.c_str());

            // One outcome, two causes needing opposite verdicts: a host whose GL context cannot
            // import what was served (a real skip), and a plugin that never asked or refused a
            // frame this host CAN import (a failure).
            if (stub.requests() == 0) {
                check(false, "the item asked the daemon for a frame");
            } else if (stub.served() == 0 && !stub.exportError().empty()) {
                notVerified(kGoal, "the stub could not export a frame on this host, so there was nothing to import");
            } else {
                std::string whyNot;
                bool supported = false;
                // Queried inside a render pass: with no context current the extension strings come
                // back empty and every failure would read as a host limit again.
                renderPass([&] {
                    supported = wallpaperx::FrameImport::supports(
                        stub.handleType() == 1 ? wallpaperx::FrameImport::HandleType::DmaBuf
                                               : wallpaperx::FrameImport::HandleType::OpaqueFd,
                        &whyNot);
                });
                if (supported) {
                    check(false, "the item imported the frame the daemon served");
                } else {
                    notVerified(kGoal, whyNot);
                }
            }
            root.reset();
            renderPass(nullptr);
            control.invalidate();
            return true;
        }

        QSGTexture* const texture = itemTexture(item);
        std::printf("item_frame_shown_test: imported on %s, handle type %s\n", glRenderer.c_str(),
                    stub.handleType() == 1 ? "dma_buf" : "opaque fd");
        std::fflush(stdout);

        // GOAL: a frame that has been imported and handed to the scene graph is CLAIMED as shown.
        //       False if the claim never arrives: main.qml then keeps the opaque message panel
        //       over a wallpaper that is being drawn, for the rest of the session.
        // WHY-THIS-SHAPE: waited for rather than read once, because the claim is published on the
        //       GUI thread by a queued hop out of the render thread that imported -- reading it on
        //       the line after the import is a race that would fail on a fast machine. The wait has
        //       a failure branch and everything below is gated on it: a run whose item never
        //       claimed a frame cannot grade "the claim is withdrawn", since it was never made.
        // CONTROL: `m_nodeHasFrame = true;` deleted from the success branch of
        //       WallpaperXItem::updatePaintNode in backend/kde/wallpaperx_item.cpp -> "FAIL: the
        //       plugin claims a frame is on screen once one has been imported and drawn", that
        //       check alone, and the run abandoned both directions below with its reason printed
        //       rather than passing them. Observed 2026-08-23. Reverted.
        const bool claimed = pumpUntil([&] { return claimsFrameShown(item); }, 5000);
        check(claimed, "the plugin claims a frame is on screen once one has been imported and drawn");
        if (!claimed) {
            std::fprintf(stderr, "  the item has a node with a live texture and still reports frameShown=false, so "
                                 "neither direction below could be graded\n");
            root.reset();
            renderPass(nullptr);
            control.invalidate();
            return true;
        }

        // GOAL: invalidating the generation marks must NOT withdraw the claim. Moving this screen
        //       to another monitor id calls the plugin's forgetGenerations(), which drops the
        //       staleness marks -- but the node keeps drawing the frame it already has. A claim
        //       withdrawn here puts an opaque grey panel over a visible wallpaper.
        // WHY-THIS-SHAPE: read SYNCHRONOUSLY, on the line after the id changes and before any
        //       event is pumped. The mark-invalidating path publishes the claim itself, so a check
        //       that pumps first would be answered by whatever published last -- the refused fetch
        //       the id change also triggers -- and grade the wrong write. The node is asserted to
        //       still hold its texture in the same breath: without that, "the claim stayed true"
        //       could be true of a run where nothing was on screen to claim.
        // CONTROL: `m_nodeHasFrame = false;` added inside the m_importedMutex block of
        //       WallpaperXItem::forgetGenerations() in backend/kde/wallpaperx_item.cpp -- the shape
        //       that put the panel over a good wallpaper -> "FAIL: a monitor-id change does not
        //       withdraw the claim that a frame is on screen", with the two later checks that read
        //       the same claim red beside it and the release direction abandoned rather than
        //       passed. Observed 2026-08-23. Reverted.
        // COVERAGE-ASSERT: the id change is asserted to have been acted on -- the item re-asks the
        //       daemon for the new screen, which only happens past the early return that would
        //       otherwise make this whole phase a no-op graded as a pass. CONTROL: the trailing
        //       `if (m_monitorId >= 0) fetchFrame(0);` deleted from WallpaperXItem::setMonitorId in
        //       backend/kde/wallpaperx_item.cpp -> "FAIL: the item asks the daemon again for the
        //       screen it was moved to", that check alone. Observed 2026-08-23. Reverted.
        const int requestsBeforeIdChange = stub.requests();
        check(item->property("monitorId").toInt() == kFirstMonitor, "the item starts on the monitor id the QML set");
        item->setProperty("monitorId", kSecondMonitor);
        const bool claimAfterIdChange = claimsFrameShown(item);
        const bool nodeAfterIdChange = itemTexture(item) == texture;
        check(nodeAfterIdChange, "the node keeps the frame it was already drawing across a monitor-id change");
        check(claimAfterIdChange, "a monitor-id change does not withdraw the claim that a frame is on screen");
        check(item->property("monitorId").toInt() == kSecondMonitor, "the monitor-id change reached the item");

        // The claim must also survive the fetch the id change triggers and the next render pass:
        // the first publishes for its own reason, the second is where a node would be torn down.
        const bool reFetched = pumpUntil([&] { return stub.requests() > requestsBeforeIdChange; }, 5000);
        check(reFetched, "the item asks the daemon again for the screen it was moved to");
        if (!reFetched) {
            std::fprintf(stderr, "  the monitor-id change produced no new request, so the path that invalidates the "
                                 "generation marks may not have been entered at all\n");
        }
        // Gated on the item having PROCESSED the refusal, not merely on the stub having received
        // the request: reading in between passes without the refusal ever reaching the item, which
        // is the state this check exists to grade.
        const bool refusalHandled =
            pumpUntil([&] { return !root->property("publishedReason").toString().isEmpty(); }, 5000);
        renderPass(nullptr);
        if (!refusalHandled) {
            wallpaperx::testing::harnessFailure(
                "the item never reported the refused fetch, so whether the claim survives it was not established");
        } else {
            check(claimsFrameShown(item), "the claim survives the refused fetch and the render pass after it");
        }
        check(itemTexture(item) == texture, "and the frame is still the one on screen");

        // GOAL: when the scene graph takes the item's resources away, the claim is withdrawn.
        //       False if it survives: main.qml keeps the message panel hidden while nothing is
        //       drawn at all, which is a black desktop with no explanation -- the defect that
        //       shipped, and one the entire rest of the suite is green for.
        // WHY-THIS-SHAPE: driven by taking the item out of its window, because that is what makes
        //       Qt call releaseResources() -- the real trigger in a compositor, where the item
        //       leaves its window or the window is torn down and rebuilt. Read synchronously, and
        //       that is MEASURED rather than argued: under the control below the claim reads true
        //       on this line and false again 300ms later, because the fetch reply still in flight
        //       republishes it from the same fact -- so the obvious "pump, then read" form of this
        //       check passes against the exact build that shipped the defect. The item is asserted
        //       to be CLAIMING a frame on the line before, or this check would pass by having had
        //       nothing to withdraw.
        // CONTROL: the `publishReason();` call at the end of WallpaperXItem::releaseResources() in
        //       backend/kde/wallpaperx_item.cpp deleted -- the shape that shipped -> "FAIL: the
        //       plugin stops claiming a frame is on screen once the scene graph takes its
        //       resources away", that check alone. Observed 2026-08-23. Reverted.
        const bool claimedBeforeRelease = claimsFrameShown(item);
        check(claimedBeforeRelease, "the plugin is still claiming a frame immediately before the release");
        if (!claimedBeforeRelease) {
            std::fprintf(stderr, "  nothing was being claimed going in, so a withdrawn claim afterwards would say "
                                 "nothing -- the release direction was not graded\n");
        } else {
            item->setParentItem(nullptr);
            const bool claimAfterRelease = claimsFrameShown(item);
            // Whether the detach really took the node away is QT'S behaviour, not the plugin's, so
            // it decides whether a verdict is issued at all rather than being asserted against the
            // plugin. A Qt that stopped releasing here would otherwise print a FAIL naming the
            // plugin for a state the harness never reached -- and the plugin would be innocent.
            // CONTROL: the setParentItem(nullptr) above replaced by a no-op -> "NOT VERIFIED --
            //       ... taking the item out of its window did not take its scene-graph node with
            //       it (Qt 6.11.2)", exit 77, and no check red. Observed 2026-08-23. Reverted.
            const bool nothingDrawn = item->window() == nullptr && itemPaintNode(item) == nullptr;
            if (!nothingDrawn) {
                notVerified(kGoal, std::string("taking the item out of its window did not take its scene-graph node "
                                               "with it (Qt ") +
                                       qVersion() + "), so nothing on this run stopped being drawn and the claim had "
                                                    "no reason to be withdrawn");
            } else {
                check(!claimAfterRelease,
                      "the plugin stops claiming a frame is on screen once the scene graph takes its resources away");
            }
        }

        // The node the detach queued is destroyed inside a render pass, with a context current --
        // which is item_import_release_test's goal, and is done here so this run does not end by
        // leaking the import it made.
        root.reset();
        renderPass(nullptr);
        control.invalidate();
        return true;
    });
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_frame_shown_test";

    wallpaperx::testing::registerFrameReplyTypes();

    // Set before any window exists: the item imports only when the scene graph reports OpenGL,
    // and Qt picks the API at first window creation.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);

    bool ok = false;
    try {
        ok = runFrameShownChecks();
    } catch (const std::exception& e) {
        // Without this boundary a host with no Vulkan ends in std::terminate, which carries no
        // reason -- and the reason is the whole output of a test that could not set up.
        wallpaperx::testing::harnessFailure(std::string("the harness could not build its preconditions: ") + e.what());
    }

    const bool checksRed = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
    if (!ok || checksRed || harnessRed) return 1;
    // NOT a skip: the checks above ran and passed. Marking this Skipped would erase the only
    // grade of the black-desktop defect. Only the GPU-side cleanliness went unjudged.
    if (g_ungradedValidation > 0) {
        std::fprintf(stderr,
                     "item_frame_shown_test: the validation grade could not be made -- "
                     "VK_LAYER_KHRONOS_validation is not loaded. The checks that ran are still judged; the "
                     "GPU-side cleanliness of this run is not.\n");
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
