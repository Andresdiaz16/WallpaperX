// SPDX-License-Identifier: GPL-2.0-or-later

// What the KDE plugin does when its item STOPS BEING ON THE SCREEN, and what it tells the daemon
// when the desktop stops drawing what it hands over.
//
// Nothing else in this suite has ever hidden a plugin item, so before this file the whole
// off-screen path was ungraded -- and it is the path that decides whether a screen's images ever
// come back. The daemon records a hold against the PROCESS, not against the item, and takes
// nothing back on its own: an image an item stops showing without giving back is an image that
// monitor never gets again, and every later hand-over on it is refused for the life of the
// session. That reads on the desktop as a wallpaper that freezes and never recovers.
//
// Two phases, each in a process of its own (argv[1]), because the plugin's release ledger and its
// record of which daemon it is talking to are process-wide singletons with no reset:
//
//   hidden  -- an item taken off the screen gives back every image it holds, keeps nothing
//              importable (no pending frame, no node, no "a frame is on screen" claim), fetches
//              nothing while it is away, and draws again when it comes back. And the one thing
//              that makes that safe: a reader left over from before the hide cannot give back an
//              image the item has since been handed AGAIN -- the ring reuses buffer ids, so the
//              stale reader's claim names exactly the image now on screen.
//
//   painted -- the item tells the daemon whether the desktop is actually drawing what it gives it.
//              Qt goes on calling an occluded window's item visible, so the only signal is whether
//              scene-graph passes this item asked for arrive. A window that stops running passes
//              is what a lock screen, a fullscreen window or another session look like from inside
//              plasmashell.
//
// Like the other importing item tests, this needs a frame the importer ACCEPTS, so it reports NOT
// VERIFIED when this host cannot pair a GL context with the Vulkan device that produced the
// export: an opaque fd is importable only by the driver that made it.

#include "kde/frame_import.h"
#include "support/validation_grade.h"
// For vulkanDeviceUuid only; this test brings up its own GL through QQuickRenderControl.
#include "tests/gl_import_harness.h"
#include "tests/item_stub_scene.h"
#include "tests/private_test_bus.h"
#include "tests/test_report.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
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
using wallpaperx::testing::claimsFrameShown;
using wallpaperx::testing::ExportingStub;
using wallpaperx::testing::glDeviceUuid;
using wallpaperx::testing::itemPaintNode;
using wallpaperx::testing::itemTexture;
using wallpaperx::testing::kFirstMonitor;
using wallpaperx::testing::kHeight;
using wallpaperx::testing::kInterface;
using wallpaperx::testing::kPath;
using wallpaperx::testing::kService;
using wallpaperx::testing::kWidth;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::publishOneFrame;
using wallpaperx::testing::vulkanDeviceUuid;

enum class Phase { Hidden, Painted };

const char* goalFor(Phase phase) {
    switch (phase) {
        case Phase::Hidden:
            return "S13.162-S13.166 (an item taken off the screen gives back every image it holds, keeps nothing "
                   "importable, asks for nothing while it is away, draws again when it comes back, and a reader "
                   "from before the hide cannot free the image it has since been re-handed)";
        case Phase::Painted:
            return "S13.167 (the item tells the daemon when the desktop has stopped drawing what it hands over, and "
                   "when it starts again)";
    }
    return "an unnamed goal";
}

// Nonzero when the validation layer was absent. Its own counter, because charging that to
// failures turns a deliberate skip into exit 1 and blames the plugin for a missing package.
int g_ungradedValidation = 0;


// The stub the other item tests use, plus the three screen reports the item files. Answered rather
// than left to Qt's unknown-method error, and recorded, because what the item tells the daemon
// about being drawn is the whole of the second phase -- and because an errored report makes the
// plugin reset its record and send again, which would turn one answer into a stream of retries.
//
// A subclass rather than a change to ExportingStub itself: four other binaries drive that stub,
// and giving them a daemon that suddenly accepts reports it used to refuse changes what their
// items do for reasons those files never asked for.
class DesktopStub : public ExportingStub {
public:
    using ExportingStub::ExportingStub;

