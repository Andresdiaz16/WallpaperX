// SPDX-License-Identifier: GPL-2.0-or-later

// The KDE plugin's side of the acquire timeline (2026-09-23): one Vulkan timeline semaphore per
// consumer connection, imported once, with a value per picture -- and everything that must keep
// the wallpaper drawing when any part of that fails.
//
// The real item, in a render-controlled scene, against a stub daemon (tests/item_stub_scene.h)
// that serves real exported frames and either form of hand-over. What is read is what the plugin
// cannot fake: its semaphore-import counter, the requests the stub received, the pixels a picture
// sampled, the one stderr line the refused-import rule prints, and whether the item blanked.
//
// One phase per process (argv[1]): the plugin's timeline record, its record of which daemon it
// talks to and its list of contexts that refused an import are process-wide and never reset.
//
//   steady              one import per connection, none per picture; pixels right (S13.215, S13.216)
//   old-daemon          a daemon without the method: asked once, never on the timeline (S13.222, S13.218)
//   unsupported         Unsupported is final (S13.218)
//   unknown-interface   so is UnknownInterface (S13.218)
//   too-many-copies     so is TooManyTimelineCopies (S15.365)
//   retry-busy          Busy is asked again after about 2 s, then used (S13.218)
//   retry-exportfailed  so is ExportFailed (S13.218)
//   lost-reply          a timeline answer that never comes costs nothing (S13.217)
//   refused-timeline    a refused timeline import: one line, the wallpaper keeps drawing (S13.223)
//   refused-binary      the same for a refused binary import (S13.223)
//   wait-no-trip        a failed wait gives up the timeline, not the context (S13.223, S13.217)
//   setup-create        a timeline that cannot be created costs one picture (S13.217)
//   setup-type          nor one whose type cannot be set (S13.217)
//   stray-value         a value in a reply to a request not on the timeline is never read (S13.221)
//   restart             a value is waited on only on its own daemon's timeline (S13.221)
//   stale-value         a late value from a daemon given up on is no failed hand-over (S13.224)
//   context-move        a value that lands in a context unable to wait on it is drawn (S13.225)
//   late-answer         a timeline answer past QtDBus's 25 s is used, the daemon asked once (S15.336)
//   replaced-asking     an ask still out to a replaced daemon does not hold up the new one (S15.337)
//   rebuild             300 node rebuilds and a daemon replacement import the timeline once each (S15.419)
//   context-share       one import per context: invalidated, new, returned to, address reused (S15.420)
//
// Every phase but refused-binary and stray-value needs a GL context that can wait on a Vulkan
// timeline (GL_NV_timeline_semaphore), and reports NOT VERIFIED where there is none -- Mesa.

#include "kde/frame_import.h"
#include "support/validation_grade.h"
#include "tests/gl_import_harness.h"
#include "tests/gl_semaphore_fault.h"
#include "tests/item_render_scene.h"
#include "tests/item_stub_scene.h"
#include "tests/stream_capture.h"
#include "tests/private_test_bus.h"
#include "tests/test_report.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusPendingCall>
#include <QDBusServiceWatcher>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QUrl>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::announce;
using wallpaperx::testing::claimsFrameShown;
using wallpaperx::testing::drawNext;
using wallpaperx::testing::ExportingStub;
using wallpaperx::testing::g_glSemaphoreFaults;
using wallpaperx::testing::itemTexture;
using wallpaperx::testing::kHeight;
using wallpaperx::testing::kPath;
using wallpaperx::testing::kService;
using wallpaperx::testing::kWidth;
using wallpaperx::testing::nativeName;
using wallpaperx::testing::nearestColour;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::publishSolidFrame;
using wallpaperx::testing::pumpFor;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::Scene;
using wallpaperx::testing::settleQuiet;

void check(bool condition, const std::string& what) { wallpaperx::testing::check(condition, what.c_str()); }

// Restated rather than shared, for item_import_cache_test's reason: what is graded is what the
// desktop is told, and taking it from the plugin would let a rewording pass unnoticed.
constexpr const char* kCouldNotImport = "WallpaperX produced a frame this desktop could not import.";
constexpr const char* kTripLine = "refused a semaphore import";
constexpr int kToleratedHandOverFailures = 8;  // mirrored from backend/kde/wallpaperx_item.cpp

int g_ungradedValidation = 0;

enum class Phase {
    Steady,
    OldDaemon,
    Unsupported,
    UnknownInterface,
    TooManyCopies,
    RetryBusy,
    RetryExportFailed,
    LostReply,
    RefusedTimeline,
    RefusedBinary,
    WaitNoTrip,
    SetupCreate,
    SetupType,
    StrayValue,
    Restart,
    RestartMemory,
    StaleValue,
    ContextMove,
    LateValue,
    LateAnswer,
    ReplacedAsking,
    RetryReplaced,
    Rebuild,
    ContextShare,
};

struct PhaseName {
    Phase phase;
    const char* name;
    const char* goal;
    bool needsTimeline;
};

constexpr PhaseName kPhases[] = {
    {Phase::Steady, "steady", "S13.215 and S13.216 (no semaphore import per picture; pixels right after the wait)", true},
    {Phase::OldDaemon, "old-daemon", "S13.222 and S13.218 (a daemon without the timeline methods)", true},
    {Phase::Unsupported, "unsupported", "S13.218 (Unsupported is not asked again)", true},
    {Phase::UnknownInterface, "unknown-interface", "S13.218 (UnknownInterface is not asked again)", true},
    {Phase::TooManyCopies, "too-many-copies", "S15.365 (TooManyTimelineCopies is not asked again)", true},
    {Phase::RetryBusy, "retry-busy", "S13.218 (Busy is asked again after about two seconds)", true},
    {Phase::RetryExportFailed, "retry-exportfailed", "S13.218 (ExportFailed is asked again after about two seconds)",
     true},
    {Phase::LostReply, "lost-reply", "S13.217 (a timeline answer that never arrives costs nothing)", true},
    {Phase::RefusedTimeline, "refused-timeline", "S13.223 (a refused timeline import keeps the wallpaper drawing)",
     true},
    {Phase::RefusedBinary, "refused-binary", "S13.223 (a refused binary import keeps the wallpaper drawing)", false},
    {Phase::WaitNoTrip, "wait-no-trip", "S13.223 and S13.217 (a failed wait gives up the timeline, not the context)",
     true},
    {Phase::SetupCreate, "setup-create", "S13.217 (a timeline that cannot be created costs one picture)", true},
    {Phase::SetupType, "setup-type", "S13.217 (a timeline whose type cannot be set costs one picture)", true},
    {Phase::StrayValue, "stray-value", "S13.221 (a value is read only from a reply to a request asked on the timeline)", false},
    {Phase::Restart, "restart",
     "S13.221 and S15.317 (a value is waited on only on the timeline of the daemon that sent it; the late reply, its "
     "memory left out, is drawn)",
     true},
    {Phase::RestartMemory, "restart-memory",
     "S13.221 (the late reply from a replaced daemon is drawn through the held import when it carries the memory too)",
     true},
    {Phase::StaleValue, "stale-value", "S13.224 (a late value from a daemon given up on is not a failed hand-over)",
     true},
    {Phase::ContextMove, "context-move",
     "S13.225 (a value that reaches a context which cannot wait on a timeline is drawn unwaited)", true},
    {Phase::LateValue, "late-value", "S13.216's ordering (a value is read only once it is reached)", true},
    {Phase::LateAnswer, "late-answer", "S15.336 (a timeline answer later than QtDBus's 25 s is used, the daemon asked once)",
     true},
    {Phase::ReplacedAsking, "replaced-asking",
     "S15.337 (a request still out to a replaced daemon does not stop the new one being asked; its late answer is "
     "closed)",
     true},
    {Phase::RetryReplaced, "retry-replaced",
     "S15.348 (a timeline refused for now by one daemon does not delay asking the daemon that replaces it)", true},
    {Phase::Rebuild, "rebuild",
     "S15.419 (node rebuilds in one context never import the timeline again; a new daemon's is imported once)", true},
    {Phase::ContextShare, "context-share",
     "S15.420 (the timeline is imported once per GL context, and a destroyed context's import is forgotten)", true},
};

// Everything written to stderr while it is alive, which is where the plugin's refused-import line
// goes.
struct StderrCapture : wallpaperx::testing::StreamCapture {
    StderrCapture() : StreamCapture(STDERR_FILENO) {}
    // Ends the capture, echoing it to the real stderr, and returns everything it held.
    std::string finish() {
        restore();
        return text();
    }
};

int occurrences(const std::string& text, const char* needle) {
    int count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

const std::vector<std::array<unsigned char, 4>> kPalette = {
    {230, 20, 20, 255}, {20, 210, 20, 255}, {20, 20, 230, 255}, {230, 210, 20, 255}};
const char* const kColourNames[] = {"red", "green", "blue", "yellow"};

// The colour at the centre of what the item's node samples, read through the scene graph's own
// context after a pass; -1 for no palette colour, -2 when the harness could not read at all.
int colourOnScreen(Scene& scene) {
    const GLuint name = nativeName(itemTexture(scene.item));
    std::vector<unsigned char> pixels;
    std::string why;
    scene.renderPass([&] { pixels = wallpaperx::testing::readSampled(name, kWidth, kHeight, why); });
    if (pixels.empty()) {
        wallpaperx::testing::harnessFailure("the texture on the screen could not be read back: " + why);
        return -2;
    }
    return nearestColour(&pixels[((kHeight / 2) * kWidth + kWidth / 2) * 4], kPalette);
}

std::string colourName(int index) { return index >= 0 ? kColourNames[index] : "no palette colour"; }

// The item showing a picture with nothing said: what every phase here requires of the wallpaper
// whatever went wrong underneath.
bool drawingCleanly(Scene& scene) {
    return claimsFrameShown(scene.item) && scene.reason().isEmpty() && itemTexture(scene.item) != nullptr;
}

std::string describe(Scene& scene) {
    return std::string("frame shown: ") + (claimsFrameShown(scene.item) ? "yes" : "no") + ", reason [" +
           scene.reason().toStdString() + "]";
}

// Pictures drawn in lockstep, one announced and one drawn each, up to `limit`, stopping early when
// `until` holds. Counts what it drew; a picture that never arrived ends the drive.
struct Drive {
    int drawn = 0;
    bool stalled = false;
};

Drive drive(Scene& scene, ExportingStub& stub, QDBusConnection& bus, int limit,
            const std::function<bool()>& until = nullptr, int pauseMs = 0) {
    Drive out;
    for (int i = 0; i < limit; ++i) {
        if (until && until()) break;
        if (!drawNext(scene, stub, bus)) {
            out.stalled = true;
            break;
        }
        ++out.drawn;
        if (pauseMs > 0) pumpFor(pauseMs);
    }
    return out;
}

// A second stub daemon on a connection of its own, granting timelines, ready to take the daemon's
// name. The change is waited for on a watcher of the plugin's own connection, whose signal the
// plugin's watch receives first. Taken off the bus and disconnected when it goes, on every path,
// before the stub it serves from is destroyed.
struct SuccessorDaemon {
    QDBusConnection bus;
    ExportingStub stub;
    bool ok = false;  // connected, with the stub's object registered
    QString newOwner;
    QDBusServiceWatcher watcher{QString::fromUtf8(kService), QDBusConnection::sessionBus(),
                                QDBusServiceWatcher::WatchForOwnerChange};

    SuccessorDaemon(wallpaperx::Device& device, const std::vector<const wallpaperx::OffscreenTarget*>& ring)
        : bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("wallpaperx-timeline-b"))),
          stub(device, ring) {
        stub.rotate();
        stub.serveTimelines(ExportingStub::Timeline::Grant);
        QObject::connect(&watcher, &QDBusServiceWatcher::serviceOwnerChanged,
                         [this](const QString&, const QString&, const QString& owner) { newOwner = owner; });
        ok = bus.isConnected() && bus.registerVirtualObject(QString::fromUtf8(kPath), &stub);
    }
    ~SuccessorDaemon() {
        bus.unregisterObject(QString::fromUtf8(kPath));
        (void)bus.unregisterService(QString::fromUtf8(kService));
        QDBusConnection::disconnectFromBus(QStringLiteral("wallpaperx-timeline-b"));
    }
    SuccessorDaemon(const SuccessorDaemon&) = delete;
    SuccessorDaemon& operator=(const SuccessorDaemon&) = delete;

    // Moves the daemon's name from `from` to this daemon, and waits, bounded, until the plugin's
    // connection has seen the change.
    bool takeNameFrom(QDBusConnection& from) {
        (void)from.unregisterService(QString::fromUtf8(kService));
        return ok && bus.registerService(QString::fromUtf8(kService)) &&
               pumpUntil([&] { return newOwner == bus.baseService(); }, 3000);
    }
};

