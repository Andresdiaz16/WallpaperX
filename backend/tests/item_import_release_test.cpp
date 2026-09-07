// SPDX-License-Identifier: GPL-2.0-or-later

// S12.4: the GL objects WallpaperXItem imports from a daemon-exported frame are freed by the
// SCENE GRAPH'S OWN TEARDOWN of the texture -- on the thread that ran updatePaintNode, while that
// thread has a GL context current -- and not one moment before the scene graph has let go.
//
// Freeing where no context is current does not fail: glDeleteTextures is a silent no-op, and the
// memory stays allocated for the life of the compositor. Freeing while the node still points at
// the texture is the opposite failure and is a use-after-free inside plasmashell. Neither shows
// up as a wrong pixel, so nothing in the pixel-comparing probes can see either one.
//
// The whole check rests on one production decision: the release is attached to
// QSGTexture::destroyed with Qt::DirectConnection, and the QSGSimpleTextureNode owns its texture.
// A direct connection runs on whichever thread emits, so this file attaches a SECOND handler to
// the same signal and asks, from outside, what was true at that instant.
//
// The THREAD half of that sentence is printed rather than asserted, and the annotation at the
// print says why: QQuickRenderControl runs the scene graph on the thread that calls sync(), so
// the two threads are the same object here by construction and no production change could
// separate them. What is asserted is the consequence that survives the harness -- that the free
// happened, inside a render pass, in a context that could see it.
//
// WHAT THIS DOES NOT ESTABLISH, so nobody reads it as more than it is. The GL texture name is
// observable from outside, so its fate is graded here; the memory object beside it is not -- its
// name never leaves FrameImport::Result, and asking for it would mean a seam in the plugin. That
// half rests on the two frees sitting on the same line of release(). Descriptor accounting across
// repeated imports is frame_import_probe's, and pixels are frame_import_probe's too.
//
// This needs a frame the importer ACCEPTS, which item_import_failure_test deliberately does not
// serve -- its stub answers with a fourcc the importer refuses, because it grades the failure
// path. So the stub here exports a real Vulkan image, the way frame_import_probe does, and the
// run reports NOT VERIFIED when this host cannot pair a GL context with the Vulkan device that
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
#include <QThread>
#include <QUrl>
#include <QVariant>

// The only way to reach the node an item handed back, and therefore the QSGTexture whose
// destruction is the fact under test. Qt's private header, not a seam added to this project:
// WallpaperXItem exposes nothing about its node and must not start to. If this include ever
// stops resolving, the answer is to drop the test with its coverage stated, never to publish the
// node from the plugin.
#include <QtQuick/private/qquickitem_p.h>
#include <QtQuick/qsgtexture_platform.h>

#include <EGL/egl.h>
#include <GL/gl.h>

#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <string>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::ExportingStub;
using wallpaperx::testing::glDeviceUuid;
using wallpaperx::testing::itemTexture;
using wallpaperx::testing::kHeight;
using wallpaperx::testing::kPath;
using wallpaperx::testing::kService;
using wallpaperx::testing::kWidth;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::publishOneFrame;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::qmlSource;
using wallpaperx::testing::vulkanDeviceUuid;


constexpr const char* kGoal = "S12.4 (an imported frame's GL objects are freed by the scene graph's own texture "
                              "teardown, on its thread with its context current)";



// What the second QSGTexture::destroyed handler saw, at the instant it saw it.
struct DestroyObservation {
    int count = 0;
    QThread* thread = nullptr;
    bool insideRenderPass = false;
    bool qtContextCurrent = false;
    bool glAnswered = false;  // glGetString(GL_VERSION) -- a context at the driver level, not Qt's view
    bool eglContextCurrent = false;
    bool nameStillATexture = false;
};


GLuint nativeName(QSGTexture* texture) {
    if (texture == nullptr) return 0;
    auto* opengl = texture->nativeInterface<QNativeInterface::QSGOpenGLTexture>();
    return opengl == nullptr ? 0 : opengl->nativeTexture();
}

// Nonzero when the validation layer was absent. Its own counter, because charging that to
// failures turned every deliberate skip below into exit 1: a host without the layer package
// reported "not verified" in its output and FAILED to ctest, blaming the plugin for a package.
int g_ungradedValidation = 0;