    struct VisibilityReport {
        QString output;
        bool visible = false;
    };

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() == QLatin1String(kInterface)) {
            if (message.member() == QLatin1String("SetOutputSize")) {
                ++m_sizeReports;
                return connection.send(message.createReply());
            }
            // Answered for the reason the other two are: declined, it is logged as a refusal.
            if (message.member() == QLatin1String("SetOutputFocus") ||
                message.member() == QLatin1String("SetOutputFocusState")) {
                return connection.send(message.createReply());
            }
            if (message.member() == QLatin1String("SetOutputVisible")) {
                const QList<QVariant> args = message.arguments();
                if (args.size() >= 2) {
                    m_visibility.push_back(VisibilityReport{args.at(0).toString(), args.at(1).toBool()});
                }
                return connection.send(message.createReply());
            }
        }
        return ExportingStub::handleMessage(message, connection);
    }

    const std::vector<VisibilityReport>& visibility() const { return m_visibility; }
    int sizeReports() const { return m_sizeReports; }

private:
    std::vector<VisibilityReport> m_visibility;
    int m_sizeReports = 0;
};

// Runs the event loop for a fixed span WITHOUT running a scene-graph pass. Its own function rather
// than a parameter, because "pumped but never drew" is the state both phases are built on: in the
// first it is what lets a fetch reply land before the pass that drops the node, and in the second
// it IS the dark screen.
void pumpFor(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// The QML: the plugin's own registered type, with a screen name, because the visibility report is
// filed under one and an item with no name sends nothing at all.
QString offscreenQml() {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: 68
    height: 36
    property Item rendererItem: renderer
    property string publishedReason: renderer.unavailableReason
    property bool rendererDrawing: renderer.frameShown

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
        output: "DP-OFFSCREEN"
    }
}
)");
}

struct Scene {
    QQuickRenderControl control;
    QQuickWindow window{&control};
    QQmlEngine engine;
    std::unique_ptr<QQmlComponent> component;
    std::unique_ptr<QObject> root;
    QQuickItem* item = nullptr;

    // One synchronisation pass, with the GL context dropped again at the end. The doneCurrent() is
    // load-bearing for the reason item_import_release_test states at length: QQuickRenderControl
    // leaves a context current on the GUI thread between frames, which a real compositor never
    // does, and a release that leaked there would still pass.
    void renderPass() {
        control.polishItems();
        control.beginFrame();
        control.sync();
        control.endFrame();
        if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();
    }

    // Pumps and renders until `done`, or gives up. Every caller branches on the answer: an expiry
    // must never read as the thing having happened.
    bool renderUntil(const std::function<bool()>& done, int limitMs) {
        QElapsedTimer clock;
        clock.start();
        while (!done()) {
            if (clock.elapsed() > limitMs) return false;
            renderPass();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return true;
    }

    // Renders and pumps for a fixed span, waiting for nothing. For the one check that asserts a
    // SILENCE: a release that must not happen is posted through the event loop and a fence watcher,
    // so reading the count the instant a pass returns would find it absent whatever the plugin did.
    void renderFor(int ms) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms) {
            renderPass();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }

