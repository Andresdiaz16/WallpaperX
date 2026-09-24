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
//   old-daemon          a daemon without the methods: asked once, GetFrameFd only (S13.222, S13.218)
//   unsupported         Unsupported is final (S13.218)
//   unknown-interface   so is UnknownInterface (S13.218)
//   retry-busy          Busy is asked again after about 2 s, then used (S13.218)
//   retry-exportfailed  so is ExportFailed (S13.218)
//   lost-reply          a timeline answer that never comes costs nothing (S13.217)
//   refused-timeline    a refused timeline import: one line, the wallpaper keeps drawing (S13.223)
//   refused-binary      the same for a refused binary import (S13.223)
//   wait-no-trip        a failed wait gives up the timeline, not the context (S13.223, S13.217)
//   setup-create        a timeline that cannot be created costs one picture (S13.217)
//   setup-type          nor one whose type cannot be set (S13.217)
//   stray-value         a value in a GetFrameFd reply is never read (S13.221)
//   restart             a value is waited on only on its own daemon's timeline (S13.221)
//   stale-value         a late value from a daemon given up on is no failed hand-over (S13.224)
//   context-move        a value that lands in a context unable to wait on it is drawn (S13.225)
//
// Every phase but refused-binary and stray-value needs a GL context that can wait on a Vulkan
// timeline (GL_NV_timeline_semaphore), and reports NOT VERIFIED where there is none -- Mesa.

#include "kde/frame_import.h"
#include "support/validation_grade.h"
#include "tests/gl_import_harness.h"
#include "tests/gl_semaphore_fault.h"
#include "tests/item_render_scene.h"
#include "tests/item_stub_scene.h"
#include "tests/test_report.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QUrl>

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
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
    StaleValue,
    ContextMove,
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
    {Phase::StrayValue, "stray-value", "S13.221 (a value is read only from a GetFrameFdOnTimeline reply)", false},
    {Phase::Restart, "restart", "S13.221 (a value is waited on only on the timeline of the daemon that sent it)", true},
    {Phase::StaleValue, "stale-value", "S13.224 (a late value from a daemon given up on is not a failed hand-over)",
     true},
    {Phase::ContextMove, "context-move",
     "S13.225 (a value that reaches a context which cannot wait on a timeline is drawn unwaited)", true},
};

// Everything written to stderr while it is alive, which is where the plugin's refused-import line
// goes. Echoed back to the real stderr when it finishes, so nothing it held is lost to a reader.
class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        m_file = ::memfd_create("wallpaperx-stderr-capture", MFD_CLOEXEC);
        m_saved = ::dup(STDERR_FILENO);
        if (m_file < 0 || m_saved < 0 || ::dup2(m_file, STDERR_FILENO) < 0) {
            if (m_saved >= 0) ::close(m_saved);
            m_saved = -1;
        }
    }
    ~StderrCapture() {
        finish();
        if (m_file >= 0) ::close(m_file);
    }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    bool capturing() const { return m_saved >= 0; }

    const std::string& finish() {
        if (m_saved < 0) return m_text;
        std::fflush(stderr);
        ::dup2(m_saved, STDERR_FILENO);
        ::close(m_saved);
        m_saved = -1;
        char buffer[4096];
        ssize_t got = 0;
        off_t at = 0;
        while ((got = ::pread(m_file, buffer, sizeof buffer, at)) > 0) {
            m_text.append(buffer, static_cast<size_t>(got));
            at += got;
        }
        if (!m_text.empty()) (void)!::write(STDERR_FILENO, m_text.data(), m_text.size());
        return m_text;
    }

