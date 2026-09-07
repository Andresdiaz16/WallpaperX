// SPDX-License-Identifier: GPL-2.0-or-later

// When the KDE plugin tells the daemon it has stopped reading an image, and when it must not.
//
// The daemon keeps three images per monitor and will not hand over the last free one, so an
// image a consumer never gives back is an image the producer never gets. Nothing about that is
// visible as a wrong pixel: a release never sent shows up as a wallpaper that stops changing,
// and a release sent too early invites the daemon to draw into memory this process is still
// sampling. Both are silent.
//
// Three properties, one per phase, each selected by argv[1] so it runs in a process of its own.
// That is not tidiness: the ledger under test is a process-wide singleton with no way to reset
// it, and the plugin's record of which daemon it is talking to is a file-scope global in the
// plugin. Phases sharing a process would carry each other's state, and the daemon-restart phase
// in particular has to be the first owner change this process ever sees.
//
//   repeat-import  -- one release per image, not per import. The plugin re-imports the image it
//                     is already showing whenever the scene changes, so one image is genuinely
//                     imported twice at once and the daemon may only be told when the last of
//                     those imports is gone.
//   dropped-frame  -- a frame fetched and then dropped without ever being imported is released.
//                     The daemon recorded the hold when it answered, so every path that walks
//                     away from a reply owes it a release.
//   daemon-restart -- a claim granted by one daemon is never released against its successor.
//                     Buffer ids restart when the daemon does.
//
// Like item_import_release_test, this needs a frame the importer ACCEPTS, so it reports NOT
// VERIFIED when this host cannot pair a GL context with the Vulkan device that produced the
// export: an opaque fd is importable only by the driver that made it.

#include "kde/frame_import.h"
#include "support/validation_grade.h"
// For vulkanDeviceUuid only; this test brings up its own GL through QQuickRenderControl.
#include "tests/gl_import_harness.h"
#include "tests/item_stub_scene.h"
#include "tests/test_report.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::ExportingStub;
using wallpaperx::testing::glDeviceUuid;
using wallpaperx::testing::itemTexture;
using wallpaperx::testing::kFirstMonitor;
using wallpaperx::testing::kHeight;
using wallpaperx::testing::kInterface;
using wallpaperx::testing::kPath;
using wallpaperx::testing::kSecondMonitor;
using wallpaperx::testing::kService;
using wallpaperx::testing::kWidth;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::publishOneFrame;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::qmlSource;
using wallpaperx::testing::vulkanDeviceUuid;

enum class Phase { RepeatImport, DroppedFrame, DaemonRestart };

const char* goalFor(Phase phase) {
    switch (phase) {
        case Phase::RepeatImport:
            return "S12.15 (the daemon is told once per image, when the LAST live import of it is gone)";
        case Phase::DroppedFrame:
            return "S12.16 (a frame fetched and dropped without being imported is still released)";
        case Phase::DaemonRestart:
            return "S12.17 (a claim granted by one daemon is never released against its successor)";
    }
    return "an unnamed goal";
}

// Nonzero when the validation layer was absent. Its own counter, because charging that to
// failures turns a deliberate skip into exit 1 and blames the plugin for a missing package.
int g_ungradedValidation = 0;