    // Pumps WITHOUT rendering until `done`. Same contract.
    bool pumpUntil(const std::function<bool()>& done, int limitMs) {
        QElapsedTimer clock;
        clock.start();
        while (!done()) {
            if (clock.elapsed() > limitMs) return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return true;
    }
};

// How many times the stub was told this buffer id is free.
int releasesOf(const DesktopStub& stub, quint64 bufferId) {
    int n = 0;
    for (const ExportingStub::Release& release : stub.releases()) {
        if (release.bufferId == bufferId) ++n;
    }
    return n;
}

std::string describeVisibility(const DesktopStub& stub) {
    std::string out;
    for (const DesktopStub::VisibilityReport& report : stub.visibility()) {
        if (!out.empty()) out += ", ";
        out += report.output.toStdString() + "=" + (report.visible ? "true" : "false");
    }
    return out.empty() ? "none" : out;
}

bool runPhase(Phase phase) {
    const char* kGoal = goalFor(phase);
    return wallpaperx::testing::gradedRun("item_offscreen_test", [phase, kGoal](
                                                                     wallpaperx::testing::ValidationGrade& grade) {
        grade.reportUngradedTo(g_ungradedValidation);
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);
        const wallpaperx::OffscreenTarget& target = targets.create(kFirstMonitor, kWidth, kHeight);
        publishOneFrame(device, targets, target, "item_offscreen_gradient.png");

        QDBusConnection bus = QDBusConnection::sessionBus();
        // The SAME image served over and over, which is what the daemon's ring does: it hands the
        // same buffer id back every few frames. That is not a convenience here -- it is the
        // collision the stale-reader check is about, since the image the item is re-handed after
        // coming back carries the id the reader from before the hide still names.
        // Deliberately long: the painted phase announces frames the way a playing video does, and a
        // stub that ran out would start answering with a plain "no frame" error -- which the item
        // does NOT treat as a hand-over, so the watch that answers "is this desktop drawing" would
        // stop being armed and the phase would grade a plugin that had simply gone quiet.
        DesktopStub stub(device, std::vector<const wallpaperx::OffscreenTarget*>(400, &target));
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
                                   " platform, so no frame could be put on the screen and taken off it");
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

        const bool needsSameDevice = wallpaperx::testing::exportNeedsSameDevice(device, target);
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
        scene.component->setData(offscreenQml().toUtf8(),
                                 QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                     QStringLiteral("/item_offscreen.qml")));
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