bool runPhase(const PhaseName& which) {
    const Phase phase = which.phase;
    const char* kGoal = which.goal;
    return wallpaperx::testing::gradedRun("item_timeline_test", [&](wallpaperx::testing::ValidationGrade& grade) {
        grade.reportUngradedTo(g_ungradedValidation);
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);

        // Two images in two colours, so a stale picture and the right one read differently.
        std::vector<const wallpaperx::OffscreenTarget*> ring;
        for (size_t i = 0; i < 2; ++i) {
            const wallpaperx::OffscreenTarget& target = targets.create(static_cast<int>(10 + i), kWidth, kHeight);
            publishSolidFrame(device, targets, target, "item_timeline_" + std::to_string(i) + ".png", kPalette[i]);
            ring.push_back(&target);
        }

        // On a connection of its own, for item_import_cache_test's reason: served on the item's own
        // connection, QtDBus answers locally and a held reply is delivered anyway.
        QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                            QStringLiteral("wallpaperx-timeline-stub-daemon"));
        ExportingStub stub(device, ring);
        stub.rotate();
        switch (phase) {
            case Phase::OldDaemon:
            case Phase::RefusedBinary:
            case Phase::StrayValue:
                break;  // Absent: GetAcquireTimeline answered UnknownMethod, as a daemon from before it
            case Phase::LostReply:
                stub.serveTimelines(ExportingStub::Timeline::Silent);
                break;
            case Phase::LateAnswer:
            case Phase::ReplacedAsking:
                stub.serveTimelines(ExportingStub::Timeline::Late);
                break;
            case Phase::RefusedTimeline:
                stub.serveTimelines(ExportingStub::Timeline::BadDescriptor);
                break;
            case Phase::Unsupported:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.refuseTimelineAsks(1000, QStringLiteral("io.wallpaperx.Daemon.Error.Unsupported"));
                break;
            case Phase::UnknownInterface:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.refuseTimelineAsks(1000, QStringLiteral("org.freedesktop.DBus.Error.UnknownInterface"));
                break;
            case Phase::TooManyCopies:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.refuseTimelineAsks(1000, QStringLiteral("io.wallpaperx.Daemon.Error.TooManyTimelineCopies"));
                break;
            case Phase::RetryBusy:
            case Phase::RetryReplaced:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.refuseTimelineAsks(1, QStringLiteral("io.wallpaperx.Daemon.Error.Busy"));
                break;
            case Phase::RetryExportFailed:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.refuseTimelineAsks(1, QStringLiteral("io.wallpaperx.Daemon.Error.ExportFailed"));
                break;
            case Phase::Restart:
            case Phase::RestartMemory:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.advanceTimelinesOnGrant(200);
                // Above any number the new daemon will issue in the phase, so a release reaching it that
                // names one of these is a late claim credited to the wrong daemon.
                stub.numberHandoversFrom(500);
                break;
            case Phase::LateValue:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.ownTimelines();
                break;
            default:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                break;
        }
        if (phase == Phase::SetupCreate) g_glSemaphoreFaults.failTimelineCreate.store(true);
        if (phase == Phase::SetupType) g_glSemaphoreFaults.failTimelineType.store(true);
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
                                   QGuiApplication::platformName().toStdString() + " platform");
            return true;
        }
        bool uuidQueryable = false;
        std::array<uint8_t, 16> glUuid{};
        std::string glRenderer = "(unknown)";
        std::string noTimeline;
        scene.control.polishItems();
        scene.control.beginFrame();
        scene.control.sync();
        {
            const GLubyte* name = glGetString(GL_RENDERER);
            if (name != nullptr) glRenderer = reinterpret_cast<const char*>(name);
            uuidQueryable = wallpaperx::testing::glDeviceUuid(glUuid);
            (void)wallpaperx::FrameImport::supportsTimeline(&noTimeline);
        }
        scene.control.endFrame();
        if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();
        const bool sameDevice =
            uuidQueryable && glUuid == wallpaperx::testing::vulkanDeviceUuid(device.physicalDevice());
        if (wallpaperx::testing::exportNeedsSameDevice(device, *ring.front()) && !sameDevice) {
            notVerified(kGoal, "the scene graph's GL context is not provably on the device the Vulkan export came "
                               "from (GL reports " + glRenderer + "), so the frame is not importable here");
            scene.control.invalidate();
            return true;
        }
        if (which.needsTimeline && !noTimeline.empty()) {
            notVerified(kGoal, "this GL context cannot wait on a Vulkan timeline: " + noTimeline);
            scene.control.invalidate();
            return true;
        }

        scene.engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
        scene.component = std::make_unique<QQmlComponent>(&scene.engine);
        scene.component->setData(wallpaperx::testing::cacheQmlSource().toUtf8(),
                                  QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                      QStringLiteral("/item_timeline.qml")));
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
            wallpaperx::testing::harnessFailure("the root object exposed no rendererItem");
            scene.control.invalidate();
            return true;
        }
        struct Teardown {
            Scene& scene;
            ~Teardown() {
                g_glSemaphoreFaults.failWaits.store(0);
                scene.root.reset();
                scene.renderPass();
                scene.control.invalidate();
            }
        } teardown{scene};

        // The refused-import line is looked for in everything from here on, the first import included.
        StderrCapture captured;
        std::fprintf(stderr, "item_timeline_test: capture marker\n");

        const bool imported = pumpUntil(
            [&scene] {
                scene.renderPass();
                return itemTexture(scene.item) != nullptr;
            },
            10000);
        if (!imported) {
            check(false, "the item imported the frame the stub served");
            std::fprintf(stderr, "  it asked %d time(s), the stub served %d, the item reports [%s]\n", stub.requests(),
                         stub.served(), scene.reason().toStdString().c_str());
            return true;
        }
        if (!settleQuiet(scene, stub)) {
            wallpaperx::testing::harnessFailure("the item was still asking for frames five seconds after its first "
                                                "picture, so no phase could say which request was its own");
            return true;
        }
        std::printf("item_timeline_test: %s -- imported on %s, handle type %s\n", which.name, glRenderer.c_str(),
                    stub.handleType() == 1 ? "dma_buf" : "opaque fd");
        std::fflush(stdout);

        // Drives until the item has drawn a picture handed over as a value, the timeline path's
        // first. False, and reported, when it never does inside `limit` pictures.
        const auto reachTimeline = [&](int limit) {
            const int values0 = stub.valuesSent();
            const Drive reach = drive(scene, stub, bus, limit, [&] { return stub.valuesSent() > values0; });
            return !reach.stalled && stub.valuesSent() > values0;
        };
        const auto tripLines = [&] { return occurrences(captured.finish(), kTripLine); };
        // COVERAGE-ASSERT for every trip-line count: the marker printed before the scene started
        // must be in the capture, or "no such line" would be true of a capture that saw nothing.
        const auto captureWorked = [&] {
            return occurrences(captured.finish(), "item_timeline_test: capture marker") == 1;
        };

        switch (phase) {
            case Phase::Steady: {
                // GOAL: S13.215 -- one timeline per connection: once the item is on the timeline a
                //       picture costs NO semaphore import, and reaching it cost the binary pictures
                //       drawn before the switch plus one import of the timeline -- at most two binary
                //       pictures once the item knows which daemon it talks to.
                // WHY-THIS-SHAPE: counted in lockstep from a settled item, so every import is one this
                //       drive caused, and held against the stub's own tally of binary acquires it sent:
                //       an identity rather than a number read off the plugin. The two-picture bound is
                //       message order on one connection -- the ask is sent before the fetch the next
                //       FrameReady causes, and answered first -- not timing.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) contextTimeline's reuse line (the
                //       context's kept import returned) deleted -> "FAIL: a picture on the timeline costs no
                //       semaphore import (20 over 20 pictures)", on the opaque-fd and dma_buf entries
                //       both (re-run 2026-10-08 via control_run.sh on the NVIDIA entry after the import moved
                //       from the node to the context, the same line); (b) WallpaperXItem::fetchOnTimeline made to return false -> "FAIL: the
                //       item reaches the timeline within six pictures (0 request(s) on it, 0 value(s)
                //       sent)"; (c) the wantAcquireTimeline call in updatePaintNode gated on more than
                //       three semaphore imports -> "FAIL: reaching the timeline costs the binary
                //       pictures before it plus one import (6 import(s) for 5 binary picture(s))".
                //       Observed 2026-09-23 on the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: the steady drive must have been handed over as values, every one.
                const quint64 semaphores0 = scene.semaphoreImports();
                const int binary0 = stub.acquireFdsSent();
                const bool reached = reachTimeline(6);
                check(reached, "the item reaches the timeline within six pictures (" +
                                   std::to_string(stub.onTimelineRequests()) + " request(s) on it, " +
                                   std::to_string(stub.valuesSent()) + " value(s) sent)");
                if (!reached) return true;
                const int binaryPictures = stub.acquireFdsSent() - binary0;
                const quint64 switchCost = scene.semaphoreImports() - semaphores0;
                check(binaryPictures <= 2 && switchCost == static_cast<quint64>(binaryPictures) + 1,
                      "reaching the timeline costs the binary pictures before it plus one import (" +
                          std::to_string(switchCost) + " import(s) for " + std::to_string(binaryPictures) +
                          " binary picture(s))");

                constexpr int kSteady = 20;
                const quint64 semaphores1 = scene.semaphoreImports();
                const int values1 = stub.valuesSent();
                const Drive steady = drive(scene, stub, bus, kSteady);
                check(!steady.stalled && steady.drawn == kSteady && stub.valuesSent() - values1 == kSteady,
                      "every steady picture is drawn and was handed over as a value (" + std::to_string(steady.drawn) +
                          " drawn, " + std::to_string(stub.valuesSent() - values1) + " values)");
                check(scene.semaphoreImports() == semaphores1,
                      "a picture on the timeline costs no semaphore import (" +
                          std::to_string(scene.semaphoreImports() - semaphores1) + " over " +
                          std::to_string(kSteady) + " pictures)");
                std::printf("item_timeline_test: %llu semaphore import(s) in all, the item's whole life, for %llu "
                            "picture(s)\n",
                            static_cast<unsigned long long>(scene.semaphoreImports()),
                            static_cast<unsigned long long>(scene.pictures()));

                // GOAL: S13.216 -- a picture handed over as a value and waited on shows what was drawn
                //       into its image, on the GPU and handle path this entry runs on (on the NVIDIA
                //       desktop, the opaque-fd one). The property the timeline change stands on.
                // WHY-THIS-SHAPE: each image redrawn in a new colour before it comes round, then read
                //       back through the scene graph's own context, sampled as a compositor samples.
                //       Four flat colours, so a stale picture or the other image's reads differently.
                //       Required to have come as values and through kept imports, or the reading would
                //       be about the binary path or a fresh import. What it cannot see is the ORDER the
                //       wait imposes: the stub finishes drawing before it answers, so a missing wait
                //       reads right too (S13.199's limit, the same here).
                // CONTROL: importKeyFor() in backend/kde/wallpaperx_item.cpp given the constant bufferId
                //       1, so both images share one import -> "FAIL: a picture waited on through the
                //       timeline shows what was drawn into its image (2 of 4)", printing "step 1: read
                //       blue where yellow was drawn; a value, through the kept import". And contextTimeline's
                //       reuse line deleted -> the same line "(0 of 4)", every colour right and each step
                //       "NOT a value through a kept import": the coverage half reddening alone. Observed
                //       2026-09-23 on the NVIDIA entry; the second re-run 2026-10-08 via control_run.sh after
                //       the import moved to the context, the same line. Reverted.
                const size_t first = static_cast<size_t>(stub.served()) % 2;  // the image served next
                // Each image's new colour differs from the one it held, so a stale read is a wrong one.
                constexpr int kColours[] = {2, 3, 3, 2};
                int right = 0;
                for (int step = 0; step < 4; ++step) {
                    const size_t image = (first + static_cast<size_t>(step)) % 2;
                    const int colour = kColours[step];
                    publishSolidFrame(device, targets, *ring[image], "item_timeline_c" + std::to_string(step) + ".png",
                                      kPalette[static_cast<size_t>(colour)]);
                    const int values = stub.valuesSent();
                    const quint64 reused = scene.reused();
                    const quint64 semaphores = scene.semaphoreImports();
                    if (!drawNext(scene, stub, bus)) {
                        check(false, "the item draws each recoloured picture");
                        return true;
                    }
                    const int seen = colourOnScreen(scene);
                    if (seen == -2) return true;
                    const bool viaTimeline = stub.valuesSent() == values + 1 && scene.reused() == reused + 1 &&
                                             scene.semaphoreImports() == semaphores;
                    if (seen == colour && viaTimeline) ++right;
                    if (seen != colour || !viaTimeline) {
                        std::fprintf(stderr, "  step %d: read %s where %s was drawn; %s\n", step, colourName(seen).c_str(),
                                     colourName(colour).c_str(),
                                     viaTimeline ? "a value, through the kept import"
                                                 : "NOT a value through a kept import");
                    }
                }
                check(right == 4, "a picture waited on through the timeline shows what was drawn into its image (" +
                                      std::to_string(right) + " of 4)");
                return true;
            }

            case Phase::LateValue: {
                // GOAL: S13.216's ordering -- a picture handed over as a value is read only once that
                //       value is reached: what the producer draws into the image before reaching it is
                //       what the item shows, never the image as it was when the reply came.
                // WHY-THIS-SHAPE: the stub's own timeline (ownTimelines()) carries a value whose drawing
                //       (blue) and signal are submitted with the reply but held on the GPU behind a gate.
                //       In the pass that draws the picture the item's texture is copied
                //       (copyWhileDrawing(), ordered in GL after the item's wait) and flushed; after a pause
                //       long enough for an unordered copy to have run, the gate opens; the copy is read only
                //       then. Reading the item's texture at the end instead would see blue whether or not
                //       the item waited. Held on the GPU, not merely unsignalled: Vulkan forbids waiting on
                //       a signal not yet submitted, and the gate keeps every step legal. Relies on this
                //       queue not running the drawing before the gated batch ahead of it, which the
                //       control shows holds here (the old picture is what an unwaited copy reads).
                // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) the
                //       timeline wait skipped for a kept import (`handedOver = reused != nullptr ||
                //       (timeline.semaphore != 0 && FrameImport::acquire(...))`), via control_run.sh,
                //       2026-10-05 -> "FAIL: a picture handed over as a value is read only once the value is
                //       reached (read red where blue was drawn before the value was reached, over red)".
                //       Reverted.
                // COVERAGE-ASSERT: the late value must have gone out and been drawn through the kept
                //       import, and the copy taken.
                if (!reachTimeline(6)) {
                    check(false, "the item reaches the timeline within six pictures");
                    return true;
                }
                {
                    const size_t image = static_cast<size_t>(stub.served()) % 2;  // the image served next
                    const quint64 reused = scene.reused();
                    // Each image starts in its own colour (image 0 red, image 1 green); blue is new to both.
                    // Built now: building it uploads a texture and waits, which the gate would hold for good.
                    const std::string bluePath = wallpaperx::testing::writeSolidPng(
                        std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/item_timeline_late.png", 16, 16, kPalette[2]);
                    wallpaperx::StaticRenderer blue(device, targets.renderPass(), bluePath, WALLPAPERX_SHADER_DIR);
                    const std::string acquireError0 = stub.acquireError();
                    stub.lateValueNext([&] { targets.render(blue, *ring[image]); });
                    announce(bus, stub.nextGeneration());
                    GLuint copy = 0;
                    const bool drawn = copyWhileDrawing(scene, [&] { return scene.reused() > reused; }, copy);
                    std::string why;
                    if (!stub.lateValueSent() && stub.acquireError() != acquireError0) {
                        // The stub's own failure, not the plugin's: no late value went out to be waited on.
                        wallpaperx::testing::harnessFailure("the stub could not send the late value: " +
                                                            stub.acquireError());
                        stub.signalLateValue(why);
                        return true;
                    }
                    if (!drawn || !stub.lateValueSent()) {
                        check(false, "the picture with the late value is drawn through the kept import and copied (late "
                                     "value sent " + std::to_string(stub.lateValueSent()) + ", drawn and copied " +
                                         std::to_string(drawn) + ")");
                        stub.signalLateValue(why);
                        return true;
                    }
                    // Long enough for a copy nothing holds back to have run.
                    pumpFor(300);
                    if (!stub.signalLateValue(why)) {
                        wallpaperx::testing::harnessFailure("the late value could not be reached: " + why);
                        return true;
                    }
                    const std::vector<unsigned char> pixels = readCopy(scene, copy, why);
                    if (pixels.empty()) {
                        wallpaperx::testing::harnessFailure("the copy of the drawn picture could not be read back: " + why);
                        return true;
                    }
                    const int seen = nearestColour(&pixels[((kHeight / 2) * kWidth + kWidth / 2) * 4], kPalette);
                    check(seen == 2, "a picture handed over as a value is read only once the value is reached (read " +
                                         colourName(seen) + " where blue was drawn before the value was reached, over " +
                                         colourName(static_cast<int>(image)) + ")");
                }
                return true;
            }

            case Phase::OldDaemon:
            case Phase::Unsupported:
            case Phase::UnknownInterface:
            case Phase::TooManyCopies: {
                // GOAL: S13.222, S13.218 and S15.365 -- a daemon that answers GetAcquireTimeline with
                //       UnknownMethod (one from before it), Unsupported, UnknownInterface or
                //       TooManyTimelineCopies (this connection has had its copies) is asked ONCE; the
                //       plugin draws every picture with its binary semaphore, and never asks GetFrame on
                //       the timeline.
                // WHY-THIS-SHAPE: the drive lasts well past the plugin's two-second retry, with a
                //       binary picture every tenth of a second, each of which would ask again if the
                //       answer were not final -- "asked once" is otherwise true of any run shorter
                //       than the delay.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) the four names in
                //       timelineRefusalIsPermanent misspelled -> "FAIL: the refusal is final: the
                //       timeline was asked for once (2 ask(s))" on all four entries; (b) the
                //       holdsAcquireTimeline test deleted from WallpaperXItem::fetchOnTimeline -> on
                //       old-daemon, "FAIL: no picture was asked for on a timeline (50)".
                const quint64 semaphores0 = scene.semaphoreImports();
                QElapsedTimer clock;
                clock.start();
                const Drive run = drive(scene, stub, bus, 100, [&] { return clock.elapsed() > 3000; }, 60);
                check(clock.elapsed() > 2500, "the drive outlasted the plugin's retry delay (" +
                                                  std::to_string(clock.elapsed()) + " ms)");
                check(stub.timelineAsks() == 1,
                      "the refusal is final: the timeline was asked for once (" + std::to_string(stub.timelineAsks()) +
                          " ask(s))");
                check(stub.onTimelineRequests() == 0, "no picture was asked for on a timeline (" +
                                                          std::to_string(stub.onTimelineRequests()) + ")");
                check(!run.stalled && drawingCleanly(scene) &&
                          scene.semaphoreImports() - semaphores0 == static_cast<quint64>(run.drawn),
                      "every picture is drawn with its own binary semaphore (" + std::to_string(run.drawn) +
                          " drawn, " + std::to_string(scene.semaphoreImports() - semaphores0) + " imports; " +
                          describe(scene) + ")");
                return true;
            }

            case Phase::RetryBusy:
            case Phase::RetryExportFailed: {
                // GOAL: S13.218 -- Busy and ExportFailed are not final: the plugin asks again, about two
                //       seconds later rather than at once (each ask takes the daemon's GPU lock), and
                //       then uses the timeline it is given. Every picture meanwhile is drawn.
                // WHY-THIS-SHAPE: the stub refuses the first ask and grants the rest, and stamps each
                //       ask as it arrives. The gap is bounded below only: the plugin's delay is a floor,
                //       and a loaded host can only lengthen it.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) Busy and ExportFailed added to
                //       timelineRefusalIsPermanent -> on both entries "FAIL: the refused ask is asked
                //       again, once (1 ask(s))", "FAIL: and not before the retry delay (-1 ms between
                //       them)" and "FAIL: the second answer is used: pictures come as values (0), ...";
                //       (b) `retryAfter` set to now in askForAcquireTimeline's reply handler -> "FAIL: and
                //       not before the retry delay (301 ms between them)". Observed 2026-09-23 on the
                //       NVIDIA entry; re-run via control_batch.sh ((a) on both entries 2026-10-05, (b) on
                //       retry-busy 2026-10-06, "302 ms"), the same lines. Reverted.
                QElapsedTimer clock;
                clock.start();
                const Drive run = drive(scene, stub, bus, 80,
                                        [&] { return stub.valuesSent() > 0 || clock.elapsed() > 6000; }, 60);
                const std::vector<qint64>& asks = stub.timelineAskTimesMs();
                const qint64 gap = asks.size() >= 2 ? asks[1] - asks[0] : -1;
                check(asks.size() == 2, "the refused ask is asked again, once (" + std::to_string(asks.size()) +
                                            " ask(s))");
                check(gap >= 1500, "and not before the retry delay (" + std::to_string(gap) + " ms between them)");
                check(stub.valuesSent() > 0 && !run.stalled && drawingCleanly(scene),
                      "the second answer is used: pictures come as values (" + std::to_string(stub.valuesSent()) +
                          "), and every picture was drawn (" + describe(scene) + ")");
                return true;
            }

            case Phase::LostReply: {
                // GOAL: S13.217 -- a timeline the daemon registered but whose answer never reached
                //       the plugin costs nothing: the plugin never asks for a value it could not wait
                //       on, and draws every picture with its binary semaphore.
                // WHY-THIS-SHAPE: the stub creates the caller's timeline and never answers, so a
                //       daemon asked for a value WOULD send one; only the plugin's own record can keep
                //       it from asking. The ask has no timeout, so nothing here changes with the drive's
                //       length; an answer that does come late, past QtDBus's default 25 s, is
                //       late-answer's case (S15.336).
                // CONTROL: the holdsAcquireTimeline test deleted from WallpaperXItem::fetchOnTimeline
                //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: and no picture was asked for on it
                //       while the answer was out (1)". Observed 2026-09-23 on the NVIDIA entry; re-run via
                //       control_batch.sh 2026-10-05 after the ask lost its timeout, the same line. Reverted.
                const Drive run = drive(scene, stub, bus, 25, nullptr, 60);
                check(stub.timelineAsks() == 1, "the timeline was asked for (" + std::to_string(stub.timelineAsks()) +
                                                    " ask(s))");
                check(stub.onTimelineRequests() == 0, "and no picture was asked for on it while the answer was out (" +
                                                          std::to_string(stub.onTimelineRequests()) + ")");
                check(!run.stalled && run.drawn == 25 && drawingCleanly(scene),
                      "every picture is drawn (" + std::to_string(run.drawn) + " of 25; " + describe(scene) + ")");
                return true;
            }

            case Phase::LateAnswer: {
                // GOAL: S15.336 -- an answer to GetAcquireTimeline that comes after QtDBus's default
                //       25 s reply timeout is adopted: pictures then come as values, and the daemon is
                //       asked exactly once over the whole run. False for an ask on the default timeout,
                //       which fails at 25 s, asks again about 2 s later -- a second descriptor the
                //       daemon charges until this process leaves the bus -- and drops the late answer.
                // WHY-THIS-SHAPE: the stub holds the ask (Timeline::Late) while pictures keep coming
                //       every ~200 ms, each of which would ask again if the record were not settled, and
                //       answers at least 28 s after the ask arrived -- a lower bound, measured from when
                //       the test saw the ask, so a slow host only makes the answer later.
                // CONTROL: in askForAcquireTimeline (backend/kde/wallpaperx_item.cpp)
                //       `bus.asyncCall(request, kNoReplyTimeout)` made `bus.asyncCall(request)`, via
                //       control_batch.sh, 2026-10-05, NVIDIA -> "FAIL: the daemon is asked for its timeline
                //       once over the whole run (2 ask(s))" and "FAIL: the late answer is adopted: pictures
                //       come as values once it arrives (0 value(s), 0 asked before it; ...)". Reverted.
                // COVERAGE-ASSERT: the answer must have come at least 28 s after the ask, with at least
                //       twenty pictures drawn while it was out.
                const Drive toAsk = drive(scene, stub, bus, 20, [&] { return stub.heldTimelineAsks() > 0; }, 60);
                if (toAsk.stalled || stub.heldTimelineAsks() != 1) {
                    check(false, "the item asks for its acquire timeline (" + std::to_string(stub.timelineAsks()) + " ask(s))");
                    return true;
                }
                constexpr qint64 kAnswerAfterMs = 28000;
                QElapsedTimer sinceAsk;
                sinceAsk.start();
                const Drive waiting =
                    drive(scene, stub, bus, 1000, [&] { return sinceAsk.elapsed() >= kAnswerAfterMs; }, 200);
                const qint64 answeredAfter = sinceAsk.elapsed();
                const int onTimelineWhileOut = stub.onTimelineRequests();
                if (!stub.answerHeldTimeline()) {
                    wallpaperx::testing::harnessFailure("the stub could not answer the held timeline ask");
                    return true;
                }
                const Drive after = drive(scene, stub, bus, 12, [&] { return stub.valuesSent() >= 5; });
                check(!waiting.stalled && waiting.drawn >= 20 && answeredAfter >= kAnswerAfterMs,
                      "the answer came at least 28 s after the ask, with pictures drawn meanwhile (" +
                          std::to_string(answeredAfter) + " ms, " + std::to_string(waiting.drawn) + " drawn)");
                check(stub.timelineAsks() == 1,
                      "the daemon is asked for its timeline once over the whole run (" +
                          std::to_string(stub.timelineAsks()) + " ask(s))");
                check(onTimelineWhileOut == 0 && stub.valuesSent() >= 5 && !after.stalled && drawingCleanly(scene),
                      "the late answer is adopted: pictures come as values once it arrives (" +
                          std::to_string(stub.valuesSent()) + " value(s), " + std::to_string(onTimelineWhileOut) +
                          " asked before it; " + describe(scene) + ")");
                return true;
            }

            case Phase::ReplacedAsking: {
                // GOAL: S15.337 -- while the plugin's GetAcquireTimeline to a daemon that has lost the
                //       name (but is still on the bus) is unanswered, the new daemon is asked for its own
                //       timeline straight away, once, and its pictures come as values; when the old
                //       daemon's answer then arrives, its descriptor is closed and the new daemon's
                //       timeline is kept.
                // WHY-THIS-SHAPE: the old stub holds the ask and then releases the name, staying
                //       connected, so its answer can still come. That answer carries the write end of
                //       a pipe this test made: the read end sees end of file only once every copy is
                //       closed, the plugin's included, which no other observation of the plugin's
                //       record can show. A Peer.Ping from the old stub's connection, answered by the
                //       item's connection as it dispatches, is the barrier: messages from one sender
                //       arrive in order, so the answer was dispatched first.
                // CONTROL: in backend/kde/wallpaperx_item.cpp via control_batch.sh, re-run 2026-10-06, NVIDIA,
                //       each reverted: (a) `if (g_timeline.state == State::Asking) return true;` put back
                //       at the top of timelineSettledLocked -> "FAIL: while the request to the replaced
                //       daemon is out, the new daemon is asked for its timeline once and its pictures come
                //       as values (0 ask(s), 0 value(s); the old ask still out)"; (b) `superseded = fd;`
                //       deleted from the `g_timeline.ask != ask` branch of the reply handler -> only "FAIL:
                //       the replaced daemon's late answer is closed, not kept (its descriptor still open 5 s
                //       on)"; (c) that branch's test made `false`, so the late answer is adopted -> "FAIL:
                //       and the new daemon's timeline is kept: ... (4 of 6, 2 ask(s); ...)" beside (b)'s line.
                // COVERAGE-ASSERT: the old daemon's ask must still be held when the new one is graded,
                //       and the ping must have been answered.
                const Drive toAsk = drive(scene, stub, bus, 20, [&] { return stub.heldTimelineAsks() > 0; }, 60);
                if (toAsk.stalled || stub.heldTimelineAsks() != 1) {
                    check(false, "the item asks the first daemon for its acquire timeline (" +
                                     std::to_string(stub.timelineAsks()) + " ask(s))");
                    return true;
                }
                SuccessorDaemon successor(device, ring);
                QDBusConnection& busB = successor.bus;
                ExportingStub& stubB = successor.stub;
                if (!successor.takeNameFrom(bus)) {
                    wallpaperx::testing::harnessFailure("the second stub daemon never became the name's owner");
                    return true;
                }
                const quint64 pictures0 = scene.pictures();
                const bool drewB = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return scene.pictures() > pictures0;
                    },
                    5000);
                if (!drewB || !settleQuiet(scene, stubB)) {
                    check(false, "the item draws the new daemon's first picture and goes quiet (" + describe(scene) + ")");
                    return true;
                }
                const Drive reachB = drive(scene, stubB, busB, 8, [&] { return stubB.valuesSent() > 0; });
                check(stub.heldTimelineAsks() == 1 && !reachB.stalled && stubB.timelineAsks() == 1 &&
                          stubB.valuesSent() > 0,
                      "while the request to the replaced daemon is out, the new daemon is asked for its timeline once "
                      "and its pictures come as values (" +
                          std::to_string(stubB.timelineAsks()) + " ask(s), " + std::to_string(stubB.valuesSent()) +
                          " value(s); the old ask " + (stub.heldTimelineAsks() == 1 ? "still out" : "gone") + ")");

                int pipeFds[2] = {-1, -1};
                if (::pipe2(pipeFds, O_CLOEXEC) != 0) {
                    wallpaperx::testing::harnessFailure("no pipe could be made to stand in for the late timeline");
                    return true;
                }
                const bool answered = stub.answerHeldTimeline(pipeFds[1]);
                ::close(pipeFds[1]);
                QDBusPendingCall pong = bus.asyncCall(
                    QDBusMessage::createMethodCall(QDBusConnection::sessionBus().baseService(), QStringLiteral("/"),
                                                   QStringLiteral("org.freedesktop.DBus.Peer"), QStringLiteral("Ping")),
                    5000);
                const bool finished = pumpUntil([&] { return pong.isFinished(); }, 6000);
                const bool barrier = finished && !pong.isError();
                // Checked before the measurement it guards, so a failed staging does not wait it out.
                if (!answered || !barrier) {
                    ::close(pipeFds[0]);
                    const std::string ping = !finished       ? std::string("unanswered after 6 s")
                                             : pong.isError() ? pong.error().message().toStdString()
                                                              : std::string("answered");
                    wallpaperx::testing::harnessFailure(std::string("the late answer could not be staged (answered ") +
                                                        (answered ? "yes" : "no") + ", ping " + ping + ")");
                    return true;
                }
                const bool closed = pumpUntil(
                    [&] {
                        pollfd end{pipeFds[0], POLLIN, 0};
                        return ::poll(&end, 1, 0) == 1 && (end.revents & POLLHUP) != 0;
                    },
                    5000);
                ::close(pipeFds[0]);
                check(closed, "the replaced daemon's late answer is closed, not kept (its descriptor still open 5 s on)");
                const int valuesB = stubB.valuesSent();
                const Drive steadyB = drive(scene, stubB, busB, 6);
                check(!steadyB.stalled && stubB.valuesSent() - valuesB == 6 && stubB.timelineAsks() == 1 &&
                          drawingCleanly(scene),
                      "and the new daemon's timeline is kept: its pictures go on coming as values (" +
                          std::to_string(stubB.valuesSent() - valuesB) + " of 6, " +
                          std::to_string(stubB.timelineAsks()) + " ask(s); " + describe(scene) + ")");
                return true;
            }

            case Phase::RefusedTimeline:
            case Phase::RefusedBinary: {
                // GOAL: S13.223 -- once the context refuses a semaphore import -- the timeline's, or a
                //       picture's binary one -- it draws without the producer's hand-over for the rest
                //       of its life: one stderr line when it trips, every picture after it drawn, no
                //       semaphore import attempted again, and the wallpaper never blanks.
                // WHY-THIS-SHAPE: the refusal is the driver's own, from a descriptor that is no
                //       semaphore payload, so the flag is set by what sets it on a desktop. Afterwards
                //       the stub keeps sending REAL hand-overs, so "no import attempted" means dropped,
                //       not absent. Served and drawn are counted apart, because a picture that failed
                //       is replaced by the item's own re-fetch and a lockstep count alone would miss it.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) noteContextRefusedSemaphores made never
                //       to set the context record's `refusesSemaphores` (`std::exchange(..., true)` made a
                //       plain read; re-run 2026-10-08 via control_run.sh), so no context is ever marked -> "FAIL:
                //       the refusal is said once, on stderr (16 line(s))" (14 on refused-binary) and
                //       "FAIL: after the refusal no semaphore import is attempted again (10 over 10
                //       pictures with real hand-overs)" on both entries; (b) `handedOver = true;` deleted
                //       after it in updatePaintNode -> "FAIL: every picture served is drawn, the one that
                //       tripped it included (17 served, 16 drawn; ...)" on both. Observed 2026-09-23 on
                //       the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: the marker printed at the start must be in the capture.
                const int served0 = stub.served();
                const quint64 pictures0 = scene.pictures();
                if (phase == Phase::RefusedBinary) {
                    stub.sendAcquireFrom(served0 + 3, ExportingStub::Acquire::Unusable);
                    stub.sendAcquireFrom(served0 + 4, ExportingStub::Acquire::Real);
                }
                // The trip happens inside the drive: the timeline's first value, or the third picture.
                const Drive run = drive(scene, stub, bus, 6);
                const quint64 semaphoresAfterTrip = scene.semaphoreImports();
                const Drive after = drive(scene, stub, bus, 10);
                const int lines = tripLines();
                check(captureWorked(), "the stderr capture saw what was written during the phase");
                check(lines == 1, "the refusal is said once, on stderr (" + std::to_string(lines) + " line(s))");
                check(!run.stalled && !after.stalled &&
                          static_cast<quint64>(stub.served() - served0) == scene.pictures() - pictures0 &&
                          drawingCleanly(scene),
                      "every picture served is drawn, the one that tripped it included (" +
                          std::to_string(stub.served() - served0) + " served, " +
                          std::to_string(scene.pictures() - pictures0) + " drawn; " + describe(scene) + ")");
                check(scene.semaphoreImports() == semaphoresAfterTrip,
                      "after the refusal no semaphore import is attempted again (" +
                          std::to_string(scene.semaphoreImports() - semaphoresAfterTrip) + " over " +
                          std::to_string(after.drawn) + " pictures with real hand-overs)");
                if (phase == Phase::RefusedBinary) {
                    check(stub.acquireFdsSent() > 0 && stub.unusableAcquiresSent() == 1,
                          "the stub sent real hand-overs after the one refused (" +
                              std::to_string(stub.unusableAcquiresSent()) + " refused)");
                } else {
                    check(stub.timelinesGranted() >= 1, "the refused descriptor was handed over as the timeline");
                }
                return true;
            }

            case Phase::WaitNoTrip: {
                // GOAL: S13.223's other half, and S13.217 -- a failed WAIT on the imported timeline is
                //       not a refused import: it does not trip the context, the item gives up only that
                //       daemon's timeline and asks it for sync_fds from then on, and those binary
                //       imports go on succeeding, one per picture. The wallpaper never blanks.
                // WHY-THIS-SHAPE: the wait is failed by the test (gl_semaphore_fault.h) after the
                //       timeline imported, which is the one failure the driver itself cannot be made
                //       to produce on demand. Imports afterwards are counted, because a tripped
                //       context would draw too -- only the counter tells "waits on each picture" from
                //       "stopped waiting".
                // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp), (a) `if
                //       (importRefused)` made `if (importRefused || !handedOver)` -> "FAIL: a failed wait
                //       does not trip the context (1 line(s))" and "FAIL: and every picture after it is
                //       drawn waiting on its own binary semaphore (10 drawn, 0 imports; ...)"; (b)
                //       `m_timelineRefusedBy = incoming->sender;` deleted -> "FAIL: the item asks that
                //       daemon for sync_fds after its timeline wait failed (10 further request(s) on
                //       it)". Observed 2026-09-23 on the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: exactly one wait must have been failed.
                if (!reachTimeline(6)) {
                    check(false, "the item reaches the timeline before its wait is failed");
                    return true;
                }
                const int failed0 = g_glSemaphoreFaults.waitsFailed.load();
                g_glSemaphoreFaults.failWaits.store(1);
                const Drive failing = drive(scene, stub, bus, 1);
                g_glSemaphoreFaults.failWaits.store(0);
                const int onTimeline = stub.onTimelineRequests();
                const quint64 semaphores = scene.semaphoreImports();
                const Drive after = drive(scene, stub, bus, 10);
                const int lines = tripLines();
                check(g_glSemaphoreFaults.waitsFailed.load() - failed0 == 1, "one wait on the timeline was failed");
                check(captureWorked(), "the stderr capture saw what was written during the phase");
                check(lines == 0, "a failed wait does not trip the context (" + std::to_string(lines) + " line(s))");
                check(stub.onTimelineRequests() == onTimeline,
                      "the item asks that daemon for sync_fds after its timeline wait failed (" +
                          std::to_string(stub.onTimelineRequests() - onTimeline) + " further request(s) on it)");
                check(!failing.stalled && !after.stalled && drawingCleanly(scene) &&
                          scene.semaphoreImports() - semaphores == static_cast<quint64>(after.drawn),
                      "and every picture after it is drawn waiting on its own binary semaphore (" +
                          std::to_string(after.drawn) + " drawn, " +
                          std::to_string(scene.semaphoreImports() - semaphores) + " imports; " + describe(scene) + ")");
                return true;
            }

            case Phase::SetupCreate:
            case Phase::SetupType: {
                // GOAL: S13.217 -- a timeline that cannot be set up in this context (no semaphore name,
                //       or a type the driver will not set) costs at most one picture: the item asks on
                //       the timeline once, goes back to sync_fds, never blanks, and does not trip the
                //       context -- binary imports go on succeeding.
                // WHY-THIS-SHAPE: the failure is staged in the GL entry points the plugin resolves
                //       (gl_semaphore_fault.h), because no descriptor a daemon sends can make
                //       glCreateSemaphoresNV or glSemaphoreParameterivNV fail.
                // CONTROL: (a) in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp),
                //       `&& timeline.semaphore != 0` added to the condition that records the refusal --
                //       the shape before the last review -> on both entries "FAIL: the item asked on the
                //       timeline once, then went back (25099 request(s))" (24490 on setup-type), the item
                //       failing picture after picture, and "FAIL: the wallpaper keeps
                //       drawing (frame shown: no, reason [WallpaperX produced a frame this desktop could
                //       not import.])"; (b) in FrameImport::importTimeline (backend/kde/frame_import.cpp)
                //       `*importRefused = true` set on the create failure -> on setup-create "FAIL: a
                //       setup failure does not trip the context (1 line(s))" and "FAIL: binary imports
                //       go on, one per picture (0 for 6)". Observed 2026-09-23 on the NVIDIA entry.
                //       Reverted.
                // COVERAGE-ASSERT: the fault must have fired.
                const Drive run = drive(scene, stub, bus, 14);
                const quint64 semaphores = scene.semaphoreImports();
                const Drive after = drive(scene, stub, bus, 6);
                const int lines = tripLines();
                check(g_glSemaphoreFaults.timelineSetupsFailed.load() >= 1,
                      "the timeline's setup was failed (" + std::to_string(g_glSemaphoreFaults.timelineSetupsFailed) +
                          " time(s))");
                check(stub.onTimelineRequests() == 1, "the item asked on the timeline once, then went back (" +
                                                          std::to_string(stub.onTimelineRequests()) + " request(s))");
                check(!run.stalled && !after.stalled && drawingCleanly(scene),
                      "the wallpaper keeps drawing (" + describe(scene) + ")");
                check(captureWorked() && lines == 0,
                      "a setup failure does not trip the context (" + std::to_string(lines) + " line(s))");
                check(scene.semaphoreImports() - semaphores == static_cast<quint64>(after.drawn),
                      "binary imports go on, one per picture (" +
                          std::to_string(scene.semaphoreImports() - semaphores) + " for " +
                          std::to_string(after.drawn) + ")");
                return true;
            }

            case Phase::StrayValue: {
                // GOAL: S13.221 -- a value is read only from a reply to a request that asked on the
                //       timeline. A reply to one that did not, carrying a nonzero acquire_value, is
                //       drawn as a picture with no hand-over: imported afresh, never through a kept
                //       import, and no timeline touched.
                // WHY-THIS-SHAPE: one image, so every picture after the first could reuse its import
                //       -- and would, if the value were read, since a value is a hand-over. The
                //       counters then tell the two apart without any wait on a timeline that could
                //       stall. The stray values start after the item's first picture, so the setup
                //       above is the ordinary one.
                // CONTROL: `askedForValue &&` deleted from WallpaperXItem::takeFrameReply
                //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: every picture with a stray value is
                //       drawn (0 of 7)" and "FAIL: each is imported afresh, as a picture without a
                //       hand-over (0 import(s), 0 reuse(s))". Observed 2026-09-23 on the NVIDIA entry;
                //       re-run 2026-10-04 via control_run.sh, the same two lines. Reverted.
                const quint64 imports0 = scene.imports();
                const quint64 reused0 = scene.reused();
                const quint64 semaphores0 = scene.semaphoreImports();
                stub.sendValueUnasked(1);
                stub.serveNext(ring.front());
                const Drive run = drive(scene, stub, bus, 1);
                const Drive more = [&] {
                    Drive d;
                    for (int i = 0; i < 6 && !d.stalled; ++i) {
                        stub.serveNext(ring.front());
                        if (drawNext(scene, stub, bus)) {
                            ++d.drawn;
                        } else {
                            d.stalled = true;
                        }
                    }
                    return d;
                }();
                const int drawn = run.drawn + more.drawn;
                check(!run.stalled && !more.stalled && drawn == 7,
                      "every picture with a stray value is drawn (" + std::to_string(drawn) + " of 7)");
                check(scene.imports() - imports0 == 7 && scene.reused() == reused0,
                      "each is imported afresh, as a picture without a hand-over (" +
                          std::to_string(scene.imports() - imports0) + " import(s), " +
                          std::to_string(scene.reused() - reused0) + " reuse(s))");
                check(scene.semaphoreImports() == semaphores0 && stub.onTimelineRequests() == 0,
                      "and no semaphore is imported for it (" + std::to_string(scene.semaphoreImports() - semaphores0) +
                          ")");
                return true;
            }

            case Phase::Restart:
            case Phase::RestartMemory: {
                // GOAL: S13.221 across a daemon restart mid-stream -- a value is only ever waited on the
                //       timeline of the daemon (unique bus name) it was asked of. A request in flight to
                //       the old daemon when the name moves is still drawn on the OLD daemon's timeline,
                //       through the import the node holds for its image -- whether the reply left the
                //       memory out under the daemon's rule (restart) or carried it (restart-memory) --
                //       before anything is asked of the new daemon, and nothing is released to either
                //       daemon for it; the new daemon's pictures are waited on its own, imported afresh.
                //       False for a value paired by the owner at reply time, which is a wait that may
                //       never end, and for a reply credited to the new daemon, which matches no import.
                // WHY-THIS-SHAPE: the old daemon releases the name but stays connected, holding one
                //       GetFrame reply asked on the timeline until the plugin has seen the new owner (a watcher on
                //       the plugin's own connection, whose change signal the plugin's watcher received
                //       first). The old daemon's payload is 200 signals ahead of the new one's, so a
                //       value waited on the wrong timeline COMPLETES -- no stalled GPU -- and only the
                //       import count and the requests tell the pairing was wrong.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) WallpaperXItem::fetchFrame's reply
                //       handler passing `BufferClaims::instance().currentDaemon()` to takeFrameReply instead
                //       of `asked` -> "FAIL: and its value is drawn on the old daemon's timeline, through the
                //       held import (..., 0 reuse(s), 0 import(s), 1 lost; memory left out)" (restart-memory:
                //       "1 import(s), 0 lost; memory sent") and "FAIL: and nothing is released to either
                //       daemon for it (0 covering it to the old, 1 to the new)"; (b) `m_fetch != Fetch::Asking
                //       &&` deleted from discardHeldFrame -> the first line; (c) `forgetStaleImports(node);`
                //       added straight after the lambda at the top of updatePaintNode -> the first line;
                //       (d) `&& record.timelineOwner == sender` deleted from contextTimeline's reuse line
                //       (re-run 2026-10-08 via control_run.sh after the import moved to the context) -> "FAIL: and
                //       imports the new daemon's timeline rather than waiting its values on the old one (1
                //       import(s) for 1 binary picture(s))". Via control_batch.sh, re-run 2026-10-06 on the
                //       NVIDIA entry, (a)-(c) on both restart entries. Reverted.
                if (!reachTimeline(6)) {
                    check(false, "the item reaches the old daemon's timeline");
                    return true;
                }
                (void)drive(scene, stub, bus, 3);
                SuccessorDaemon successor(device, ring);
                QDBusConnection& busB = successor.bus;
                ExportingStub& stubB = successor.stub;
                if (!successor.ok) {
                    wallpaperx::testing::harnessFailure("the second stub daemon could not be put on the bus");
                    return true;
                }

                if (phase == Phase::RestartMemory) stub.sendMemory(ExportingStub::Memory::Always);
                stub.holdServeNumber(stub.served() + 1);
                announce(bus, static_cast<quint64>(stub.served()) + 1);
                if (!pumpUntil([&] { return stub.replyIsHeld(); }, 3000)) {
                    wallpaperx::testing::harnessFailure("the item's request to the old daemon never arrived to be held");
                    return true;
                }
                const bool heldOnTimeline = stub.onTimelineRequests() > 0;
                const int onTimelineBefore = stub.onTimelineRequests();
                stubB.holdServeNumber(1);
                if (!successor.takeNameFrom(bus)) {
                    wallpaperx::testing::harnessFailure("the second stub daemon never became the name's owner");
                    return true;
                }
                pumpFor(50);
                const quint64 pictures0 = scene.pictures();
                const quint64 semaphores0 = scene.semaphoreImports();
                const quint64 imports0 = scene.imports();
                const quint64 reused0 = scene.reused();
                const quint64 lost0 = scene.lost();
                // Releases of earlier pictures land after their fences, so what is counted is a release
                // covering the late reply's own hand-over, not every release after this point.
                const quint64 lateNumber = stub.asks().back().handover;
                const auto coveringLate = [&] {
                    return std::count_if(stub.releases().begin(), stub.releases().end(),
                                         [&](const ExportingStub::Release& r) { return r.handover >= lateNumber; });
                };
                const int askedOfB = stubB.requests();
                const bool memorySent = stub.asks().back().memorySent;
                stub.sendHeldReply();
                const bool oldDrawn = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return scene.pictures() > pictures0 && stubB.replyIsHeld();
                    },
                    3000);
                check(heldOnTimeline && stub.onTimelineRequests() == onTimelineBefore,
                      "the request in flight across the restart was asked on the old daemon's timeline");
                check(oldDrawn && drawingCleanly(scene) && scene.semaphoreImports() == semaphores0 &&
                          scene.pictures() == pictures0 + 1 && scene.reused() == reused0 + 1 &&
                          scene.imports() == imports0 && scene.lost() == lost0,
                      "and its value is drawn on the old daemon's timeline, through the held import (" + describe(scene) +
                          ", " + std::to_string(scene.semaphoreImports() - semaphores0) + " semaphore import(s), " +
                          std::to_string(scene.reused() - reused0) + " reuse(s), " +
                          std::to_string(scene.imports() - imports0) + " import(s), " +
                          std::to_string(scene.lost() - lost0) + " lost; memory " + (memorySent ? "sent" : "left out") +
                          ")");
                // CONTROL: `m_fetch = Fetch::Idle;` added before the `fetchFrame(0)` that ends the
                //       DaemonWatch::replaced handler (backend/kde/wallpaperx_item.cpp), so the fetch is not
                //       queued behind the reply out -> "FAIL: nothing was asked of the new daemon before the
                //       old daemon's reply landed (1 request(s))". Via control_batch.sh, re-run 2026-10-07.
                //       Reverted.
                check(askedOfB == 0, "nothing was asked of the new daemon before the old daemon's reply landed (" +
                                         std::to_string(askedOfB) + " request(s))");
                // The staging this entry claims, which the plugin's declaring decides: a fetch naming
                // nothing would bring the memory and turn restart into restart-memory.
                // CONTROL: `if (m_declareNoImports) return declared;` made `return declared;` in
                //       WallpaperXItem::importsToDeclare (backend/kde/wallpaperx_item.cpp) -> "FAIL: the late
                //       reply left out the memory, as this entry stages it". Via control_batch.sh, re-run
                //       2026-10-06. Reverted.
                check(memorySent == (phase == Phase::RestartMemory),
                      std::string("the late reply ") + (phase == Phase::RestartMemory ? "carried" : "left out") +
                          " the memory, as this entry stages it");
                pumpFor(100);
                check(coveringLate() == 0 && stubB.releases().empty(),
                      "and nothing is released to either daemon for it (" + std::to_string(coveringLate()) +
                          " covering it to the old, " + std::to_string(stubB.releases().size()) + " to the new)");
                // Every binary acquire the new daemon sent is in the count, the held first one included.
                const quint64 semaphores1 = scene.semaphoreImports();
                const int binaryB0 = 0;
                const quint64 pictures1 = scene.pictures();
                stubB.sendHeldReply();
                // The new daemon's first picture, and whatever the item asks after it, before the drive.
                if (!pumpUntil(
                        [&] {
                            scene.renderPass();
                            return scene.pictures() > pictures1;
                        },
                        3000) ||
                    !settleQuiet(scene, stubB)) {
                    // GOAL: S15.317 -- after a replaced daemon's late reply is drawn, the new daemon's
                    //       pictures, numbered from 1, are drawn: the late reply's generation counts in
                    //       the old daemon's numbering and is not written back as the screen's.
                    // CONTROL: `replaced ? 0 : ` deleted from the generation in
                    //       WallpaperXItem::takeFrameReply (backend/kde/wallpaperx_item.cpp) -> "FAIL: the
                    //       item draws the new daemon's first picture and goes quiet (frame shown: yes,
                    //       reason [])". Via control_batch.sh, re-run 2026-10-06 on the NVIDIA entry. Reverted.
                    check(false, "the item draws the new daemon's first picture and goes quiet (" + describe(scene) + ")");
                    return true;
                }
                const Drive reachB = drive(scene, stubB, busB, 6, [&] { return stubB.valuesSent() > 0; });
                check(!reachB.stalled && stubB.valuesSent() > 0,
                      "the item reaches the new daemon's timeline (" + std::to_string(stubB.timelineAsks()) + " ask(s), " +
                          std::to_string(stubB.onTimelineRequests()) + " request(s) on it)");
                check(scene.semaphoreImports() - semaphores1 ==
                          static_cast<quint64>(stubB.acquireFdsSent() - binaryB0) + 1,
                      "and imports the new daemon's timeline rather than waiting its values on the old one (" +
                          std::to_string(scene.semaphoreImports() - semaphores1) + " import(s) for " +
                          std::to_string(stubB.acquireFdsSent() - binaryB0) + " binary picture(s))");
                const int valuesB = stubB.valuesSent();
                const Drive steadyB = drive(scene, stubB, busB, 8);
                check(!steadyB.stalled && stubB.valuesSent() - valuesB == 8 && drawingCleanly(scene),
                      "the new daemon's pictures keep coming as values and are drawn (" +
                          std::to_string(stubB.valuesSent() - valuesB) + " of 8; " + describe(scene) + ")");

                // GOAL: S13.221 through the replacement -- the late reply's claim is never released,
                //       even once the new daemon's pictures have replaced it and its import has ended:
                //       nothing to the old daemon covers it, and nothing to the new names a number the new
                //       daemon never issued, which a claim credited to the wrong daemon would.
                // WHY-THIS-SHAPE: read after the new daemon's pictures have been drawn and their fences
                //       passed, which is when the late import can end; the at-reply check above samples a
                //       moment before that. The old daemon numbers its hand-overs from 501, above any the
                //       new one reaches here, so a release naming one at the new daemon is that claim.
                // CONTROL: in WallpaperXItem::fetchFrame's reply handler (backend/kde/wallpaperx_item.cpp),
                //       `BufferClaims::instance().currentDaemon()` passed to takeFrameReply instead of `asked`,
                //       crediting the late reply to the new daemon -> "FAIL: and the late reply is never
                //       released, after the new daemon's pictures replaced it either (0 covering it to the old,
                //       1 naming an unissued number to the new)", the at-reply lines above red beside it. Via
                //       control_batch.sh, 2026-10-06. Reverted. Deleting only the `daemon ==
                //       m_daemon.lifetime &&` term in BufferClaims::take leaves this green: the late claim
                //       still carries the old lifetime, and BufferClaims::giveBack refuses it on that.
                pumpFor(100);
                const auto unissuedAtB = std::count_if(
                    stubB.releases().begin(), stubB.releases().end(),
                    [&stubB](const ExportingStub::Release& r) { return r.handover > stubB.lastHandover(); });
                check(coveringLate() == 0 && unissuedAtB == 0,
                      "and the late reply is never released, after the new daemon's pictures replaced it either (" +
                          std::to_string(coveringLate()) + " covering it to the old, " + std::to_string(unissuedAtB) +
                          " naming an unissued number to the new)");
                return true;
            }

            case Phase::RetryReplaced: {
                // GOAL: S15.348's sibling -- a "not now" answer to GetAcquireTimeline delays asking only the
                //       daemon that gave it: when that daemon is replaced, the new one is asked for its
                //       timeline at once, not when the old answer's retry delay would have ended.
                // WHY-THIS-SHAPE: "at once" is graded as "before the old retry delay could have ended",
                //       both asks stamped by the stubs and compared on one monotonic clock: the delay is
                //       a floor (now must pass the retry time), so an item still honouring it cannot ask
                //       sooner. A replacement that itself took most of the delay leaves no margin and is
                //       reported not verified.
                // CONTROL: `if (g_timeline.owner != daemon) return false;` deleted from
                //       timelineSettledLocked (backend/kde/wallpaperx_item.cpp) -> "FAIL: a daemon that
                //       replaced one that refused the timeline for now is asked at once (asked -1 ms after the
                //       refusal, replaced at 301 ms)". Via control_batch.sh, 2026-10-06. Reverted.
                const Drive toAsk = drive(scene, stub, bus, 20, [&] { return stub.timelineAsks() > 0; }, 60);
                if (toAsk.stalled || stub.timelineAsks() != 1) {
                    check(false, "the item asks the first daemon for its acquire timeline (" +
                                     std::to_string(stub.timelineAsks()) + " ask(s))");
                    return true;
                }
                const qint64 refusedAt = stub.clockReferenceMs() + stub.timelineAskTimesMs().front();
                SuccessorDaemon successor(device, ring);
                if (!successor.takeNameFrom(bus)) {
                    wallpaperx::testing::harnessFailure("the second stub daemon never became the name's owner");
                    return true;
                }
                QElapsedTimer now;
                now.start();
                const qint64 replacedAt = now.msecsSinceReference();
                constexpr qint64 kExpectedRetryMs = 2000;  // mirrored from the plugin's kRefusedRetryMs
                if (replacedAt - refusedAt > kExpectedRetryMs - 500) {
                    notVerified("S15.348's timeline half", "the replacement ended " + std::to_string(replacedAt - refusedAt) +
                                                               " ms after the refusal, leaving the retry delay no margin");
                    return true;
                }
                const Drive toB = drive(scene, successor.stub, successor.bus, 10,
                                        [&] { return successor.stub.timelineAsks() > 0; });
                const qint64 askedB = successor.stub.timelineAsks() > 0
                                          ? successor.stub.clockReferenceMs() + successor.stub.timelineAskTimesMs().front()
                                          : -1;
                check(!toB.stalled && askedB >= 0 && askedB - refusedAt < kExpectedRetryMs,
                      "a daemon that replaced one that refused the timeline for now is asked at once (asked " +
                          std::to_string(askedB >= 0 ? askedB - refusedAt : -1) + " ms after the refusal, replaced at " +
                          std::to_string(replacedAt - refusedAt) + " ms)");
                return true;
            }

            case Phase::StaleValue: {
                // GOAL: S13.224 -- a reply carrying a value from a daemon whose timeline this item has
                //       just given up on is not a failed hand-over: it is not drawn, and it does not
                //       count toward the eight tolerated failures. S13.203 on the timeline: the wait
                //       that failed DOES count, and the eight after it are binary waits.
                // WHY-THIS-SHAPE: the stale reply exists only when a second value request went out
                //       before the render pass that failed the first. So every reply is held by the
                //       stub and released one at a time: the first is taken and not rendered, the
                //       second requested, and only then is the pass run. From there the item always
                //       has its next request out when a reply lands, so each step releases exactly one
                //       reply and renders it before the next is released -- released as they came, a
                //       reply could replace one the render pass had not seen, and the count would be
                //       of whatever the timing let through. The run lands exactly on the edge: the
                //       failed timeline wait, the stale reply and seven failed binary waits keep the
                //       picture; the next failure blanks. One more counted anywhere blanks it early.
                // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp), (a)
                //       `++m_handOversFailed;` added to the stale-reply branch -> "FAIL: the stale value
                //       does not count: eight failures and the stale reply keep the picture, silently
                //       (frame shown: no, reason [WallpaperX produced a frame this desktop could not
                //       import.])"; (b) kToleratedHandOverFailures 8 -> 7 -> the same line; (c) the
                //       `m_handOversFailed <= kToleratedHandOverFailures &&` term deleted -> "FAIL: and
                //       the ninth counted failure blanks it with the reason (frame shown: yes, reason
                //       [])"; (d) `m_timelineRefusedBy = incoming->sender;` deleted, so nothing is stale
                //       -> both lines red, "frame shown: yes" in each. Observed 2026-09-23 on the NVIDIA
                //       entry. Reverted.
                // COVERAGE-ASSERT: the waits failed are counted, and the stale reply is required to
                //       have been asked on the timeline.
                if (!reachTimeline(6)) {
                    check(false, "the item reaches the timeline");
                    return true;
                }
                QSGTexture* shown = itemTexture(scene.item);
                const auto stillShowing = [&] { return drawingCleanly(scene) && itemTexture(scene.item) == shown; };
                const int failed0 = g_glSemaphoreFaults.waitsFailed.load();
                const int onTimeline0 = stub.onTimelineRequests();
                const auto held = [&](size_t count) { return pumpUntil([&] { return stub.heldReplies() >= count; }, 3000); };
                // Releases the oldest held reply, waits for the request the item sends on taking it,
                // and renders what it took.
                const auto step = [&] {
                    if (!stub.sendOldestHeld() || !held(1)) return false;
                    for (int pass = 0; pass < 3; ++pass) scene.renderPass();
                    pumpFor(20);
                    return true;
                };

                stub.holdEveryReply(true);
                g_glSemaphoreFaults.failWaits.store(1);
                const int first = stub.served() + 1;
                announce(bus, static_cast<quint64>(first));
                announce(bus, static_cast<quint64>(first) + 1);
                bool built = held(1) && stub.sendOldestHeld() && held(1);
                const bool staleOnTimeline = stub.onTimelineRequests() - onTimeline0 == 2;
                for (int pass = 0; pass < 3; ++pass) scene.renderPass();
                pumpFor(20);
                const bool timelineWaitFailed = g_glSemaphoreFaults.waitsFailed.load() - failed0 == 1;
                // The stale reply, then seven failed binary waits.
                g_glSemaphoreFaults.failWaits.store(kToleratedHandOverFailures - 1);
                for (int i = 0; built && i < kToleratedHandOverFailures; ++i) built = step();
                if (!built) {
                    // The item's own contract, not the harness's: a failed hand-over is asked for again.
                    check(false, "the item asks again after each failure of the run (" +
                                     std::to_string(stub.served() - first + 1) + " served; " + describe(scene) + ")");
                    return true;
                }
                check(staleOnTimeline && timelineWaitFailed,
                      "the run starts with a failed wait on the timeline and a value request already out");
                check(g_glSemaphoreFaults.waitsFailed.load() - failed0 == kToleratedHandOverFailures,
                      "eight waits failed in all, one on the timeline and seven binary (" +
                          std::to_string(g_glSemaphoreFaults.waitsFailed.load() - failed0) + ")");
                check(stillShowing(), "the stale value does not count: eight failures and the stale reply keep the "
                                      "picture, silently (" + describe(scene) + ")");
                g_glSemaphoreFaults.failWaits.store(1);
                (void)step();
                check(!claimsFrameShown(scene.item) && scene.reason() == QLatin1String(kCouldNotImport),
                      "and the ninth counted failure blanks it with the reason (" + describe(scene) + ")");
                g_glSemaphoreFaults.failWaits.store(0);
                stub.holdEveryReply(false);
                stub.refuseFurtherWith(QStringLiteral("io.wallpaperx.Error.NoFrame"));
                while (stub.sendOldestHeld()) {
                }
                pumpFor(100);
                return true;
            }

            case Phase::ContextMove: {
                // GOAL: S13.225 -- a picture asked for as a value that arrives in a GL context which
                //       cannot wait on a timeline (the item moved to another window meanwhile) is drawn
                //       without the hand-over, as a context without semaphores draws sync_fds, and the
                //       next picture is asked for as sync_fds and waited on. False for the shape before
                //       the last review, which checked only for sync_fds support and then failed the
                //       value's wait -- on a node that has no picture yet, a blank.
                // WHY-THIS-SHAPE: two render-controlled windows in one process, the second with
                //       GL_NV_timeline_semaphore hidden from its extension list (gl_semaphore_fault.h),
                //       which is how a context without it looks to the plugin. The value request goes
                //       out from the first window and its reply is held until the item has moved, so
                //       the value lands in the second; no other ordering reaches the check. The failure
                //       it guards is transient -- the item's re-fetch draws the next picture -- so the end
                //       state cannot show it: drawn is counted against served, and the published reason
                //       is required never to have changed.
                // CONTROL: in dropUnwaitableSync (backend/kde/wallpaperx_item.cpp), the per-form test
                //       made `if (FrameImport::supportsSync(&whyNot)) return;` for both forms -> "FAIL: the
                //       value that landed in a context without timeline waits is drawn, and nothing is
                //       said (2 drawn of 4 served, 2 reason change(s); frame shown: yes, reason [])" and
                //       "FAIL: and the plugin says once that it draws without the hand-over". Observed
                //       2026-09-23 on the NVIDIA entry. Reverted.
                if (!reachTimeline(6)) {
                    check(false, "the item reaches the timeline in the first window");
                    return true;
                }
                Scene other;
                other.window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
                if (!other.control.initialize()) {
                    notVerified(kGoal, "a second OpenGL scene graph could not be brought up");
                    return true;
                }
                struct Invalidate {
                    Scene& other;
                    ~Invalidate() {
                        g_glSemaphoreFaults.hideTimelineIn.store(EGL_NO_CONTEXT);
                        other.renderPass();
                        other.control.invalidate();
                    }
                } invalidate{other};
                EGLContext otherContext = EGL_NO_CONTEXT;
                bool hidden = false;
                other.renderPass([&] {
                    otherContext = eglGetCurrentContext();
                    g_glSemaphoreFaults.hideTimelineIn.store(otherContext);
                    hidden = otherContext != EGL_NO_CONTEXT && !wallpaperx::FrameImport::supportsTimeline() &&
                             wallpaperx::FrameImport::supportsSync();
                });
                if (!hidden) {
                    notVerified(kGoal, "the second window's context could not be made to look like one without "
                                       "GL_NV_timeline_semaphore (it is not EGL-backed, or the extension list is not "
                                       "read through glGetStringi)");
                    return true;
                }

                // With the memory, though the request named the image: left out, the picture is a lost
                // import -- the node it named stayed with the first window -- and is asked again
                // declaring nothing, so the path that draws without the hand-over is never reached.
                stub.sendMemory(ExportingStub::Memory::Always);
                stub.holdEveryReply(true);
                const int onTimeline0 = stub.onTimelineRequests();
                announce(bus, stub.nextGeneration());
                if (!pumpUntil([&] { return stub.heldReplies() == 1; }, 3000) ||
                    stub.onTimelineRequests() != onTimeline0 + 1) {
                    wallpaperx::testing::harnessFailure("the item's next request was not a value request to hold");
                    return true;
                }
                auto* rootItem = qobject_cast<QQuickItem*>(scene.root.get());
                rootItem->setParentItem(other.window.contentItem());
                other.item = scene.item;  // not owned: the first scene's root still is
                pumpFor(20);
                scene.renderPass();  // the first window lets its node go, with its own context current
                const int values = stub.valuesSent();
                const quint64 pictures0 = scene.pictures();
                const int served0 = stub.served();  // the held picture included
                const int reasonSignals0 = scene.root->property("reasonSignals").toInt();
                StderrCapture movedOutput;
                stub.holdEveryReply(false);
                stub.sendOldestHeld();
                const bool drawn = pumpUntil(
                    [&] {
                        other.renderPass();
                        return scene.pictures() > pictures0;
                    },
                    3000);
                // Whatever the item had already asked before the new window's first pass lands too.
                const bool settled = settleQuiet(other, stub);
                const std::string said = movedOutput.finish();
                // The held picture counts in the drawn total, so a failure of it -- replaced by the
                // item's own re-fetch, which is drawn -- leaves drawn one short of served.
                const int servedSince = stub.served() - served0 + 1;
                const int reasonSignals = scene.root->property("reasonSignals").toInt() - reasonSignals0;
                check(values > 0 && drawn && settled && drawingCleanly(scene) &&
                          scene.pictures() - pictures0 == static_cast<quint64>(servedSince) && reasonSignals == 0,
                      "the value that landed in a context without timeline waits is drawn, and nothing is said (" +
                          std::to_string(scene.pictures() - pictures0) + " drawn of " + std::to_string(servedSince) +
                          " served, " + std::to_string(reasonSignals) + " reason change(s); " + describe(scene) + ")");
                check(occurrences(said, "drawing frames without the producer's hand-over") == 1,
                      "and the plugin says once that it draws without the hand-over");
                const int onTimeline1 = stub.onTimelineRequests();
                const quint64 semaphores = scene.semaphoreImports();
                const Drive after = drive(other, stub, bus, 4);
                check(!after.stalled && stub.onTimelineRequests() == onTimeline1 &&
                          scene.semaphoreImports() - semaphores == static_cast<quint64>(after.drawn),
                      "the pictures after it are asked for as sync_fds and waited on (" +
                          std::to_string(stub.onTimelineRequests() - onTimeline1) + " value request(s), " +
                          std::to_string(scene.semaphoreImports() - semaphores) + " import(s) for " +
                          std::to_string(after.drawn) + " picture(s))");
                rootItem->setParentItem(scene.window.contentItem());
                pumpFor(20);
                return true;
            }

            case Phase::Rebuild: {
                // Hides and shows the item, which destroys its node and builds a new one, and draws the
                // new node's first picture. `waited`: that picture came as a value and a wait was queued.
                const auto rebuildOnce = [&](ExportingStub& from, QDBusConnection& on, int cycle, bool& waited) {
                    scene.item->setVisible(false);
                    scene.renderPass();
                    pumpFor(2);
                    scene.renderPass();
                    if (itemTexture(scene.item) != nullptr) {
                        wallpaperx::testing::harnessFailure("the hidden item kept its node (cycle " +
                                                            std::to_string(cycle) + "), so nothing was rebuilt");
                        return false;
                    }
                    const quint64 before = scene.pictures();
                    const int waitsBefore = g_glSemaphoreFaults.waitsSeen.load();
                    const int valuesBefore = from.valuesSent();
                    scene.item->setVisible(true);
                    bool drawn = pumpUntil(
                        [&] {
                            scene.renderPass();
                            return scene.pictures() > before;
                        },
                        3000);
                    if (!drawn) drawn = drawNext(scene, from, on);
                    if (!drawn) {
                        check(false, "the rebuilt node draws a picture (cycle " + std::to_string(cycle) + ")");
                        return false;
                    }
                    waited = from.valuesSent() > valuesBefore && g_glSemaphoreFaults.waitsSeen.load() > waitsBefore;
                    return true;
                };

                // GOAL: S15.419 -- node rebuilds in one GL context never import the daemon's timeline
                //       again: over 300 hide/show rebuilds the semaphore-import counter stays flat, each
                //       rebuilt node's picture comes as a value and is waited on, and no refusal is
                //       printed. False for an import per node, which NVIDIA refuses from about the 253rd
                //       in one context, after which nothing there is waited on.
                // WHY-THIS-SHAPE: 300 rather than a handful: a handful of re-imports is green on the
                //       driver too, and the refusal only comes past 250. Hiding the item is the cheapest
                //       real trigger of a node rebuild; the item's own re-fetch draws the new node's picture.
                // CONTROL: in contextTimeline (backend/kde/wallpaperx_item.cpp), the reuse line `if
                //       (record.timeline.semaphore != 0 && record.timelineOwner == sender) return
                //       record.timeline;` deleted, via control_batch.sh, 2026-10-08, NVIDIA -> "FAIL: every
                //       rebuilt node's picture comes as a value and is waited on (first not: 252 of 300)",
                //       "FAIL: no node rebuild imports the timeline again (253 import(s) over 300 rebuilds)"
                //       and "FAIL: no semaphore import was refused (1 line(s))": the driver's refusal at the
                //       253rd import. Reverted.
                // COVERAGE-ASSERT: every one of the 300 rebuilds must have drawn, and the capture must
                //       have seen its own marker.
                if (!reachTimeline(6)) {
                    check(false, "the item reaches the timeline within six pictures");
                    return true;
                }
                constexpr int kRebuilds = 300;
                const quint64 semaphores0 = scene.semaphoreImports();
                int rebuilt = 0;
                int firstUnwaited = -1;
                for (int i = 0; i < kRebuilds; ++i) {
                    bool waited = false;
                    if (!rebuildOnce(stub, bus, i, waited)) break;
                    ++rebuilt;
                    if (!waited && firstUnwaited < 0) firstUnwaited = i;
                }
                const quint64 rebuildImports = scene.semaphoreImports() - semaphores0;
                std::printf("item_timeline_test: %d rebuild(s), %llu timeline import(s) during them, %llu picture(s) in "
                            "all\n",
                            rebuilt, static_cast<unsigned long long>(rebuildImports),
                            static_cast<unsigned long long>(scene.pictures()));
                std::fflush(stdout);
                check(rebuilt == kRebuilds, "every rebuilt node draws a picture (" + std::to_string(rebuilt) + " of " +
                                                std::to_string(kRebuilds) + ")");
                check(firstUnwaited < 0, "every rebuilt node's picture comes as a value and is waited on (first not: " +
                                             std::to_string(firstUnwaited) + " of " + std::to_string(rebuilt) + ")");
                check(rebuildImports == 0, "no node rebuild imports the timeline again (" +
                                               std::to_string(rebuildImports) + " import(s) over " +
                                               std::to_string(rebuilt) + " rebuilds)");

                // GOAL: S15.419's replacement half -- a new daemon's timeline is imported once into a
                //       context holding the old one's, its values are waited on, and node rebuilds under
                //       it import nothing either.
                // WHY-THIS-SHAPE: the old daemon's timeline is hundreds of values ahead of anything the
                //       new one sends here, so a new value waited on the OLD timeline completes rather
                //       than stalling, and only the import count tells the pairing was wrong. The late
                //       reply from the old daemon, waited on its own timeline, is the restart entry's.
                // CONTROL: `&& record.timelineOwner == sender` deleted from contextTimeline's reuse line
                //       (backend/kde/wallpaperx_item.cpp), via control_batch.sh, 2026-10-08, NVIDIA ->
                //       "FAIL: the new daemon's timeline is imported once and its values are waited on (2
                //       import(s) for 2 binary picture(s); 8 of 8 steady pictures as values, 8 wait(s); ...)".
                //       Reverted. The rebuilds under the new daemon are red only under the reuse-line
                //       deletion above ("0 import(s), 20 not waited"), the context having refused by then.
                SuccessorDaemon successor(device, ring);
                if (!successor.takeNameFrom(bus)) {
                    wallpaperx::testing::harnessFailure("the second stub daemon never became the name's owner");
                    return true;
                }
                pumpFor(50);
                const quint64 semaphoresB = scene.semaphoreImports();
                const Drive reachB =
                    drive(scene, successor.stub, successor.bus, 8, [&] { return successor.stub.valuesSent() > 0; });
                const int binaryB = successor.stub.acquireFdsSent();
                const int waitsB = g_glSemaphoreFaults.waitsSeen.load();
                const int valuesB = successor.stub.valuesSent();
                const Drive steadyB = drive(scene, successor.stub, successor.bus, 8);
                const quint64 importsB = scene.semaphoreImports() - semaphoresB;
                const int steadyValues = successor.stub.valuesSent() - valuesB;
                const int steadyWaits = g_glSemaphoreFaults.waitsSeen.load() - waitsB;
                check(!reachB.stalled && !steadyB.stalled && valuesB > 0 && steadyValues == 8 && steadyWaits >= 8 &&
                          importsB == static_cast<quint64>(binaryB) + 1 && drawingCleanly(scene),
                      "the new daemon's timeline is imported once and its values are waited on (" +
                          std::to_string(importsB) + " import(s) for " + std::to_string(binaryB) +
                          " binary picture(s); " + std::to_string(steadyValues) + " of 8 steady pictures as values, " +
                          std::to_string(steadyWaits) + " wait(s); " + describe(scene) + ")");
                constexpr int kRebuildsB = 20;
                const quint64 semaphoresB2 = scene.semaphoreImports();
                int rebuiltB = 0;
                int unwaitedB = 0;
                for (int i = 0; i < kRebuildsB; ++i) {
                    bool waited = false;
                    if (!rebuildOnce(successor.stub, successor.bus, i, waited)) break;
                    ++rebuiltB;
                    if (!waited) ++unwaitedB;
                }
                check(rebuiltB == kRebuildsB && unwaitedB == 0 && scene.semaphoreImports() == semaphoresB2,
                      "node rebuilds under the new daemon import nothing and wait on every value (" +
                          std::to_string(scene.semaphoreImports() - semaphoresB2) + " import(s), " +
                          std::to_string(unwaitedB) + " not waited, over " + std::to_string(rebuiltB) + " rebuilds)");
                const int lines = tripLines();
                check(captureWorked(), "the stderr capture saw what was written during the phase");
                check(lines == 0, "no semaphore import was refused (" + std::to_string(lines) + " line(s))");
                return true;
            }

            case Phase::ContextShare: {
                if (!reachTimeline(6)) {
                    check(false, "the item reaches the timeline within six pictures");
                    return true;
                }
                // Pictures drawn in `in` after the item moved there from the first window, then two back
                // in the first. Every count is of what this call caused.
                struct Visit {
                    Drive there;
                    Drive back;
                    quint64 importsThere = 0;
                    quint64 importsBack = 0;
                    int values = 0;
                    int waits = 0;
                };
                auto* rootItem = qobject_cast<QQuickItem*>(scene.root.get());
                const auto visit = [&](Scene& in) {
                    Visit v;
                    const quint64 semaphores0 = scene.semaphoreImports();
                    const int waits0 = g_glSemaphoreFaults.waitsSeen.load();
                    const int values0 = stub.valuesSent();
                    in.item = scene.item;  // not owned: the first scene's root still is
                    rootItem->setParentItem(in.window.contentItem());
                    pumpFor(20);
                    v.there = drive(in, stub, bus, 3);
                    rootItem->setParentItem(scene.window.contentItem());
                    in.renderPass();
                    pumpFor(20);
                    v.importsThere = scene.semaphoreImports() - semaphores0;
                    v.back = drive(scene, stub, bus, 2);
                    v.importsBack = scene.semaphoreImports() - semaphores0 - v.importsThere;
                    v.values = stub.valuesSent() - values0;
                    v.waits = g_glSemaphoreFaults.waitsSeen.load() - waits0;
                    return v;
                };

                // GOAL: S15.420 -- invalidating the scene graph while its GL context is kept
                //       (QQuickRenderControl::invalidate() then initialize()) costs no timeline import,
                //       and every value after it is waited on.
                // WHY-THIS-SHAPE: no production code reacts to invalidation, so the property holds by
                //       absence; what could break it is tying the shared import to the node, which
                //       invalidation destroys (with no context current, measured). Asserted as an import
                //       count, because a dropped import is re-made on the next picture and draws the same.
                // CONTROL: in ~FrameNode (backend/kde/wallpaperx_item.cpp) every record's timeline
                //       forgotten, via control_batch.sh, 2026-10-08, NVIDIA -> "FAIL: after the scene graph
                //       is invalidated in the same context, the import is kept and every value waited (1
                //       import(s), 6 value(s), 6 wait(s); ...)". Releasing only the current context's left
                //       this green, and so did clearing the records in releaseResources(), which
                //       invalidation does not call. Reverted.
                // COVERAGE-ASSERT: the item's node must be gone after the invalidation.
                {
                    const quint64 semaphores0 = scene.semaphoreImports();
                    const int waits0 = g_glSemaphoreFaults.waitsSeen.load();
                    const int values0 = stub.valuesSent();
                    scene.control.invalidate();
                    pumpFor(20);
                    if (itemTexture(scene.item) != nullptr) {
                        wallpaperx::testing::harnessFailure("invalidating the scene graph left the item's node in "
                                                            "place, so nothing was rebuilt");
                        return true;
                    }
                    if (!scene.control.initialize()) {
                        wallpaperx::testing::harnessFailure("the scene graph could not be brought up again");
                        return true;
                    }
                    const Drive after = drive(scene, stub, bus, 6);
                    const quint64 imports = scene.semaphoreImports() - semaphores0;
                    const int values = stub.valuesSent() - values0;
                    const int waits = g_glSemaphoreFaults.waitsSeen.load() - waits0;
                    check(!after.stalled && values == 6 && waits >= 6 && imports == 0 && drawingCleanly(scene),
                          "after the scene graph is invalidated in the same context, the import is kept and every "
                          "value waited (" + std::to_string(imports) + " import(s), " + std::to_string(values) +
                              " value(s), " + std::to_string(waits) + " wait(s); " + describe(scene) + ")");
                }

                // GOAL: S15.420 -- each new GL context imports the timeline exactly once and waits on the
                //       values drawn in it; moving the item back to a context that already holds the
                //       import costs nothing.
                // WHY-THIS-SHAPE: several contexts, each destroyed after its visit, so a record keyed by
                //       anything shared across contexts (or not keyed at all) shows as a zero, and one
                //       re-made per picture as a three. Values and waits are summed over the visit,
                //       because which of its pictures lands in which window is the item's to decide.
                // CONTROL: in backend/kde/wallpaperx_item.cpp via control_batch.sh, 2026-10-08, NVIDIA, each
                //       reverted: (a) contextRecordLocked's lookup made to match any record -> "FAIL: each new
                //       context imports the timeline once and waits on its values (6 of 6 wrong; first:
                //       context 0: 3 drawn, 3 import(s) there, 2 back; 1 value(s), 5 wait(s))" -- the first
                //       context's semaphore name fails its wait, and the item falls back to binary imports;
                //       (b) contextTimeline's reuse line deleted -> "FAIL: moving back to the first context
                //       costs no import (6 of 6 wrong; ... 2 back ...)".
                constexpr int kContexts = 6;
                int wrongThere = 0;
                int wrongBack = 0;
                std::string firstWrong;
                for (int k = 0; k < kContexts; ++k) {
                    Visit v;
                    {
                        Scene other;
                        other.window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
                        if (!other.control.initialize()) {
                            wallpaperx::testing::harnessFailure("a new scene graph could not be brought up");
                            return true;
                        }
                        v = visit(other);
                        other.control.invalidate();
                    }
                    const bool rightThere = !v.there.stalled && v.there.drawn == 3 && v.importsThere == 1 &&
                                            v.values >= 1 && v.waits >= v.values;
                    const bool rightBack = !v.back.stalled && v.importsBack == 0 && drawingCleanly(scene);
                    if (!rightThere) ++wrongThere;
                    if (!rightBack) ++wrongBack;
                    if ((!rightThere || !rightBack) && firstWrong.empty()) {
                        firstWrong = "context " + std::to_string(k) + ": " + std::to_string(v.there.drawn) + " drawn, " +
                                     std::to_string(v.importsThere) + " import(s) there, " +
                                     std::to_string(v.importsBack) + " back; " + std::to_string(v.values) +
                                     " value(s), " + std::to_string(v.waits) + " wait(s)";
                    }
                }
                check(wrongThere == 0, "each new context imports the timeline once and waits on its values (" +
                                           std::to_string(wrongThere) + " of " + std::to_string(kContexts) +
                                           " wrong; first: " + firstWrong + ")");
                check(wrongBack == 0, "moving back to the first context costs no import (" + std::to_string(wrongBack) +
                                          " of " + std::to_string(kContexts) + " wrong; first: " + firstWrong + ")");

                // GOAL: S15.420 -- a destroyed context's record is forgotten: a new context made at the
                //       very address of a destroyed one that held the timeline imports it afresh, rather
                //       than being handed the dead context's semaphore name.
                // WHY-THIS-SHAPE: contexts the scene graph makes never landed on a freed address in 20
                //       tries, so the reuse is forced: the test makes the QOpenGLContext itself, by placement
                //       new into one buffer, and hands it to the scene graph
                //       (QQuickGraphicsDevice::fromOpenGLContext). Destroying it emits the
                //       aboutToBeDestroyed the plugin listens for, as the scene graph's own teardown does.
                // CONTROL: `g_contexts.erase(it);` deleted from the aboutToBeDestroyed handler in
                //       contextRecordLocked (backend/kde/wallpaperx_item.cpp), via control_batch.sh,
                //       2026-10-08, NVIDIA -> "FAIL: a context made where a destroyed one was imports the
                //       timeline afresh (1 then 3 import(s); 1 value(s), 5 wait(s) in the second; ...)": handed
                //       the dead context's name, the wait fails and the item falls back to binary imports.
                //       Reverted.
                // COVERAGE-ASSERT: both contexts must be at one address, and each must have been the one
                //       current while the scene graph drew, or nothing here was keyed on them.
                {
                    const std::unique_ptr<unsigned char[]> storage(new unsigned char[sizeof(QOpenGLContext)]);
                    std::array<Visit, 2> visits{};
                    std::array<const void*, 2> addresses{};
                    std::array<bool, 2> currentWhileDrawing{};
                    for (size_t round = 0; round < 2; ++round) {
                        auto* context = new (storage.get()) QOpenGLContext;
                        // Declared before the scene, so the scene goes first and the context after it.
                        struct DestroyContext {
                            QOpenGLContext* context;
                            ~DestroyContext() { context->~QOpenGLContext(); }
                        } destroyContext{context};
                        addresses[round] = context;
                        if (!context->create()) {
                            wallpaperx::testing::harnessFailure("no OpenGL context could be made for the scene graph to "
                                                                "adopt");
                            return true;
                        }
                        Scene adopted;
                        adopted.window.setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(context));
                        adopted.window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
                        if (!adopted.control.initialize()) {
                            wallpaperx::testing::harnessFailure("the scene graph would not adopt a context of the test's");
                            return true;
                        }
                        adopted.renderPass(
                            [&] { currentWhileDrawing[round] = QOpenGLContext::currentContext() == context; });
                        visits[round] = visit(adopted);
                        adopted.control.invalidate();
                    }
                    std::printf("item_timeline_test: adopted contexts at %p and %p, %llu then %llu import(s)\n",
                                addresses[0], addresses[1], static_cast<unsigned long long>(visits[0].importsThere),
                                static_cast<unsigned long long>(visits[1].importsThere));
                    std::fflush(stdout);
                    if (addresses[0] != addresses[1] || !currentWhileDrawing[0] || !currentWhileDrawing[1]) {
                        wallpaperx::testing::harnessFailure("the two adopted contexts were not one address both drawn "
                                                            "with, so address reuse was not staged");
                        return true;
                    }
                    check(!visits[0].there.stalled && !visits[1].there.stalled && visits[0].importsThere == 1 &&
                              visits[1].importsThere == 1 && visits[1].values >= 1 &&
                              visits[1].waits >= visits[1].values && drawingCleanly(scene),
                          "a context made where a destroyed one was imports the timeline afresh (" +
                              std::to_string(visits[0].importsThere) + " then " +
                              std::to_string(visits[1].importsThere) + " import(s); " +
                              std::to_string(visits[1].values) + " value(s), " + std::to_string(visits[1].waits) +
                              " wait(s) in the second; " + describe(scene) + ")");
                }

                // GOAL: S15.430 -- a context that shares the scene's GL share group and is destroyed while not
                //       current leaks no timeline import: the semaphore it imported outlives it only until the next
                //       paint pass of any kind in a sibling context of the group, which deletes it; the destruction
                //       itself deletes nothing, since no context of the group is current then. (The sibling here
                //       draws value pictures; S15.434 below covers one drawing only sync_fds.)
                // WHY-THIS-SHAPE: the test makes the sharing context itself (setShareContext on the scene's own)
                //       and hands it to a second scene graph; the scene graph's own contexts share nothing here.
                //       Read off the GL calls (gl_semaphore_fault.h logs every glCreateSemaphoresNV name and every
                //       glDeleteSemaphoresEXT) and off glIsSemaphoreEXT in the sibling, since the import count
                //       cannot see a leak.
                // CONTROL: via control_run.sh, item_timeline_context-share, 2026-10-09, NVIDIA, reverted: the
                //       orphanTimelineLocked() call removed from the aboutToBeDestroyed handler in contextRecordLocked
                //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: a sibling context's next value picture deletes
                //       the dead sharing context's timeline import (cycle 0: deleted 0 time(s), alive: yes; ...)". The
                //       handler releasing whether or not the context is current (`if (true)`) -> "FAIL: a sharing
                //       context destroyed while not current deletes no semaphore then (cycle 0: deleted 1 time(s),
                //       alive: yes)" and the line above: the delete reached no context, and the semaphore leaked. Not
                //       controlled: that orphans are forgotten when their group is destroyed. Only a group made
                //       later at the freed address could show it, and Qt allocates groups itself.
                // COVERAGE-ASSERT: in every cycle, both contexts are in one share group, exactly one timeline was
                //       imported in the sharing context, and it was not current when destroyed.
                //
                // GOAL: S15.432 -- a share group that gains and loses orphans again and again is watched once: over
                //       three orphan-then-release cycles in one group, the group's destroyed() gains exactly one
                //       receiver over its count before the first orphan and keeps it, and each cycle's orphan is
                //       still deleted by the sibling's next paint pass.
                // WHY-THIS-SHAPE: the count is read with QObject::receivers (protected, reached through a derived
                //       class's using-declaration), since a repeated connection changes nothing else visible until
                //       the group goes. One cycle cannot show it: the first connect is correct either way. Graded
                //       against a baseline read before the first orphan, not as "at least one, constant": an
                //       absolute count would credit any other receiver of the group as the plugin's connection.
                // CONTROL: via control_batch.sh, item_timeline_context-share, 2026-10-10, NVIDIA, reverted, in
                //       backend/kde/wallpaperx_item.cpp: releaseOrphanedTimelinesLocked() erasing the group's record
                //       instead of clearing its orphans -> "FAIL: a share group gaining and losing orphans three times
                //       is watched once (the group's destroyed() receivers: 0 before the first orphan, after each
                //       cycle: 1 2 3)", every cycle's orphan still deleted; the QObject::connect in
                //       orphanTimelineLocked() dropped -> the same line ending "after each cycle: 0 0 0)". From
                //       2026-10-09, not re-run: orphanTimelineLocked() keeping only a group's first orphan ->
                //       "FAIL: a sibling context's next value picture deletes ... (cycle 1: deleted 0 time(s), ...)".
                // COVERAGE-ASSERT: all three cycles ran, and the group had no destroyed() receivers before the
                //       first orphan (otherwise reported as not staged), so "baseline + 1" is the plugin's one.
                QOpenGLContext* sibling = nullptr;
                scene.renderPass([&] { sibling = QOpenGLContext::currentContext(); });
                if (sibling == nullptr) {
                    wallpaperx::testing::harnessFailure("the scene graph left no context current during its pass");
                    return true;
                }
                struct Peek : QObject {
                    using QObject::receivers;
                };
                const auto groupReceivers = [&] {
                    return (sibling->shareGroup()->*&Peek::receivers)(SIGNAL(destroyed(QObject*)));
                };
                const auto created = [] {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    return g_glSemaphoreFaults.timelinesCreated;
                };
                const auto deleted = [](GLuint name) {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    int count = 0;
                    for (const auto& call : g_glSemaphoreFaults.semaphoresDeleted) count += call.name == name ? 1 : 0;
                    return count;
                };
                using IsFn = GLboolean (*)(GLuint);
                static const IsFn isSemaphore = wallpaperx::testing::gl_fault_detail::realGl<IsFn>("glIsSemaphoreEXT");
                // A context sharing the scene's group, visited by the item through a second scene graph that
                // adopts it. Returned owned and not current, the scene graph gone; null after a reported failure.
                struct Sharing {
                    std::unique_ptr<QOpenGLContext> context;
                    GLuint name = 0;  // the one timeline imported there; 0 unless exactly one was
                    size_t timelines = 0;
                    Visit visit;
                };
                const auto visitSharing = [&] {
                    Sharing out;
                    auto shared = std::make_unique<QOpenGLContext>();
                    shared->setShareContext(sibling);
                    if (!shared->create() || shared->shareGroup() != sibling->shareGroup()) {
                        wallpaperx::testing::harnessFailure("no context sharing the scene's group could be made");
                        return out;
                    }
                    const size_t created0 = created().size();
                    {
                        Scene adopted;
                        adopted.window.setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(shared.get()));
                        adopted.window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
                        if (!adopted.control.initialize()) {
                            wallpaperx::testing::harnessFailure("the scene graph would not adopt the sharing context");
                            return out;
                        }
                        out.visit = visit(adopted);
                        adopted.control.invalidate();
                    }
                    const auto made = created();
                    out.timelines = made.size() - created0;
                    out.name = out.timelines == 1 ? made.back().name : 0;
                    if (QOpenGLContext::currentContext() == shared.get()) shared->doneCurrent();
                    out.context = std::move(shared);
                    return out;
                };
                constexpr int kOrphanCycles = 3;
                const int receiversBefore = groupReceivers();
                std::printf("item_timeline_test: the scene's share group has %d destroyed() receiver(s) before any "
                            "orphan\n", receiversBefore);
                std::fflush(stdout);
                std::array<int, kOrphanCycles> receiversAfter{};
                int cyclesDone = 0;
                for (int cycle = 0; cycle < kOrphanCycles; ++cycle) {
                    Sharing made = visitSharing();
                    if (made.context == nullptr) return true;
                    QOpenGLContext* shared = made.context.get();
                    const GLuint name = made.name;
                    const Visit& v = made.visit;
                    bool currentAtDestroy = false;
                    QObject::connect(shared, &QOpenGLContext::aboutToBeDestroyed,
                                     [&] { currentAtDestroy = QOpenGLContext::currentContext() == shared; });
                    const int deletedBefore = deleted(name);
                    made.context.reset();
                    const int deletedAtDestroy = deleted(name) - deletedBefore;
                    bool aliveBefore = false;
                    scene.renderPass([&] { aliveBefore = isSemaphore != nullptr && isSemaphore(name) == GL_TRUE; });
                    const Drive after = drive(scene, stub, bus, 2);
                    bool aliveAfter = true;
                    scene.renderPass([&] { aliveAfter = isSemaphore != nullptr && isSemaphore(name) == GL_TRUE; });
                    const int deletedAfter = deleted(name) - deletedBefore - deletedAtDestroy;
                    receiversAfter[static_cast<size_t>(cycle)] = groupReceivers();
                    std::printf("item_timeline_test: cycle %d: sharing context imported name %u (%zu import(s)); at its "
                                "destruction current: %s, deleted %d time(s), alive in the sibling: %s; after 2 sibling "
                                "pictures deleted %d time(s), alive: %s; the group's destroyed() has %d receiver(s)\n",
                                cycle, name, made.timelines, currentAtDestroy ? "yes" : "no", deletedAtDestroy,
                                aliveBefore ? "yes" : "no", deletedAfter, aliveAfter ? "yes" : "no",
                                receiversAfter[static_cast<size_t>(cycle)]);
                    std::fflush(stdout);
                    if (name == 0 || currentAtDestroy || v.importsThere != 1 || isSemaphore == nullptr) {
                        wallpaperx::testing::harnessFailure("the sharing context did not import one timeline and go "
                                                            "while not current, so the release was not staged");
                        return true;
                    }
                    const bool keptAtDestroy = deletedAtDestroy == 0 && aliveBefore;
                    const bool releasedBySibling = !after.stalled && deletedAfter == 1 && !aliveAfter && drawingCleanly(scene);
                    check(keptAtDestroy, "a sharing context destroyed while not current deletes no semaphore then (cycle " +
                                             std::to_string(cycle) + ": deleted " + std::to_string(deletedAtDestroy) +
                                             " time(s), alive: " + (aliveBefore ? "yes" : "no") + ")");
                    check(releasedBySibling,
                          "a sibling context's next value picture deletes the dead sharing context's timeline import "
                          "(cycle " + std::to_string(cycle) + ": deleted " + std::to_string(deletedAfter) +
                              " time(s), alive: " + (aliveAfter ? "yes" : "no") + "; " + describe(scene) + ")");
                    if (!keptAtDestroy || !releasedBySibling) break;
                    ++cyclesDone;
                }
                if (receiversBefore != 0) {
                    wallpaperx::testing::harnessFailure("the scene's share group already had destroyed() receivers "
                                                        "before the first orphan, so watched-once was not staged");
                } else if (cyclesDone == kOrphanCycles) {
                    bool once = true;
                    std::string counts;
                    for (const int count : receiversAfter) {
                        once = once && count == receiversBefore + 1;
                        counts += (counts.empty() ? "" : " ") + std::to_string(count);
                    }
                    check(once, "a share group gaining and losing orphans three times is watched once (the group's "
                                "destroyed() receivers: " + std::to_string(receiversBefore) +
                                    " before the first orphan, after each cycle: " + counts + ")");
                } else {
                    std::printf("item_timeline_test: S15.432 not graded: an orphan cycle above failed\n");
                    std::fflush(stdout);
                }

                // GOAL: S15.435 -- a context of the group destroyed while current deletes the group's orphans as
                //       well as its own timeline import, with no paint pass between the orphaning and that
                //       destruction.
                // WHY-THIS-SHAPE: two sharing contexts, each visited by the item so each holds a record and an
                //       import; the second is destroyed not current (an orphan), then the first is made current on
                //       an offscreen surface and destroyed, with nothing drawn in between. Counted on the orphan's
                //       name, which nothing else can be handed while it is alive; the first context's own delete
                //       is graded beside it, as the other half of the same handler.
                // CONTROL: via control_batch.sh, 2026-10-10, NVIDIA, reverted: `releaseOrphanedTimelinesLocked(context);`
                //       removed from the aboutToBeDestroyed handler in contextRecordLocked
                //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: a context of the group destroyed while current
                //       deletes the group's orphan and its own import (orphan deleted 0 time(s), its own 1)".
                // COVERAGE-ASSERT: the orphan's context was not current at its destruction and nothing deleted its
                //       name then; the survivor was current at its own.
                {
                    Sharing survivor = visitSharing();
                    if (survivor.context == nullptr) return true;
                    Sharing dead = visitSharing();
                    if (dead.context == nullptr) return true;
                    bool deadCurrent = false;
                    QObject::connect(dead.context.get(), &QOpenGLContext::aboutToBeDestroyed,
                                     [&, context = dead.context.get()] {
                                         deadCurrent = QOpenGLContext::currentContext() == context;
                                     });
                    const int orphanBefore = deleted(dead.name);
                    const int ownBefore = deleted(survivor.name);
                    dead.context.reset();
                    const int orphanAtItsDestroy = deleted(dead.name) - orphanBefore;
                    QOffscreenSurface surface;
                    surface.setFormat(survivor.context->format());
                    surface.create();
                    const bool madeCurrent = surface.isValid() && survivor.context->makeCurrent(&surface);
                    bool survivorCurrent = false;
                    QObject::connect(survivor.context.get(), &QOpenGLContext::aboutToBeDestroyed,
                                     [&, context = survivor.context.get()] {
                                         survivorCurrent = QOpenGLContext::currentContext() == context;
                                     });
                    survivor.context.reset();
                    const int orphanDeleted = deleted(dead.name) - orphanBefore - orphanAtItsDestroy;
                    const int ownDeleted = deleted(survivor.name) - ownBefore;
                    std::printf("item_timeline_test: destroyed while current: orphan %u (its context current at its "
                                "destruction: %s, deleted then %d time(s)), survivor %u (current: %s); deleted by the "
                                "survivor's destruction: orphan %d time(s), its own %d time(s)\n",
                                dead.name, deadCurrent ? "yes" : "no", orphanAtItsDestroy, survivor.name,
                                survivorCurrent ? "yes" : "no", orphanDeleted, ownDeleted);
                    std::fflush(stdout);
                    if (dead.name == 0 || survivor.name == 0 || deadCurrent || orphanAtItsDestroy != 0 ||
                        !madeCurrent || !survivorCurrent) {
                        wallpaperx::testing::harnessFailure("an orphan and a sibling destroyed while current were not "
                                                            "staged in one group");
                        return true;
                    }
                    check(orphanDeleted == 1 && ownDeleted == 1,
                          "a context of the group destroyed while current deletes the group's orphan and its own "
                          "import (orphan deleted " + std::to_string(orphanDeleted) + " time(s), its own " +
                              std::to_string(ownDeleted) + ")");
                }

                // GOAL: S15.434 -- a sibling that draws only sync_fd pictures (no timeline value sent or asked for)
                //       deletes a dead sharing context's timeline import within two pictures.
                // WHY-THIS-SHAPE: GL_NV_timeline_semaphore is hidden in the scene's own context
                //       (gl_semaphore_fault.h) before the sharing context is visited, since the plugin re-reads a
                //       context's extensions only after another context was current. The test binary's own
                //       FrameImport::supportsTimeline() caches separately and cannot confirm the hide, so the
                //       stub's value and on-timeline-request counts are required unchanged instead. "Deleted at
                //       least once", not once and not glIsSemaphoreEXT: each sync_fd picture's semaphore can be
                //       handed the freed name and delete it again.
                // CONTROL: via control_batch.sh, 2026-10-10, NVIDIA, each reverted, in backend/kde/wallpaperx_item.cpp:
                //       (a) the releaseOrphansOfCurrentGroup() call in updatePaintNode removed; (b) the same, with
                //       releaseOrphanedTimelinesLocked(context) added to contextTimeline instead (S15.430 stayed
                //       green, value pictures still release) -> both "FAIL: a sibling drawing only sync_fd pictures
                //       deletes the dead sharing context's timeline import (2 drawn, deleted 0 time(s); ...)".
                // COVERAGE-ASSERT: both sibling pictures were drawn with sync_fds, no value or timeline request
                //       went out, and nothing deleted the name at its context's destruction.
                {
                    EGLContext sceneContext = EGL_NO_CONTEXT;
                    scene.renderPass([&] { sceneContext = eglGetCurrentContext(); });
                    struct Unhide {
                        ~Unhide() { g_glSemaphoreFaults.hideTimelineIn.store(EGL_NO_CONTEXT); }
                    } unhide;
                    g_glSemaphoreFaults.hideTimelineIn.store(sceneContext);
                    Sharing dead = visitSharing();
                    if (dead.context == nullptr) return true;
                    bool deadCurrent = false;
                    QObject::connect(dead.context.get(), &QOpenGLContext::aboutToBeDestroyed,
                                     [&, context = dead.context.get()] {
                                         deadCurrent = QOpenGLContext::currentContext() == context;
                                     });
                    const int before = deleted(dead.name);
                    dead.context.reset();
                    const int atDestroy = deleted(dead.name) - before;
                    const int values0 = stub.valuesSent();
                    const int onTimeline0 = stub.onTimelineRequests();
                    const int syncFds0 = stub.acquireFdsSent();
                    const Drive after = drive(scene, stub, bus, 2);
                    const int deletedAfter = deleted(dead.name) - before - atDestroy;
                    const int values = stub.valuesSent() - values0;
                    const int onTimeline = stub.onTimelineRequests() - onTimeline0;
                    const int syncFds = stub.acquireFdsSent() - syncFds0;
                    std::printf("item_timeline_test: sync_fd sibling: orphan %u (current at destruction: %s, deleted "
                                "then %d time(s)); %d picture(s) drawn with %d sync_fd(s), %d value(s), %d timeline "
                                "request(s); deleted after %d time(s)\n",
                                dead.name, deadCurrent ? "yes" : "no", atDestroy, after.drawn, syncFds, values,
                                onTimeline, deletedAfter);
                    std::fflush(stdout);
                    if (sceneContext == EGL_NO_CONTEXT || dead.name == 0 || deadCurrent || atDestroy != 0 ||
                        values != 0 || onTimeline != 0 || syncFds < after.drawn) {
                        wallpaperx::testing::harnessFailure("a sibling drawing only sync_fds beside an orphan was not "
                                                            "staged");
                        return true;
                    }
                    check(!after.stalled && after.drawn == 2 && deletedAfter >= 1 && drawingCleanly(scene),
                          "a sibling drawing only sync_fd pictures deletes the dead sharing context's timeline import "
                          "(" + std::to_string(after.drawn) + " drawn, deleted " + std::to_string(deletedAfter) +
                              " time(s); " + describe(scene) + ")");
                }
                const int lines = tripLines();
                check(captureWorked() && lines == 0, "no semaphore import was refused (" + std::to_string(lines) +
                                                         " line(s))");
                return true;
            }
        }
        return true;
    });
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_timeline_test";
    // X.34: nothing owned or sent off the private bus.
    if (const std::optional<int> refused = wallpaperx::testing::refuseOffPrivateTestBus()) return *refused;
    wallpaperx::testing::registerFrameReplyTypes();
    QGuiApplication app(argc, argv);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    const PhaseName* which = nullptr;
    for (const PhaseName& candidate : kPhases) {
        if (argc > 1 && std::strcmp(argv[1], candidate.name) == 0) which = &candidate;
    }
    if (which == nullptr) {
        wallpaperx::testing::harnessFailure("no known phase was named on the command line");
        (void)wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
        return 1;
    }

    bool ok = false;
    try {
        ok = runPhase(*which);
    } catch (const std::exception& e) {
        wallpaperx::testing::harnessFailure(std::string("the harness could not build its preconditions: ") + e.what());
    }

    const bool checksRed = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
    if (!ok || checksRed || harnessRed) return 1;
    if (g_ungradedValidation > 0) {
        std::fprintf(stderr, "item_timeline_test: the validation grade could not be made -- "
                             "VK_LAYER_KHRONOS_validation is not loaded.\n");
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