// Runs the event loop for a fixed span, for the checks that assert something did NOT happen.
//
// The one shape a bounded wait cannot take: there is no condition to wait for, so this cannot
// fail by expiring -- expiring IS the run. What makes it worth anything is that the paired
// positive check in the same phase waits for the same event and gets it, so the span is
// demonstrably long enough for a release to arrive when one is owed.
void pumpFor(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// Everything a phase needs standing up, so the three bodies read as what they do rather than as
// three copies of the same forty lines.
struct Scene {
    QQuickRenderControl control;
    QQuickWindow window{&control};
    QQmlEngine engine;
    std::unique_ptr<QQmlComponent> component;
    std::unique_ptr<QObject> root;
    QQuickItem* item = nullptr;
    bool insideRenderPass = false;

    // One synchronisation pass, with the GL context dropped again at the end of it. The
    // doneCurrent() is load-bearing for the reason item_import_release_test states at length:
    // QQuickRenderControl leaves a context current on the GUI thread between frames, which a
    // real compositor never does, and a release that leaked there would still pass.
    void renderPass() {
        control.polishItems();
        control.beginFrame();
        insideRenderPass = true;
        control.sync();
        control.endFrame();
        insideRenderPass = false;
        if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();
    }
};

bool runPhase(Phase phase) {
    const char* kGoal = goalFor(phase);
    return wallpaperx::testing::gradedRun("item_release_ledger_test", [phase,
                                                                      kGoal](wallpaperx::testing::ValidationGrade&
                                                                                 grade) {
        grade.reportUngradedTo(g_ungradedValidation);
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);
        // Two images, because the daemon-restart phase needs a SECOND image carrying the FIRST
        // one's buffer id -- which is what a restarted daemon produces, since ids begin again at
        // one. The other phases use the first alone.
        const wallpaperx::OffscreenTarget& first = targets.create(kFirstMonitor, kWidth, kHeight);
        publishOneFrame(device, targets, first, "item_release_ledger_first.png");
        const wallpaperx::OffscreenTarget& second = targets.create(kSecondMonitor, kWidth, kHeight);
        publishOneFrame(device, targets, second, "item_release_ledger_second.png");

        QDBusConnection bus = QDBusConnection::sessionBus();
        // The image sequence this phase's daemon serves, all of it the SAME image, which is what
        // the plugin asks for: it re-fetches the frame it is already showing on every scene
        // change. Deliberately longer than the phases drive, because how many times the item
        // asks while it is joining a scene is Qt's business and not a number to pin -- the
        // phases below settle first and then count from where they find themselves. The
        // dropped-frame phase wants exactly one served and a refusal after it, so it takes one.
        std::vector<const wallpaperx::OffscreenTarget*> sequence(phase == Phase::DroppedFrame ? 1 : 4, &first);
        ExportingStub stub(device, sequence);
        if (!bus.isConnected() || !bus.registerService(QString::fromUtf8(kService)) ||
            !bus.registerVirtualObject(QString::fromUtf8(kPath), &stub)) {
            notVerified(kGoal, "no session bus this run could own io.wallpaperx.Daemon on: " +
                                   bus.lastError().message().toStdString());
            return true;
        }

        Scene scene;
        scene.window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
        if (!scene.control.initialize() || scene.window.rendererInterface() == nullptr ||
            scene.window.rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
            notVerified(kGoal, std::string("no OpenGL scene graph could be brought up under the ") +
                                   QGuiApplication::platformName().toStdString() +
                                   " platform, so no frame could be imported and released");
            return true;
        }

        // An opaque fd is importable only by the driver that produced it, so a GL context on
        // another device says nothing about any of this. Asked, not assumed: a headless EGL
        // context on this machine comes up on llvmpipe even where Vulkan resolves to NVIDIA.
        bool uuidQueryable = false;
        std::array<uint8_t, 16> glUuid{};
        std::string glRenderer = "(unknown)";
        scene.control.polishItems();
        scene.control.beginFrame();
        scene.control.sync();
        {
            const GLubyte* name = glGetString(GL_RENDERER);
            if (name != nullptr) glRenderer = reinterpret_cast<const char*>(name);
            uuidQueryable = glDeviceUuid(glUuid);
        }
        const bool sameDevice = uuidQueryable && glUuid == vulkanDeviceUuid(device.physicalDevice());
        scene.control.endFrame();
        if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();

        // Only the opaque path needs the match: a dma_buf imports across devices, so refusing to
        // run for a UUID mismatch would skip every AMD and Intel host for a reason untrue of them.
        const bool needsSameDevice = wallpaperx::testing::exportNeedsSameDevice(device, first);
        if (needsSameDevice && !uuidQueryable) {
            notVerified(kGoal, "this GL context does not answer GL_DEVICE_UUID_EXT (GL reports " + glRenderer +
                                   "), so whether it is on the exporting device could not be established");
            scene.control.invalidate();
            return true;
        }
        if (needsSameDevice && !sameDevice) {
            notVerified(kGoal, "the scene graph's GL context is not on the device the Vulkan export came from (GL "
                               "reports " + glRenderer + "), so the frame is not importable here at all");
            scene.control.invalidate();
            return true;
        }

        scene.engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
        scene.component = std::make_unique<QQmlComponent>(&scene.engine);
        scene.component->setData(qmlSource().toUtf8(),
                                 QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                     QStringLiteral("/item_release_ledger.qml")));
        scene.root.reset(scene.component->create());
        auto* rootItem = qobject_cast<QQuickItem*>(scene.root.get());
        if (rootItem == nullptr) {
            for (const QQmlError& error : scene.component->errors()) {
                std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
            }
            check(false, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");
            scene.control.invalidate();
            return true;
        }
        rootItem->setParentItem(scene.window.contentItem());
        scene.item = qobject_cast<QQuickItem*>(scene.root->property("rendererItem").value<QObject*>());
        if (scene.item == nullptr) {
            wallpaperx::testing::harnessFailure(
                "the root object exposed no rendererItem, so there was no WallpaperXRenderer to drive");
            scene.control.invalidate();
            return true;
        }

        if (phase == Phase::DroppedFrame) {
            // GOAL: a frame the plugin fetched and then dropped without importing is released.
            //       The daemon records the hold when it ANSWERS GetFrameFd, so a reply the
            //       plugin walks away from -- because the item moved to another monitor, or a
            //       newer frame superseded it -- costs the daemon an image until this process
            //       exits.
            // WHY-THIS-SHAPE: no render pass runs before the monitor is changed, and that is the
            //       whole claim. A pass would import the frame, which moves the claim on to the
            //       texture and makes this a test of the texture path that item_import_release
            //       already covers.
            //
            //       The barrier before the monitor changes is a SECOND fetch that the stub
            //       refuses, because the item publishes a reason for a refusal and publishes
            //       nothing for a frame it parsed. Waiting instead on the stub having SENT the
            //       first reply races the item's own dispatch of it -- measured, and the run
            //       reported "0 release(s)" against a plugin that was behaving correctly,
            //       because at the moment the monitor changed there was nothing pending to drop.
            //       The ordering that makes the refusal a sound barrier is the plugin's own: it
            //       keeps one fetch in flight, so the second request cannot be sent until the
            //       first reply has been handed to takeFrameReply.
            // CONTROL: `releaseClaim(claim);` deleted from
            //       WallpaperXItem::PendingFrame::~PendingFrame (backend/kde/wallpaperx_item.cpp)
            //       -> "FAIL: a frame dropped without ever being imported is released", printing
            //       0 releases. Reverted.
            const auto publishedReason = [&scene] {
                return scene.root->property("publishedReason").toString();
            };
            QDBusMessage ready = QDBusMessage::createSignal(QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                                                            QStringLiteral("FrameReady"));
            ready.setArguments({kFirstMonitor, static_cast<qulonglong>(2)});
            bus.send(ready);

            const bool parsed =
                pumpUntil([&] { return stub.served() > 0 && stub.requests() > 1 && !publishedReason().isEmpty(); },
                          5000);
            check(parsed, "the item read one reply and was then refused a second, so the first is in its hands");
            if (!parsed) {
                std::fprintf(stderr, "  the item asked %d time(s), the stub served %d, the item reports [%s]\n",
                             stub.requests(), stub.served(), publishedReason().toStdString().c_str());
                scene.control.invalidate();
                return true;
            }
            check(stub.releases().empty(), "nothing is released while the item still holds the reply");

            scene.item->setProperty("monitorId", kSecondMonitor);
            const bool released = pumpUntil([&stub] { return !stub.releases().empty(); }, 2000);
            check(released, "a frame dropped without ever being imported is released");
            if (!released) {
                std::fprintf(stderr, "  0 release(s) arrived after the item moved to another monitor\n");
            } else {
                const ExportingStub::Release& sent = stub.releases().front();
                check(sent.monitorId == kFirstMonitor && sent.bufferId == first.bufferId,
                      "and it names the monitor and image the reply carried, not the one the item moved to");
                if (sent.monitorId != kFirstMonitor || sent.bufferId != first.bufferId) {
                    std::fprintf(stderr, "  it released monitor %d image %llu; the reply carried monitor %d image "
                                         "%llu\n",
                                 sent.monitorId, static_cast<unsigned long long>(sent.bufferId), kFirstMonitor,
                                 static_cast<unsigned long long>(first.bufferId));
                }
            }
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }

        // Both remaining phases need a frame genuinely on the scene graph first.
        const bool imported = pumpUntil(
            [&scene] {
                scene.renderPass();
                return itemTexture(scene.item) != nullptr;
            },
            10000);
        if (!imported) {
            std::string whyNot;
            bool supported = false;
            scene.control.polishItems();
            scene.control.beginFrame();
            scene.control.sync();
            supported = wallpaperx::FrameImport::supports(stub.handleType() == 1
                                                              ? wallpaperx::FrameImport::HandleType::DmaBuf
                                                              : wallpaperx::FrameImport::HandleType::OpaqueFd,
                                                          &whyNot);
            scene.control.endFrame();
            if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();
            // One outcome, two causes needing opposite verdicts: a host whose GL context cannot
            // import what was served is a skip, a plugin that refused a frame this host CAN
            // import is a failure.
            if (supported) {
                check(false, "the item imported the frame the stub served");
                std::fprintf(stderr, "  it asked %d time(s), the stub served %d, export error [%s], the item "
                                     "reports [%s]\n",
                             stub.requests(), stub.served(), stub.exportError().c_str(),
                             scene.root->property("publishedReason").toString().toStdString().c_str());
            } else {
                notVerified(kGoal, whyNot);
            }
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }
        // Let the item's own startup traffic finish before anything is counted, and read the
        // served count from where it leaves off rather than assuming a number. A phase that
        // started driving the moment the first texture appeared could be racing a fetch it did
        // not make, and a reply superseded that way leaves only ONE import live -- which is the
        // state this phase exists to avoid.
        for (int settle = 0; settle < 10; ++settle) {
            scene.renderPass();
            pumpFor(30);
        }
        const int servedAtSettle = stub.served();
        QSGTexture* firstTexture = itemTexture(scene.item);
        if (firstTexture == nullptr) {
            wallpaperx::testing::harnessFailure("the item had no texture once its own startup fetches had settled");
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }
        // The premise every release count below is read against: while the item is showing the
        // frame, nothing has been released. A run that reached here having already released
        // would leave those counts describing a different state.
        check(stub.releases().empty(), "nothing has been released while the item is showing the frame");

        // Whether the scene graph has destroyed that first texture, watched rather than inferred
        // from the item's texture pointer changing. Two reasons: setTexture() DELETES the one it
        // replaces, so comparing pointers afterwards reads freed memory and the allocator is free
        // to hand the same address back -- measured, and a genuine re-import read as no change at
        // all. And it is the destruction, not the replacement, that the release hangs off.
        //
        // Scoped to the window, which outlives every local here, so a texture reaped during the
        // window's own teardown cannot write through a dead stack frame.
        bool firstTextureDestroyed = false;
        struct Disconnector {
            QMetaObject::Connection connection;
            ~Disconnector() { QObject::disconnect(connection); }
        };
        Disconnector watchFirst;
        watchFirst.connection = QObject::connect(
            firstTexture, &QObject::destroyed, &scene.window, [&firstTextureDestroyed] { firstTextureDestroyed = true; },
            Qt::DirectConnection);

        if (phase == Phase::RepeatImport) {
            // GOAL: the daemon is told once per IMAGE, not once per import. A newer generation can
            //       arrive in the image the item is already showing -- the daemon's ring cycles,
            //       so it hands the same buffer id back every few frames -- and the item then has
            //       two imports of one image live at once; releasing on the first teardown hands
            //       the daemon an image the older texture is still sampling from. This phase
            //       drives that state with its own FrameReady rather than waiting for a scene
            //       re-entry, which no longer re-asks: the item's scene-change path now goes
            //       through ensureFrameRequested(), which is silent when it already holds a reply.
            // WHY-THIS-SHAPE: the negative check comes first and is what carries the property.
            //       "One release at the end" alone is equally true of a plugin that releases on
            //       the FIRST teardown and sends nothing on the second -- which is exactly what a
            //       claim held as a flag rather than a count does. Only asking, after the first
            //       texture is gone and before the second is, whether anything was sent can tell
            //       those apart.
            // CONTROL: `++entry->imports;` in BufferClaims::take (backend/kde/buffer_claims.cpp)
            //       replaced by a bare `;`, so a repeat import records nothing -> "FAIL: nothing
            //       is released while a second import of that image is still live", printing "it
            //       released monitor 0 image 1 with an import still live", "FAIL: and it is
            //       released once, not once per import" printing 2 releases, and the settle
            //       premise above red as well -- a superseded reply releases early there for the
            //       same reason. Reverted.
            // COVERAGE-ASSERT: the first texture is required to have been DESTROYED and a second
            //       frame served, so a run in which the item never re-imported -- and therefore
            //       never built the two-live-imports state -- fails here instead of reporting the
            //       release accounting as established.
            QDBusMessage ready = QDBusMessage::createSignal(QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                                                            QStringLiteral("FrameReady"));
            // One past the newest generation the item has read, so it is worth fetching. Counted
            // from where the settle left it rather than fixed at 2: how many frames the item
            // asked for on its own is not a number this file may pin.
            ready.setArguments({kFirstMonitor, static_cast<qulonglong>(servedAtSettle + 1)});
            bus.send(ready);

            const bool reimported = pumpUntil(
                [&scene, &firstTextureDestroyed] {
                    scene.renderPass();
                    return firstTextureDestroyed && itemTexture(scene.item) != nullptr;
                },
                5000);
            check(reimported, "the item re-imported the image it was already showing, replacing its texture");
            if (!reimported) {
                std::fprintf(stderr, "  the stub served %d frame(s) and the item asked %d time(s), first texture "
                                     "destroyed: %s, a texture is present: %s, the item reports [%s]; the "
                                     "two-live-imports state was never built\n",
                             stub.served(), stub.requests(), firstTextureDestroyed ? "yes" : "no",
                             itemTexture(scene.item) != nullptr ? "yes" : "no",
                             scene.root->property("publishedReason").toString().toStdString().c_str());
                scene.root.reset();
                scene.renderPass();
                scene.control.invalidate();
                return true;
            }
            check(stub.served() > servedAtSettle,
                  "the stub served the same image again, which is what makes two imports of it live");

            // The first texture was destroyed by the replacement above. Nothing may have been
            // sent: the second import of that image is still live.
            pumpFor(300);
            check(stub.releases().empty(), "nothing is released while a second import of that image is still live");
            if (!stub.releases().empty()) {
                std::fprintf(stderr, "  it released monitor %d image %llu with an import still live\n",
                             stub.releases().front().monitorId,
                             static_cast<unsigned long long>(stub.releases().front().bufferId));
            }

            // GOAL: and exactly one release when the last import goes.
            // CONTROL: `releaseClaim(claim);` deleted from the QSGTexture::destroyed lambda in
            //       WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) -> "FAIL:
            //       the last import going is what releases the image", printing "0 release(s)
            //       for one image imported twice". Reverted.
            // CONTROL: `releaseClaim(claim);` deleted from
            //       WallpaperXItem::PendingFrame::~PendingFrame (backend/kde/wallpaperx_item.cpp)
            //       -> the same line red, for the other reason: the claim a superseded reply
            //       never gives back leaves the count one too high forever, so the last texture
            //       teardown decrements to one instead of nought and says nothing. Reverted.
            scene.root.reset();
            scene.renderPass();
            const bool released = pumpUntil([&stub] { return !stub.releases().empty(); }, 2000);
            check(released, "the last import going is what releases the image");
            check(stub.releases().size() <= 1, "and it is released once, not once per import");
            if (stub.releases().size() != 1) {
                std::fprintf(stderr, "  %zu release(s) for one image imported twice\n", stub.releases().size());
            }
            scene.control.invalidate();
            return true;
        }

        // ---- Phase::DaemonRestart ---- //

        // GOAL: a claim granted by one daemon is never released against its successor. Buffer
        //       ids restart at one when the daemon does, so a release that crossed a restart
        //       would name an image the NEW daemon has already handed to somebody -- this
        //       process included -- and invite it to draw into memory being read.
        // WHY-THIS-SHAPE: the successor is made to hand out a DIFFERENT image under the SAME
        //       buffer id, and the item is made to import it, so the successor's ledger really
        //       does hold an entry the stale claim would match. Without both of those the check
        //       passes for a plugin with no notion of daemon lifetime at all -- the stale
        //       give-back would simply find no entry and be refused for that reason instead.
        //       The colliding id is what puts the guard on the hook.
        // CONTROL: `if (claim.daemon != m_daemon) return false;` in BufferClaims::giveBack
        //       (backend/kde/buffer_claims.cpp) deleted -> "FAIL: the claim the previous daemon
        //       granted is not released against its successor", printing "it released monitor 0
        //       image 1 against the new daemon". Reverted.
        //       MEASURED, and it is why reportBufferIdAs is here: with that same guard deleted
        //       and the successor left to report its own buffer id, this phase stays GREEN. The
        //       stale claim then matches no entry and is refused for a reason that has nothing
        //       to do with daemon lifetime, so the control would have attested a guard that was
        //       doing nothing.
        // COVERAGE-ASSERT: the successor is required to have served the item a frame carrying
        //       the same buffer id, and the item to have replaced its texture. A run where the
        //       hand-over never happened has no entry for the stale claim to collide with, and
        //       fails here rather than reporting the guard as established.
        QDBusConnection successorBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                                     QStringLiteral("wallpaperx-successor-daemon"));
        // A different image reporting the FIRST one's buffer id: what a restarted daemon hands
        // out, since its ids begin again at one.
        ExportingStub successor(device, std::vector<const wallpaperx::OffscreenTarget*>{&second});
        successor.reportBufferIdAs(first.bufferId);

        bus.unregisterObject(QString::fromUtf8(kPath));
        const bool handedOver = successorBus.isConnected() &&
                                bus.unregisterService(QString::fromUtf8(kService)) &&
                                successorBus.registerService(QString::fromUtf8(kService)) &&
                                successorBus.registerVirtualObject(QString::fromUtf8(kPath), &successor);
        if (!handedOver) {
            wallpaperx::testing::harnessFailure(
                "io.wallpaperx.Daemon could not be moved to a second bus connection, so no daemon replacement "
                "could be staged: " +
                successorBus.lastError().message().toStdString());
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }

        // The item's own service watcher is what notices, and re-fetching is what it does next.
        const bool tookSuccessorFrame = pumpUntil(
            [&scene, &successor, &firstTextureDestroyed] {
                scene.renderPass();
                return successor.served() > 0 && firstTextureDestroyed && itemTexture(scene.item) != nullptr;
            },
            10000);
        check(tookSuccessorFrame, "the item fetched and imported a frame from the daemon that replaced the first");
        if (!tookSuccessorFrame) {
            std::fprintf(stderr, "  the successor was asked %d time(s) and served %d; without its own claim on the "
                                 "same image there is nothing for a stale release to collide with\n",
                         successor.requests(), successor.served());
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }

        // Replacing the texture destroyed the first one, which is where the previous daemon's
        // claim lived. Nothing may have been sent to the successor.
        pumpFor(300);
        check(successor.releases().empty(),
              "the claim the previous daemon granted is not released against its successor");
        if (!successor.releases().empty()) {
            std::fprintf(stderr, "  it released monitor %d image %llu against the new daemon\n",
                         successor.releases().front().monitorId,
                         static_cast<unsigned long long>(successor.releases().front().bufferId));
        }

        // And the successor's own claim still works, so the guard is not "refuse everything
        // after a restart" -- which would leak an image per restart instead of per process.
        scene.root.reset();
        scene.renderPass();
        const bool successorReleased = pumpUntil([&successor] { return !successor.releases().empty(); }, 2000);
        check(successorReleased, "the successor's own hand-over is still released normally");
        if (!successorReleased) {
            std::fprintf(stderr, "  the image the new daemon handed over was never released\n");
        }
        scene.control.invalidate();
        return true;
    });
}