        // Everything below is about an item that HAS a frame on the screen, so a run that never
        // imported one must say so rather than fall through into it.
        const bool imported = scene.renderUntil([&scene] { return itemTexture(scene.item) != nullptr; }, 10000);
        if (!imported) {
            std::string whyNot;
            scene.control.polishItems();
            scene.control.beginFrame();
            scene.control.sync();
            const bool supported = wallpaperx::FrameImport::supports(
                stub.handleType() == 1 ? wallpaperx::FrameImport::HandleType::DmaBuf
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

        // Let the item's own startup traffic finish before anything is counted. How many times an
        // item asks while it is joining a scene is Qt's business and not a number this file may
        // pin, so every count below is taken from where this leaves off.
        for (int settle = 0; settle < 10; ++settle) {
            scene.renderPass();
            pumpFor(30);
        }
        check(claimsFrameShown(scene.item), "the item is showing a frame before anything is hidden");
        check(stub.releases().empty(), "and it has released nothing while it is showing it");

        if (phase == Phase::Painted) {
            // GOAL: S13.167 -- the item tells the daemon that this screen is NOT being drawn once
            //       the passes it asks for stop arriving, and tells it the screen is drawn again
            //       when they come back. Qt goes on calling an occluded window's item visible and
            //       the compositor tells the process nothing, so a scene graph that has stopped
            //       running is the only signal there is -- and without the report the daemon keeps
            //       a decoder and a GPU pass per picture running for a locked screen.
            // WHY-THIS-SHAPE: the negative half comes FIRST and is what carries the property: "it
            //       eventually reports false" is equally true of a plugin that reports false on a
            //       timer regardless of what the desktop is doing, which would stop a wallpaper the
            //       user is watching. Only a window in which passes keep running, over a span
            //       longer than the plugin's own deadline, can tell those apart.
            // CONTROL: in backend/kde/wallpaperx_item.cpp, checkPainting()'s `} else if (m_painting
            //       && !m_paintUnconfirmed) { m_paintUnconfirmed = true; } else { m_painting =
            //       false; }` reduced to `} else { m_painting = false; }` -> the first check goes
            //       red only if a pass is ever missed while rendering, which it is not; the
            //       mutation that reddens the FIRST check is `m_paintCount.fetch_add(...)` deleted
            //       from updatePaintNode() -> "FAIL: an item whose passes keep arriving never tells
            //       the daemon the screen is dark". Deleting `reportVisible();` from checkPainting()
            //       -> "FAIL: an item whose requested passes stop arriving tells the daemon the
            //       screen is not being drawn". Both observed 2026-09-20, re-run 2026-10-27. Reverted.
            // A frame announced every so often, which is what a playing video looks like on this
            // interface -- and it is a precondition, not decoration: the item arms its paint watch
            // when the daemon ANSWERS a hand-over, so an item nobody is announcing frames to never
            // asks the question at all. That is correct for a still wallpaper, which has nothing to
            // stop; it means this phase has to drive the video case to reach the watch.
            quint64 announced = 0;
            const auto announce = [&bus, &announced] {
                QDBusMessage ready = QDBusMessage::createSignal(QString::fromUtf8(kPath),
                                                                QString::fromUtf8(kInterface),
                                                                QStringLiteral("FrameReady"));
                ready.setArguments({kFirstMonitor, static_cast<qulonglong>(++announced + 1000)});
                bus.send(ready);
            };

            const size_t reportsBefore = stub.visibility().size();
            QElapsedTimer drawing;
            QElapsedTimer sinceAnnounce;
            drawing.start();
            sinceAnnounce.start();
            while (drawing.elapsed() < 3000) {
                if (sinceAnnounce.elapsed() >= 200) {
                    announce();
                    sinceAnnounce.restart();
                }
                scene.renderPass();
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            }
            bool saidDark = false;
            for (size_t i = reportsBefore; i < stub.visibility().size(); ++i) {
                if (!stub.visibility()[i].visible) saidDark = true;
            }
            check(!saidDark, "an item whose passes keep arriving never tells the daemon the screen is dark");
            if (saidDark) std::fprintf(stderr, "  reports: %s\n", describeVisibility(stub).c_str());

            // Now the dark screen: the window stops running passes, which is what a lock screen, a
            // fullscreen window or another session looks like from inside the process.
            const size_t beforeDark = stub.visibility().size();
            QElapsedTimer sinceLastPass;
            sinceLastPass.start();
            sinceAnnounce.restart();
            const bool reportedDark = scene.pumpUntil(
                [&] {
                    if (sinceAnnounce.elapsed() >= 200) {
                        announce();
                        sinceAnnounce.restart();
                    }
                    for (size_t i = beforeDark; i < stub.visibility().size(); ++i) {
                        if (!stub.visibility()[i].visible) return true;
                    }
                    return false;
                },
                15000);
            const qint64 tookMs = sinceLastPass.elapsed();
            check(reportedDark, "an item whose requested passes stop arriving tells the daemon the screen is not "
                                "being drawn");
            if (!reportedDark) {
                std::fprintf(stderr, "  15 s with no scene-graph pass produced no report; reports so far: %s\n",
                             describeVisibility(stub).c_str());
            } else {
                // GOAL: one missed window is not enough. A visible desktop can miss a pass during a
                //       reconfigure or a mode change, and telling the daemon then costs a visible
                //       freeze and, past the daemon's release interval, the video's place in the
                //       file.
                // WHY-THIS-SHAPE: graded as a LOWER BOUND on how long the report took rather than
                //       by slipping a pass into the second window -- which window a pass lands in
                //       is a race between two timers, and asserting on it would be asserting on
                //       the scheduler. The bound is one-sided: a plugin that reported on the first
                //       empty window fails it, and one that waits longer does not.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, armPaintWatch()'s
                //       `m_paintWatch.start(m_painting ? kPaintDeadlineMs : m_probeMs);` changed to
                //       `m_paintWatch.start(50);` and checkPainting()'s unconfirmed arm removed ->
                //       "FAIL: and it waited for more than one empty window before saying so",
                //       printing about 100 ms. Observed 2026-09-20, re-run 2026-10-27. Reverted.
                std::printf("item_offscreen_test: the dark screen was reported %lld ms after the last pass\n",
                            static_cast<long long>(tookMs));
                std::fflush(stdout);
                check(tookMs >= 900, "and it waited for more than one empty window before saying so");
                if (tookMs < 900) {
                    std::fprintf(stderr, "  it reported after %lld ms, which is inside a single window\n",
                                 static_cast<long long>(tookMs));
                }
            }

            // And back again: passes resume, and the item says so.
            // CONTROL: in backend/kde/wallpaperx_item.cpp, checkPainting()'s `if (drew) { m_painting
            //       = true; m_paintUnconfirmed = false; }` emptied -> "FAIL: and it tells the
            //       daemon the screen is being drawn again once passes come back". Observed
            //       2026-09-20, re-run 2026-10-27. Reverted.
            const size_t beforeBack = stub.visibility().size();
            const bool reportedBack = scene.renderUntil(
                [&stub, beforeBack] {
                    for (size_t i = beforeBack; i < stub.visibility().size(); ++i) {
                        if (stub.visibility()[i].visible) return true;
                    }
                    return false;
                },
                15000);
            check(reportedBack, "and it tells the daemon the screen is being drawn again once passes come back");
            if (!reportedBack) {
                std::fprintf(stderr, "  reports: %s\n", describeVisibility(stub).c_str());
            }

            // GOAL: S15.980 -- when a cover ends on a screen the paint watch had found dark, the item reports
            //       visible at once, without waiting for a pass its slowed probe would ask for.
            // WHY-THIS-SHAPE: no pass runs from the uncover on, so the only way to "visible" is the item's own
            //       assumption; a plugin that left it to the probe never gets there without a pass, whatever
            //       the timing. The dark state is reached afresh, so this does not ride on the step above.
            // CONTROL: via control_batch.sh 2026-10-27, `m_painting = true;` deleted from hiddenByStateChanged
            //       (backend/kde/wallpaperx_item.cpp) -> this line red. Reverted.
            if (reportedBack) {
                const size_t beforeDarkAgain = stub.visibility().size();
                sinceAnnounce.restart();
                const bool darkAgain = scene.pumpUntil(
                    [&] {
                        if (sinceAnnounce.elapsed() >= 200) {
                            announce();
                            sinceAnnounce.restart();
                        }
                        for (size_t i = beforeDarkAgain; i < stub.visibility().size(); ++i) {
                            if (!stub.visibility()[i].visible) return true;
                        }
                        return false;
                    },
                    15000);
                if (!darkAgain) {
                    notVerified(kGoal, "the screen was not reported dark a second time, so the uncover step had no "
                                       "dark screen to start from");
                } else {
                    scene.item->setProperty("desktopCovered", true);
                    pumpFor(300);
                    const size_t beforeUncover = stub.visibility().size();
                    scene.item->setProperty("desktopCovered", false);
                    const bool shownAtOnce = scene.pumpUntil(
                        [&] {
                            for (size_t i = beforeUncover; i < stub.visibility().size(); ++i) {
                                if (stub.visibility()[i].visible) return true;
                            }
                            return false;
                        },
                        2000);
                    check(shownAtOnce, "a cover ending on a dark screen reports it visible with no pass having run");
                    if (!shownAtOnce) std::fprintf(stderr, "  reports: %s\n", describeVisibility(stub).c_str());
                }
            }

            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }

        // ---- Phase::Hidden ---- //

        const quint64 shownBufferId = target.bufferId;

        // GOAL: S13.162 -- an item taken off the screen gives back every image it holds. The daemon
        //       records a hold against this PROCESS and takes nothing back on its own, so an image
        //       the item stops showing and does not release is one that monitor never gets again:
        //       every later hand-over on it is refused for the life of the session, and the screen
        //       freezes with no way back. False for the shape the plugin had before Stage E4, where
        //       the only release was the scene graph destroying the texture -- which a desktop that
        //       HIDES a wallpaper rather than tearing it down may never do.
        // WHY-THIS-SHAPE: driven by hiding the item, not by destroying it or taking it out of the
        //       window. Destroying it is already covered elsewhere and takes the texture with it,
        //       which releases through the other path entirely; hiding is the case that had no
        //       release at all, and it is what a desktop does when it switches wallpaper plugins or
        //       a containment stops being shown. No render pass is run between the hide and the
        //       reading, so what is observed is the hide's own doing and not a teardown's.
        // CONTROL: in backend/kde/wallpaperx_item.cpp, the `for (const BufferClaims::Claim& claim :
        //       retired->removeAll()) releaseClaim(claim);` loop deleted from releaseHeldImages()
        //       -> "FAIL: an item taken off the screen gives the image it was showing back",
        //       printing 0 releases. Observed 2026-09-20. Reverted.
        scene.item->setVisible(false);
        const bool released = scene.pumpUntil([&stub] { return !stub.releases().empty(); }, 5000);
        check(released, "an item taken off the screen gives the image it was showing back");
        if (!released) {
            std::fprintf(stderr, "  %zu release(s) arrived in five seconds after the item was hidden\n",
                         stub.releases().size());
        } else {
            // CONTROL: backend/kde/wallpaperx_item.cpp:531, releaseClaim's queued call sends ReleaseBuffer twice,
            //       control_batch 2026-10-15, item_offscreen_hidden -> "FAIL: and it names the image it was showing,
            //       exactly once", printing "image 1 was released 2 time(s)", with S13.166's "exactly once more";
            //       restored, green. Calling releaseClaim twice instead stays green: the claim ledger gives the
            //       second call nothing to send.
            check(releasesOf(stub, shownBufferId) == 1,
                  "and it names the image it was showing, exactly once");
            if (releasesOf(stub, shownBufferId) != 1) {
                std::fprintf(stderr, "  image %llu was released %d time(s)\n",
                             static_cast<unsigned long long>(shownBufferId), releasesOf(stub, shownBufferId));
            }
        }

        // GOAL: S13.163 -- and it keeps nothing importable. The pending frame is dropped, the node
        //       is destroyed by the next pass, and the item stops claiming a frame is on the
        //       screen. All three matter together: an item that gave the image back but went on
        //       reporting a frame leaves main.qml hiding its message panel over a wallpaper that is
        //       no longer there, and a node still pointing at that image is a live reader of memory
        //       the daemon has just been invited to draw into.
        // WHY-THIS-SHAPE: the claim is read BEFORE any pass runs and the node AFTER one, because
        //       they are withdrawn by different things -- the claim by the hide itself, the node by
        //       the pass the hide asks for -- and reading both at one moment would let a missing
        //       one hide behind the other.
        // CONTROL: in backend/kde/wallpaperx_item.cpp, the `dropShownFrame();` call deleted from
        //       updateOnScreen()'s off-screen branch -> "FAIL: and it stops claiming a frame is on
        //       the screen" and "FAIL: and the next pass destroys the node that was drawing it".
        //       Deleting only the `m_nodeRequest = NodeRequest::Destroy;` line inside
        //       dropShownFrame() reddens the node line, and the later "and says a frame is on the
        //       screen again", in item_offscreen_hidden. Both re-run 2026-09-23. Reverted.
        check(!claimsFrameShown(scene.item), "and it stops claiming a frame is on the screen");
        scene.renderPass();
        check(itemPaintNode(scene.item) == nullptr, "and the next pass destroys the node that was drawing it");

        // GOAL: S13.164 -- and it asks for nothing while it is away, EVEN WHILE FRAMES ARE BEING
        //       ANNOUNCED. Every frame fetched takes an image out of the ring for a wallpaper
        //       nobody is looking at, and an item that goes on fetching while hidden re-acquires
        //       exactly what it just gave back -- on a playing video, several times a second.
        // WHY-THIS-SHAPE: the announcements are the check, not decoration. MEASURED: with nothing
        //       announced, a hidden item asks for nothing whatever the guard says -- there is
        //       simply no trigger -- and deleting the guard leaves the check green. A playing video
        //       announces a frame per picture, which is when the guard is the only thing standing
        //       between a hidden item and a ring it is quietly emptying.
        //       It is a silence, so its worth rests on the same item being shown to ask: it
        //       imported a frame before the hide, which takes a request, and it is required to ask
        //       again after the show, in this same run.
        // CONTROL: in backend/kde/wallpaperx_item.cpp, the `|| !m_onScreen` term deleted from
        //       fetchFrame()'s first guard -> "FAIL: and it asks the daemon for nothing while it is
        //       off the screen, however many frames are announced", printing the extra requests.
        //       Observed 2026-09-20; re-run 2026-10-05 via control_batch.sh. Reverted.
        const int requestsAtHide = stub.requests();
        quint64 announcedWhileHidden = 0;
        {
            QElapsedTimer hiddenFor;
            hiddenFor.start();
            while (hiddenFor.elapsed() < 1500) {
                QDBusMessage ready = QDBusMessage::createSignal(QString::fromUtf8(kPath),
                                                                QString::fromUtf8(kInterface),
                                                                QStringLiteral("FrameReady"));
                ready.setArguments({kFirstMonitor, static_cast<qulonglong>(++announcedWhileHidden + 5000)});
                bus.send(ready);
                pumpFor(100);
            }
        }
        scene.renderPass();
        const int askedWhileHidden = stub.requests() - requestsAtHide;
        check(askedWhileHidden == 0,
              "and it asks the daemon for nothing while it is off the screen, however many frames are announced");
        if (askedWhileHidden != 0) {
            std::fprintf(stderr, "  it asked %d more time(s) while hidden\n", askedWhileHidden);
        }

        // GOAL: S13.165 -- coming back re-fetches and draws.
        // CONTROL: in backend/kde/wallpaperx_item.cpp, the `fetchFrame(0);` call deleted from
        //       updateOnScreen()'s on-screen branch -> "FAIL: an item put back on the screen asks
        //       the daemon for a frame again", printing that the stub served nothing more.
        //       Observed 2026-09-20. Reverted.
        const int servedBeforeShow = stub.served();
        scene.item->setVisible(true);
        const bool reFetched = scene.pumpUntil([&stub, servedBeforeShow] { return stub.served() > servedBeforeShow; },
                                               5000);
        check(reFetched, "an item put back on the screen asks the daemon for a frame again");
        if (!reFetched) {
            std::fprintf(stderr, "  it asked %d time(s) and the stub served %d after the item came back\n",
                         stub.requests(), stub.served());
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }
        const bool redrawn = scene.renderUntil([&scene] { return itemTexture(scene.item) != nullptr; }, 10000);
        check(redrawn, "and it draws the frame it is handed");
        check(claimsFrameShown(scene.item), "and says a frame is on the screen again");
        if (!redrawn) {
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }

        // GOAL: S13.166 -- the same image comes and goes without the count drifting: hiding the
        //       item a second time gives it back again, exactly once more, and showing it again
        //       gives nothing back. What this rules out is a plugin whose release path works the
        //       first time and then either stops -- the image is gone for the session -- or
        //       doubles, which tells the daemon an image on screen is free to draw into.
        // WHY-THIS-SHAPE: read against a count taken after the FIRST show-and-draw, so it is a
        //       statement about the second cycle rather than a repeat of the first. The "nothing
        //       more was released" reading is taken after a settling window, because a release
        //       from a dying texture travels through the event loop and a fence watcher.
        //
        //       NOT GRADED HERE, and stated because it is the check a reviewer would expect
        //       beside these: that the claim set is RETIRED rather than emptied, so a reader left
        //       over from before a hide cannot free the image the item has since been re-handed.
        //       It could not be made to fail through a real item. MEASURED: with the retirement
        //       removed, this whole binary stayed green and the claim set was empty at the texture
        //       teardown in both builds -- by then the item's own bookkeeping has emptied it, so
        //       the two shapes produce identical traffic. backend/tests/buffer_claims_test.cpp grades
        //       that sets are independent (a retired set cannot give back its replacement's claim)
        //       and records the same gap for the exchange itself.
        // CONTROL: in backend/kde/wallpaperx_item.cpp, the `for (const BufferClaims::Claim& claim :
        //       retired->removeAll()) releaseClaim(claim);` loop deleted from releaseHeldImages()
        //       -> "FAIL: hiding it a second time gives the image back again" as well as the first
        //       hide's own line. Observed 2026-09-20. Reverted.
        const int releasesBeforeSecondHide = releasesOf(stub, shownBufferId);
        scene.item->setVisible(false);
        const bool releasedAgain = scene.pumpUntil(
            [&] { return releasesOf(stub, shownBufferId) > releasesBeforeSecondHide; }, 5000);
        check(releasedAgain, "hiding it a second time gives the image back again");
        const int releasesAfterSecondHide = releasesOf(stub, shownBufferId);
        // CONTROL: backend/kde/wallpaperx_item.cpp:531, releaseClaim's queued call sends ReleaseBuffer twice,
        //       control_batch 2026-10-15, item_offscreen_hidden -> "FAIL: and exactly once more, not twice",
        //       printing "image 1 was released 4 time(s), 2 of them before the second hide"; restored, green.
        check(releasesAfterSecondHide == releasesBeforeSecondHide + 1, "and exactly once more, not twice");
        if (releasesAfterSecondHide != releasesBeforeSecondHide + 1) {
            std::fprintf(stderr, "  image %llu was released %d time(s), %d of them before the second hide\n",
                         static_cast<unsigned long long>(shownBufferId), releasesAfterSecondHide,
                         releasesBeforeSecondHide);
        }
        const int servedBeforeSecondShow = stub.served();
        scene.item->setVisible(true);
        const bool reFetchedAgain = scene.pumpUntil(
            [&stub, servedBeforeSecondShow] { return stub.served() > servedBeforeSecondShow; }, 5000);
        check(reFetchedAgain, "and it is handed that same image again when it comes back");
        scene.renderPass();
        const bool drawnAgain = scene.renderUntil([&scene] { return itemTexture(scene.item) != nullptr; }, 10000);
        check(drawnAgain, "and draws it");
        // Long enough for a release to arrive if one were owed: a release from a dying texture is
        // posted through the event loop and a fence watcher, so reading the count the instant a
        // pass returns would find it absent whatever the plugin did.
        scene.renderFor(800);
        // CONTROL: backend/kde/wallpaperx_item.cpp, after :1214 in the show path, a 400 ms timer on the third show
        //       releasing every held claim, control_batch 2026-10-15, item_offscreen_hidden -> "FAIL: and showing it
        //       again releases nothing -- the item is holding that image, not giving it back", with "FAIL: and the
        //       image it holds after coming back is still released normally"; restored, green.
        check(releasesOf(stub, shownBufferId) == releasesAfterSecondHide,
              "and showing it again releases nothing -- the item is holding that image, not giving it back");

        // And the guard is not "refuse every release after a hide", which would leak an image per
        // hide instead of per process: the claim the item holds NOW is given back normally.
        const int releasesBeforeTeardown = releasesOf(stub, shownBufferId);
        scene.root.reset();
        scene.renderPass();
        const bool releasedAtEnd =
            scene.pumpUntil([&] { return releasesOf(stub, shownBufferId) > releasesBeforeTeardown; }, 5000);
        check(releasedAtEnd, "and the image it holds after coming back is still released normally");
        scene.control.invalidate();
        return true;
    });
}

