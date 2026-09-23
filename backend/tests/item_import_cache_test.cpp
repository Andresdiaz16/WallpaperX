// SPDX-License-Identifier: GPL-2.0-or-later

// Step 13 Stage F: the KDE plugin keeps what it imports. A daemon image is described to the driver
// once and drawn again, picture after picture, through the import kept for it -- so what must hold
// is that the import is kept for the right image, shows what was drawn into it since, is given up
// whenever nothing could name it again, and never changes what the daemon is told about holds.
//
// None of that is visible as a wrong pixel on a healthy run, which is why each phase counts or reads
// something the plugin cannot fake: the item's own import counters, the GL objects' lifetimes, the
// daemon-side release log, and -- for the one property everything rests on -- the pixels a kept
// import samples, read back through the scene graph's own context on the GPU that drew them.
//
// One phase per process (argv[1]), for item_release_ledger_test's reason: the plugin's release
// ledger and its record of which daemon it is talking to are process-wide singletons with no reset.
//
//   counts             a ring served round and round costs one import per image (S13.198, S13.206),
//                      the counters' sum is the pictures drawn, and every picture is given back
//                      once the next is on the screen (S13.201)
//   single             one image served over and over costs one import (S13.198)
//   pixels             a kept import shows the picture drawn into its image since (S13.199)
//   no-acquire         a picture without the producer's hand-over is imported afresh (S13.200)
//   handover           failed hand-overs keep the picture for a bounded run, then blank (S13.203)
//   handover-reset     a picture drawn ends the run (S13.203)
//   handover-modifier  a failed hand-over withdraws no layout (S13.202; its entry forces dma_buf)
//   stale-daemon       a daemon that leaves takes its imports with it, not the picture (S13.204)
//   stale-monitor      so does a monitor the item stops showing (S13.204)
//   offscreen          off the screen every import goes, and coming back imports again (S13.205)
//
// Like the other importing item tests, this reports NOT VERIFIED when this host cannot pair a GL
// context with the Vulkan device that produced the export: an opaque fd is importable only by the
// driver that made it.

#include "daemon/ring_depth.h"
#include "kde/frame_import.h"
#include "support/validation_grade.h"
#include "tests/gl_import_harness.h"
#include "tests/item_stub_scene.h"
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
#include <QSGTexture>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QtQuick/qsgtexture_platform.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <set>
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
using wallpaperx::testing::kSecondMonitor;
using wallpaperx::testing::kService;
using wallpaperx::testing::kWidth;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::publishSolidFrame;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::vulkanDeviceUuid;

void check(bool condition, const std::string& what) { wallpaperx::testing::check(condition, what.c_str()); }

enum class Phase {
    Counts,
    Single,
    Pixels,
    NoAcquire,
    HandOver,
    HandOverReset,
    HandOverModifier,
    StaleDaemon,
    StaleMonitor,
    Offscreen,
};

struct PhaseName {
    Phase phase;
    const char* name;
    const char* goal;
};

constexpr PhaseName kPhases[] = {
    {Phase::Counts, "counts",
     "S13.198, S13.201 and S13.206 (one import per image of the ring; the counters count pictures drawn; every "
     "picture is still given back)"},
    {Phase::Single, "single", "S13.198 (one image served over and over costs one import)"},
    {Phase::Pixels, "pixels", "S13.199 (a kept import shows the picture drawn into its image since it was imported)"},
    {Phase::NoAcquire, "no-acquire", "S13.200 (a picture with no hand-over is imported afresh, never reused)"},
    {Phase::HandOver, "handover", "S13.203 (failed hand-overs keep the picture for a bounded run, then blank)"},
    {Phase::HandOverReset, "handover-reset", "S13.203 (a picture drawn ends a run of failed hand-overs)"},
    {Phase::HandOverModifier, "handover-modifier", "S13.202 (a failed hand-over withdraws no layout)"},
    {Phase::StaleDaemon, "stale-daemon", "S13.204 (a departed daemon's imports are freed; its picture stays)"},
    {Phase::StaleMonitor, "stale-monitor", "S13.204 (a left monitor's imports are freed; its picture stays)"},
    {Phase::Offscreen, "offscreen", "S13.205 (off the screen every import goes; coming back imports again)"},
};

// The words the item blanks with once a hand-over has failed for too long. Restated rather than
// shared: what is under test is what the desktop says, and taking it from the plugin's own string
// would let a rewording pass unnoticed.
constexpr const char* kCouldNotImport = "WallpaperX produced a frame this desktop could not import.";

// Mirrored from backend/kde/wallpaperx_item.cpp's file-local constant, which no header carries and
// no test-only accessor may publish. The runs below are sized from it, so a change to it in
// production alone moves the edge off the drive and reddens the phases that sit on it.
constexpr int kToleratedHandOverFailures = 8;

int g_ungradedValidation = 0;