Phase phaseFromArgs(int argc, char** argv, bool& recognised) {
    recognised = true;
    if (argc > 1 && std::strcmp(argv[1], "repeat-import") == 0) return Phase::RepeatImport;
    if (argc > 1 && std::strcmp(argv[1], "dropped-frame") == 0) return Phase::DroppedFrame;
    if (argc > 1 && std::strcmp(argv[1], "daemon-restart") == 0) return Phase::DaemonRestart;
    recognised = false;
    return Phase::RepeatImport;
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_release_ledger_test";

    wallpaperx::testing::registerFrameReplyTypes();

    // Set before any window exists: the item imports only when the scene graph reports OpenGL,
    // and Qt picks the API at first window creation.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);

    bool recognised = false;
    const Phase phase = phaseFromArgs(argc, argv, recognised);
    if (!recognised) {
        // A harness fault, not a skip: an entry that names a phase this binary does not have
        // would otherwise run the first one twice and report both as covered.
        wallpaperx::testing::harnessFailure("no phase was named on the command line; expected one of repeat-import, "
                                            "dropped-frame, daemon-restart");
        (void)wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
        return 1;
    }

    bool ok = false;
    try {
        ok = runPhase(phase);
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
    // unjudged.
    if (g_ungradedValidation > 0) {
        std::fprintf(stderr,
                     "item_release_ledger_test: the validation grade could not be made -- "
                     "VK_LAYER_KHRONOS_validation is not loaded. The checks that ran are still judged; the "
                     "GPU-side cleanliness of this run is not.\n");
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