Phase phaseFromArgs(int argc, char** argv, bool& recognised) {
    recognised = true;
    if (argc > 1 && std::strcmp(argv[1], "hidden") == 0) return Phase::Hidden;
    if (argc > 1 && std::strcmp(argv[1], "painted") == 0) return Phase::Painted;
    recognised = false;
    return Phase::Hidden;
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_offscreen_test";
    // X.34: nothing owned or sent off the private bus.
    if (const std::optional<int> refused = wallpaperx::testing::refuseOffPrivateTestBus()) return *refused;

    wallpaperx::testing::registerFrameReplyTypes();

    // Set before any window exists: the item imports only when the scene graph reports OpenGL,
    // and Qt picks the API at first window creation.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);

    bool recognised = false;
    const Phase phase = phaseFromArgs(argc, argv, recognised);
    if (!recognised) {
        // A harness fault, not a skip: an entry naming a phase this binary does not have would
        // otherwise run the first one twice and report both as covered.
        wallpaperx::testing::harnessFailure("no phase was named on the command line; expected one of hidden, painted");
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
    if (g_ungradedValidation > 0) {
        std::fprintf(stderr,
                     "item_offscreen_test: the validation grade could not be made -- "
                     "VK_LAYER_KHRONOS_validation is not loaded. The checks that ran are still judged; the "
                     "GPU-side cleanliness of this run is not.\n");
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
