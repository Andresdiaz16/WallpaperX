// SPDX-License-Identifier: GPL-2.0-or-later

// When the KDE plugin tells the daemon it has stopped reading an image, and when it must not.
//
// The daemon keeps a fixed ring of images per monitor, will not hand over the last free one, and
// takes nothing back from a consumer -- so an image a consumer never gives back is an image the
// producer never gets, for the life of the connection. Nothing about that is
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
//   repeat-import  -- one release per image, not per hand-over. The daemon hands the image the
//                     item is already showing over again as its ring cycles, so the item briefly
//                     holds two claims on one image, and the daemon may only be told when the last
//                     of those claims is gone.
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
#include "tests/private_test_bus.h"
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

enum class Phase {
    RepeatImport,
    RepeatMany,
    DroppedFrame,
    InFlightMonitorChange,
    DaemonRestart,
    LateReply,
    LateReplyAlive,
    LateReplyReplaced
};

const char* goalFor(Phase phase) {
    switch (phase) {
        case Phase::RepeatImport:
            return "S12.15, S13.201 and S15.345 (the image is given back when the LAST live claim on it is gone, and "
                   "only hand-overs below the newest before that)";
        case Phase::RepeatMany:
            return "S15.345 (re-fetches of an image held all the while never reach the daemon's descriptor cap)";
        case Phase::LateReplyReplaced:
            return "S15.324 (a daemon that replaced one whose GetFrame went unanswered is asked at once)";
        case Phase::DroppedFrame:
            return "S12.16 (a frame fetched and dropped without being imported is still released)";
        case Phase::InFlightMonitorChange:
            return "a reply that ARRIVES for a monitor this item has stopped showing is released rather than "
                   "dropped with the message";
        case Phase::DaemonRestart:
            return "S12.17 (a claim granted by one daemon is never released against its successor)";
        case Phase::LateReply:
            return "S15.309 (a reply arriving after its item is destroyed is taken and released by number)";
        case Phase::LateReplyAlive:
            return "S15.324 (one unanswered GetFrame per item per daemon lifetime; a reply arriving after the item "
                   "stopped waiting for it is released by number, not drawn)";
    }
    return "an unnamed goal";
}

// Nonzero when the validation layer was absent. Its own counter, because charging that to
// failures turns a deliberate skip into exit 1 and blames the plugin for a missing package.
int g_ungradedValidation = 0;

// Mirrored from WallpaperManager::kMaxDescriptorsPerClient, which the plugin does not link.
constexpr int kExpectedDescriptorCap = 64;