void pumpFor(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// The plugin's own type, with its two counters BOUND the way a QML reader binds them -- which is
// the only reader they exist for, and one that is frozen at its first value without a NOTIFY.
QString cacheQmlSource() {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: 68
    height: 36
    property Item rendererItem: renderer
    property string publishedReason: renderer.unavailableReason
    property var boundImports: renderer.importCount
    property var boundReused: renderer.reusedImportCount

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
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

    // One scene-graph pass. `afterFrame` runs once the frame has been rendered and before the
    // context is dropped, which is where a readback may run without disturbing Qt's own state.
    void renderPass(const std::function<void()>& afterFrame = nullptr) {
        control.polishItems();
        control.beginFrame();
        control.sync();
        control.endFrame();
        if (afterFrame) afterFrame();
        // For item_import_release_test's reason: a real compositor's GUI thread holds no context
        // between frames, and a harness that left one current would hide a free made there.
        if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();
    }

    quint64 imports() const { return item->property("importCount").toULongLong(); }
    quint64 reused() const { return item->property("reusedImportCount").toULongLong(); }
    quint64 pictures() const { return imports() + reused(); }
    QString reason() const { return root->property("publishedReason").toString(); }
};

GLuint nativeName(QSGTexture* texture) {
    if (texture == nullptr) return 0;
    auto* opengl = texture->nativeInterface<QNativeInterface::QSGOpenGLTexture>();
    return opengl == nullptr ? 0 : opengl->nativeTexture();
}

void announce(QDBusConnection& bus, quint64 generation) {
    QDBusMessage ready =
        QDBusMessage::createSignal(QString::fromUtf8(kPath), QString::fromUtf8(kInterface), QStringLiteral("FrameReady"));
    ready.setArguments({kFirstMonitor, static_cast<qulonglong>(generation)});
    bus.send(ready);
}

// Runs passes until the item has asked for nothing new for a third of a second, which is what makes
// "the next request" mean the next one this phase causes. Needed rather than a fixed number of
// passes: the item's startup makes several forced fetches that coalesce into trailing ones, and one
// landing after a phase has started scheduling replies is served as that phase's first -- measured,
// it put a ninth failed hand-over into a run meant to stop at eight. False when the item never goes
// quiet inside the bound.
bool settleQuiet(Scene& scene, ExportingStub& stub) {
    int quietRounds = 0;
    int seen = stub.requests();
    QElapsedTimer clock;
    clock.start();
    while (quietRounds < 10) {
        if (clock.elapsed() > 5000) return false;
        scene.renderPass();
        pumpFor(30);
        if (stub.requests() == seen) {
            ++quietRounds;
        } else {
            seen = stub.requests();
            quietRounds = 0;
        }
    }
    return true;
}

// Announces the next picture and runs passes until the item has drawn one more. Lockstep: one
// picture asked for, one drawn, so a count read afterwards describes exactly the drive. False when
// no picture arrived in the bound, which every caller must branch on.
bool drawNext(Scene& scene, ExportingStub& stub, QDBusConnection& bus) {
    const quint64 before = scene.pictures();
    announce(bus, static_cast<quint64>(stub.served()) + 1);
    return pumpUntil(
        [&] {
            scene.renderPass();
            return scene.pictures() > before;
        },
        5000);
}

// Which of a few flat colours a pixel is, or -1: the readback is compared by nearest colour rather
// than exactly, so the check is about WHICH picture is shown and not about a driver's rounding.
int nearestColour(const unsigned char* pixel, const std::vector<std::array<unsigned char, 4>>& palette) {
    int best = -1;
    int bestDistance = 1 << 30;
    for (size_t i = 0; i < palette.size(); ++i) {
        int distance = 0;
        for (int c = 0; c < 3; ++c) distance += std::abs(static_cast<int>(pixel[c]) - palette[i][c]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return bestDistance <= 60 ? best : -1;
}

// Watches QSGTexture objects die, keyed by the object. Connected to the window, which outlives
// every local of a phase, so a texture reaped in the window's own teardown writes through nothing
// dead.
class TextureWatch {
public:
    explicit TextureWatch(QObject* context) : m_context(context) {}
    ~TextureWatch() {
        for (const QMetaObject::Connection& c : m_connections) QObject::disconnect(c);
    }
    // Records `texture` once; false when it was already watched.
    bool watch(QSGTexture* texture) {
        if (texture == nullptr || m_watched.count(texture) != 0) return false;
        m_watched.insert(texture);
        m_connections.push_back(QObject::connect(
            texture, &QObject::destroyed, m_context, [this, texture] { m_destroyed.insert(texture); },
            Qt::DirectConnection));
        return true;
    }
    size_t watched() const { return m_watched.size(); }
    size_t destroyed() const { return m_destroyed.size(); }
    bool isDestroyed(QSGTexture* texture) const { return m_destroyed.count(texture) != 0; }

private:
    QObject* m_context;
    std::set<QSGTexture*> m_watched;
    std::set<QSGTexture*> m_destroyed;
    std::vector<QMetaObject::Connection> m_connections;
};

// Draws round the ring until every one of its images has been on the screen, watching each
// texture. Returns false, having reported why, when the ring was never covered.
bool importWholeRing(Scene& scene, ExportingStub& stub, QDBusConnection& bus, TextureWatch& watch, size_t ringSize) {
    watch.watch(itemTexture(scene.item));
    for (size_t step = 0; step < 3 * ringSize && watch.watched() < ringSize; ++step) {
        if (!drawNext(scene, stub, bus)) {
            check(false, "the item draws each picture of the ring it is served");
            std::fprintf(stderr, "  %zu picture(s) in, the stub served %d and the item reports [%s]\n", step,
                         stub.served(), scene.reason().toStdString().c_str());
            return false;
        }
        watch.watch(itemTexture(scene.item));
    }
    if (watch.watched() < ringSize) {
        wallpaperx::testing::harnessFailure("only " + std::to_string(watch.watched()) + " of " +
                                            std::to_string(ringSize) +
                                            " images of the ring were seen on the screen, so the imports whose fate "
                                            "is graded were never all made");
        return false;
    }
    return true;
}

bool runPhase(const PhaseName& which) {
    const Phase phase = which.phase;
    const char* kGoal = which.goal;
    return wallpaperx::testing::gradedRun("item_import_cache_test", [&](wallpaperx::testing::ValidationGrade& grade) {
        grade.reportUngradedTo(g_ungradedValidation);
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);

        // The daemon's own ring depth, from the header the plugin sizes its cache against, for the
        // phases about the ring. Two images for the pixel and hand-over phases, one where a single
        // image is the point.
        size_t imageCount = 1;
        if (phase == Phase::Counts || phase == Phase::StaleDaemon || phase == Phase::StaleMonitor ||
            phase == Phase::Offscreen) {
            imageCount = wallpaperx::kTargetDepth;
        } else if (phase == Phase::Pixels || phase == Phase::HandOverModifier) {
            imageCount = 2;
        }
        // Four colours a readback can tell apart at a glance. Images start on the first two.
        const std::vector<std::array<unsigned char, 4>> palette = {
            {230, 20, 20, 255}, {20, 210, 20, 255}, {20, 20, 230, 255}, {230, 210, 20, 255}};
        std::vector<const wallpaperx::OffscreenTarget*> ring;
        for (size_t i = 0; i < imageCount; ++i) {
            const wallpaperx::OffscreenTarget& target = targets.create(static_cast<int>(10 + i), kWidth, kHeight);
            publishSolidFrame(device, targets, target, "item_import_cache_" + std::to_string(i) + ".png",
                              palette[i % 2]);
            ring.push_back(&target);
        }

        // The stub on a bus connection of its own, NOT the process's session connection the item
        // uses. Measured, and it is what the held replies below rest on: QtDBus answers a call to a
        // service registered on the caller's own connection and thread locally and at once, taking
        // whatever reply the handler CREATED as the answer -- so a reply the stub built and meant to
        // hold was delivered anyway, and the item drew a picture no test step had released.
        QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                            QStringLiteral("wallpaperx-cache-stub-daemon"));
        ExportingStub stub(device, ring);
        stub.rotate();
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
        const bool needsSameDevice = wallpaperx::testing::exportNeedsSameDevice(device, *ring.front());
        if (needsSameDevice && (!uuidQueryable || !sameDevice)) {
            notVerified(kGoal, "the scene graph's GL context is not provably on the device the Vulkan export came "
                               "from (GL reports " + glRenderer + "), so the frame is not importable here");
            scene.control.invalidate();
            return true;
        }

        scene.engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
        scene.component = std::make_unique<QQmlComponent>(&scene.engine);
        scene.component->setData(cacheQmlSource().toUtf8(),
                                  QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                      QStringLiteral("/item_import_cache.qml")));
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
        // Everything this phase leaves behind is torn down in a pass of its own, for
        // item_import_release_test's reason: deleting the item only queues its node.
        struct Teardown {
            Scene& scene;
            ~Teardown() {
                scene.root.reset();
                scene.renderPass();
                scene.control.invalidate();
            }
        } teardown{scene};

        const bool imported = pumpUntil(
            [&scene] {
                scene.renderPass();
                return itemTexture(scene.item) != nullptr;
            },
            10000);
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
            if (supported) {
                check(false, "the item imported the frame the stub served");
                std::fprintf(stderr, "  it asked %d time(s), the stub served %d, the item reports [%s]\n",
                             stub.requests(), stub.served(), scene.reason().toStdString().c_str());
            } else {
                notVerified(kGoal, whyNot);
            }
            return true;
        }
        // The item's own startup traffic settles before anything is counted, for
        // item_release_ledger_test's reason; every figure below is read from where it leaves off.
        if (!settleQuiet(scene, stub)) {
            wallpaperx::testing::harnessFailure("the item was still asking for frames five seconds after its first "
                                                "picture, so no phase could say which request was its own");
            return true;
        }
        std::printf("item_import_cache_test: %s -- imported on %s, handle type %s, %zu image(s) in the ring\n",
                    which.name, glRenderer.c_str(), stub.handleType() == 1 ? "dma_buf" : "opaque fd", imageCount);
        std::fflush(stdout);

        switch (phase) {
            case Phase::Counts: {
                // GOAL: S13.198 and S13.206 -- a ring served round and round costs ONE import per
                //       image, however many pictures are drawn from it: with the ring exactly as deep
                //       as the daemon's own (wallpaperx::kTargetDepth, read from the header the
                //       plugin sizes its cache against), three times round is kTargetDepth imports
                //       and the rest drawn through kept ones. False for an import per picture -- the
                //       cost this stage removed -- and for a cache shallower than the ring, which is
                //       refilled every time round and says nothing.
                // WHY-THIS-SHAPE: the pictures are drawn in lockstep, one announced and one drawn,
                //       so the served count is an independent tally of pictures to hold the
                //       counters against: the counters' SUM must equal the pictures served in the
                //       drive, which is what makes "they count pictures drawn, each once" a
                //       comparison rather than a restatement.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) `reused` forced to nullptr in
                //       updatePaintNode (no import is ever kept) -> "FAIL: a ring served round and
                //       round costs one import per image", printing "16 import(s), 0 drawn through
                //       a kept one, for a ring of 5"; (b) `kMaxImports = kTargetDepth` ->
                //       `kTargetDepth - 1` -> the same line red with the same count, the cache
                //       refilling as the ring comes round (and item_cache_stale-daemon's premise red
                //       beside it); (c) the ring's depth raised to 6 in backend/daemon/ring_depth.h
                //       (`1 +` -> `2 +`, both sides rebuilt) -> GREEN, a ring of 6 costing 6 imports,
                //       while `kMaxImports = 5` written as a literal with that depth -> red again,
                //       "19 import(s) ... for a ring of 6". All observed 2026-09-22 on the NVIDIA
                //       entry. Reverted.
                const size_t ring = imageCount;
                const quint64 imports0 = scene.imports();
                const quint64 reused0 = scene.reused();
                const int served0 = stub.served();
                const size_t drive = 3 * ring;
                for (size_t step = 0; step < drive; ++step) {
                    if (!drawNext(scene, stub, bus)) {
                        check(false, "the item draws each picture of the ring it is served");
                        std::fprintf(stderr, "  stopped after %zu of %zu, the item reports [%s]\n", step, drive,
                                     scene.reason().toStdString().c_str());
                        return true;
                    }
                }
                const quint64 imports = scene.imports();
                const quint64 reused = scene.reused();
                const int servedInDrive = stub.served() - served0;
                check(imports == ring, "a ring served round and round costs one import per image (" +
                                           std::to_string(imports) + " import(s), " + std::to_string(reused) +
                                           " drawn through a kept one, for a ring of " + std::to_string(ring) + ")");
                // GOAL: S13.198's accounting -- the counters count pictures DRAWN, each once: over
                //       the drive their sum rose by exactly the pictures served, one per announcement.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, a second `m_reusedImportCount.fetch_add(1,
                //       ...)` beside the count at the drawn picture -> "FAIL: the counters rose by
                //       exactly the pictures drawn in the drive", printing 30 for 15. Observed
                //       2026-09-22 on the NVIDIA entry. Reverted.
                const quint64 rose = (imports + reused) - (imports0 + reused0);
                check(rose == static_cast<quint64>(servedInDrive) && servedInDrive == static_cast<int>(drive),
                      "the counters rose by exactly the pictures drawn in the drive (" + std::to_string(rose) +
                          " for " + std::to_string(servedInDrive) + " served, " + std::to_string(drive) +
                          " announced)");

                // GOAL: S13.198's NOTIFY -- a QML binding on either counter follows it. Without the
                //       change signal a binding is read once and frozen, and the counters exist for
                //       exactly that reader.
                // CONTROL: `emit importCountsChanged();` deleted from the drawn-picture hop in
                //       WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) -> "FAIL: a
                //       QML binding on importCount and reusedImportCount follows them", printing
                //       "bound 0/0, read 5/11". Observed 2026-09-22 on the NVIDIA entry. Reverted.
                pumpFor(50);
                const quint64 boundImports = scene.root->property("boundImports").toULongLong();
                const quint64 boundReused = scene.root->property("boundReused").toULongLong();
                check(boundImports == scene.imports() && boundReused == scene.reused(),
                      "a QML binding on importCount and reusedImportCount follows them (bound " +
                          std::to_string(boundImports) + "/" + std::to_string(boundReused) + ", read " +
                          std::to_string(scene.imports()) + "/" + std::to_string(scene.reused()) + ")");

                // GOAL: S13.201 -- claims are not cached: every picture is given back once the next
                //       one is on the screen, the image the node samples and no other kept. False for a
                //       hold that rode on the import, which with imports kept would pin every image of
                //       the ring for the session and leave the daemon nowhere to draw.
                // WHY-THIS-SHAPE: the release is sent after the GPU has run the reads, through a
                //       fence and a hop, so the count is waited for with a bound rather than read at
                //       once; and it is bounded above as well -- one release per picture, with the one
                //       on the screen still held.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, FrameNode::present()'s
                //       `releaseClaimWhenReadsFinish(previous);` deleted -> "FAIL: every picture but
                //       the one on the screen has been given back", printing "0 release(s) for 16
                //       picture(s) served". Observed 2026-09-22 on the NVIDIA entry. Reverted.
                const int servedTotal = stub.served();
                const bool givenBack = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return static_cast<int>(stub.releases().size()) >= servedTotal - 1;
                    },
                    3000);
                check(givenBack && static_cast<int>(stub.releases().size()) == servedTotal - 1,
                      "every picture but the one on the screen has been given back (" +
                          std::to_string(stub.releases().size()) + " release(s) for " + std::to_string(servedTotal) +
                          " picture(s) served)");
                return true;
            }

            case Phase::Single: {
                // GOAL: S13.198 -- one image served over and over costs ONE import: every picture
                //       after the first is drawn through the import kept for it.
                // CONTROL: `reused` forced to nullptr in WallpaperXItem::updatePaintNode
                //       (backend/kde/wallpaperx_item.cpp) -> "FAIL: one image served over and over
                //       costs one import", printing "11 import(s), 0 kept". Observed 2026-09-22 on the
                //       NVIDIA entry. Reverted.
                constexpr int kDrive = 10;
                for (int step = 0; step < kDrive; ++step) {
                    if (!drawNext(scene, stub, bus)) {
                        check(false, "the item draws each picture of the one image it is served");
                        return true;
                    }
                }
                check(scene.imports() == 1 && scene.reused() == scene.pictures() - 1 &&
                          scene.pictures() >= static_cast<quint64>(kDrive + 1),
                      "one image served over and over costs one import (" + std::to_string(scene.imports()) +
                          " import(s), " + std::to_string(scene.reused()) + " kept)");
                return true;
            }

            case Phase::Pixels: {
                // GOAL: S13.199 -- a kept import shows the picture drawn into its image SINCE it was
                //       imported, not the one it was imported with and not another image's: the
                //       property the whole stage stands on. Read on the GPU and handle path this
                //       entry runs on, which on the NVIDIA desktop is the opaque-fd one.
                // WHY-THIS-SHAPE: two images, each redrawn in a new colour before it comes round
                //       again, and the texture the node samples read back through the scene graph's
                //       own context after the frame -- sampled, as a compositor reads it
                //       (gl_import_harness.h's readSampled). Four flat colours, so a stale picture
                //       and another image's picture read as different answers. Both redrawn pictures
                //       are asserted to have been drawn through KEPT imports, or the reading would be
                //       of fresh imports and prove nothing about reuse.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, importKeyFor() given the constant
                //       bufferId 1, so every image of the ring shares one key and one import -- the
                //       first image's -> "FAIL: the other image shows what was drawn into it (read red
                //       where green was drawn)" and "FAIL: and so does the other kept import, with its
                //       own image's new picture (read blue where yellow was drawn)"; the first image's
                //       own redraw reads correctly, being drawn through its own import.
                //       Also measured, and NOT a control: the acquire skipped on the reuse path
                //       (`reused == nullptr &&` added to the FrameImport::acquire condition) leaves
                //       every reading correct on this driver -- the stub's renderer waits for its own
                //       submission before exporting, so the memory is already coherent; what the
                //       acquire orders is not observable through this stub. Both observed 2026-09-22
                //       on the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: every reading is of a picture the item drew -- the counters move
                //       by one per step -- and the two redrawn ones are drawn through kept imports.
                const auto onScreen = [&](int expected, const std::string& what) {
                    GLuint name = nativeName(itemTexture(scene.item));
                    std::vector<unsigned char> pixels;
                    std::string why;
                    scene.renderPass([&] { pixels = wallpaperx::testing::readSampled(name, kWidth, kHeight, why); });
                    if (pixels.empty()) {
                        wallpaperx::testing::harnessFailure("the texture on the screen could not be read back: " + why);
                        return;
                    }
                    const unsigned char* centre = &pixels[((kHeight / 2) * kWidth + kWidth / 2) * 4];
                    const int seen = nearestColour(centre, palette);
                    const char* names[] = {"red", "green", "blue", "yellow"};
                    check(seen == expected, what + " (read " + (seen < 0 ? std::string("no palette colour") : names[seen]) +
                                                " where " + names[expected] + " was drawn)");
                };
                // Which image is on the screen after the startup fetches, and the other.
                const size_t onScreenIndex = static_cast<size_t>(stub.served() - 1) % 2;
                const size_t otherIndex = 1 - onScreenIndex;
                onScreen(static_cast<int>(onScreenIndex % 2), "the first picture shows what was drawn into its image");

                if (!drawNext(scene, stub, bus)) {
                    check(false, "the item draws the second image");
                    return true;
                }
                onScreen(static_cast<int>(otherIndex % 2), "the other image shows what was drawn into it");

                const quint64 reusedBefore = scene.reused();
                publishSolidFrame(device, targets, *ring[onScreenIndex], "item_import_cache_blue.png", palette[2]);
                if (!drawNext(scene, stub, bus)) {
                    check(false, "the item draws the first image again once it is redrawn");
                    return true;
                }
                check(scene.reused() == reusedBefore + 1, "the redrawn first image is drawn through its kept import");
                onScreen(2, "a kept import shows the picture drawn into its image since");

                publishSolidFrame(device, targets, *ring[otherIndex], "item_import_cache_yellow.png", palette[3]);
                if (!drawNext(scene, stub, bus)) {
                    check(false, "the item draws the second image again once it is redrawn");
                    return true;
                }
                check(scene.reused() == reusedBefore + 2, "the redrawn second image is drawn through its kept import");
                onScreen(3, "and so does the other kept import, with its own image's new picture");
                return true;
            }

            case Phase::NoAcquire: {
                // GOAL: S13.200 -- a picture that arrives WITHOUT the producer's hand-over is imported
                //       afresh, never drawn through a kept import: neither extension promises that an
                //       unsynchronised texture sees a later write, so a driver with no hand-over
                //       signal keeps exactly the behaviour it had before imports were kept.
                // WHY-THIS-SHAPE: the same image throughout, and it HAS a kept import -- the first
                //       picture came with an acquire -- so a reuse is available and only the rule can
                //       refuse it. Two pictures without, so one reused by accident still shows.
                // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp),
                //       `node != nullptr && carriesHandOver(incoming->request) ? node->held(key) :
                //       nullptr` made `node != nullptr ? node->held(key) : nullptr` -> "FAIL: a
                //       picture with no hand-over is imported afresh, never reused", printing "0
                //       import(s) and 2 reuse(s) over the two pictures". Observed 2026-09-22 on the
                //       NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: the stub is asserted to have sent the first picture's acquire and
                //       none after, so the rule is read against the replies it is about.
                const quint64 imports0 = scene.imports();
                const quint64 reused0 = scene.reused();
                const int acquires0 = stub.acquireFdsSent();
                check(acquires0 >= 1, "the first picture came with the producer's hand-over");
                stub.sendAcquireFrom(stub.served() + 1, ExportingStub::Acquire::None);
                for (int step = 0; step < 2; ++step) {
                    if (!drawNext(scene, stub, bus)) {
                        check(false, "a picture without a hand-over is still drawn");
                        return true;
                    }
                }
                check(stub.acquireFdsSent() == acquires0, "and the two after it came without one");
                check(scene.imports() == imports0 + 2 && scene.reused() == reused0,
                      "a picture with no hand-over is imported afresh, never reused (" +
                          std::to_string(scene.imports() - imports0) + " import(s) and " +
                          std::to_string(scene.reused() - reused0) + " reuse(s) over the two pictures)");
                return true;
            }

            case Phase::HandOver:
            case Phase::HandOverReset: {
                QSGTexture* shown = itemTexture(scene.item);
                const quint64 pictures0 = scene.pictures();
                if (phase == Phase::HandOver && !stub.releases().empty()) {
                    wallpaperx::testing::harnessFailure("the item had already given an image back before any hand-over "
                                                        "failed, so the release count below would not be about them");
                    return true;
                }
                const auto stillShowing = [&] {
                    return claimsFrameShown(scene.item) && scene.reason().isEmpty() && itemTexture(scene.item) == shown;
                };
                if (!stillShowing()) {
                    wallpaperx::testing::harnessFailure("the item was not showing its first picture cleanly before any "
                                                        "hand-over failed");
                    return true;
                }
                const int base = stub.served();

                if (phase == Phase::HandOverReset) {
                    // GOAL: S13.203's reset -- a picture drawn ends a run of failed hand-overs, so
                    //       failures separated by one good picture are two runs, not one: five, a
                    //       good picture, then eight more keep the wallpaper silently.
                    // WHY-THIS-SHAPE: every reply's acquire is decided by the stub as it is built,
                    //       because after each failure the item asks again by itself and nothing the
                    //       test does from outside can land between the two. The second run is held
                    //       at its ninth request, so what is read is the state after exactly eight.
                    //       The good picture between the runs comes WITHOUT an acquire, and that is
                    //       forced: on the NVIDIA driver one rejected glImportSemaphoreFdEXT makes
                    //       every later import in that context fail with GL_OUT_OF_MEMORY -- measured
                    //       in a scratch program -- so a picture WITH a real acquire after the first
                    //       run can never be drawn here, and the reset would be unreachable.
                    // CONTROL: `m_handOversFailed = 0;` deleted from the drawn-picture branch of
                    //       WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) -> "FAIL:
                    //       eight failures after a good picture still keep it, silently (frame shown:
                    //       no, reason [WallpaperX produced a frame this desktop could not import.])".
                    //       kToleratedHandOverFailures 8 -> 7 reddens the same line. Both observed
                    //       2026-09-22 on the NVIDIA entry. Reverted.
                    // COVERAGE-ASSERT: the good picture between the runs is asserted to have been
                    //       drawn, and the stub to have sent exactly thirteen unusable acquires.
                    stub.sendAcquireFrom(base + 1, ExportingStub::Acquire::Unusable);
                    stub.sendAcquireFrom(base + 6, ExportingStub::Acquire::None);
                    stub.sendAcquireFrom(base + 7, ExportingStub::Acquire::Unusable);
                    stub.holdServeNumber(base + 15);
                    announce(bus, static_cast<quint64>(base) + 1);
                    const bool firstRun = pumpUntil(
                        [&] {
                            scene.renderPass();
                            return stub.served() >= base + 6 && scene.pictures() > pictures0;
                        },
                        10000);
                    check(firstRun, "five failed hand-overs are followed by a good picture, which is drawn");
                    if (!firstRun) {
                        std::fprintf(stderr, "  served %d after the first picture, the item reports [%s]\n",
                                     stub.served() - base, scene.reason().toStdString().c_str());
                        return true;
                    }
                    shown = itemTexture(scene.item);
                    announce(bus, static_cast<quint64>(stub.served()) + 1);
                    const bool held = pumpUntil(
                        [&] {
                            scene.renderPass();
                            return stub.replyIsHeld();
                        },
                        10000);
                    check(held, "the second run reaches its ninth request");
                    if (!held) return true;
                    for (int pass = 0; pass < 5; ++pass) scene.renderPass();
                    pumpFor(50);
                    // Built: five, eight, and the second run's ninth, held.
                    check(stub.unusableAcquiresSent() == 5 + kToleratedHandOverFailures + 1,
                          "thirteen hand-overs failed, in two runs (" + std::to_string(stub.unusableAcquiresSent()) +
                              " unusable acquires built, the last held)");
                    check(stillShowing(), "eight failures after a good picture still keep it, silently (frame "
                                          "shown: " +
                                              std::string(claimsFrameShown(scene.item) ? "yes" : "no") + ", reason [" +
                                              scene.reason().toStdString() + "])");
                    stub.sendHeldReply();
                    stub.refuseFurtherWith(QStringLiteral("io.wallpaperx.Error.NoFrame"));
                    pumpFor(100);
                    return true;
                }

                // GOAL: S13.203 -- a failed hand-over is tolerated for a bounded run: up to eight in a
                //       row keep the picture on the screen silently -- a frame shown, no reason, the
                //       node and its texture kept -- and the ninth blanks the item with "WallpaperX
                //       produced a frame this desktop could not import." Both directions: a blip must
                //       not take a wallpaper off the screen, and a hand-over that has stopped working
                //       must not freeze it on one picture while the item reports it is fine.
                // WHY-THIS-SHAPE: the stub decides each reply's acquire as it builds it and holds the
                //       ninth request, because the item asks again by itself after every failure and
                //       nothing from outside can land between two of those. The eight are hand-overs
                //       on the import kept for this image -- the reuse path -- which is where a
                //       failure leaves a picture worth keeping.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) kToleratedHandOverFailures 8 -> 7
                //       -> "FAIL: eight failed hand-overs in a row keep the picture, silently",
                //       printing "frame shown: no, reason [WallpaperX produced a frame this desktop
                //       could not import.]"; (b) the `m_handOversFailed <= kToleratedHandOverFailures
                //       &&` term deleted from `stillDrawing` -> "FAIL: the ninth failure takes the
                //       picture off and says why (frame shown: yes, reason [])". Also measured, and
                //       unreachable rather than covered: that the run's count lives on the item and
                //       not on the node. The count reset wherever a node is built leaves every phase
                //       here green, because a rebuilt node holds no picture -- a failure then blanks
                //       whatever the count says, and the first picture drawn resets it anyway.
                //       Observed 2026-09-22 on the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: exactly eight unusable acquires are asserted to have gone out
                //       before the held ninth, and the counters not to have moved.
                stub.sendAcquireFrom(base + 1, ExportingStub::Acquire::Unusable);
                stub.holdServeNumber(base + kToleratedHandOverFailures + 1);
                announce(bus, static_cast<quint64>(base) + 1);
                const bool reached = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return stub.replyIsHeld();
                    },
                    10000);
                check(reached, "the item asks again after each failed hand-over, up to the held ninth request");
                if (!reached) {
                    std::fprintf(stderr, "  the stub served %d after the good picture\n", stub.served() - base);
                    return true;
                }
                for (int pass = 0; pass < 5; ++pass) scene.renderPass();
                pumpFor(50);
                // Nine built: the eight that failed, and the ninth, held.
                check(stub.unusableAcquiresSent() == kToleratedHandOverFailures + 1,
                      "eight hand-overs failed before the held ninth (" +
                          std::to_string(stub.unusableAcquiresSent()) + " unusable acquires built)");
                check(stillShowing(), "eight failed hand-overs in a row keep the picture, silently (frame shown: " +
                                          std::string(claimsFrameShown(scene.item) ? "yes" : "no") + ", reason [" +
                                          scene.reason().toStdString() + "])");

                // GOAL: S13.198's other half -- a picture that failed counts in neither counter:
                //       their sum is what reached the screen.
                // CONTROL: the drawn-picture count moved in WallpaperXItem::updatePaintNode
                //       (backend/kde/wallpaperx_item.cpp) to just after `reused` is chosen, counting
                //       attempts -> "FAIL: failed pictures count in neither counter", printing "8
                //       more". Observed 2026-09-22 on the NVIDIA entry. Reverted.
                check(scene.pictures() == pictures0, "failed pictures count in neither counter (" +
                                                         std::to_string(scene.pictures() - pictures0) + " more)");

                // The ninth, and the one held after it so the item's next re-ask does not run on.
                stub.holdServeNumber(base + kToleratedHandOverFailures + 2);
                stub.sendHeldReply();
                const bool blanked = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return !claimsFrameShown(scene.item);
                    },
                    5000);
                check(blanked && scene.reason() == QLatin1String(kCouldNotImport) && itemTexture(scene.item) == nullptr,
                      "the ninth failure takes the picture off and says why (frame shown: " +
                          std::string(claimsFrameShown(scene.item) ? "yes" : "no") + ", reason [" +
                          scene.reason().toStdString() + "])");

                // GOAL: S13.201 on the one path where the NODE goes and the item stays -- the picture
                //       it was drawing is given back to the daemon when the failure takes it off the
                //       screen. Until then nothing was: each failed picture's own claim went back
                //       beside the drawn one's, which kept the image counted as held.
                // WHY-THIS-SHAPE: one image throughout, so the whole run is one image in the release
                //       ledger: exactly one release, and only once the node has gone.
                // CONTROL: in ~FrameNode (backend/kde/wallpaperx_item.cpp), `releaseClaimWhenReadsFinish
                //       (m_claim);` made `(void)m_claim;` -> "FAIL: the picture the dropped node was
                //       drawing is given back once it goes", printing "0 release(s)". Observed
                //       2026-09-22 on the NVIDIA entry. Reverted.
                const bool givenBack = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return !stub.releases().empty();
                    },
                    3000);
                check(givenBack && stub.releases().size() == 1,
                      "the picture the dropped node was drawing is given back once it goes (" +
                          std::to_string(stub.releases().size()) + " release(s))");
                return true;
            }

            case Phase::HandOverModifier: {
                // GOAL: S13.202 -- only a refusal to describe the memory withdraws the DRM format
                //       modifier. A failed hand-over after a fresh import, and one on a kept import,
                //       say nothing about the layout -- the wait is the same whichever modifier the
                //       frame arrived in -- so the plugin must not report its list again without it.
                //       False for the defect this stage's review found: a failed hand-over read as
                //       proof the layout cannot be imported, taking a layout this driver imports out
                //       of the daemon's hands for the session.
                // WHY-THIS-SHAPE: dma_buf frames (this entry forces the export), the only kind with a
                //       layout; two images, the second never drawn successfully, so the run holds both
                //       kinds of failure: its first hand-over follows a fresh import and the rest are
                //       on the import kept for it. The initial report is required to list the served
                //       layout, or a withdrawal of it would be dropped before it was sent and the
                //       check could not fail.
                // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp),
                //       `stopped == Stopped::Layout &&` made `stopped != Stopped::Nothing &&` -> "FAIL:
                //       no failed hand-over withdraws the layout it arrived in", printing the report
                //       that dropped it. Observed 2026-09-22 on the NVIDIA entry with the dma_buf export
                //       forced. Reverted.
                if (stub.handleType() != 1) {
                    notVerified(kGoal, "this entry's export is not a dma_buf, so the frame has no layout to withdraw "
                                       "(the entry forces WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf)");
                    return true;
                }
                const quint64 served = stub.lastModifier();
                const bool reported = pumpUntil([&stub] { return !stub.modifierReports().empty(); }, 3000);
                const bool listed =
                    reported && std::find(stub.modifierReports().front().modifiers.begin(),
                                          stub.modifierReports().front().modifiers.end(),
                                          served) != stub.modifierReports().front().modifiers.end();
                if (!listed) {
                    notVerified(kGoal, "the plugin's report does not list the layout the frames arrive in, so a "
                                       "withdrawal of it could not be sent and the check could not fail");
                    return true;
                }
                const size_t reports0 = stub.modifierReports().size();
                const int base = stub.served();
                stub.sendAcquireFrom(base + 1, ExportingStub::Acquire::Unusable);
                stub.holdServeNumber(base + 5);
                announce(bus, static_cast<quint64>(base) + 1);
                const bool reached = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return stub.replyIsHeld();
                    },
                    10000);
                check(reached, "four hand-overs failed, fresh and kept");
                if (!reached) return true;
                pumpFor(200);
                // Four failed, and the fifth built and held.
                check(stub.unusableAcquiresSent() == 5, "four hand-overs failed before the held fifth (" +
                                                            std::to_string(stub.unusableAcquiresSent()) +
                                                            " unusable acquires built)");
                bool dropped = false;
                for (size_t i = reports0; i < stub.modifierReports().size(); ++i) {
                    const auto& list = stub.modifierReports()[i].modifiers;
                    if (std::find(list.begin(), list.end(), served) == list.end()) dropped = true;
                }
                check(!dropped, "no failed hand-over withdraws the layout it arrived in (" +
                                    std::to_string(stub.modifierReports().size() - reports0) +
                                    " further report(s), the served layout " +
                                    (dropped ? std::string("dropped") : std::string("kept")) + ")");
                stub.refuseFurtherWith(QStringLiteral("io.wallpaperx.Error.NoFrame"));
                stub.sendHeldReply();
                pumpFor(100);
                return true;
            }

            case Phase::StaleDaemon:
            case Phase::StaleMonitor: {
                TextureWatch watch(&scene.window);
                if (!importWholeRing(scene, stub, bus, watch, imageCount)) return true;
                QSGTexture* drawn = itemTexture(scene.item);
                const size_t destroyed0 = watch.destroyed();
                check(destroyed0 == 0, "every image of the ring is still imported before anything goes stale (" +
                                           std::to_string(destroyed0) + " freed)");

                if (phase == Phase::StaleDaemon) {
                    // GOAL: S13.204, a daemon that leaves -- when the daemon's bus name goes away and
                    //       nothing takes it again, the imports no later hand-over could name are
                    //       freed without waiting for a picture (none will come), and the picture on
                    //       the screen is kept. False for the rule this stage replaced, which freed
                    //       them only as later pictures pushed them out: a departed daemon's whole
                    //       ring stayed resident.
                    // WHY-THIS-SHAPE: every image of the ring is drawn first, so there are
                    //       kTargetDepth imports to free, each watched by its scene-graph texture --
                    //       the object an import dies with. Only render passes follow the departure,
                    //       no picture. The drawn one is required ALIVE as well as the rest freed.
                    // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) the `forgetGenerations();` in
                    //       the service watcher's lambda in WallpaperXItem::connectToDaemon deleted ->
                    //       "FAIL: a departed daemon's imports are freed without a picture", printing
                    //       "0 of 4"; (b) `update();` deleted from WallpaperXItem::forgetGenerations ->
                    //       the same line red, nothing asking for the pass; (c) `node->forgetAllButDrawn();`
                    //       deleted from updatePaintNode -> the same. Observed 2026-09-22 on the NVIDIA
                    //       entry. Reverted.
                    bus.unregisterObject(QString::fromUtf8(kPath));
                    if (!bus.unregisterService(QString::fromUtf8(kService))) {
                        wallpaperx::testing::harnessFailure("the stub could not give up the daemon's bus name");
                        return true;
                    }
                } else {
                    // GOAL: S13.204, a monitor left -- when the item is moved to another monitor, the
                    //       imports of the monitor it left are freed without waiting for a picture of
                    //       the new one, and the picture on the screen is kept.
                    // WHY-THIS-SHAPE: the new monitor's reply is HELD, so no picture arrives and the
                    //       only thing that can free the imports is the request the move made.
                    // CONTROL: in WallpaperXItem::discardHeldFrame (backend/kde/wallpaperx_item.cpp),
                    //       `if (m_nodeRequest == NodeRequest::None) m_nodeRequest =
                    //       NodeRequest::ForgetStaleImports;` deleted -> "FAIL: a left monitor's
                    //       imports are freed without a picture", printing "0 of 4". Observed
                    //       2026-09-22 on the NVIDIA entry. Reverted.
                    stub.holdNextReply();
                    scene.item->setProperty("monitorId", kSecondMonitor);
                    const bool held = pumpUntil([&stub] { return stub.replyIsHeld(); }, 3000);
                    if (!held) {
                        wallpaperx::testing::harnessFailure("the item did not ask for the new monitor's picture, so "
                                                            "what freed the imports could not be told apart");
                        return true;
                    }
                }
                for (int pass = 0; pass < 5; ++pass) {
                    scene.renderPass();
                    pumpFor(20);
                }
                const char* what = phase == Phase::StaleDaemon ? "a departed daemon's imports are freed without a picture"
                                                               : "a left monitor's imports are freed without a picture";
                check(watch.destroyed() == imageCount - 1 && !watch.isDestroyed(drawn),
                      std::string(what) + " (" + std::to_string(watch.destroyed()) + " of " +
                          std::to_string(imageCount - 1) + ", the drawn one " +
                          (watch.isDestroyed(drawn) ? "freed" : "kept") + ")");
                check(claimsFrameShown(scene.item) && itemTexture(scene.item) == drawn,
                      "and the picture on the screen stays");
                if (phase == Phase::StaleMonitor) {
                    stub.refuseFurtherWith(QStringLiteral("io.wallpaperx.Error.NoFrame"));
                    stub.sendHeldReply();
                }
                return true;
            }

            case Phase::Offscreen: {
                // GOAL: S13.205 -- an item taken off the screen keeps no import: the node goes and
                //       EVERY import with it, not only the one on the screen; and coming back imports
                //       afresh rather than drawing through anything kept from before.
                // WHY-THIS-SHAPE: the whole ring is imported first and each import watched through
                //       its texture, so "the node went" cannot pass while four imports survive it.
                //       The return is graded by the counters: the first picture back must be an
                //       import, not a reuse -- of the image that was on the screen, since any other
                //       image of the ring would be imported afresh by a node that had survived too
                //       (measured: with the ring left to rotate, the control below reddened only the
                //       first check).
                // CONTROL: in WallpaperXItem::dropShownFrame (backend/kde/wallpaperx_item.cpp),
                //       `m_nodeRequest = NodeRequest::Destroy;` made `NodeRequest::ForgetStaleImports`
                //       -> "FAIL: off the screen, the node and every import go", printing "4 of 5 freed,
                //       node kept", and "FAIL: coming back imports afresh", printing "0 import(s), 1
                //       reuse(s)"; item_offscreen_hidden red beside them. Observed 2026-09-22 on the
                //       NVIDIA entry. Reverted.
                TextureWatch watch(&scene.window);
                if (!importWholeRing(scene, stub, bus, watch, imageCount)) return true;
                // The image on the screen when the item is hidden is the one served first on its
                // return: the one image whose import a node that survived the hide would still hold,
                // so "imports afresh" is a verdict rather than what any other image would cost.
                const wallpaperx::OffscreenTarget* onScreenImage =
                    ring[static_cast<size_t>(stub.served() - 1) % ring.size()];
                scene.item->setVisible(false);
                for (int pass = 0; pass < 5; ++pass) {
                    scene.renderPass();
                    pumpFor(20);
                }
                check(watch.destroyed() == imageCount && itemPaintNode(scene.item) == nullptr,
                      "off the screen, the node and every import go (" + std::to_string(watch.destroyed()) + " of " +
                          std::to_string(imageCount) + " freed, node " +
                          (itemPaintNode(scene.item) == nullptr ? "gone" : "kept") + ")");
                const quint64 imports0 = scene.imports();
                const quint64 reused0 = scene.reused();
                stub.serveNext(onScreenImage);
                scene.item->setVisible(true);
                const bool back = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return scene.pictures() > imports0 + reused0;
                    },
                    5000);
                check(back && scene.imports() == imports0 + 1 && scene.reused() == reused0,
                      "coming back imports afresh (" + std::to_string(scene.imports() - imports0) + " import(s), " +
                          std::to_string(scene.reused() - reused0) + " reuse(s))");
                return true;
            }
        }
        return true;
    });
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_import_cache_test";
    wallpaperx::testing::registerFrameReplyTypes();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);

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
        std::fprintf(stderr, "item_import_cache_test: the validation grade could not be made -- "
                             "VK_LAYER_KHRONOS_validation is not loaded.\n");
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