private:
    int m_file = -1;
    int m_saved = -1;
    std::string m_text;
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
            case Phase::RetryBusy:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.refuseTimelineAsks(1, QStringLiteral("io.wallpaperx.Daemon.Error.Busy"));
                break;
            case Phase::RetryExportFailed:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.refuseTimelineAsks(1, QStringLiteral("io.wallpaperx.Daemon.Error.ExportFailed"));
                break;
            case Phase::Restart:
                stub.serveTimelines(ExportingStub::Timeline::Grant);
                stub.advanceTimelinesOnGrant(200);
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
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) the first line of FrameNode::timeline
                //       (the kept import returned) deleted -> "FAIL: a picture on the timeline costs no
                //       semaphore import (20 over 20 pictures)", on the opaque-fd and dma_buf entries
                //       both; (b) WallpaperXItem::fetchOnTimeline made to return false -> "FAIL: the
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
                //       blue where yellow was drawn; a value, through the kept import". And FrameNode::
                //       timeline's kept-import line deleted -> the same line "(0 of 4)", every colour
                //       right and each step "NOT a value through a kept import": the coverage half
                //       reddening alone. Observed 2026-09-23 on the NVIDIA entry. Reverted.
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

            case Phase::OldDaemon:
            case Phase::Unsupported:
            case Phase::UnknownInterface: {
                // GOAL: S13.222 and S13.218 -- a daemon that answers GetAcquireTimeline with
                //       UnknownMethod (one from before it), Unsupported or UnknownInterface is asked
                //       ONCE; the plugin stays on GetFrameFd and draws every picture with its binary
                //       semaphore, and never sends GetFrameFdOnTimeline.
                // WHY-THIS-SHAPE: the drive lasts well past the plugin's two-second retry, with a
                //       binary picture every tenth of a second, each of which would ask again if the
                //       answer were not final -- "asked once" is otherwise true of any run shorter
                //       than the delay.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) the three names in
                //       timelineRefusalIsPermanent misspelled -> "FAIL: the refusal is final: the
                //       timeline was asked for once (2 ask(s))" on all three entries; (b) the
                //       holdsAcquireTimeline test deleted from WallpaperXItem::fetchOnTimeline -> on
                //       old-daemon, "FAIL: no picture was asked for on a timeline (50)". Observed
                //       2026-09-23 on the NVIDIA entry. Reverted.
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
                //       NVIDIA entry. Reverted.
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
                //       it from asking. Shorter than QtDBus's reply timeout, after which the ask fails
                //       and is retried -- a different case.
                // CONTROL: the holdsAcquireTimeline test deleted from WallpaperXItem::fetchOnTimeline
                //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: and no picture was asked for on it
                //       while the answer was out (1)". Observed 2026-09-23 on the NVIDIA entry. Reverted.
                const Drive run = drive(scene, stub, bus, 25, nullptr, 60);
                check(stub.timelineAsks() == 1, "the timeline was asked for (" + std::to_string(stub.timelineAsks()) +
                                                    " ask(s))");
                check(stub.onTimelineRequests() == 0, "and no picture was asked for on it while the answer was out (" +
                                                          std::to_string(stub.onTimelineRequests()) + ")");
                check(!run.stalled && run.drawn == 25 && drawingCleanly(scene),
                      "every picture is drawn (" + std::to_string(run.drawn) + " of 25; " + describe(scene) + ")");
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
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) `g_refusingContexts.push_back(context);`
                //       deleted from noteContextRefusedSemaphores, so no context is ever marked -> "FAIL:
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
                // GOAL: S13.221 -- a value is read only from a reply to GetFrameFdOnTimeline. A GetFrameFd
                //       reply carrying a thirteenth field is drawn as a picture with no hand-over:
                //       imported afresh, never through a kept import, and no timeline touched.
                // WHY-THIS-SHAPE: one image, so every picture after the first could reuse its import
                //       -- and would, if the value were read, since a value is a hand-over. The
                //       counters then tell the two apart without any wait on a timeline that could
                //       stall. The stray values start after the item's first picture, so the setup
                //       above is the ordinary one.
                // CONTROL: `askedForValue &&` deleted from WallpaperXItem::takeFrameReply
                //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: every picture with a stray value is
                //       drawn (0 of 7)" and "FAIL: each is imported afresh, as a picture without a
                //       hand-over (0 import(s), 0 reuse(s))". Observed 2026-09-23 on the NVIDIA entry.
                //       Reverted.
                const quint64 imports0 = scene.imports();
                const quint64 reused0 = scene.reused();
                const quint64 semaphores0 = scene.semaphoreImports();
                stub.sendValueOnGetFrameFd(1);
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

            case Phase::Restart: {
                // GOAL: S13.221 across a daemon restart mid-stream -- a value is only ever waited on the
                //       timeline of the daemon (unique bus name) it was asked of. A request in flight to
                //       the old daemon when the name moves is still drawn on the OLD daemon's timeline;
                //       the new daemon's pictures are waited on its own, imported afresh. False for a
                //       value paired by the owner at reply time, which is a wait that may never end.
                // WHY-THIS-SHAPE: the old daemon releases the name but stays connected, holding one
                //       GetFrameFdOnTimeline reply until the plugin has seen the new owner (a watcher on
                //       the plugin's own connection, whose change signal the plugin's watcher received
                //       first). The old daemon's payload is 200 signals ahead of the new one's, so a
                //       value waited on the wrong timeline COMPLETES -- no stalled GPU -- and only the
                //       import count and the requests tell the pairing was wrong.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) the reply's label in
                //       WallpaperXItem::fetchFrame made the owner as known when the reply is read,
                //       whatever the request was addressed to -> "FAIL: and its value is drawn on the old
                //       daemon's timeline, with no import (frame shown: yes, reason [], 0 import(s))",
                //       "FAIL: the item reaches the new daemon's timeline (1 ask(s), 0 request(s) on
                //       it)", and the import and steady lines after it; (b) `&& m_timelineSender ==
                //       sender` deleted from FrameNode::timeline -> "FAIL: and imports the new daemon's
                //       timeline rather than waiting its values on the old one (1 import(s) for 1
                //       binary picture(s))". Observed 2026-09-23 on the NVIDIA entry. Reverted.
                if (!reachTimeline(6)) {
                    check(false, "the item reaches the old daemon's timeline");
                    return true;
                }
                (void)drive(scene, stub, bus, 3);
                QDBusConnection busB =
                    QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("wallpaperx-timeline-b"));
                ExportingStub stubB(device, ring);
                stubB.rotate();
                stubB.serveTimelines(ExportingStub::Timeline::Grant);
                // Past anything the old daemon numbered. The old daemon's late reply is drawn after
                // the item forgot its marks and writes its own generation back, so a new daemon
                // counting from 1 is read as older than the screen -- reported as a concern of its
                // own, not what this phase grades.
                stubB.numberGenerationsFrom(1000);
                if (!busB.isConnected() || !busB.registerVirtualObject(QString::fromUtf8(kPath), &stubB)) {
                    wallpaperx::testing::harnessFailure("the second stub daemon could not be put on the bus");
                    return true;
                }
                QString newOwner;
                QDBusServiceWatcher watcher(QString::fromUtf8(kService), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForOwnerChange);
                QObject::connect(&watcher, &QDBusServiceWatcher::serviceOwnerChanged,
                                 [&newOwner](const QString&, const QString&, const QString& owner) { newOwner = owner; });

                stub.holdServeNumber(stub.served() + 1);
                announce(bus, static_cast<quint64>(stub.served()) + 1);
                if (!pumpUntil([&] { return stub.replyIsHeld(); }, 3000)) {
                    wallpaperx::testing::harnessFailure("the item's request to the old daemon never arrived to be held");
                    return true;
                }
                const bool heldOnTimeline = stub.onTimelineRequests() > 0;
                const int onTimelineBefore = stub.onTimelineRequests();
                (void)bus.unregisterService(QString::fromUtf8(kService));
                stubB.holdServeNumber(1);
                if (!busB.registerService(QString::fromUtf8(kService)) ||
                    !pumpUntil([&] { return newOwner == busB.baseService(); }, 3000)) {
                    wallpaperx::testing::harnessFailure("the second stub daemon never became the name's owner");
                    return true;
                }
                pumpFor(50);
                const quint64 pictures0 = scene.pictures();
                const quint64 semaphores0 = scene.semaphoreImports();
                stub.sendHeldReply();
                const bool oldDrawn = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return scene.pictures() > pictures0 && stubB.replyIsHeld();
                    },
                    3000);
                check(heldOnTimeline && stub.onTimelineRequests() == onTimelineBefore,
                      "the request in flight across the restart was asked on the old daemon's timeline");
                check(oldDrawn && drawingCleanly(scene) && scene.semaphoreImports() == semaphores0,
                      "and its value is drawn on the old daemon's timeline, with no import (" + describe(scene) + ", " +
                          std::to_string(scene.semaphoreImports() - semaphores0) + " import(s))");
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
                busB.unregisterObject(QString::fromUtf8(kPath));
                (void)busB.unregisterService(QString::fromUtf8(kService));
                QDBusConnection::disconnectFromBus(QStringLiteral("wallpaperx-timeline-b"));
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
        }
        return true;
    });
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_timeline_test";
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