bool runReleaseChecks() {
    return wallpaperx::testing::gradedRun("item_import_release_test", [](wallpaperx::testing::ValidationGrade& grade) {
        grade.reportUngradedTo(g_ungradedValidation);
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);
        // Rendered into, so the target carries a published frame. Nothing here reads a pixel --
        // frame_import_probe is what grades that the contents survive the trip -- but the render
        // is what makes the export a real one in both halves: an acquire descriptor is a hand-over
        // of a publication, and exportFrameAcquireFd refuses a target that has published nothing.
        const wallpaperx::OffscreenTarget& target = targets.create(0, kWidth, kHeight);
        publishOneFrame(device, targets, target, "item_import_release_gradient.png");

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

        // CONTROL: run with QT_QPA_PLATFORM=offscreen -> "NOT VERIFIED -- ... no OpenGL scene
        //       graph could be brought up under the offscreen platform", exit 77. Observed
        //       2026-08-22.
        if (!control.initialize() || window.rendererInterface() == nullptr ||
            window.rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
            notVerified(kGoal, std::string("no OpenGL scene graph could be brought up under the ") +
                                   QGuiApplication::platformName().toStdString() +
                                   " platform, so no frame could be imported and released");
            return true;
        }

        // The thread the scene graph synchronises on, recorded from Qt's own signal rather than
        // from anything this project publishes. Under QQuickRenderControl that is the thread
        // calling sync(); recorded anyway, because the fact under test is a comparison and
        // assuming one side of it would be assuming the answer.
        QThread* syncThread = nullptr;
        bool insideRenderPass = false;
        QObject::connect(
            &window, &QQuickWindow::beforeSynchronizing, &window,
            [&syncThread] { syncThread = QThread::currentThread(); }, Qt::DirectConnection);

        // One synchronisation pass, with the GL context dropped again at the end of it.
        //
        // The doneCurrent() is load-bearing and was MEASURED. QQuickRenderControl makes the
        // context current at beginFrame() and leaves it current after endFrame(), on the thread
        // that called it -- so in this harness, and only in this harness, the GUI thread happens
        // to hold a context between frames. A free on the GUI thread would then quietly succeed
        // here while leaking in a real compositor, whose GUI thread never has one: without this
        // line the pre-fix "release from releaseResources" shape was reconstructed in the plugin
        // and every check in this file still passed. Dropping it makes the harness say what
        // plasmashell says.
        const auto renderPass = [&control, &insideRenderPass](const std::function<void()>& withContext) {
            control.polishItems();
            control.beginFrame();
            insideRenderPass = true;
            control.sync();
            if (withContext) withContext();
            // Cleared AFTER endFrame(): Qt is free to reap scene-graph nodes there rather than in
            // sync(), and clearing first would fail the context check on a correct Qt build --
            // blaming the plugin for where Qt chose to do its teardown.
            control.endFrame();
            insideRenderPass = false;
            if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();
        };

        // An opaque fd is importable only by the driver that produced it, so a GL context on a
        // different device says nothing about the release path. Asked, not assumed: a headless
        // EGL context on this machine comes up on llvmpipe even where Vulkan resolves to NVIDIA.
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
        // CONTROL: run with VK_ICD_FILENAMES pointed at lavapipe, so Vulkan resolves to llvmpipe
        //       while the session's GL context is still NVIDIA -> "NOT VERIFIED -- ... the scene
        //       graph's GL context is not on the device the Vulkan export came from (GL reports
        //       NVIDIA GeForce RTX 3080 Ti/PCIe/SSE2)", exit 77 rather than a failure blamed on
        //       the plugin. Observed 2026-08-22.
        // Only the opaque path needs the match. A dma_buf is importable across devices, so
        // refusing to run for a UUID mismatch would skip every AMD and Intel host with a reason
        // that is not true of them.
        const bool needsSameDevice = wallpaperx::testing::exportNeedsSameDevice(device, target);

        // Two causes, named apart: a context on another GPU, and a context that cannot answer
        // which GPU it is on. Both mean this run establishes nothing, but reporting the second
        // as the first sends the reader hunting for a device mismatch that may not exist.
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
                                                                     QStringLiteral("/item_import_release.qml")));
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
            // Checked, or a QML wiring fault spins the pump for its full deadline and then
            // reports "the item never imported the frame" -- blaming the import path for a
            // property this file failed to resolve.
            wallpaperx::testing::harnessFailure(
                "the root object exposed no rendererItem, so there was no WallpaperXRenderer to drive");
            control.invalidate();
            return true;
        }

        // GOAL (S12.13, S12.14): a frame that arrives with a real acquire descriptor still ends up
        //       on screen. On a context that can wait, the wait is performed and the texture
        //       reaches the scene graph; on one that cannot, the descriptor is dropped and the
        //       frame is drawn without it rather than refused. False in the first direction if the
        //       importer refuses a wait it was given, and in the second if a driver with no
        //       GL_EXT_semaphore turns every wallpaper into a blank desktop.
        // WHY-THIS-SHAPE: it is the SAME check for both arms -- "a texture exists" -- because the
        //       property is the same and only the mechanism differs. Grading the two separately
        //       would need a seam telling the test which path the plugin took, and the plugin
        //       exposes none. Which arm this host ran is printed instead, so a reader knows what
        //       was demonstrated. `imported` is the existing bounded pump: it cannot pass by
        //       expiring, since expiry leaves it false and the branch below reports.
        // CONTROL (S12.13): `return false;` at the top of FrameImport::acquire in
        //       backend/kde/frame_import.cpp -> "FAIL: the item imported the frame the daemon
        //       served", with the run printing that this context CAN wait and that the stub sent
        //       one acquire descriptor. Observed 2026-08-31. Reverted.
        // CONTROL (S12.14): two stacked, the first to reach the state and the second to violate
        //       the property. FrameImport::supportsSync in backend/kde/frame_import.cpp forced to
        //       return false puts this host on the cannot-wait arm -- the run printed CANNOT and
        //       stayed GREEN, which is the property holding. dropUnwaitableSync in
        //       backend/kde/wallpaperx_item.cpp then made an immediate `return;` on top of that ->
        //       "FAIL: the item imported the frame the daemon served". Observed 2026-08-31. Both
        //       reverted.
        // COVERAGE-ASSERT: the number of acquire descriptors the stub actually sent is asserted,
        //       against the driver's own answer to whether it can export one. Without it, a stub
        //       that stopped sending one leaves both arms of this claim about a reply that carries
        //       no wait at all -- which is the state that shipped before this work.
        if (device.info().hasExternalSemaphoreFd) {
            check(stub.acquireFdsSent() >= 1, "the stub served the frame with an acquire descriptor beside it");
            if (stub.acquireFdsSent() < 1) {
                std::fprintf(stderr, "  the exporter produced no acquire on a device that reports it can: [%s]\n",
                             stub.acquireError().c_str());
            }
        } else {
            std::printf("item_import_release_test: this device cannot export an acquire semaphore, so the frame is "
                        "handed over unsynchronised and S12.13/S12.14 are not exercised here\n");
            std::fflush(stdout);
        }

        // Bounded, with a failure branch: everything below is gated on a texture existing, so a
        // run that never imported must not fall through into checks about freeing one.
        const bool imported = pumpUntil(
            [&] {
                renderPass(nullptr);
                return itemTexture(item) != nullptr;
            },
            10000);

        // Which of the two arms above this host ran. Asked inside a render pass because with no
        // context current the extension strings come back empty, which would report every host as
        // unable to wait.
        {
            std::string syncWhyNot;
            bool canWait = false;
            renderPass([&] { canWait = wallpaperx::FrameImport::supportsSync(&syncWhyNot); });
            std::printf("item_import_release_test: this GL context %s wait on the daemon's acquire%s%s\n",
                        canWait ? "CAN" : "CANNOT", canWait ? "" : " -- ", canWait ? "" : syncWhyNot.c_str());
            std::fflush(stdout);
        }

        if (!imported) {
            const std::string detail = std::string("it asked ") + std::to_string(stub.requests()) +
                                       " time(s), the stub served " + std::to_string(stub.served()) +
                                       ", handle type " + std::to_string(stub.handleType()) + ", export error [" +
                                       stub.exportError() + "], acquire descriptors sent " +
                                       std::to_string(stub.acquireFdsSent()) + ", acquire error [" +
                                       stub.acquireError() + "], the item reports [" +
                                       root->property("publishedReason").toString().toStdString() + "]";
            std::fprintf(stderr, "  %s\n", detail.c_str());

            // One outcome, two causes needing opposite verdicts: a host whose GL context cannot
            // import what was served (a real skip), and a plugin that never asked or refused a
            // frame this host CAN import (a failure). Everything needed to tell them apart was
            // already gathered here and only printed, so both skipped and the suite stayed green.
            if (stub.requests() == 0) {
                check(false, "the item asked the daemon for a frame");
            } else if (stub.served() == 0 && !stub.exportError().empty()) {
                notVerified(kGoal, "the stub could not export a frame on this host, so there was nothing to import");
            } else {
                std::string whyNot;
                bool supported = false;
                renderPass([&] {
                    supported = wallpaperx::FrameImport::supports(
                        stub.handleType() == 1 ? wallpaperx::FrameImport::HandleType::DmaBuf
                                               : wallpaperx::FrameImport::HandleType::OpaqueFd,
                        &whyNot);
                });
                // Queried inside a render pass: with no context current the extension strings come
                // back empty and every failure would read as a host limit again.
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

        QSGTexture* texture = itemTexture(item);
        const GLuint name = nativeName(texture);
        if (name == 0) {
            wallpaperx::testing::harnessFailure(
                "the scene-graph texture exposed no OpenGL name, so the imported object could not be watched");
            root.reset();
            renderPass(nullptr);
            control.invalidate();
            return true;
        }
        std::printf("item_import_release_test: imported on %s, handle type %s, GL texture %u\n", glRenderer.c_str(),
                    stub.handleType() == 1 ? "dma_buf" : "opaque fd", name);
        std::fflush(stdout);

        // GOAL: S12.4's "not before the scene graph has let go" half -- while the item is still
        //       showing the frame, the imported GL objects must still exist. False if the import
        //       is released eagerly, which is a texture the node samples from after it is freed.
        // WHY-THIS-SHAPE: asked once per render pass and COUNTED, from inside the pass so a
        //       context is current -- glIsTexture with no context answers GL_FALSE, which would
        //       read as "already freed" and fail for the wrong reason. The obvious simpler form,
        //       a single query after one sync, cannot distinguish "still alive" from "not yet
        //       reached the code that would free it".
        // CONTROL: `FrameImport::release(imported);` added immediately after `node->setTexture(
        //       texture);` in WallpaperXItem::updatePaintNode, backend/kde/wallpaperx_item.cpp
        //       -> "FAIL: the imported GL texture stays alive while the item is showing the
        //       frame", printing "0 of 6 passes found it live". Observed 2026-08-22. Reverted.
        // COVERAGE-ASSERT: the pass count is asserted, and the control for it is the reason it is
        //       here rather than a comment. With the loop bound forced to zero -- a stand-in for
        //       any future edit that stops entering it -- the liveness check itself stayed GREEN,
        //       0 == 0, while "the liveness check ran once per render pass" went red alone.
        //       Observed 2026-08-22. Reverted.
        constexpr int kLivePasses = 6;
        int passesRun = 0;
        int passesLive = 0;
        for (int pass = 0; pass < kLivePasses; ++pass) {
            renderPass([&] {
                ++passesRun;
                if (glIsTexture(name) == GL_TRUE) ++passesLive;
            });
        }
        check(passesRun == kLivePasses, "the liveness check ran once per render pass");
        check(passesLive == passesRun, "the imported GL texture stays alive while the item is showing the frame");
        if (passesLive != passesRun) {
            std::fprintf(stderr, "  %d of %d passes found it live\n", passesLive, passesRun);
        }
        // The barrier the destruction phase below rests on, and it is checked rather than assumed
        // for a concrete reason: node->setTexture() destroys the texture it replaces, so a second
        // served frame would delete the object this file is about to attach a handler to. That is
        // a dangling pointer in the harness -- measured, by removing the stub's one-frame guard:
        // the run died without printing its own summary, which is the one thing a harness must
        // never do. Both facts are reported, then the phase is abandoned.
        // CONTROL: the stub's `if (m_served > 0)` refusal disabled, and a FrameReady signal sent
        //       from this file after the texture was captured -> both checks red, "the item kept
        //       the same scene-graph texture across the liveness passes" and "the stub served
        //       exactly one frame, so no second import replaced the texture", and the phase
        //       abandoned instead of dereferencing the replaced texture. Observed 2026-08-22.
        //       Both reverted.
        const bool sameTexture = itemTexture(item) == texture;
        check(sameTexture, "the item kept the same scene-graph texture across the liveness passes");
        check(stub.served() == 1, "the stub served exactly one frame, so no second import replaced the texture");
        if (!sameTexture) {
            std::fprintf(stderr, "  the texture was replaced mid-run, so what happens to it when the scene graph "
                                 "drops it was not established\n");
            control.invalidate();
            return true;
        }

        // GOAL: S12.4's core -- when the scene graph destroys the imported texture, the release
        //       has ALREADY run, on that thread, with that thread's GL context current.
        // WHY-THIS-SHAPE: a second handler on the same QSGTexture::destroyed signal, attached
        //       AFTER the plugin's, because Qt activates direct slots in connection order -- so
        //       this one observes the state the plugin's release left behind. That ordering is
        //       load-bearing: reversed, glIsTexture would still report the name live and this
        //       would fail for the wrong reason. The obvious simpler form -- check the name after
        //       the frame that tears the node down -- cannot tell "freed by the texture's own
        //       destruction" from "freed later by anything at all", and it is exactly the "later,
        //       somewhere else" shapes (a queued connection, a free from releaseResources on the
        //       GUI thread) that this goal exists to exclude.
        // CONTROL: Qt::DirectConnection changed to Qt::QueuedConnection on the
        //       QSGTexture::destroyed connection in WallpaperXItem::updatePaintNode,
        //       backend/kde/wallpaperx_item.cpp -> "FAIL: the import was already released by the
        //       time the scene graph's texture destruction returned", printing "GL name 1 was
        //       still a live GL texture after the scene graph destroyed the QSGTexture that owned
        //       it", and no other check red. Observed 2026-08-22. Reverted.
        // CONTROL, the pre-fix shape: that connection removed entirely, the FrameImport::Result
        //       stored on the item instead, and FrameImport::release() called from
        //       WallpaperXItem::releaseResources() in backend/kde/wallpaperx_item.cpp -- freeing
        //       on the GUI thread, which is what shipped before -> the same check red with the
        //       same diagnostic. Observed 2026-08-22. Reverted.
        DestroyObservation seen;
        // Disconnected on every exit, and declared AFTER `seen` so it runs BEFORE `seen` dies.
        // The connection is scoped to `window`, which is declared earlier and so outlives these
        // locals: a texture reaped during ~QQuickWindow would otherwise run the lambda and write
        // through a dead stack frame -- on exactly the paths where the checks below went red.
        struct Disconnector {
            QMetaObject::Connection connection;
            ~Disconnector() { QObject::disconnect(connection); }
        };
        Disconnector guard;
        guard.connection = QObject::connect(
            texture, &QObject::destroyed, &window,
            [&] {
                ++seen.count;
                seen.thread = QThread::currentThread();
                seen.insideRenderPass = insideRenderPass;
                seen.qtContextCurrent = QOpenGLContext::currentContext() != nullptr;
                seen.eglContextCurrent = eglGetCurrentContext() != EGL_NO_CONTEXT;
                seen.glAnswered = glGetString(GL_VERSION) != nullptr;
                if (seen.glAnswered) {
                    while (glGetError() != GL_NO_ERROR) {
                    }
                    seen.nameStillATexture = glIsTexture(name) == GL_TRUE;
                }
            },
            Qt::DirectConnection);

        // Deleting the item only QUEUES its node for the scene graph to clean up, so the
        // destruction happens in the next sync -- which is the point: it happens inside a render
        // pass rather than here on this line.
        root.reset();
        renderPass(nullptr);

        // GOAL: the scene graph destroys the imported texture at all. Everything else here is
        //       about WHEN and WHERE, and all of it is vacuous if the texture simply outlives the
        //       item -- which is a whole screen's worth of GPU memory leaked per unplugged
        //       monitor, silently.
        // CONTROL: `node->setOwnsTexture(true);` changed to `false` in
        //       WallpaperXItem::updatePaintNode, backend/kde/wallpaperx_item.cpp -> "FAIL: the
        //       scene graph destroyed the imported texture when the item went away", printing
        //       "QSGTexture::destroyed never fired, so nothing could have released the import".
        //       The checks below did not run, which is why they sit inside the else. Observed
        //       2026-08-22. Reverted.
        const bool destroyed = seen.count > 0;
        check(destroyed, "the scene graph destroyed the imported texture when the item went away");
        if (!destroyed) {
            std::fprintf(stderr, "  QSGTexture::destroyed never fired, so nothing could have released the import\n");
        } else {
            // GOAL: the destruction -- and therefore the release attached to it -- happens with a
            //       GL context current on the destroying thread. False for a free on the GUI
            //       thread outside a render pass, where glDeleteTextures is a silent no-op.
            // WHY-THIS-SHAPE: three witnesses to "a context was current", because each alone can
            //       lie. QOpenGLContext::currentContext() is bookkeeping Qt maintains;
            //       eglGetCurrentContext() answers EGL_NO_CONTEXT on a GLX-backed platform even
            //       where a context IS current, which this host's xcb fallback would be; and
            //       glGetString(GL_VERSION) asks the driver directly but cannot say which context
            //       it answered for. The assertion takes the two that no windowing system can
            //       defeat, plus the harness's own record of whether a render pass was open; the
            //       EGL one is printed beside them rather than asserted.
            // CONTROL: the pre-fix shape one level up -- `node->setOwnsTexture(false)`, the
            //       QSGTexture stored on the item, and `delete`d from
            //       WallpaperXItem::releaseResources() in backend/kde/wallpaperx_item.cpp, so the
            //       item frees it on the GUI thread as the item is torn down -> "FAIL: the
            //       imported texture was destroyed inside a render pass, with a GL context
            //       current", printing "in a render pass: no, Qt context current: no, EGL context
            //       current: no, driver answered: no", and the check below correctly reported
            //       that it could not answer rather than passing. Observed 2026-08-22. Reverted.
            check(seen.insideRenderPass && seen.qtContextCurrent && seen.glAnswered,
                  "the imported texture was destroyed inside a render pass, with a GL context current");

            // Gated on the driver having answered, and that gate is not defensive housekeeping:
            // glIsTexture with no current context returns GL_FALSE, which reads as "already
            // freed". Ungated, the one build this check exists to catch -- a release on a thread
            // with no context -- would pass it. Reported as unexercised rather than as a pass;
            // the check above is what goes red for that build.
            if (!seen.glAnswered) {
                notVerified(kGoal, "the texture was destroyed with no GL context current, so whether the import had "
                                   "already been freed could not be asked -- glIsTexture answers GL_FALSE either "
                                   "way there");
            } else {
                check(!seen.nameStillATexture,
                      "the import was already released by the time the scene graph's texture destruction returned");
                if (seen.nameStillATexture) {
                    std::fprintf(stderr, "  GL name %u was still a live GL texture after the scene graph destroyed "
                                         "the QSGTexture that owned it\n",
                                 name);
                }
            }

            // PRINTED, NOT ASSERTED, and the reason is the harness rather than the property.
            // QQuickRenderControl runs the scene graph on the thread that calls sync(), so under
            // this harness the two threads are the same object by construction and NO production
            // mutation can separate them -- an assertion here could not fail, which is the one
            // thing this project refuses to ship. The falsifiable form of the same property is
            // the render-pass and context checks above: a release that moved off the scene
            // graph's thread would have no context current on the thread it ran on.
            // The count rides here for the same reason: every field of `seen` holds the LAST
            // firing, so a second one would quietly change what the checks above are about --
            // worth seeing, but not something a production change can cause.
            std::printf("item_import_release_test: destroyed %d time(s) on %p, synchronised on %p, in a render "
                        "pass: %s, Qt context current: %s, EGL context current: %s, driver answered: %s\n",
                        seen.count, static_cast<void*>(seen.thread), static_cast<void*>(syncThread),
                        seen.insideRenderPass ? "yes" : "no", seen.qtContextCurrent ? "yes" : "no",
                        seen.eglContextCurrent ? "yes" : "no", seen.glAnswered ? "yes" : "no");
            std::fflush(stdout);
        }

        // GOAL (S12.18): the daemon is TOLD, and told which image. Freeing this process's GL
        //       objects is only half of letting go -- until a ReleaseBuffer names the image, the
        //       daemon goes on
        //       treating it as held and keeps it out of the ring for the life of the connection,
        //       which on a monitor of three images is a wallpaper that stops changing.
        // WHY-THIS-SHAPE: the event loop is pumped AFTER the render pass that destroyed the
        //       texture, and that pump is the whole point. The release hops to the application
        //       object with a queued connection, so at the moment the texture is destroyed
        //       nothing has been sent yet; a check written where the destruction checks are
        //       would read zero for a correct plugin. The wait is bounded and its expiry leaves
        //       the count at whatever it was, which fails the check rather than passing it.
        //       The buffer id is compared, not just the count: a release naming the wrong image
        //       is worse than none, since it invites the daemon to draw into one this process
        //       may still be sampling.
        // CONTROL: `releaseClaim(claim);` deleted from the QSGTexture::destroyed lambda in
        //       WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) -> "FAIL: the
        //       plugin told the daemon it had let the image go", printing "0 release(s) arrived
        //       in two seconds after the texture was destroyed". Reverted.
        // CONTROL, for the id: `request.setArguments({monitorId, bufferId});` in
        //       sendReleaseBuffer (backend/kde/wallpaperx_item.cpp) changed to `bufferId + 1`
        //       -> "FAIL: and named the monitor and the image it had been given", printing "it
        //       released monitor 0 image 2; it was given monitor 0 image 1". Reverted.
        {
            const bool sent = pumpUntil([&stub] { return !stub.releases().empty(); }, 2000);
            check(sent, "the plugin told the daemon it had let the image go");
            if (!sent) {
                std::fprintf(stderr, "  %zu release(s) arrived in two seconds after the texture was destroyed\n",
                             stub.releases().size());
            } else {
                const wallpaperx::testing::ExportingStub::Release& first = stub.releases().front();
                check(first.monitorId == wallpaperx::testing::kFirstMonitor && first.bufferId == target.bufferId,
                      "and named the monitor and the image it had been given");
                if (first.monitorId != wallpaperx::testing::kFirstMonitor || first.bufferId != target.bufferId) {
                    std::fprintf(stderr, "  it released monitor %d image %llu; it was given monitor %d image %llu\n",
                                 first.monitorId, static_cast<unsigned long long>(first.bufferId),
                                 wallpaperx::testing::kFirstMonitor,
                                 static_cast<unsigned long long>(target.bufferId));
                }
            }
            // The stub's own channel, so a ReleaseBuffer this harness could not parse is not
            // counted as one that was never sent.
            // CONTROL: `request.setArguments({monitorId, bufferId});` in sendReleaseBuffer
            //       (backend/kde/wallpaperx_item.cpp) changed to `{monitorId}` -> "FAIL: every
            //       ReleaseBuffer the stub saw carried a monitor and an image" AND "FAIL: the
            //       plugin told the daemon it had let the image go" -- which is the point of
            //       having both: a release the harness cannot read must not print as one the
            //       plugin never sent. Reverted.
            check(stub.malformedReleases() == 0, "every ReleaseBuffer the stub saw carried a monitor and an image");
        }

        control.invalidate();
        return true;
    });
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_import_release_test";

    wallpaperx::testing::registerFrameReplyTypes();

    // Set before any window exists: the item imports only when the scene graph reports OpenGL,
    // and Qt picks the API at first window creation.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);

    bool ok = false;
    try {
        ok = runReleaseChecks();
    } catch (const std::exception& e) {
        // Without this boundary a host with no Vulkan ends in std::terminate, which carries no
        // reason -- and the reason is the whole output of a test that could not set up.
        wallpaperx::testing::harnessFailure(std::string("the harness could not build its preconditions: ") + e.what());
    }

    const bool checksRed = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
    if (!ok || checksRed || harnessRed) return 1;
    // NOT a skip: the checks above ran and passed, so reporting the goal as unexercised is false
    // and marks the entry Skipped, erasing the coverage. Only the GPU-side cleanliness went
    // unjudged. Same shape as device_affinity_probe.
    if (g_ungradedValidation > 0) {
        std::fprintf(stderr,
                     "item_import_release_test: the validation grade could not be made -- "
                     "VK_LAYER_KHRONOS_validation is not loaded. The checks that ran are still judged; the "
                     "GPU-side cleanliness of this run is not.\n");
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