// One more forced re-fetch than the cap has descriptors, so even replies of one descriptor each
// would reach it if nothing were given back.
constexpr int kForcedRefetches = kExpectedDescriptorCap + 1;

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

        // The phases that hold a reply put the stub on a connection of their own: QtDBus answers a
        // call to a service on the caller's own connection locally, with the reply the handler built,
        // so a reply the stub meant to hold is delivered at once.
        const bool holdsReplies = phase == Phase::InFlightMonitorChange || phase == Phase::LateReply ||
                                  phase == Phase::LateReplyAlive || phase == Phase::LateReplyReplaced;
        QDBusConnection bus = holdsReplies ? QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                                           QStringLiteral("wallpaperx-ledger-stub"))
                                           : QDBusConnection::sessionBus();
        // The image sequence this phase's daemon serves, all of it the SAME image, which is what
        // the plugin asks for: it re-fetches the frame it is already showing on every scene
        // change. Deliberately longer than the phases drive, because how many times the item
        // asks while it is joining a scene is Qt's business and not a number to pin -- the
        // phases below settle first and then count from where they find themselves. The
        // dropped-frame phase wants exactly one served and a refusal after it, so it takes one.
        const bool oneFrameOnly = phase == Phase::DroppedFrame || holdsReplies || phase == Phase::RepeatMany;
        std::vector<const wallpaperx::OffscreenTarget*> sequence(oneFrameOnly ? 1 : 4, &first);
        ExportingStub stub(device, sequence);
        // The same image for every request, as long as it is asked, charged as the daemon charges.
        if (phase == Phase::RepeatMany) {
            stub.rotate();
            stub.enforceDescriptorCap(kExpectedDescriptorCap);
        }
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

        // Armed BEFORE the item exists: the item's first fetch can complete during construction,
        // because subscribing to FrameReady is a blocking call to the bus daemon and Qt
        // dispatches incoming messages while one is outstanding.
        if (holdsReplies) stub.holdNextReply();

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

        const auto announce = [&bus](quint64 generation) {
            QDBusMessage ready = QDBusMessage::createSignal(QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                                                            QStringLiteral("FrameReady"));
            ready.setArguments({kFirstMonitor, static_cast<qulonglong>(generation)});
            bus.send(ready);
        };
        const auto reasons = [&scene] { return scene.root->property("reasonsSeen").toString(); };

        if (phase == Phase::LateReplyAlive) {
            // GOAL: S15.324, restated -- an item has at most one unanswered GetFrame per daemon lifetime:
            //       once its own 25 s patience runs out it says "could not be reached" (not before 20 s),
            //       and announcements meanwhile send nothing more to that daemon; when the late reply
            //       comes it is read, given back naming its own hand-over and never drawn, and only then
            //       is the fetch the announcements queued sent.
            // WHY-THIS-SHAPE: the stub holds the first reply past the deadline with the item alive,
            //       which is the only state that reaches the abandoned branch, and four announcements are
            //       sent after the deadline, each given 300 ms, so an item asking again would be seen.
            //       The reasons are read from the QML record of every change. The deadline is only
            //       bounded below: a timer never fires early, but a loaded host may fire it late.
            // CONTROL: in WallpaperXItem::fetchFrame (backend/kde/wallpaperx_item.cpp), via control_batch.sh,
            //       2026-10-06, each reverted: (a) `bus.asyncCall(request, kNoReplyTimeout)` made
            //       `bus.asyncCall(request)` -> "FAIL: while its GetFrame is unanswered, announcements send
            //       nothing more to that daemon (5 more request(s) after the deadline)" and "FAIL: a reply
            //       arriving after the item stopped waiting is released, naming its own hand-over (0
            //       release(s) naming 1, 0 in all)"; (b) the `releaseFrameReply(...)` in the
            //       `!patience->isActive()` branch deleted -> the second line alone; (c) that branch made to
            //       call `answered(done->reply())` -> the second line and "FAIL: and it is never drawn (1
            //       import(s) after it arrived)"; (d) the `if (m_unansweredLifetime == asked.lifetime) {
            //       queue(); return; }` guard deleted -> the first line alone; (e) the `fetchFrame(*queued)`
            //       in that branch deleted -> "FAIL: and the fetch the announcements queued is sent once the
            //       late reply comes".
            // COVERAGE-ASSERT: the reply must have been held before anything is counted, and the
            //       announcements sent after the deadline.
            const bool held = pumpUntil([&stub] { return stub.replyIsHeld(); }, 5000);
            if (!held) {
                wallpaperx::testing::harnessFailure("the item's first request never arrived to be held");
                scene.control.invalidate();
                return true;
            }
            const quint64 heldNumber = stub.asks().back().handover;
            const int requestsAtHold = stub.requests();
            QElapsedTimer waited;
            waited.start();
            announce(2);
            const bool gaveUp =
                pumpUntil([&] { return reasons().contains(QStringLiteral("WallpaperX could not be reached.")); }, 40000);
            const qint64 gaveUpAfter = waited.elapsed();
            check(gaveUp && gaveUpAfter >= 20000,
                  ("an item whose request is not answered says it could not be reached once its own patience runs "
                   "out, and not before (after " +
                   std::to_string(gaveUpAfter) + " ms; reasons " + reasons().toStdString() + ")")
                      .c_str());
            for (quint64 generation = 3; gaveUp && generation < 7; ++generation) {
                announce(generation);
                pumpFor(300);
            }
            check(gaveUp && stub.requests() == requestsAtHold,
                  ("while its GetFrame is unanswered, announcements send nothing more to that daemon (" +
                   std::to_string(stub.requests() - requestsAtHold) + " more request(s) after the deadline)")
                      .c_str());
            const quint64 importsBefore = scene.item->property("importCount").toULongLong() +
                                          scene.item->property("reusedImportCount").toULongLong();
            if (!gaveUp || !stub.sendHeldReply()) {
                if (gaveUp) wallpaperx::testing::harnessFailure("the held reply could not be sent");
                scene.root.reset();
                scene.renderPass();
                scene.control.invalidate();
                return true;
            }
            const bool askedAfterReply = pumpUntil([&] { return stub.requests() > requestsAtHold; }, 5000);
            check(askedAfterReply, "and the fetch the announcements queued is sent once the late reply comes");
            pumpUntil([&stub] { return !stub.releases().empty(); }, 5000);
            for (int pass = 0; pass < 5; ++pass) {
                scene.renderPass();
                pumpFor(20);
            }
            std::vector<ExportingStub::Release> late;
            for (const ExportingStub::Release& r : stub.releases()) {
                if (r.handover == heldNumber) late.push_back(r);
            }
            check(late.size() == 1 && late.front().bufferId == first.bufferId,
                  ("a reply arriving after the item stopped waiting is released, naming its own hand-over (" +
                   std::to_string(late.size()) + " release(s) naming " + std::to_string(heldNumber) + ", " +
                   std::to_string(stub.releases().size()) + " in all)")
                      .c_str());
            const quint64 importsAfter = scene.item->property("importCount").toULongLong() +
                                         scene.item->property("reusedImportCount").toULongLong();
            check(importsAfter == importsBefore, ("and it is never drawn (" + std::to_string(importsAfter - importsBefore) +
                                                  " import(s) after it arrived)")
                                                     .c_str());
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }

        if (phase == Phase::LateReplyReplaced) {
            // GOAL: S15.324's other half -- the one-unanswered-GetFrame rule is per daemon lifetime: a
            //       daemon that replaces one whose GetFrame went unanswered past the item's patience is
            //       asked at once, without waiting for the old call's reply.
            // WHY-THIS-SHAPE: the first daemon's reply is held past the deadline and never sent before
            //       the successor is asked about, so only the replacement itself can have produced the
            //       successor's request; the wait for it is bounded and expiring is the failure.
            // CONTROL: in WallpaperXItem::fetchFrame (backend/kde/wallpaperx_item.cpp), the guard
            //       `if (m_unansweredLifetime == asked.lifetime)` made `if (m_unansweredLifetime != 0)`,
            //       so any unanswered call holds every daemon back -> "FAIL: a daemon that replaced one whose
            //       GetFrame went unanswered is asked at once, before that reply comes (0 request(s) to it)".
            //       Via control_batch.sh, 2026-10-06. Reverted.
            // COVERAGE-ASSERT: the item must have given up on the first daemon before it is replaced.
            const bool held = pumpUntil([&stub] { return stub.replyIsHeld(); }, 5000);
            if (!held) {
                wallpaperx::testing::harnessFailure("the item's first request never arrived to be held");
                scene.control.invalidate();
                return true;
            }
            const bool gaveUp =
                pumpUntil([&] { return reasons().contains(QStringLiteral("WallpaperX could not be reached.")); }, 40000);
            if (!gaveUp) {
                check(false, ("an item whose request is not answered gives up on it (reasons " +
                              reasons().toStdString() + ")")
                                 .c_str());
                scene.control.invalidate();
                return true;
            }
            QDBusConnection successorBus = QDBusConnection::connectToBus(
                QDBusConnection::SessionBus, QStringLiteral("wallpaperx-ledger-successor"));
            ExportingStub successor(device, std::vector<const wallpaperx::OffscreenTarget*>{&second});
            struct LeaveBus {
                QDBusConnection& bus;
                ~LeaveBus() {
                    bus.unregisterObject(QString::fromUtf8(kPath));
                    QDBusConnection::disconnectFromBus(QStringLiteral("wallpaperx-ledger-successor"));
                }
            } leave{successorBus};
            const bool handedOver = successorBus.isConnected() &&
                                    bus.unregisterService(QString::fromUtf8(kService)) &&
                                    successorBus.registerVirtualObject(QString::fromUtf8(kPath), &successor) &&
                                    successorBus.registerService(QString::fromUtf8(kService));
            if (!handedOver) {
                wallpaperx::testing::harnessFailure("io.wallpaperx.Daemon could not be moved to a second bus connection: " +
                                                    successorBus.lastError().message().toStdString());
                scene.control.invalidate();
                return true;
            }
            const bool asked = pumpUntil([&successor] { return successor.requests() > 0; }, 5000);
            check(asked && stub.replyIsHeld(),
                  ("a daemon that replaced one whose GetFrame went unanswered is asked at once, before that reply "
                   "comes (" +
                   std::to_string(successor.requests()) + " request(s) to it)")
                      .c_str());
            (void)stub.sendHeldReply();
            pumpFor(100);
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }

        if (phase == Phase::LateReply) {
            // GOAL: S15.309 -- a reply that arrives after the item that asked for it is destroyed is
            //       still taken and given back, naming its own hand-over. The daemon recorded the
            //       hold when it answered, and since releases are numbered, a hand-over nobody names
            //       keeps that image out of the ring until this process leaves the bus.
            // WHY-THIS-SHAPE: the stub holds the item's first reply, the item is destroyed with it
            //       out, and only then is it sent; nothing was imported before, so the one release
            //       that can follow is that reply's.
            // CONTROL: in WallpaperXItem::fetchFrame (backend/kde/wallpaperx_item.cpp), the
            //       `connect(watcher, ...)` whose receiver is the watcher, which gives back a reply whose
            //       item is gone, deleted -> "FAIL: a reply arriving after its item is destroyed is
            //       released, naming its hand-over (0 release(s), hand-over 0 for 1)". Via
            //       control_batch.sh, 2026-10-05 on the NVIDIA entry, with the stub on its own connection
            //       (on the shared one the same break stays green); re-run via control_run.sh 2026-10-05.
            //       Reverted.
            // COVERAGE-ASSERT: the reply is required to have been held before the item went.
            const bool held = pumpUntil([&stub] { return stub.replyIsHeld(); }, 5000);
            if (!held) {
                wallpaperx::testing::harnessFailure("the item's first request never arrived to be held");
                scene.control.invalidate();
                return true;
            }
            const quint64 heldNumber = stub.asks().back().handover;
            scene.root.reset();
            scene.renderPass();
            if (!stub.sendHeldReply()) {
                wallpaperx::testing::harnessFailure("the held reply could not be sent");
                scene.control.invalidate();
                return true;
            }
            const bool released = pumpUntil([&stub] { return !stub.releases().empty(); }, 5000);
            const bool named = released && stub.releases().size() == 1 &&
                               stub.releases().front().bufferId == first.bufferId &&
                               stub.releases().front().handover == heldNumber;
            check(named, ("a reply arriving after its item is destroyed is released, naming its hand-over (" +
                          std::to_string(stub.releases().size()) + " release(s), hand-over " +
                          std::to_string(stub.releases().empty() ? 0 : stub.releases().front().handover) + " for " +
                          std::to_string(heldNumber) + ")")
                             .c_str());
            scene.control.invalidate();
            return true;
        }

        if (phase == Phase::InFlightMonitorChange) {
            // GOAL: a reply that ARRIVES for a monitor this item has stopped showing gives its
            //       image back. The daemon records the hold when it answers, so a reply the item
            //       drops with the message costs that monitor an image until this process leaves
            //       the bus -- nothing is ever taken back from a consumer.
            // WHY-THIS-SHAPE: the stub HOLDS the reply while the monitor changes, which is the
            //       only way to reach this state at all: with a stub that answers synchronously
            //       the reply is always parsed for the monitor it was asked for, and the item
            //       lands in the path item_release_ledger's dropped-frame phase already covers --
            //       a reply it took and then walked away from, which is a different piece of
            //       code and a different failure. The distinction is exactly what makes this
            //       phase worth having: dropped-frame grades the PendingFrame destructor, this
            //       one grades the branch that never builds a PendingFrame.
            // CONTROL: the `else { releaseFrameReply(requestedMonitor, reply, asked.lifetime); }` arm
            //       in WallpaperXItem::fetchFrame's reply handler (backend/kde/wallpaperx_item.cpp)
            //       emptied -> "FAIL: a reply for a monitor the item has stopped showing is released".
            //       Via control_run.sh 2026-10-05, with the stub on a connection of its own; on the shared
            //       connection the same break stays green, the reply delivered at once and released by
            //       the dropped-frame path. Reverted.
            // COVERAGE-ASSERT: the reply is required to have been HELD and then sent, and the
            //       item required to have moved monitors before it was, so a run where the stub
            //       answered straight away cannot report the branch as established -- it would
            //       have exercised the dropped-frame path instead.
            const bool held = pumpUntil([&stub] { return stub.replyIsHeld(); }, 5000);
            check(held, "the stub is holding a reply the item asked for, so one is genuinely in flight");
            if (!held) {
                std::fprintf(stderr, "  the item asked %d time(s) and the stub served %d; nothing was in flight, so "
                                     "the monitor change below would grade the dropped-reply path instead\n",
                             stub.requests(), stub.served());
                scene.root.reset();
                scene.renderPass();
                scene.control.invalidate();
                return true;
            }

            const quint64 heldNumber = stub.asks().back().handover;
            scene.item->setProperty("monitorId", kSecondMonitor);
            check(scene.item->property("monitorId").toInt() == kSecondMonitor,
                  "the item has moved to another monitor while that reply is still in flight");
            check(stub.releases().empty(), "and nothing has been released yet, because the reply has not arrived");

            const bool sent = stub.sendHeldReply();
            if (!sent) {
                wallpaperx::testing::harnessFailure("the held reply could not be sent, so the item never received "
                                                    "the hand-over this phase is about");
                scene.root.reset();
                scene.renderPass();
                scene.control.invalidate();
                return true;
            }

            const bool released = pumpUntil([&stub] { return !stub.releases().empty(); }, 5000);
            check(released, "a reply for a monitor the item has stopped showing is released");
            if (!released) {
                std::fprintf(stderr, "  0 release(s) arrived in five seconds after the in-flight reply landed\n");
            } else {
                const ExportingStub::Release& sent2 = stub.releases().front();
                check(sent2.monitorId == kFirstMonitor && sent2.bufferId == first.bufferId,
                      "and it names the monitor and image the reply carried, not the one the item moved to");
                // GOAL: S15.310 -- and the hand-over that reply carried, so the daemon ends the claim
                //       that reply made rather than an older one.
                // CONTROL: in WallpaperXItem::takeGranted (backend/kde/wallpaperx_item.cpp),
                //       `args.at(kHandoverField)` made `args.at(kAcquireValueField)` -> "FAIL: and the
                //       hand-over that reply carried (named 0 for 1)", here and in dropped-frame. Via
                //       control_batch.sh on the NVIDIA entry, re-run 2026-10-06 here. Reverted.
                check(sent2.handover == heldNumber,
                      ("and the hand-over that reply carried (named " + std::to_string(sent2.handover) + " for " +
                       std::to_string(heldNumber) + ")")
                          .c_str());
                if (sent2.monitorId != kFirstMonitor || sent2.bufferId != first.bufferId) {
                    std::fprintf(stderr, "  it released monitor %d image %llu; the reply carried monitor %d image "
                                         "%llu\n",
                                 sent2.monitorId, static_cast<unsigned long long>(sent2.bufferId), kFirstMonitor,
                                 static_cast<unsigned long long>(first.bufferId));
                }
            }
            scene.root.reset();
            scene.renderPass();
            scene.control.invalidate();
            return true;
        }

        if (phase == Phase::DroppedFrame) {
            // GOAL: a frame the plugin fetched and then dropped without importing is released.
            //       The daemon records the hold when it ANSWERS GetFrame, so a reply the
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
            // CONTROL: `releaseUnimported(claims, claim);` deleted from
            //       WallpaperXItem::PendingFrame::~PendingFrame (backend/kde/wallpaperx_item.cpp)
            //       -> "FAIL: a frame dropped without ever being imported is released", printing
            //       "0 release(s) arrived after the item moved to another monitor". Re-observed
            //       2026-09-23 and 2026-10-05 (via control_batch.sh). Reverted.
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
                check(sent.handover == stub.asks().front().handover,
                      ("and the hand-over that reply carried (named " + std::to_string(sent.handover) + " for " +
                       std::to_string(stub.asks().front().handover) + ")")
                          .c_str());
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
        // frame, nothing released has named its newest hand-over -- only replies a newer one of the same
        // image superseded, if the item asked more than once while joining the scene. A run that
        // reached here having ended the claim would leave those counts describing a different state.
        {
            quint64 newestAtSettle = 0;
            for (const ExportingStub::Ask& ask : stub.asks()) newestAtSettle = std::max(newestAtSettle, ask.handover);
            size_t ended = 0;
            for (const ExportingStub::Release& r : stub.releases()) {
                if (r.handover >= newestAtSettle) ++ended;
            }
            check(ended == 0, "nothing released has ended the claim while the item is showing the frame");
        }

        // Whether that first texture has been destroyed, watched rather than inferred from the
        // item's texture pointer changing: once destroyed, its address is the allocator's to hand
        // straight back, so comparing pointers afterwards can read a new import as no change at all.
        // The daemon-restart phase reads it: the node drops the imports of a daemon that has gone,
        // which is when this texture goes.
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
            // GOAL: S12.15, restated by S13.201 and S15.345 -- the claim on an IMAGE ends when the LAST
            //       claim on it goes, not at each hand-over. A newer generation can arrive in the image
            //       the item is already showing -- the daemon's ring cycles, so it hands the same buffer
            //       id back every few frames -- and between that reply being taken and the node moving
            //       on to it, the item holds two live claims on one image. The older one is given back
            //       as the node switches; ending the claim then would hand the daemon an image the node
            //       is about to sample. Releases of hand-overs a newer one superseded go out meanwhile,
            //       and are expected: they name numbers below the newest, which end nothing.
            // WHY-THIS-SHAPE: the state is two live CLAIMS, not two live imports: imports are kept per
            //       image, so a second hand-over of the same image is drawn through the import already
            //       held. The check while the claim is live comes first and is what carries the
            //       property: "the last release names the newest" alone is equally true of a plugin that
            //       ends the claim on the FIRST give-back. Only asking, after the older claim has been
            //       given back and before the newer one is, whether any release named the newest can
            //       tell those apart.
            // COVERAGE-ASSERT: the second picture is required to have been drawn through the
            //       import kept for that image, and a second reply served, so a run that never put
            //       two claims on one image fails here instead of reporting the accounting as
            //       established.
            const quint64 reusedBefore = scene.item->property("reusedImportCount").toULongLong();
            QDBusMessage ready = QDBusMessage::createSignal(QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                                                            QStringLiteral("FrameReady"));
            // One past the newest generation the item has read, so it is worth fetching. Counted
            // from where the settle left it rather than fixed at 2: how many frames the item
            // asked for on its own is not a number this file may pin.
            ready.setArguments({kFirstMonitor, static_cast<qulonglong>(servedAtSettle + 1)});
            bus.send(ready);

            const bool drawnAgain = pumpUntil(
                [&scene, reusedBefore] {
                    scene.renderPass();
                    return scene.item->property("reusedImportCount").toULongLong() > reusedBefore;
                },
                5000);
            check(drawnAgain, "the image the item was already showing is handed over again and drawn through the "
                              "import kept for it");
            if (!drawnAgain) {
                std::fprintf(stderr, "  the stub served %d frame(s) and the item asked %d time(s), the item reports "
                                     "[%s]; the two-live-claims state was never built\n",
                             stub.served(), stub.requests(),
                             scene.root->property("publishedReason").toString().toStdString().c_str());
                scene.root.reset();
                scene.renderPass();
                scene.control.invalidate();
                return true;
            }
            check(stub.served() > servedAtSettle,
                  "the stub served the same image again, which is what puts two claims on it");

            // The newest hand-over of the image the item has taken so far. While a claim on the image is
            // live, a release may name only numbers below it -- the replies a newer one superseded --
            // and never it, which would end the claim the node is drawing under.
            const auto newestServedOf = [&stub](quint64 bufferId) {
                quint64 newest = 0;
                for (const ExportingStub::Ask& ask : stub.asks()) {
                    if (ask.served == bufferId) newest = std::max(newest, ask.handover);
                }
                return newest;
            };
            pumpFor(300);
            const quint64 newestWhileLive = newestServedOf(first.bufferId);
            size_t namingNewestWhileLive = 0;
            for (const ExportingStub::Release& r : stub.releases()) {
                if (r.bufferId == first.bufferId && r.handover >= newestWhileLive) ++namingNewestWhileLive;
            }
            // GOAL: S12.15 and S15.345 -- while a claim on the image is live, nothing released names its
            //       newest hand-over: the releases that go out name only the replies a newer one of the
            //       same image superseded.
            // CONTROL: `++entry->imports;` in BufferClaims::take (backend/kde/buffer_claims.cpp) replaced
            //       by a bare `;`, so a repeat claim records nothing -> "FAIL: while a claim on the image is
            //       live, no release names its newest hand-over (1 of 2 release(s) named 2 or later)". Via
            //       control_batch.sh, 2026-10-06. Reverted.
            check(namingNewestWhileLive == 0,
                  ("while a claim on the image is live, no release names its newest hand-over (" +
                   std::to_string(namingNewestWhileLive) + " of " + std::to_string(stub.releases().size()) +
                   " release(s) named " + std::to_string(newestWhileLive) + " or later)")
                      .c_str());

            // GOAL: S15.308 -- exactly one release names the NEWEST hand-over of the image the item took,
            //       when the last claim goes; naming an older one would leave the image held for the
            //       session, since the daemon ends a claim only for a release covering its newest.
            // WHY-THIS-SHAPE: compared with the stub's own record of the numbers it served for that image,
            //       read once a release naming it has arrived (bounded); newest above 1 shows the image was
            //       handed over more than once, so "the newest" and "the first" differ.
            // CONTROL: via control_batch.sh, 2026-10-06, each reverted: (a) in
            //       WallpaperXItem::releaseHeldImages (backend/kde/wallpaperx_item.cpp), the retired set's
            //       `releaseClaim(claim);` made `(void)claim;` -> this line "(0 release(s) naming 2, 0 sent
            //       at the teardown)"; (b) `entry->newest = std::max(entry->newest, handover);` deleted from
            //       BufferClaims::take (backend/kde/buffer_claims.cpp) -> "(0 release(s) naming 2, 1 sent at
            //       the teardown)". The item's own teardown gives the last claim back: it retires the set
            //       first, so the node's destructor finds nothing to release, and emptying ~FrameNode's
            //       release instead leaves this green (the node's release is graded in item_cache_handover,
            //       where a failure destroys the node and not the item).
            const size_t releasesWhileLive = stub.releases().size();
            scene.root.reset();
            scene.renderPass();
            const quint64 newestServed = newestServedOf(first.bufferId);
            const auto finals = [&stub, &first, newestServed] {
                size_t count = 0;
                for (const ExportingStub::Release& r : stub.releases()) {
                    if (r.bufferId == first.bufferId && r.handover == newestServed) ++count;
                }
                return count;
            };
            const bool released = pumpUntil([&] { return finals() > 0; }, 2000);
            check(released && finals() == 1 && newestServed > 1,
                  ("the last claim going releases the image once, naming its newest hand-over (" +
                   std::to_string(finals()) + " release(s) naming " + std::to_string(newestServed) + ", " +
                   std::to_string(stub.releases().size() - releasesWhileLive) + " sent at the teardown)")
                      .c_str());
            scene.control.invalidate();
            return true;
        }

        if (phase == Phase::RepeatMany) {
            // GOAL: S15.345 at the item -- an image the item keeps showing while it is handed over again
            //       and again never brings the daemon's descriptor cap down on it: 65 forced re-fetches of
            //       the one image, against a stub charging descriptors exactly as the daemon does, are all
            //       served and drawn, none refused TooManyDescriptors; and the image stays held throughout
            //       -- no release names its newest hand-over until the item goes, when one does.
            // WHY-THIS-SHAPE: 65 is one past the cap, so even replies carrying one descriptor each would
            //       reach it without the superseded releases. Each re-fetch is forced by an announcement
            //       and waited for, bounded, by its picture drawn through the kept import, so every one is
            //       a real hand-over of the same held image.
            // CONTROL: (a) `if (taken.superseded) sendReleaseBuffer(...)` deleted from
            //       WallpaperXItem::takeGranted (backend/kde/wallpaperx_item.cpp) -> "FAIL: 65 forced
            //       re-fetches ... none refused for descriptors (62 drawn, refused at re-fetch 63, 64
            //       descriptor(s) charged at the end)"; (b) in BufferClaims::take (backend/kde/buffer_claims.cpp)
            //       the superseded release made to name `handover`, the reply just taken -> "FAIL: and the
            //       image stays held while it is shown: the releases name only superseded hand-overs (65
            //       release(s), 1 naming the newest, 66)". Via control_batch.sh, 2026-10-06. Reverted.
            // COVERAGE-ASSERT: the number of forced re-fetches served and drawn is asserted, so a drive cut
            //       short cannot read as "never refused".
            int drawn = 0;
            int refusedAt = 0;
            for (int i = 1; i <= kForcedRefetches && refusedAt == 0; ++i) {
                const quint64 reused0 = scene.item->property("reusedImportCount").toULongLong();
                const int refusals0 = stub.descriptorRefusals();
                announce(stub.nextGeneration());
                const bool next = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return scene.item->property("reusedImportCount").toULongLong() > reused0 ||
                               stub.descriptorRefusals() > refusals0;
                    },
                    5000);
                if (stub.descriptorRefusals() > refusals0) {
                    refusedAt = i;
                } else if (next) {
                    ++drawn;
                } else {
                    break;
                }
            }
            const quint64 newestServed = [&] {
                quint64 newest = 0;
                for (const ExportingStub::Ask& ask : stub.asks()) newest = std::max(newest, ask.handover);
                return newest;
            }();
            size_t namingNewest = 0;
            for (const ExportingStub::Release& r : stub.releases()) {
                if (r.handover >= newestServed) ++namingNewest;
            }
            check(drawn == kForcedRefetches && refusedAt == 0 && stub.descriptorRefusals() == 0,
                  ("65 forced re-fetches of an image the item keeps showing are all served and drawn, none refused "
                   "for descriptors (" +
                   std::to_string(drawn) + " drawn" +
                   (refusedAt != 0 ? ", refused at re-fetch " + std::to_string(refusedAt) : std::string()) + ", " +
                   std::to_string(stub.descriptorsCharged()) + " descriptor(s) charged at the end)")
                      .c_str());
            check(namingNewest == 0 && !stub.releases().empty(),
                  ("and the image stays held while it is shown: the releases name only superseded hand-overs (" +
                   std::to_string(stub.releases().size()) + " release(s), " + std::to_string(namingNewest) +
                   " naming the newest, " + std::to_string(newestServed) + ")")
                      .c_str());
            scene.root.reset();
            scene.renderPass();
            const bool givenBack = pumpUntil(
                [&] {
                    return !stub.releases().empty() && stub.releases().back().handover == newestServed;
                },
                2000);
            check(givenBack, "and it is given back, naming its newest hand-over, when the item goes");
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
        // CONTROL: `if (claim.daemon != m_daemon.lifetime) return std::nullopt;` in BufferClaims::giveBack
        //       (backend/kde/buffer_claims.cpp) deleted -> "FAIL: the claim the previous daemon
        //       granted is not released against its successor". Re-run 2026-10-06 via control_batch.sh.
        //       Reverted.
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

        // Drawing the successor's picture let go of the one before it, whose claim the previous
        // daemon granted. Nothing may have been sent to the successor.
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
    if (argc > 1 && std::strcmp(argv[1], "repeat-many") == 0) return Phase::RepeatMany;
    if (argc > 1 && std::strcmp(argv[1], "late-reply-replaced") == 0) return Phase::LateReplyReplaced;
    if (argc > 1 && std::strcmp(argv[1], "dropped-frame") == 0) return Phase::DroppedFrame;
    if (argc > 1 && std::strcmp(argv[1], "in-flight-monitor-change") == 0) return Phase::InFlightMonitorChange;
    if (argc > 1 && std::strcmp(argv[1], "daemon-restart") == 0) return Phase::DaemonRestart;
    if (argc > 1 && std::strcmp(argv[1], "late-reply") == 0) return Phase::LateReply;
    if (argc > 1 && std::strcmp(argv[1], "late-reply-alive") == 0) return Phase::LateReplyAlive;
    recognised = false;
    return Phase::RepeatImport;
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_release_ledger_test";
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
        // A harness fault, not a skip: an entry that names a phase this binary does not have
        // would otherwise run the first one twice and report both as covered.
        wallpaperx::testing::harnessFailure("no phase was named on the command line; expected one of repeat-import, "
                                            "repeat-many, dropped-frame, in-flight-monitor-change, daemon-restart, "
                                            "late-reply, late-reply-alive, late-reply-replaced");
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
