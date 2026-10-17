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
//   publish            a picture that changes nothing emits nothing and posts one call (S13.212)
//   not-opengl         a scene graph that is not OpenGL is told why nothing is shown (S13.213)
//   descriptor-backoff refused for holding too many descriptors, the item keeps its frame and waits
//                      out a back-off before asking again, logging once per run (S15.273)
//   declare            each fetch names the images the node can draw again, and a reply leaving
//                      their memory out is drawn through the kept import (S15.296)
//   declare-refused    a process whose driver refused the producer's acquire names nothing (S15.297)
//   declare-monitor    a fetch for another monitor names nothing of the old one (S15.296)
//   declare-daemon     a fetch to a new daemon names nothing of the old one's (S15.296)
//   unique-name        a fetch goes to the daemon's unique name, not the well-known one (S15.298)
//   lost-import        a reply leaving out memory the node has since dropped: counted, logged with a
//                      cap, asked again once naming nothing, and drawn (S15.299, S15.300)
//   protocol           memory left out for an image not named, or sent twice: not drawn, said once,
//                      not asked again (S15.301)
//   restart-pending    a reply in hand but not yet drawn when the daemon is replaced is drawn, and the
//                      new daemon's first picture after it (S15.313)
//   pending-monitor    such a reply is dropped, and given back by number, on a monitor change (S15.314)
//   pending-offscreen  and on leaving the screen, never drawn on coming back (S15.314)
//   no-owner           no request goes out while no daemon is known, so none is credited to a daemon
//                      that has left and none leaks (S15.312)
//   no-item            a daemon replaced while no item exists is seen by the next item (S15.315)
//   asking-monitor     a monitor change with a request out frees the left monitor's imports once its
//                      reply lands, and gives that reply back by its number (S15.316, S15.310)
//   startup-no-daemon  with no daemon at startup the item says so and asks nothing (S15.311)
//
// Every phase also runs a bus monitor, and ends by requiring that no GetFrame or ReleaseBuffer was
// addressed to the daemon's well-known name (S15.311).
//
// Like the other importing item tests, this reports NOT VERIFIED when this host cannot pair a GL
// context with the Vulkan device that produced the export: an opaque fd is importable only by the
// driver that made it.

#include "test_image_dir.h"
#include "daemon/ring_depth.h"
#include "kde/frame_import.h"
#include "support/validation_grade.h"
#include "tests/bus_monitor.h"
#include "tests/gl_import_harness.h"
#include "tests/gl_semaphore_fault.h"
#include "tests/item_render_scene.h"
#include "tests/item_stub_scene.h"
#include "tests/stream_capture.h"
#include "tests/test_report.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QElapsedTimer>
#include <QEvent>
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
#include <QTemporaryDir>
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
#include <thread>
#include <typeinfo>
#include <vector>

namespace {

using wallpaperx::testing::announce;
using wallpaperx::testing::cacheQmlSource;
using wallpaperx::testing::check;
using wallpaperx::testing::claimsFrameShown;
using wallpaperx::testing::drawNext;
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
using wallpaperx::testing::nativeName;
using wallpaperx::testing::nearestColour;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::publishSolidFrame;
using wallpaperx::testing::pumpFor;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::Scene;
using wallpaperx::testing::settleQuiet;
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
    Publish,
    NotOpenGl,
    DescriptorBackoff,
    Declare,
    DeclareRefused,
    DeclareMonitor,
    DeclareDaemon,
    UniqueName,
    LostImport,
    Protocol,
    RestartPending,
    PendingMonitor,
    PendingOffscreen,
    NoOwner,
    NoItem,
    AskingMonitor,
    StartupNoDaemon,
    LateAcquire,
    NewerReply,
    NewerFailure,
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
    {Phase::Publish, "publish",
     "S13.212 (a picture that changes nothing emits nothing and posts one call to the item's thread)"},
    {Phase::NotOpenGl, "not-opengl", "S13.213 (a desktop not running on OpenGL is told why nothing is shown)"},
    {Phase::DescriptorBackoff, "descriptor-backoff",
     "S15.273 (refused for descriptors, the item keeps its frame, backs off, and logs once per run)"},
    {Phase::Declare, "declare", "S15.296 (each fetch names exactly the images the node can draw again)"},
    {Phase::DeclareRefused, "declare-refused", "S15.297 (a process that cannot wait on the acquire names nothing)"},
    {Phase::DeclareMonitor, "declare-monitor", "S15.296 (a fetch for another monitor names nothing of the old one)"},
    {Phase::DeclareDaemon, "declare-daemon", "S15.296 (a fetch to a new daemon names nothing of the old one's)"},
    {Phase::UniqueName, "unique-name", "S15.298 (a fetch is addressed to the daemon's unique name)"},
    {Phase::LostImport, "lost-import", "S15.299 and S15.300 (a lost import is counted, logged, and asked again once)"},
    {Phase::Protocol, "protocol", "S15.301 (a reply breaking the memory rule is not drawn and not asked again)"},
    {Phase::RestartPending, "restart-pending",
     "S15.313 (a reply in hand when the daemon is replaced is drawn, then the new daemon's first picture)"},
    {Phase::PendingMonitor, "pending-monitor", "S15.314 (a reply in hand is dropped and given back on a monitor change)"},
    {Phase::PendingOffscreen, "pending-offscreen",
     "S15.314 (a reply in hand is dropped and given back on leaving the screen)"},
    {Phase::NoOwner, "no-owner", "S15.312 (no request while no daemon is known; nothing leaks, nothing stalls)"},
    {Phase::NoItem, "no-item", "S15.315 (a daemon replaced while no item exists is seen by the next one)"},
    {Phase::AskingMonitor, "asking-monitor",
     "S15.316 and S15.310 (a left monitor's imports go once the reply out at the change lands)"},
    {Phase::StartupNoDaemon, "startup-no-daemon", "S15.311 (no daemon at startup: said so, nothing asked)"},
    {Phase::LateAcquire, "late-acquire",
     "S13.199's ordering (a kept import is read only after the producer's acquire is signalled)"},
    {Phase::NewerReply, "newer-reply", "S15.351 (a lost import with a newer reply already in asks nothing more)"},
    {Phase::NewerFailure, "newer-failure",
     "S15.351 (a failed hand-over with a newer reply already in asks nothing more)"},
};

// The private bus seen from outside, for the whole run, so the end of main can say whether any frame
// request or release was ever addressed to the well-known name. Null when the bus refused a monitor.
wallpaperx::testing::BusMonitor* g_monitor = nullptr;

// Returns once the item's bus connection has read everything `stubBus` sent it so far, without
// running this thread's event loop: a blocking Ping from the stub's connection to the item's, which
// the bus delivers after the stub's earlier messages and QtDBus answers on its own thread. What the
// item's connection read is then queued for the item's thread, unprocessed. False, with why, when the
// Ping was not answered.
bool readByItemConnection(QDBusConnection& stubBus, std::string& why) {
    const QDBusMessage ping =
        QDBusMessage::createMethodCall(QDBusConnection::sessionBus().baseService(), QStringLiteral("/"),
                                       QStringLiteral("org.freedesktop.DBus.Peer"), QStringLiteral("Ping"));
    const QDBusMessage pong = stubBus.call(ping, QDBus::Block, 3000);
    if (pong.type() == QDBusMessage::ReplyMessage) return true;
    why = pong.errorName().toStdString() + ": " + pong.errorMessage().toStdString();
    return false;
}

// readByItemConnection(), then this thread's event loop run over what that queued: a reply the stub
// sent is then provably taken by the item, with no render pass run.
bool takenByItem(QDBusConnection& stubBus, std::string& why) {
    if (!readByItemConnection(stubBus, why)) return false;
    pumpFor(20);
    return true;
}

// Whether `monitor` has seen everything the item's connection sent so far: a barrier call is sent on
// that connection, and the bus routes one connection's messages in order, so once the monitor has
// seen it it has seen everything before it. False, with why, when the monitor stopped reading or never
// saw the barrier -- a count read then would be a silence nobody observed.
bool monitorCaughtUp(const wallpaperx::testing::BusMonitor& monitor, std::string& why) {
    using wallpaperx::testing::BusMonitor;
    const int before = monitor.calls(BusMonitor::kBarrierMember);
    QDBusMessage barrier =
        QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                       QStringLiteral("org.freedesktop.DBus"), QString::fromUtf8(BusMonitor::kBarrierMember));
    barrier.setArguments({QString::fromUtf8(BusMonitor::kBarrierName)});
    (void)QDBusConnection::sessionBus().call(barrier, QDBus::Block, 3000);
    const bool seen =
        pumpUntil([&] { return !monitor.ok() || monitor.calls(BusMonitor::kBarrierMember) > before; }, 3000);
    if (!monitor.ok()) {
        why = "the bus monitor stopped reading partway: " + monitor.why();
        return false;
    }
    if (!seen) {
        why = "the bus monitor never saw the barrier the item's connection sent last";
        return false;
    }
    return true;
}

// A second stub daemon on a bus connection of its own, serving `ring` round and round, its object on
// the bus but its name not taken: each scenario moves the name in its own way. Taken off the bus and
// disconnected when it goes, before the stub it serves from is destroyed.
struct SecondStub {
    QDBusConnection bus;
    ExportingStub stub;
    bool ok = false;  // connected, with the stub's object registered

    SecondStub(wallpaperx::Device& device, const std::vector<const wallpaperx::OffscreenTarget*>& ring)
        : bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("wallpaperx-cache-stub-b"))),
          stub(device, ring) {
        stub.rotate();
        ok = bus.isConnected() && bus.registerVirtualObject(QString::fromUtf8(kPath), &stub);
    }
    ~SecondStub() {
        bus.unregisterObject(QString::fromUtf8(kPath));
        (void)bus.unregisterService(QString::fromUtf8(kService));
        QDBusConnection::disconnectFromBus(QStringLiteral("wallpaperx-cache-stub-b"));
    }
    SecondStub(const SecondStub&) = delete;
    SecondStub& operator=(const SecondStub&) = delete;
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

// Counts the queued calls delivered to one object, split by who posted them. QtDBus delivers a bus
// signal to its receiver as a MetaCall event of its own class, so FrameReady arrives through the
// same door as the item's own queued lambdas; telling them apart by class is what lets the item's
// posts be counted at all.
class MetaCallCounter : public QObject {
public:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::MetaCall) {
            const std::string kind = typeid(*event).name();
            ++(kind.find("QDBus") != std::string::npos ? busDeliveries : ownPosts);
            ++byKind[kind];
        }
        return false;
    }
    int ownPosts = 0;
    int busDeliveries = 0;
    std::map<std::string, int> byKind;
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

// The not-opengl phase. A plain window, not the render control every other phase uses: Qt's render
// control cannot host a software scene graph (measured -- initialize() leaves it without one). The
// entry runs on the offscreen platform, so nothing is shown on a real desktop.
bool runNotOpenGl(ExportingStub& stub, const char* kGoal) {
    QQuickWindow window;
    window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    QQmlComponent component(&engine);
    component.setData(cacheQmlSource().toUtf8(), QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                                      QStringLiteral("/item_import_cache.qml")));
    std::unique_ptr<QObject> root(component.create());
    auto* rootItem = qobject_cast<QQuickItem*>(root.get());
    if (rootItem == nullptr) {
        for (const QQmlError& error : component.errors()) std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
        check(false, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");
        return true;
    }
    auto* item = qobject_cast<QQuickItem*>(root->property("rendererItem").value<QObject*>());
    if (item == nullptr) {
        wallpaperx::testing::harnessFailure("the root object exposed no rendererItem");
        return true;
    }
    rootItem->setParentItem(window.contentItem());
    window.show();
    const bool exposed = pumpUntil([&] { return window.rendererInterface() != nullptr && window.isExposed(); }, 5000);
    if (!exposed || window.rendererInterface()->graphicsApi() != QSGRendererInterface::Software) {
        notVerified(kGoal, std::string("no software scene graph came up under the ") +
                               QGuiApplication::platformName().toStdString() + " platform");
        return true;
    }

    // GOAL: S13.213's non-OpenGL path -- on a desktop whose scene graph is not OpenGL, the item says
    //       why it shows nothing. False if the reason set on that path never reaches QML. (No frame
    //       is claimed and no node exists is asserted beside it, but nothing here could draw one, so
    //       that half has no control of its own.)
    // WHY-THIS-SHAPE: the stub serves real frames so the daemon side has nothing to say: an
    //       unreachable daemon's reason outranks the render side's and would satisfy "some reason
    //       appeared" by itself. Hence equality with the exact words, and a served request
    //       required. The wait is bounded; expiry leaves the reason empty and fails.
    // CONTROL: the `setRenderReason(tr("This desktop is not running on OpenGL..."))` in
    //       WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) deleted -> "FAIL: a
    //       desktop not running on OpenGL is told so", printing "reason [], frame shown: no".
    //       Observed 2026-09-23. Reverted.
    const QString kNotOpenGl =
        QStringLiteral("This desktop is not running on OpenGL, which WallpaperX needs to show a frame.");
    auto reason = [&] { return root->property("publishedReason").toString(); };
    const bool reported = pumpUntil([&] { return stub.served() >= 1 && !reason().isEmpty(); }, 5000);
    pumpFor(100);
    check(stub.served() >= 1,
          "the item fetched a frame from the stub daemon (" + std::to_string(stub.served()) + " served)");
    check(reported && reason() == kNotOpenGl && !claimsFrameShown(item) && itemPaintNode(item) == nullptr,
          "a desktop not running on OpenGL is told so (reason [" + reason().toStdString() +
              "], frame shown: " + (claimsFrameShown(item) ? "yes" : "no") + ")");
    root.reset();
    pumpFor(50);
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
            phase == Phase::Offscreen || phase == Phase::Declare) {
            imageCount = wallpaperx::kTargetDepth;
        } else if (phase == Phase::Pixels || phase == Phase::HandOverModifier || phase == Phase::DeclareMonitor ||
                   phase == Phase::DeclareDaemon || phase == Phase::RestartPending) {
            imageCount = 2;
        } else if (phase == Phase::LostImport || phase == Phase::NewerReply || phase == Phase::PendingMonitor ||
                   phase == Phase::PendingOffscreen) {
            // One past what the node keeps: drawing the image it does not hold evicts another, and
            // there is always an image the startup traffic did not serve.
            imageCount = wallpaperx::kTargetDepth + 1;
        } else if (phase == Phase::AskingMonitor) {
            imageCount = wallpaperx::kTargetDepth;
        } else if (phase == Phase::NoOwner || phase == Phase::NoItem) {
            // Two: the image on the screen and one free. A daemon that never serves a held image then
            // has nothing to serve once a single hold leaks.
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
        // The startup-no-daemon phase's daemon is absent: its object is on the bus, its name is not.
        const bool ownsName = phase != Phase::StartupNoDaemon;
        if (!bus.isConnected() || (ownsName && !bus.registerService(QString::fromUtf8(kService))) ||
            !bus.registerVirtualObject(QString::fromUtf8(kPath), &stub)) {
            notVerified(kGoal, "no session bus this run could own io.wallpaperx.Daemon on: " +
                                   bus.lastError().message().toStdString());
            return true;
        }

        if (phase == Phase::NotOpenGl) return runNotOpenGl(stub, kGoal);

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

        if (phase == Phase::StartupNoDaemon) {
            // GOAL: S15.311 at startup with no daemon -- an item made while nobody owns the daemon's
            //       name says "WallpaperX is not running." and sends no frame request at all: there is
            //       no unique name to send one to, and the well-known name is never used.
            // WHY-THIS-SHAPE: the reason is the same words whether or not a request was sent -- a
            //       request to the unowned well-known name is answered ServiceUnknown, which reads the
            //       same -- so the request count is taken from the bus monitor, which sees every
            //       GetFrame whatever it was addressed to. A third of a second past the reason, so a
            //       request sent beside it would have been routed.
            // CONTROL: in WallpaperXItem::fetchFrame (backend/kde/wallpaperx_item.cpp), the `if
            //       (asked.owner.empty()) {...}` early return deleted, the destination made the well-known
            //       name when the owner is empty, and the one-unanswered guard made `m_unansweredLifetime !=
            //       0 && ...` -> "FAIL: with no daemon at startup the item says it is not running and asks
            //       nothing (reason [WallpaperX is not running.], 1 GetFrame(s) on the bus)" -- the reason
            //       the same, the request count the difference. Via control_batch.sh, 2026-10-06 on the NVIDIA
            //       entry. Reverted. Two guards, because either alone holds this: before the bus has named
            //       an owner the lifetime is 0, which is also the guard's "no call unanswered".
            if (g_monitor == nullptr) {
                notVerified(kGoal, "the private bus would not accept a monitor, so the requests sent cannot be counted");
                return true;
            }
            const QString kNotRunning = QStringLiteral("WallpaperX is not running.");
            const bool said = pumpUntil([&] { return scene.reason() == kNotRunning; }, 5000);
            pumpFor(300);
            std::string why;
            if (!monitorCaughtUp(*g_monitor, why)) {
                wallpaperx::testing::harnessFailure(why);
                return true;
            }
            const int asked = g_monitor->calls("GetFrame");
            check(said && asked == 0, "with no daemon at startup the item says it is not running and asks nothing (reason [" +
                                          scene.reason().toStdString() + "], " + std::to_string(asked) +
                                          " GetFrame(s) on the bus)");
            return true;
        }

        const bool imported = pumpUntil(
            [&scene] {
                scene.renderPass();
                return itemTexture(scene.item) != nullptr;
            },
            10000);
        // GOAL: S15.350 -- every GetFrame the item sends carries the daemon's signature exactly (i
        //       monitor_id, at known_buffer_ids, b on_timeline), so the daemon would read what was meant
        //       rather than refuse the call.
        // WHY-THIS-SHAPE: read from the stub's own count of requests it could not parse, before the
        //       import is judged: a request with the wrong signature is refused, and the item then never
        //       draws, which the import check would report under a cause that names nothing.
        // CONTROL: in WallpaperXItem::fetchFrame (backend/kde/wallpaperx_item.cpp), the monitor id sent
        //       as `static_cast<quint32>(m_monitorId)` -> on item_cache_counts, "FAIL: every GetFrame the
        //       item sent carried the daemon's signature (1 the stub could not read)", then the import
        //       line. Via control_batch.sh, 2026-10-06. Reverted.
        check(stub.malformedRequests() == 0,
              "every GetFrame the item sent carried the daemon's signature (" + std::to_string(stub.malformedRequests()) +
                  " the stub could not read)");
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
                //       entry. Reverted. Re-run 2026-10-04 via control_run.sh: (a) the same line, "16
                //       import(s), 0 drawn through a kept one", and now the lost-import line below too --
                //       declared images arrive without the memory, so with no reuse each is a lost import.
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
                //       exactly the pictures drawn in the drive", printing 26 for 15 under the daemon's
                //       memory rule. Re-run 2026-10-06 on the NVIDIA entry via control_batch.sh. Reverted.
                const quint64 rose = (imports + reused) - (imports0 + reused0);
                check(rose == static_cast<quint64>(servedInDrive) && servedInDrive == static_cast<int>(drive),
                      "the counters rose by exactly the pictures drawn in the drive (" + std::to_string(rose) +
                          " for " + std::to_string(servedInDrive) + " served, " + std::to_string(drive) +
                          " announced)");

                // GOAL: S15.299's other direction -- the ordinary paths raise no lost import: a ring
                //       imported, then drawn round twice through kept imports whose memory the stub
                //       left out, counts none.
                // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp), a
                //       `m_lostImportCount.fetch_add(1, ...)` added to the reuse branch -> "FAIL: the
                //       ordinary import and reuse paths count no lost import (11 counted, 11 replies without
                //       the memory)". Via control_run.sh, 2026-10-04 on the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: the stub must have left the memory out of the reused pictures, so
                //       the reuse being graded is the no-memory one.
                check(scene.lost() == 0 && stub.memoryOmitted() > 0,
                      "the ordinary import and reuse paths count no lost import (" + std::to_string(scene.lost()) +
                          " counted, " + std::to_string(stub.memoryOmitted()) + " replies without the memory)");

                // GOAL: S13.198's NOTIFY -- a QML binding on either counter follows it. Without the
                //       change signal a binding is read once and frozen, and the counters exist for
                //       exactly that reader.
                // CONTROL: `emit importCountsChanged();` deleted from the drawn-picture hop in
                //       WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) -> "FAIL: a
                //       QML binding on importCount and reusedImportCount follows them", printing
                //       "bound 0/0, read 5/11". Re-run 2026-10-06 on the NVIDIA entry via control_batch.sh.
                //       Reverted.
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
                //       picture(s) served". Observed 2026-09-22 on the NVIDIA entry; re-run 2026-10-05 via
                //       control_batch.sh, the same line. Reverted.
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
                //       NVIDIA entry; re-run 2026-10-04 via control_run.sh, the same. Reverted.
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
                //       acquire orders is not observable through this stub. Observed 2026-09-22 on the
                //       NVIDIA entry; the importKeyFor control re-run 2026-10-06 via control_batch.sh under
                //       the daemon's memory rule, the same two lines. Reverted.
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

            case Phase::LateAcquire: {
                // GOAL: S13.199's ordering -- a picture drawn through a kept import is read only after
                //       the producer's acquire (sync_fds) is signalled: what is drawn into the image before
                //       the signal is what the item shows, never the image as it was when the reply came.
                // WHY-THIS-SHAPE: the stub's binary semaphore (lateBinaryNext()) is signalled by a batch
                //       submitted with the reply, behind the drawing (blue) and a gate; exported while
                //       pending, which keeps every step legal Vulkan. The item's texture is copied in the
                //       pass that draws the picture (copyWhileDrawing(), ordered in GL after the item's
                //       wait) and flushed; after a pause long enough for an unordered copy to have run the
                //       gate opens and the copy is read. Reading the item's texture at the end would see
                //       blue whether or not the item waited. One image, so the picture is a kept import.
                //       Relies on the queue not running the drawing ahead of the gated batch before it, which
                //       Vulkan permits but this driver does not do -- the control is what shows it.
                // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) the
                //       sync_fds acquire skipped for a kept import (`handedOver = reused != nullptr ||
                //       FrameImport::acquire(incoming->request, ...)`), via control_run.sh, 2026-10-05 ->
                //       "FAIL: a kept import is read only after its acquire is signalled (read red where blue
                //       was drawn before the signal, over red)". Reverted.
                // COVERAGE-ASSERT: the late acquire must have gone out and been drawn through the kept
                //       import, and the copy taken.
                if (!settleQuiet(scene, stub) || scene.pictures() == 0) {
                    wallpaperx::testing::harnessFailure("the item never settled on its first picture");
                    return true;
                }
                const quint64 reusedBefore = scene.reused();
                // Built now: building it uploads a texture and waits, which the gate would hold for good.
                const std::string bluePath = wallpaperx::testing::writeSolidPng(
                    wallpaperx::testing::testImageDir() + "/item_import_cache_late.png", 16, 16, palette[2]);
                wallpaperx::StaticRenderer blue(device, targets.renderPass(), bluePath, WALLPAPERX_SHADER_DIR);
                const std::string acquireError0 = stub.acquireError();
                stub.lateBinaryNext([&] { targets.render(blue, *ring.front()); });
                announce(bus, stub.nextGeneration());
                GLuint copy = 0;
                const bool drawn = copyWhileDrawing(scene, [&] { return scene.reused() > reusedBefore; }, copy);
                std::string why;
                if (!stub.lateValueSent() && stub.acquireError() != acquireError0) {
                    // The stub's own failure, not the plugin's: no late acquire went out to be waited on.
                    wallpaperx::testing::harnessFailure("the stub could not send the late acquire: " + stub.acquireError());
                    stub.signalLateValue(why);
                    return true;
                }
                if (!drawn || !stub.lateValueSent()) {
                    check(false, "the picture with the late acquire is drawn through the kept import and copied (late "
                                 "acquire sent " + std::to_string(stub.lateValueSent()) + ", drawn and copied " +
                                     std::to_string(drawn) + ")");
                    stub.signalLateValue(why);
                    return true;
                }
                // Long enough for a copy nothing holds back to have run.
                pumpFor(300);
                if (!stub.signalLateValue(why)) {
                    wallpaperx::testing::harnessFailure("the late acquire could not be signalled: " + why);
                    return true;
                }
                const std::vector<unsigned char> pixels = readCopy(scene, copy, why);
                if (pixels.empty()) {
                    wallpaperx::testing::harnessFailure("the copy of the drawn picture could not be read back: " + why);
                    return true;
                }
                const int seen = nearestColour(&pixels[((kHeight / 2) * kWidth + kWidth / 2) * 4], palette);
                const char* names[] = {"red", "green", "blue", "yellow"};
                check(seen == 2, std::string("a kept import is read only after its acquire is signalled (read ") +
                                     (seen < 0 ? "no palette colour" : names[seen]) +
                                     " where blue was drawn before the signal, over red)");
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
                //       import(s) and 2 reuse(s) over the two pictures". Re-run 2026-10-06 on the NVIDIA
                //       entry via control_batch.sh, the same. Reverted.
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
                // Releases naming the newest hand-over served of the one image: the ones that end the claim.
                // Any other is a hand-over a newer one superseded (S15.345), sent as the newer was taken.
                const auto endingReleases = [&stub](quint64 newest) {
                    size_t count = 0;
                    for (const ExportingStub::Release& r : stub.releases()) {
                        if (r.handover >= newest) ++count;
                    }
                    return count;
                };
                const auto newestServed = [&stub] {
                    quint64 newest = 0;
                    for (const ExportingStub::Ask& ask : stub.asks()) newest = std::max(newest, ask.handover);
                    return newest;
                };
                if (phase == Phase::HandOver && endingReleases(newestServed()) != 0) {
                    wallpaperx::testing::harnessFailure("the item had already given its image back before any hand-over "
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
                wallpaperx::testing::GlSemaphoreFaults& faults = wallpaperx::testing::g_glSemaphoreFaults;
                const int failed0 = faults.waitsFailed.load();

                if (phase == Phase::HandOverReset) {
                    // GOAL: S13.203's reset -- a picture drawn ends a run of failed hand-overs, so
                    //       failures separated by one good picture are two runs, not one: five, a
                    //       good picture, then eight more keep the wallpaper silently.
                    // WHY-THIS-SHAPE: the failures are waits the test fails (gl_semaphore_fault.h),
                    //       not descriptors the driver refuses: a refused import now gives up the
                    //       hand-over for the context and draws, so it makes no run at all. The first
                    //       run is armed as five failures and the item's own re-asks consume them;
                    //       its sixth picture, WITH its real acquire, waits and is drawn. The second
                    //       run is armed only once the item is idle, and held at its ninth request, so
                    //       what is read is the state after exactly eight.
                    // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp),
                    //       (a) `m_handOversFailed = 0;` deleted from the drawn-picture branch -> "FAIL:
                    //       eight failures after a good picture still keep it, silently (frame shown:
                    //       no, reason [WallpaperX produced a frame this desktop could not import.])";
                    //       (b) kToleratedHandOverFailures 8 -> 7 reddens the same line; (c) `if
                    //       (importRefused)` made `if (importRefused || !handedOver)`, a failed wait
                    //       giving up the context -> "FAIL: five failed hand-overs are followed by a
                    //       good picture, which is drawn (1 waits failed)". Re-run 2026-10-06 on the NVIDIA
                    //       entry via control_batch.sh, each the same line. Reverted.
                    // COVERAGE-ASSERT: the good picture between the runs is asserted to have been
                    //       drawn, and exactly thirteen waits to have been failed.
                    faults.failWaits.store(5);
                    announce(bus, static_cast<quint64>(base) + 1);
                    const bool firstRun = pumpUntil(
                        [&] {
                            scene.renderPass();
                            return scene.pictures() > pictures0;
                        },
                        10000);
                    check(firstRun && faults.waitsFailed.load() - failed0 == 5,
                          "five failed hand-overs are followed by a good picture, which is drawn (" +
                              std::to_string(faults.waitsFailed.load() - failed0) + " waits failed)");
                    if (!firstRun) {
                        std::fprintf(stderr, "  served %d after the first picture, the item reports [%s]\n",
                                     stub.served() - base, scene.reason().toStdString().c_str());
                        return true;
                    }
                    shown = itemTexture(scene.item);
                    faults.failWaits.store(kToleratedHandOverFailures);
                    stub.holdServeNumber(stub.served() + kToleratedHandOverFailures + 1);
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
                    check(faults.waitsFailed.load() - failed0 == 5 + kToleratedHandOverFailures,
                          "thirteen hand-overs failed, in two runs (" +
                              std::to_string(faults.waitsFailed.load() - failed0) + " waits failed)");
                    check(stillShowing(), "eight failures after a good picture still keep it, silently (frame "
                                          "shown: " +
                                              std::string(claimsFrameShown(scene.item) ? "yes" : "no") + ", reason [" +
                                              scene.reason().toStdString() + "])");
                    faults.failWaits.store(0);
                    stub.refuseFurtherWith(QStringLiteral("io.wallpaperx.Error.NoFrame"));
                    stub.sendHeldReply();
                    pumpFor(100);
                    return true;
                }

                // GOAL: S13.203 -- a failed hand-over is tolerated for a bounded run: up to eight in a
                //       row keep the picture on the screen silently -- a frame shown, no reason, the
                //       node and its texture kept -- and the ninth blanks the item with "WallpaperX
                //       produced a frame this desktop could not import." Both directions: a blip must
                //       not take a wallpaper off the screen, and a hand-over that has stopped working
                //       must not freeze it on one picture while the item reports it is fine.
                // WHY-THIS-SHAPE: each failure is a wait that fails after its semaphore imported
                //       (gl_semaphore_fault.h). A descriptor the driver refuses cannot build the run
                //       any more: the first refused import gives up the hand-over for the context and
                //       draws. The ninth request is held by the stub as it arrives, because the item
                //       asks again by itself after every failure. The eight are hand-overs on the
                //       import kept for this image -- the reuse path -- which is where a failure
                //       leaves a picture worth keeping.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) kToleratedHandOverFailures 8 -> 7
                //       -> "FAIL: eight failed hand-overs in a row keep the picture, silently",
                //       printing "frame shown: no, reason [WallpaperX produced a frame this desktop
                //       could not import.]"; (b) the `m_handOversFailed <= kToleratedHandOverFailures
                //       &&` term deleted from `stillDrawing` -> "FAIL: the ninth failure takes the
                //       picture off and says why (frame shown: yes, reason [])"; (c) `if
                //       (importRefused)` made `if (importRefused || !handedOver)` -> "FAIL: the item
                //       asks again after each failed hand-over, up to the held ninth request", the
                //       first failure drawn unwaited. Also measured, and unreachable rather than
                //       covered: that the run's count lives on the item and not on the node (GOALS.md,
                //       S13.203). (a)-(c) re-run 2026-10-06 on the NVIDIA entry via control_batch.sh, the
                //       same lines. Reverted.
                // COVERAGE-ASSERT: exactly eight waits are asserted to have been failed before the
                //       held ninth, and the counters not to have moved.
                faults.failWaits.store(kToleratedHandOverFailures);
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
                check(faults.waitsFailed.load() - failed0 == kToleratedHandOverFailures,
                      "eight hand-overs failed before the held ninth (" +
                          std::to_string(faults.waitsFailed.load() - failed0) + " waits failed)");
                check(stillShowing(), "eight failed hand-overs in a row keep the picture, silently (frame shown: " +
                                          std::string(claimsFrameShown(scene.item) ? "yes" : "no") + ", reason [" +
                                          scene.reason().toStdString() + "])");
                // GOAL: S13.201 while the picture stays -- no failed hand-over gives back the claim on the
                //       image still on the screen: no release names the newest hand-over the item has read.
                //       Releases of hand-overs a newer one superseded (S15.345) are allowed.
                // WHY-THIS-SHAPE: read against the newest number among the replies delivered -- every ask
                //       but the last, which is the held ninth -- since a release naming that number ends the
                //       claim and the daemon would draw into the image on screen. The release count after the
                //       picture goes (below) cannot see this: a release naming the eighth is below the ninth.
                // CONTROL: in WallpaperXItem::updatePaintNode's failed hand-over branch
                //       (backend/kde/wallpaperx_item.cpp), `if (stillDrawing)
                //       releaseClaimWhenReadsFinish(incoming->claim);` added, a failed wait handing back its own
                //       claim while the node is kept -> "FAIL: no failed hand-over gives back the claim on the
                //       picture still shown (1 release(s) naming 9 or later)".
                const auto newestDelivered = [&stub] {
                    quint64 newest = 0;
                    const std::vector<ExportingStub::Ask>& asks = stub.asks();
                    for (size_t i = 0; i + 1 < asks.size(); ++i) newest = std::max(newest, asks[i].handover);
                    return newest;
                };
                const quint64 liveNewest = newestDelivered();
                check(liveNewest != 0 && endingReleases(liveNewest) == 0,
                      "no failed hand-over gives back the claim on the picture still shown (" +
                          std::to_string(endingReleases(liveNewest)) + " release(s) naming " +
                          std::to_string(liveNewest) + " or later)");

                // GOAL: S13.198's other half -- a picture that failed counts in neither counter:
                //       their sum is what reached the screen.
                // CONTROL: the drawn-picture count moved in WallpaperXItem::updatePaintNode
                //       (backend/kde/wallpaperx_item.cpp) to just after `reused` is chosen, counting
                //       attempts -> "FAIL: failed pictures count in neither counter", printing "8
                //       more". Re-run 2026-10-06 on the NVIDIA entry via control_batch.sh. Reverted.
                check(scene.pictures() == pictures0, "failed pictures count in neither counter (" +
                                                         std::to_string(scene.pictures() - pictures0) + " more)");

                // The ninth, and the one held after it so the item's next re-ask does not run on. The
                // ninth's number is the newest hand-over the item will have read.
                faults.failWaits.store(1);
                stub.holdServeNumber(base + kToleratedHandOverFailures + 2);
                const quint64 ninthNumber = stub.asks().back().handover;
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
                //       it was drawing is given back to the daemon, naming the newest hand-over the item
                //       read of it, when the failure takes it off the screen, and once. Before that only
                //       hand-overs a newer one superseded go back (S15.345); none ends the claim.
                // WHY-THIS-SHAPE: one image throughout, so the whole run is one claim in the ledger and
                //       exactly one release can name the ninth reply's number. The partial releases are
                //       allowed, and bounded: every one names a number below it.
                // CONTROL: in ~FrameNode (backend/kde/wallpaperx_item.cpp), `releaseClaimWhenReadsFinish
                //       (m_claim);` made `(void)m_claim;` -> "FAIL: the picture the dropped node was drawing
                //       is given back once it goes, naming the newest hand-over read (0 release(s) naming 10, 9
                //       in all)". Via control_batch.sh, 2026-10-06. Reverted.
                const bool givenBack = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return endingReleases(ninthNumber) != 0;
                    },
                    3000);
                const size_t ending = endingReleases(ninthNumber);
                const size_t naming = [&] {
                    size_t count = 0;
                    for (const ExportingStub::Release& r : stub.releases()) count += r.handover == ninthNumber ? 1 : 0;
                    return count;
                }();
                check(givenBack && ending == 1 && naming == 1,
                      "the picture the dropped node was drawing is given back once it goes, naming the newest "
                      "hand-over read (" +
                          std::to_string(naming) + " release(s) naming " + std::to_string(ninthNumber) + ", " +
                          std::to_string(stub.releases().size()) + " in all)");
                faults.failWaits.store(0);
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
                //       check could not fail. Each failure is a failed wait (gl_semaphore_fault.h): a
                //       descriptor the driver refuses now gives up the hand-over and draws.
                // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp),
                //       `stopped == Stopped::Layout &&` made `stopped != Stopped::Nothing &&` -> "FAIL:
                //       no failed hand-over withdraws the layout it arrived in (1 further report(s), the
                //       served layout dropped)". And `if (importRefused)` made `if (importRefused ||
                //       !handedOver)` -> "FAIL: four hand-overs failed, fresh and kept", no run built.
                //       Re-run 2026-10-06 on the NVIDIA entry via control_batch.sh with the dma_buf export
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
                wallpaperx::testing::GlSemaphoreFaults& faults = wallpaperx::testing::g_glSemaphoreFaults;
                const int failed0 = faults.waitsFailed.load();
                faults.failWaits.store(4);
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
                check(faults.waitsFailed.load() - failed0 == 4, "four hand-overs failed before the held fifth (" +
                                                                    std::to_string(faults.waitsFailed.load() - failed0) +
                                                                    " waits failed)");
                faults.failWaits.store(0);
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
                    // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) the `forgetGenerations(
                    //       Undrawn::Keep);` in the DaemonWatch::replaced handler of
                    //       WallpaperXItem::connectToDaemon deleted -> "FAIL: a departed daemon's imports are
                    //       freed without a picture (0 of 4, the drawn one kept)"; (b) `update();` deleted
                    //       from WallpaperXItem::forgetGenerations -> the same line, nothing asking for the
                    //       pass; (c) `current->forgetAllButDrawn()` deleted from the forgetStaleImports
                    //       lambda in updatePaintNode -> the same. Re-run 2026-10-05 via control_batch.sh on
                    //       the NVIDIA entry. Reverted.
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
                    //       the `if (m_fetch != Fetch::Asking && m_nodeRequest == NodeRequest::None)
                    //       {...ForgetStaleImports...}` deleted -> "FAIL: a left monitor's imports are freed
                    //       without a picture (0 of 4, the drawn one kept)". Re-run 2026-10-05 via
                    //       control_batch.sh on the NVIDIA entry. Reverted.
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
                //       NVIDIA entry; this entry's two lines re-run 2026-10-04 via control_run.sh. Reverted.
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

            case Phase::Publish: {
                // The item is showing its first picture cleanly by now; everything below is measured
                // from that settled state.
                if (!claimsFrameShown(scene.item) || !scene.reason().isEmpty()) {
                    wallpaperx::testing::harnessFailure("the item was not showing its first picture cleanly, so a "
                                                        "picture that changes nothing could not be driven");
                    return true;
                }
                MetaCallCounter counter;
                scene.item->installEventFilter(&counter);
                const int reasonSignals0 = scene.root->property("reasonSignals").toInt();
                const int shownSignals0 = scene.root->property("frameShownSignals").toInt();
                const quint64 pictures0 = scene.pictures();
                constexpr int kDrive = 12;
                for (int step = 0; step < kDrive; ++step) {
                    if (!drawNext(scene, stub, bus)) {
                        check(false, "the item draws each picture it is served");
                        scene.item->removeEventFilter(&counter);
                        return true;
                    }
                }
                const int drawn = static_cast<int>(scene.pictures() - pictures0);
                // Every post a pass makes is queued by the time drawNext returns; the pump lets them
                // all land, and the extra 200 ms lets a surplus land too rather than be missed.
                pumpUntil([&] { return counter.ownPosts >= drawn; }, 2000);
                pumpFor(200);
                scene.item->removeEventFilter(&counter);
                std::string kinds;
                for (const auto& [kind, count] : counter.byKind) kinds += " " + std::to_string(count) + " x " + kind;

                // GOAL: S13.212 -- a drawn picture that changes nothing emits nothing: with the item
                //       already showing a picture and no reason standing, a dozen more pictures fire
                //       neither unavailableReasonChanged nor frameShownChanged.
                // WHY-THIS-SHAPE: counted by a QML handler on each signal, the reader the signals are
                //       for. The two properties' VALUES alone cannot fail here: they are the same
                //       before and after, whether or not a redundant emit fired in between.
                // CONTROL: in WallpaperXItem::publishReason (backend/kde/wallpaperx_item.cpp), `if
                //       (m_unavailableReason == settled) return;` deleted -> "FAIL: a picture that
                //       changes nothing emits no reason or frameShown change", printing "24 reason,
                //       0 frameShown" -- two per picture, the fetch reply's publish and the draw's. And
                //       the `if (m_frameShown != shown)` guard made unconditional -> the same line,
                //       "0 reason, 24 frameShown". Re-run 2026-10-06 on the NVIDIA entry via
                //       control_batch.sh. Reverted.
                const int reasonSignals = scene.root->property("reasonSignals").toInt() - reasonSignals0;
                const int shownSignals = scene.root->property("frameShownSignals").toInt() - shownSignals0;
                check(reasonSignals == 0 && shownSignals == 0,
                      "a picture that changes nothing emits no reason or frameShown change (" +
                          std::to_string(reasonSignals) + " reason, " + std::to_string(shownSignals) +
                          " frameShown, over " + std::to_string(drawn) + " picture(s))");

                // GOAL: S13.212's cost half -- a drawn picture posts ONE queued call to the item's
                //       thread, the one that announces the counts and publishes; not a second hop for
                //       a reason that has not changed.
                // WHY-THIS-SHAPE: counted at the item's own event queue with a filter, because the
                //       cost is the posting, not any visible effect -- a redundant publish of an
                //       unchanged reason changes nothing a reader can see. FrameReady reaches the
                //       item as a MetaCall event too, of QtDBus's own class, and is counted apart.
                // CONTROL: `setRenderReason(QString());` restored after the per-picture
                //       invokeMethod in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp)
                //       -- the shape before this change -> "FAIL: each picture drawn posts one call to
                //       the item's thread", printing "24 post(s) for 12 picture(s)". Observed
                //       2026-09-23 on the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: the bus deliveries must number the pictures announced, so a QtDBus
                //       whose event class no longer matches cannot silently move FrameReady into the
                //       item's own count, or out of it. Its control is in this file, standing in for
                //       such a Qt: the classifier's "QDBus" made "QDBusRenamed" -> this check red with
                //       "0 for 12", and the post count red beside it with 24. Reverted.
                check(drawn == kDrive, "the drive drew every picture it announced (" + std::to_string(drawn) + " of " +
                                           std::to_string(kDrive) + ")");
                check(counter.busDeliveries == kDrive,
                      "every FrameReady reached the item as a bus delivery, told apart from its own posts (" +
                          std::to_string(counter.busDeliveries) + " for " + std::to_string(kDrive) + ";" + kinds + ")");
                check(counter.ownPosts == drawn, "each picture drawn posts one call to the item's thread (" +
                                                     std::to_string(counter.ownPosts) + " post(s) for " +
                                                     std::to_string(drawn) + " picture(s);" + kinds + ")");
                return true;
            }

            case Phase::DescriptorBackoff: {
                // GOAL: S15.273 and S15.361 -- refused as TooManyDescriptors while showing a frame, the item keeps
                //       that frame and shows no reason; it makes no fetch during its back-off however
                //       many frames are announced, then exactly one for the refused frame and what was
                //       announced; a back-off with nothing announced ends by asking again for the refused
                //       frame, which is drawn; and stderr gets one line per run of refusals, a frame served
                //       ending the run.
                // WHY-THIS-SHAPE: gaps are read on the stub's clock between the refusal being sent and
                //       the next request (Ask::arrivedMs), so a slow test thread cannot shorten them. The
                //       quiet window is 1800 ms, inside the first back-off (2 s) and the second of a run
                //       (4 s) even for a coarse Qt timer (which may fire up to 5% early); a fetch on each
                //       announcement would land within milliseconds of it.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) in WallpaperXItem::fetchFrame's reply
                //       handler, `m_fetch = backOff ? Fetch::BackingOff : Fetch::Idle;` made `m_fetch =
                //       Fetch::Idle;` -> "FAIL: a refused item makes no fetch during its back-off, across three
                //       announcements (3 fetch(es) within 1800 ms)" (re-run via control_batch.sh 2026-10-07, after
                //       `backOff` stopped requiring the item on screen). (b) the `exchange` guard on
                //       g_descriptorRefusalReported in that handler's `if (backOff)` block made unconditional ->
                //       "FAIL: two refusals in one run write one line to stderr (2 line(s))" (re-run via
                //       control_batch.sh 2026-10-07, after that block's doubledWait call changed). (c) the reply
                //       handler's `else if (requestedMonitor == m_monitorId) queueFetch(generation);` deleted
                //       -> "FAIL: a back-off with nothing announced, while a frame is shown, ends by asking again
                //       for the refused frame, which is drawn (0 fetch(es) after it ...)".
                // COVERAGE-ASSERT: each refusal must be seen arriving at the stub, and the served fetch
                //       must be drawn, before the next step is graded; otherwise the phase stops there. No
                //       request may have been malformed, or it would have gone untimed.
                constexpr qint64 kQuietMs = 1800;
                const QString kRefusal = QStringLiteral("io.wallpaperx.Daemon.Error.TooManyDescriptors");
                const char* kLine = "wallpaperx: the daemon is refusing this process frames";
                wallpaperx::testing::StreamCapture capture(STDERR_FILENO);
                if (!capture.ok()) {
                    wallpaperx::testing::harnessFailure("stderr could not be captured");
                    return true;
                }
                const auto lines = [&] { return capture.count(kLine); };
                const size_t base = stub.asks().size();
                const auto fetchesSince = [&](size_t index) { return stub.asks().size() - index; };
                // Pumps until the stub has seen request number `n` of this phase; its time, or -1.
                const auto awaitFetch = [&](size_t n, int limitMs) -> qint64 {
                    const bool arrived = pumpUntil(
                        [&] {
                            scene.renderPass();
                            return fetchesSince(base) >= n;
                        },
                        limitMs);
                    return arrived ? stub.asks()[base + n - 1].arrivedMs : -1;
                };
                const auto passUntilStubAt = [&](qint64 ms) {
                    (void)pumpUntil(
                        [&] {
                            scene.renderPass();
                            return stub.nowMs() >= ms;
                        },
                        static_cast<int>(ms - stub.nowMs()) + 2000);
                };
                const auto fetchesBetween = [&](qint64 from, qint64 to) {
                    int n = 0;
                    for (size_t i = base; i < stub.asks().size(); ++i) {
                        if (stub.asks()[i].arrivedMs > from && stub.asks()[i].arrivedMs < to) ++n;
                    }
                    return n;
                };

                stub.refuseFurtherWith(kRefusal);
                announce(bus, stub.nextGeneration());
                const qint64 firstRefused = awaitFetch(1, 3000);
                if (firstRefused < 0) {
                    check(false, "an announced frame is asked for");
                    return true;
                }
                for (int k = 1; k <= 3; ++k) {
                    passUntilStubAt(firstRefused + 150 * k);
                    announce(bus, stub.nextGeneration() + static_cast<quint64>(k));
                }
                passUntilStubAt(firstRefused + kQuietMs);
                const int early = fetchesBetween(firstRefused, firstRefused + kQuietMs);
                check(early == 0, "a refused item makes no fetch during its back-off, across three announcements (" +
                                      std::to_string(early) + " fetch(es) within " + std::to_string(kQuietMs) + " ms)");
                check(claimsFrameShown(scene.item) && itemTexture(scene.item) != nullptr && scene.reason().isEmpty(),
                      std::string("and keeps the frame it shows, with no reason (frame shown: ") +
                          (claimsFrameShown(scene.item) ? "yes" : "no") + ", reason [" + scene.reason().toStdString() +
                          "])");

                // The back-off ends with one fetch, refused again: the same run, so the next back-off is 4 s.
                const qint64 secondRefused = awaitFetch(2, 4000);
                if (secondRefused < 0) {
                    check(false, "after its back-off it fetches what was announced meanwhile");
                    return true;
                }
                // Served from here, and one more frame announced, so the next back-off ends in a fetch.
                stub.refuseFurtherWith(QString());
                const quint64 drawnBefore = scene.pictures();
                announce(bus, stub.nextGeneration() + 4);
                passUntilStubAt(secondRefused + kQuietMs);
                const int early2 = fetchesBetween(secondRefused, secondRefused + kQuietMs);
                const qint64 served = awaitFetch(3, 5000);
                const bool drawn = served >= 0 && pumpUntil(
                                                      [&] {
                                                          scene.renderPass();
                                                          return scene.pictures() > drawnBefore;
                                                      },
                                                      5000);
                passUntilStubAt((served >= 0 ? served : stub.nowMs()) + 1500);
                check(early2 == 0 && served >= 0 && fetchesSince(base) == 3,
                      "each back-off ends in exactly one fetch for what was announced during it (" +
                          std::to_string(early2) + " early, " + std::to_string(fetchesSince(base)) +
                          " fetch(es) in all where 3 were due)");
                if (!drawn) {
                    check(false, "the frame served after the back-off is drawn");
                    return true;
                }
                const int linesFirstRun = lines();
                check(linesFirstRun == 1,
                      "two refusals in one run write one line to stderr (" + std::to_string(linesFirstRun) + " line(s))");

                // A frame was served, so the next refusal is a new run with a 2 s back-off; nothing is
                // announced during it, and the stub serves again once the refusal has gone out.
                stub.refuseFurtherWith(kRefusal);
                announce(bus, stub.nextGeneration());
                const qint64 thirdRefused = awaitFetch(4, 3000);
                if (thirdRefused < 0) {
                    check(false, "an announced frame is asked for after a frame was drawn");
                    return true;
                }
                stub.refuseFurtherWith(QString());
                const quint64 drawnBeforeReask = scene.pictures();
                passUntilStubAt(thirdRefused + kQuietMs);
                const int early3 = fetchesBetween(thirdRefused, thirdRefused + kQuietMs);
                const qint64 reasked = awaitFetch(5, 3000);
                const bool redrawn = reasked >= 0 && pumpUntil(
                                                         [&] {
                                                             scene.renderPass();
                                                             return scene.pictures() > drawnBeforeReask;
                                                         },
                                                         5000);
                passUntilStubAt((reasked >= 0 ? reasked : stub.nowMs()) + 1500);
                const int linesSecondRun = lines();
                check(linesSecondRun == 2, "a refusal after a frame was served starts a new run with its own line (" +
                                               std::to_string(linesSecondRun) + " line(s) in all)");
                const int after = static_cast<int>(fetchesSince(base)) - 4;
                check(early3 == 0 && after == 1 && redrawn && stub.malformedRequests() == 0,
                      "a back-off with nothing announced, while a frame is shown, ends by asking again for the refused "
                      "frame, which is drawn (" +
                          std::to_string(after) + " fetch(es) after it, " + std::to_string(early3) +
                          " during the back-off, " + (redrawn ? "drawn" : "not drawn") + ")");
                return true;
            }

            case Phase::Declare: {
                // GOAL: S15.296 -- each fetch names exactly the images the node holds an import of and
                //       can draw again, and a reply that then leaves their memory out is drawn through
                //       the kept import: no import, no lost picture.
                // WHY-THIS-SHAPE: the whole ring is drawn first, so the node provably holds every image
                //       and the expected list is the ring's ids, known to the test independently of the
                //       item. The stub applies the daemon's rule, so the drive also grades that a reply
                //       without the memory is drawn.
                // CONTROL: in WallpaperXItem::importsToDeclare (backend/kde/wallpaperx_item.cpp), an
                //       unconditional `return declared;` at the top -> "FAIL: every fetch names exactly the
                //       ring's images (0 of 5 did)" and "FAIL: and each reply, its memory left out, is
                //       drawn through the kept import (0 without the memory, 0 import(s), 5 reuse(s), 0
                //       lost)". Via control_run.sh, 2026-10-04 on the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: the number of fetches compared must equal the pictures driven.
                TextureWatch watch(&scene.window);
                if (!importWholeRing(scene, stub, bus, watch, imageCount)) return true;
                std::set<quint64> ringIds;
                for (const wallpaperx::OffscreenTarget* target : ring) ringIds.insert(target->bufferId);
                const size_t asks0 = stub.asks().size();
                const int omitted0 = stub.memoryOmitted();
                const quint64 imports0 = scene.imports();
                const quint64 reused0 = scene.reused();
                const size_t drive = imageCount;
                for (size_t step = 0; step < drive; ++step) {
                    if (!drawNext(scene, stub, bus)) {
                        check(false, "the item draws each picture of the ring it is served");
                        return true;
                    }
                }
                int exact = 0;
                const size_t compared = stub.asks().size() - asks0;
                for (size_t i = asks0; i < stub.asks().size(); ++i) {
                    const std::vector<quint64>& declared = stub.asks()[i].declared;
                    if (declared.size() == ringIds.size() &&
                        std::set<quint64>(declared.begin(), declared.end()) == ringIds &&
                        stub.asks()[i].monitorId == kFirstMonitor) {
                        ++exact;
                    }
                }
                check(compared == drive && exact == static_cast<int>(compared),
                      "every fetch names exactly the ring's images (" + std::to_string(exact) + " of " +
                          std::to_string(compared) + " did, " + std::to_string(drive) + " pictures driven)");
                const int omitted = stub.memoryOmitted() - omitted0;
                check(omitted == static_cast<int>(drive) && scene.imports() == imports0 &&
                          scene.reused() == reused0 + drive && scene.lost() == 0,
                      "and each reply, its memory left out, is drawn through the kept import (" +
                          std::to_string(omitted) + " without the memory, " +
                          std::to_string(scene.imports() - imports0) + " import(s), " +
                          std::to_string(scene.reused() - reused0) + " reuse(s), " + std::to_string(scene.lost()) +
                          " lost)");
                return true;
            }

            case Phase::DeclareRefused: {
                // GOAL: S15.297 -- once the driver has refused the producer's acquire, the item names
                //       nothing: without the acquire an import is never drawn again, so naming one would
                //       only have the daemon leave out memory the picture then needs.
                // WHY-THIS-SHAPE: the refusal is staged with a descriptor that is no semaphore (the
                //       stub's Unusable acquire), the one way to make the driver refuse from outside the
                //       item; it marks the whole process, and is never cleared, so nothing may run after
                //       this in the same process. The fetch before it must name the image, or nothing is
                //       shown to stop.
                // CONTROL: in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp), the
                //       `&& !g_semaphoresRefused.load()` term dropped from publishKnownImports -> "FAIL: after
                //       the acquire was refused, no fetch names an image (3 of 3 named one)". Via
                //       control_run.sh, 2026-10-10 on the NVIDIA entry. Reverted.
                const size_t asks0 = stub.asks().size();
                stub.sendAcquireFrom(stub.served() + 1, ExportingStub::Acquire::Unusable);
                stub.sendAcquireFrom(stub.served() + 2, ExportingStub::Acquire::Real);
                if (!drawNext(scene, stub, bus)) {
                    check(false, "the picture whose acquire the driver refuses is still drawn");
                    return true;
                }
                const bool namedBefore = stub.asks().size() > asks0 && !stub.asks()[asks0].declared.empty();
                if (!namedBefore || stub.unusableAcquiresSent() != 1) {
                    wallpaperx::testing::harnessFailure("the fetch before the refusal named no image, or no unusable "
                                                        "acquire was sent, so there was nothing to stop naming");
                    return true;
                }
                const size_t asks1 = stub.asks().size();
                for (int step = 0; step < 3; ++step) {
                    if (!drawNext(scene, stub, bus)) {
                        check(false, "pictures after the refusal are drawn");
                        return true;
                    }
                }
                int named = 0;
                for (size_t i = asks1; i < stub.asks().size(); ++i) named += stub.asks()[i].declared.empty() ? 0 : 1;
                check(stub.asks().size() - asks1 == 3 && named == 0,
                      "after the acquire was refused, no fetch names an image (" + std::to_string(named) +
                          " of " + std::to_string(stub.asks().size() - asks1) + " named one)");
                return true;
            }

            case Phase::DeclareMonitor: {
                // GOAL: S15.296's monitor half -- a fetch for the monitor the item has just moved to
                //       names nothing of the monitor it left, though the node still holds those imports.
                // WHY-THIS-SHAPE: the new monitor's reply is held, so the request is read as it arrived
                //       and nothing a later pass did can have emptied the list first.
                // CONTROL: the `&& known.monitorId == m_monitorId` term deleted from
                //       WallpaperXItem::importsToDeclare (backend/kde/wallpaperx_item.cpp) -> "FAIL: the
                //       first fetch for another monitor names nothing of the old one (named 2)". Via
                //       control_run.sh, 2026-10-04 on the NVIDIA entry. Reverted.
                if (!drawNext(scene, stub, bus) || !drawNext(scene, stub, bus)) {
                    check(false, "the item draws both images");
                    return true;
                }
                const size_t asks0 = stub.asks().size();
                if (stub.asks().back().declared.empty()) {
                    wallpaperx::testing::harnessFailure("the last fetch on the first monitor named nothing, so a "
                                                        "fetch for the second naming nothing would grade nothing");
                    return true;
                }
                stub.holdNextReply();
                scene.item->setProperty("monitorId", kSecondMonitor);
                if (!pumpUntil([&stub] { return stub.replyIsHeld(); }, 3000) || stub.asks().size() <= asks0) {
                    wallpaperx::testing::harnessFailure("the item did not ask for the new monitor's picture");
                    return true;
                }
                const ExportingStub::Ask& ask = stub.asks()[asks0];
                check(ask.monitorId == kSecondMonitor && ask.declared.empty(),
                      "the first fetch for another monitor names nothing of the old one (named " +
                          std::to_string(ask.declared.size()) + ")");
                stub.refuseFurtherWith(QStringLiteral("io.wallpaperx.Error.NoFrame"));
                stub.sendHeldReply();
                pumpFor(100);
                return true;
            }

            case Phase::DeclareDaemon:
            case Phase::UniqueName: {
                if (!drawNext(scene, stub, bus) || !drawNext(scene, stub, bus)) {
                    check(false, "the item draws its pictures from the first daemon");
                    return true;
                }
                if (stub.asks().back().declared.empty()) {
                    wallpaperx::testing::harnessFailure("the last fetch to the first daemon named nothing, so the "
                                                        "second's naming nothing would grade nothing");
                    return true;
                }
                SecondStub second(device, ring);
                QDBusConnection& busB = second.bus;
                ExportingStub& stubB = second.stub;
                if (!second.ok) {
                    wallpaperx::testing::harnessFailure("the second stub daemon could not be put on the bus");
                    return true;
                }
                // The name moves from a thread of its own, so this thread runs no event loop meanwhile
                // and the item cannot have heard of the move when the next line asks it to fetch.
                bool moved = false;
                std::thread([&] {
                    moved = bus.unregisterService(QString::fromUtf8(kService)) &&
                            busB.registerService(QString::fromUtf8(kService));
                }).join();
                if (!moved) {
                    wallpaperx::testing::harnessFailure("the daemon's name could not be moved to the second stub");
                    return true;
                }
                const size_t asksA = stub.asks().size();

                if (phase == Phase::UniqueName) {
                    // GOAL: S15.298 -- a fetch goes to the unique name of the daemon the item last saw
                    //       own the well-known one, never to the well-known name itself: an image id
                    //       means something only to the daemon that issued it.
                    // WHY-THIS-SHAPE: the well-known name has already moved to a second stub, and the
                    //       fetch is made synchronously (a monitor change fetches at once) before this
                    //       thread runs an event loop, so the item still knows only the first daemon.
                    //       Addressed to the unique name it reaches the first; to the well-known name,
                    //       the second. Nothing else tells the two addressings apart from outside.
                    // CONTROL: in WallpaperXItem::fetchFrame (backend/kde/wallpaperx_item.cpp), the
                    //       request's destination `QString::fromStdString(asked.owner)` made the well-known
                    //       name -> "FAIL: the fetch made before the item heard of the move reaches the
                    //       daemon it knows (first 0, second 2)". Re-run 2026-10-05 via control_batch.sh on
                    //       the NVIDIA entry. Reverted.
                    // Read as "the first daemon got it": once the item hears of the move it asks the
                    // second on its own, so the second's count says nothing about this request.
                    scene.item->setProperty("monitorId", kSecondMonitor);
                    (void)pumpUntil([&] { return stub.asks().size() > asksA || !stubB.asks().empty(); }, 3000);
                    pumpFor(200);
                    const size_t toA = stub.asks().size() - asksA;
                    check(toA == 1 && stub.asks()[asksA].monitorId == kSecondMonitor,
                          "the fetch made before the item heard of the move reaches the daemon it knows (first " +
                              std::to_string(toA) + ", second " + std::to_string(stubB.requests()) + ")");
                    return true;
                }

                // GOAL: S15.296's daemon half -- the first fetch to a new daemon names nothing of the old
                //       one's images, though the node still holds the one it is drawing.
                // CONTROL: the `known.daemon == daemon.lifetime &&` term deleted from
                //       WallpaperXItem::importsToDeclare (backend/kde/wallpaperx_item.cpp) -> "FAIL: the
                //       first fetch to a new daemon names nothing of the old one's images (named 2)". Re-run
                //       2026-10-05 via control_batch.sh on the NVIDIA entry. Reverted.
                const bool asked = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return !stubB.asks().empty();
                    },
                    5000);
                if (!asked) {
                    check(false, "the item asks the new daemon for a picture");
                    return true;
                }
                check(stubB.asks().front().declared.empty(),
                      "the first fetch to a new daemon names nothing of the old one's images (named " +
                          std::to_string(stubB.asks().front().declared.size()) + ")");
                return true;
            }

            case Phase::LostImport: {
                // GOAL: S15.299 and S15.300 -- a reply that leaves out the memory of an image the
                //       request named, after the node has dropped that import, is not drawn and is no
                //       failure: lostImportCount rises by one and its QML binding follows, exactly one
                //       fetch follows naming nothing, and it is drawn. The first eight are each logged,
                //       then one line says no more will be.
                // WHY-THIS-SHAPE: the race is staged, not imitated. With one image more than the node
                //       keeps, two pictures are announced at once and their replies held: the first,
                //       for the image the node lacks, is taken; the item at once asks for the second,
                //       naming its imports as the last pass left them; only then does a pass draw the
                //       first, which evicts the least recent import -- the image the second reply is
                //       for, its memory left out by the daemon's own rule. Nine rounds, for the cap.
                //       Which image is least recent is tracked here from what was drawn.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) `m_lostImportCount.fetch_add` deleted
                //       -> "FAIL: each lost picture is counted once and the binding follows (0 counted,
                //       bound 0, 9 staged)"; (b) `m_declareNoImports = true;` deleted -> "FAIL: each is
                //       followed by exactly one fetch, naming nothing, which is drawn (0 of 9 ...)"; (c)
                //       reportImportLost's `n > kMaxReports` made `n > kMaxReports + 1` -> "FAIL: eight lost
                //       pictures are logged, then one line says no more will be (9 line(s), cap line 1)".
                //       Each re-run 2026-10-06 on the NVIDIA entry via control_batch.sh. Reverted. The binding
                //       clause has no control of its own: the re-fetched picture's draw emits the same
                //       importCountsChanged, so deleting the lost path's emit leaves the binding current.
                // COVERAGE-ASSERT: each round must be seen staged -- the second reply for the evicted
                //       image, its memory left out -- or the phase stops there.
                wallpaperx::testing::StreamCapture capture(STDERR_FILENO);
                if (!capture.ok()) {
                    wallpaperx::testing::harnessFailure("stderr could not be captured");
                    return true;
                }
                const auto linesWith = [&](const char* needle) { return capture.count(needle); };

                // The node keeps the five most recently drawn: draw all six in order, so it holds the
                // last five and lacks the first.
                std::vector<const wallpaperx::OffscreenTarget*> recency;  // most recent first
                const auto drew = [&](const wallpaperx::OffscreenTarget* target) {
                    std::erase(recency, target);
                    recency.insert(recency.begin(), target);
                    if (recency.size() > wallpaperx::kTargetDepth) recency.pop_back();
                };
                for (const wallpaperx::OffscreenTarget* target : ring) {
                    stub.serveNext(target);
                    if (!drawNext(scene, stub, bus)) {
                        check(false, "the item draws each image of the ring once");
                        return true;
                    }
                    drew(target);
                }
                const quint64 lost0 = scene.lost();
                const auto held = [&](size_t count) { return pumpUntil([&] { return stub.heldReplies() >= count; }, 3000); };
                constexpr int kRounds = 9;
                int refetchedClean = 0;
                for (int round = 0; round < kRounds; ++round) {
                    const wallpaperx::OffscreenTarget* missing = nullptr;
                    for (const wallpaperx::OffscreenTarget* target : ring) {
                        if (std::find(recency.begin(), recency.end(), target) == recency.end()) missing = target;
                    }
                    const wallpaperx::OffscreenTarget* evicted = recency.back();
                    const quint64 pictures0 = scene.pictures();
                    const size_t asks0 = stub.asks().size();
                    stub.holdEveryReply(true);
                    stub.serveNext(missing);
                    const quint64 first = stub.nextGeneration();
                    announce(bus, first);
                    announce(bus, first + 1);
                    if (!held(1)) {
                        wallpaperx::testing::harnessFailure("round " + std::to_string(round) + ": the first reply never "
                                                            "arrived to be held");
                        return true;
                    }
                    stub.serveNext(evicted);
                    if (!stub.sendOldestHeld() || !held(1)) {
                        wallpaperx::testing::harnessFailure("round " + std::to_string(round) + ": the item did not ask "
                                                            "for the second picture on taking the first");
                        return true;
                    }
                    const ExportingStub::Ask second = stub.asks().back();
                    if (second.served != evicted->bufferId || second.memorySent) {
                        wallpaperx::testing::harnessFailure("round " + std::to_string(round) + ": the second reply was not "
                                                            "for the image about to be evicted with its memory left out");
                        return true;
                    }
                    for (int pass = 0; pass < 3; ++pass) scene.renderPass();
                    drew(missing);
                    stub.holdEveryReply(false);
                    stub.serveNext(evicted);
                    stub.sendOldestHeld();
                    const bool redrawn = pumpUntil(
                        [&] {
                            scene.renderPass();
                            return scene.pictures() >= pictures0 + 2;
                        },
                        3000);
                    pumpFor(30);
                    drew(evicted);
                    // The first picture, the lost one, and the one fetch after it.
                    const bool oneRefetch = stub.asks().size() == asks0 + 3 && stub.asks().back().declared.empty() &&
                                            stub.asks().back().memorySent;
                    if (redrawn && oneRefetch && scene.pictures() == pictures0 + 2) ++refetchedClean;
                }
                pumpFor(50);
                const quint64 counted = scene.lost() - lost0;
                const quint64 bound = scene.root->property("boundLost").toULongLong();
                check(counted == kRounds && bound == scene.lost(),
                      "each lost picture is counted once and the binding follows (" + std::to_string(counted) +
                          " counted, bound " + std::to_string(bound) + ", " + std::to_string(kRounds) + " staged)");
                check(refetchedClean == kRounds,
                      "each is followed by exactly one fetch, naming nothing, which is drawn (" +
                          std::to_string(refetchedClean) + " of " + std::to_string(kRounds) + ")");
                check(claimsFrameShown(scene.item) && scene.reason().isEmpty(),
                      "a lost picture is no failure: the frame stays and no reason is shown (reason [" +
                          scene.reason().toStdString() + "])");
                const int logged = linesWith("a frame arrived without the memory of an image");
                const int capLine = linesWith("no further lost imports will be reported");
                check(logged == 8 && capLine == 1, "eight lost pictures are logged, then one line says no more will be (" +
                                                       std::to_string(logged) + " line(s), cap line " +
                                                       std::to_string(capLine) + ")");
                return true;
            }

            case Phase::NewerReply: {
                // GOAL: S15.351 -- a picture whose import was lost asks for nothing more when a newer
                //       reply has already landed: that reply is drawn or retried on its own, and asking
                //       again would be one more export and one more descriptor for a picture already
                //       superseded.
                // WHY-THIS-SHAPE: the race is staged, as in lost-import, with a third reply delivered
                //       to the item's thread before the pass that finds the second's import lost. The
                //       third reply is put in the item's queue without the GUI thread running: a blocking
                //       Ping from the stub's connection to the item's comes back only after the item's
                //       connection has read everything the stub sent before it. Three rounds.
                // CONTROL: `if (m_fetchedGeneration > lost) return;` deleted from the ImportLost handler
                //       in WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) -> "FAIL: a lost
                //       import with a newer reply already in asks nothing more (3 of 3 round(s) fetched
                //       again)". Via control_batch.sh, 2026-10-06. Reverted.
                // COVERAGE-ASSERT: each round must lose exactly one import with the third reply already
                //       delivered, or it is not counted as staged, and all three must be.
                std::vector<const wallpaperx::OffscreenTarget*> recency;  // most recent first
                const auto drew = [&](const wallpaperx::OffscreenTarget* target) {
                    std::erase(recency, target);
                    recency.insert(recency.begin(), target);
                    if (recency.size() > wallpaperx::kTargetDepth) recency.pop_back();
                };
                for (const wallpaperx::OffscreenTarget* target : ring) {
                    stub.serveNext(target);
                    if (!drawNext(scene, stub, bus)) {
                        check(false, "the item draws each image of the ring once");
                        return true;
                    }
                    drew(target);
                }
                const auto held = [&](size_t count) { return pumpUntil([&] { return stub.heldReplies() >= count; }, 3000); };
                constexpr int kRounds = 3;
                int staged = 0;
                int fetchedAgain = 0;
                for (int round = 0; round < kRounds; ++round) {
                    const std::string at = "round " + std::to_string(round) + ": ";
                    const wallpaperx::OffscreenTarget* missing = nullptr;
                    for (const wallpaperx::OffscreenTarget* target : ring) {
                        if (std::find(recency.begin(), recency.end(), target) == recency.end()) missing = target;
                    }
                    const wallpaperx::OffscreenTarget* evicted = recency.back();
                    const quint64 lost0 = scene.lost();
                    stub.holdEveryReply(true);
                    stub.serveNext(missing);
                    const quint64 first = stub.nextGeneration();
                    announce(bus, first);
                    announce(bus, first + 1);
                    if (!held(1)) {
                        wallpaperx::testing::harnessFailure(at + "the first reply never arrived to be held");
                        return true;
                    }
                    stub.serveNext(evicted);
                    if (!stub.sendOldestHeld() || !held(1)) {
                        wallpaperx::testing::harnessFailure(at + "the item did not ask for the second picture");
                        return true;
                    }
                    for (int pass = 0; pass < 3; ++pass) scene.renderPass();
                    drew(missing);
                    // A third picture, announced while the second is out, so a third request follows it.
                    announce(bus, first + 2);
                    stub.serveNext(missing);  // drawable through the import the node now holds
                    if (!stub.sendOldestHeld() || !held(1)) {
                        wallpaperx::testing::harnessFailure(at + "the item did not ask for the third picture");
                        return true;
                    }
                    const size_t asks0 = stub.asks().size();
                    stub.sendOldestHeld();
                    std::string why;
                    if (!readByItemConnection(bus, why)) {
                        wallpaperx::testing::harnessFailure(at + "the third reply could not be put in the item's "
                                                            "queue before the pass: " + why);
                        return true;
                    }
                    scene.renderPass();  // finds the second reply's import gone
                    stub.holdEveryReply(false);
                    (void)pumpUntil(
                        [&] {
                            scene.renderPass();
                            return scene.lost() > lost0 && stub.asks().size() > asks0;
                        },
                        1000);
                    if (scene.lost() == lost0 + 1) ++staged;
                    if (stub.asks().size() > asks0) ++fetchedAgain;
                    std::fprintf(stderr, "  %slost %llu, fetches after the lost picture %zu\n", at.c_str(),
                                 static_cast<unsigned long long>(scene.lost() - lost0), stub.asks().size() - asks0);
                    drew(missing);
                }
                if (staged != kRounds) {
                    wallpaperx::testing::harnessFailure("only " + std::to_string(staged) + " of " +
                                                        std::to_string(kRounds) + " rounds lost an import");
                    return true;
                }
                check(fetchedAgain == 0, "a lost import with a newer reply already in asks nothing more (" +
                                             std::to_string(fetchedAgain) + " of " + std::to_string(kRounds) +
                                             " round(s) fetched again)");
                return true;
            }

            case Phase::NewerFailure: {
                // GOAL: S15.351's sibling -- a failed hand-over is not asked for again when a newer reply has
                //       already landed: the retry exists for a still wallpaper that sends no further
                //       FrameReady, and a newer reply is already the next picture.
                // WHY-THIS-SHAPE: as in newer-reply, with the second picture's wait failed
                //       (gl_semaphore_fault.h) instead of its import lost, on the one image the node keeps.
                // CONTROL: `|| m_fetchedGeneration > failed` deleted from the failed-hand-over retry in
                //       WallpaperXItem::updatePaintNode (backend/kde/wallpaperx_item.cpp) -> "FAIL: a failed
                //       hand-over with a newer reply already in asks nothing more (3 of 3 round(s) fetched
                //       again)". Via control_batch.sh, 2026-10-06. Reverted.
                // COVERAGE-ASSERT: each round must fail exactly one wait with the third reply already
                //       delivered, or it is not counted as staged, and all three must be.
                wallpaperx::testing::GlSemaphoreFaults& faults = wallpaperx::testing::g_glSemaphoreFaults;
                const auto held = [&](size_t count) { return pumpUntil([&] { return stub.heldReplies() >= count; }, 3000); };
                constexpr int kRounds = 3;
                int staged = 0;
                int fetchedAgain = 0;
                for (int round = 0; round < kRounds; ++round) {
                    const std::string at = "round " + std::to_string(round) + ": ";
                    const int failed0 = faults.waitsFailed.load();
                    stub.holdEveryReply(true);
                    const quint64 first = stub.nextGeneration();
                    announce(bus, first);
                    if (!held(1)) {
                        wallpaperx::testing::harnessFailure(at + "the second picture's reply never arrived to be held");
                        return true;
                    }
                    announce(bus, first + 1);
                    pumpFor(50);  // queued behind the held reply: the item keeps one fetch out
                    if (!stub.sendOldestHeld() || !held(1)) {
                        wallpaperx::testing::harnessFailure(at + "the item did not ask for the third picture");
                        return true;
                    }
                    const size_t asks0 = stub.asks().size();
                    faults.failWaits.store(1);
                    stub.sendOldestHeld();
                    std::string why;
                    if (!readByItemConnection(bus, why)) {
                        wallpaperx::testing::harnessFailure(at + "the third reply could not be put in the item's "
                                                            "queue before the pass: " + why);
                        return true;
                    }
                    scene.renderPass();  // draws the second picture, whose wait fails
                    stub.holdEveryReply(false);
                    (void)pumpUntil(
                        [&] {
                            scene.renderPass();
                            return faults.waitsFailed.load() > failed0 && stub.asks().size() > asks0;
                        },
                        1000);
                    faults.failWaits.store(0);
                    if (faults.waitsFailed.load() == failed0 + 1) ++staged;
                    if (stub.asks().size() > asks0) ++fetchedAgain;
                    std::fprintf(stderr, "  %s%d wait(s) failed, fetches after the failure %zu\n", at.c_str(),
                                 faults.waitsFailed.load() - failed0, stub.asks().size() - asks0);
                }
                if (staged != kRounds) {
                    wallpaperx::testing::harnessFailure("only " + std::to_string(staged) + " of " +
                                                        std::to_string(kRounds) + " rounds failed a hand-over");
                    return true;
                }
                check(fetchedAgain == 0, "a failed hand-over with a newer reply already in asks nothing more (" +
                                             std::to_string(fetchedAgain) + " of " + std::to_string(kRounds) +
                                             " round(s) fetched again)");
                return true;
            }

            case Phase::Protocol: {
                // GOAL: S15.301 -- a reply leaving out the memory of an image the request did not name,
                //       or carrying two memory descriptors, is a daemon not keeping the protocol: not
                //       drawn, "WallpaperX answered with something this plugin cannot read." shown, one
                //       stderr line for the process, no fetch of its own after it, and the next
                //       announcement fetches and draws as usual.
                // WHY-THIS-SHAPE: the image served is one the item never saw, so it cannot have been
                //       named; "no fetch after it" is read over a second of passes, long enough for the
                //       queued re-fetch the lost-import path would post.
                // CONTROL: in WallpaperXItem::takeFrameReply (backend/kde/wallpaperx_item.cpp), (a) the
                //       protocol-violation block deleted -> "FAIL: memory left out for an image not named
                //       is not drawn, says why, and is not asked again (1 drawn, 1 fetches after it, 1 lost,
                //       reason [])", with the Twice line and the stderr line red beside it; (b) its
                //       `memoryFdCount > 1 ||` term deleted -> "FAIL: two memory descriptors for one image
                //       are not drawn either, and say why (1 drawn, 0 fetches after it, reason [])", alone.
                //       Each re-run 2026-10-06 on the NVIDIA entry via control_batch.sh. Reverted.
                const wallpaperx::OffscreenTarget& stranger = targets.create(30, kWidth, kHeight);
                publishSolidFrame(device, targets, stranger, "item_import_cache_stranger.png", palette[2]);
                wallpaperx::testing::StreamCapture capture(STDERR_FILENO);
                if (!capture.ok()) {
                    wallpaperx::testing::harnessFailure("stderr could not be captured");
                    return true;
                }
                const QString kCannotRead = QStringLiteral("WallpaperX answered with something this plugin cannot read.");
                const auto violate = [&](ExportingStub::Memory rule) {
                    const quint64 pictures0 = scene.pictures();
                    const quint64 lost0 = scene.lost();
                    const int requests0 = stub.requests();
                    stub.sendMemory(rule);
                    stub.serveNext(&stranger);
                    announce(bus, stub.nextGeneration());
                    const bool said = pumpUntil(
                        [&] {
                            scene.renderPass();
                            return scene.reason() == kCannotRead;
                        },
                        3000);
                    for (int pass = 0; pass < 20; ++pass) {
                        scene.renderPass();
                        pumpFor(50);
                    }
                    stub.sendMemory(ExportingStub::Memory::Daemon);
                    return std::make_tuple(said, scene.pictures() - pictures0, stub.requests() - requests0 - 1,
                                           scene.lost() - lost0);
                };
                const auto [saidOmitted, drawnOmitted, refetchOmitted, lostOmitted] =
                    violate(ExportingStub::Memory::OmitUndeclared);
                check(saidOmitted && drawnOmitted == 0 && refetchOmitted == 0 && lostOmitted == 0,
                      "memory left out for an image not named is not drawn, says why, and is not asked again (" +
                          std::to_string(drawnOmitted) + " drawn, " + std::to_string(refetchOmitted) +
                          " fetches after it, " + std::to_string(lostOmitted) + " lost, reason [" +
                          scene.reason().toStdString() + "])");
                // Back to a clean picture first, so the second reason is the second violation's.
                const quint64 before = scene.pictures();
                announce(bus, stub.nextGeneration());
                const bool recovered = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return scene.pictures() > before && scene.reason().isEmpty();
                    },
                    3000);
                check(recovered, "the next announcement after a protocol violation fetches and draws as usual (reason [" +
                                     scene.reason().toStdString() + "])");
                const auto [saidTwice, drawnTwice, refetchTwice, lostTwice] = violate(ExportingStub::Memory::Twice);
                check(saidTwice && drawnTwice == 0 && refetchTwice == 0,
                      "two memory descriptors for one image are not drawn either, and say why (" +
                          std::to_string(drawnTwice) + " drawn, " + std::to_string(refetchTwice) +
                          " fetches after it, reason [" + scene.reason().toStdString() + "])");
                const int lines = capture.count("frame not shown --");
                check(lines == 1, "both violations together write one line to stderr (" + std::to_string(lines) + ")");
                return true;
            }

            case Phase::RestartPending: {
                // GOAL: S15.313 -- a reply the item has already received but not yet drawn when the
                //       daemon's name moves is still drawn: it is this screen's picture, asked of the
                //       daemon that drew it. Drawn through the import held for it (a reuse, not a lost
                //       import), with no release to either daemon -- its claim belongs to a lifetime
                //       that has ended -- and the new daemon's first picture, numbered from 1, is drawn
                //       after it rather than dropped as older.
                // WHY-THIS-SHAPE: two images, both imported, and the reply is for the one NOT on the
                //       screen: the daemon's rule leaves its memory out where an acquire comes, and the
                //       import it must be drawn through is one the clean-up after a change would drop
                //       (with one image the drawn import is the reply's, and survives any clean-up). The
                //       reply is let through with no render pass and the name moved before the next one,
                //       which is the only order that reaches "in hand, not drawn"; the new daemon's first
                //       reply is held until the old picture is graded.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) `forgetGenerations(Undrawn::Keep);`
                //       in the DaemonWatch::replaced handler of connectToDaemon made `Undrawn::Drop` ->
                //       "FAIL: the reply in hand when the daemon was replaced is drawn, as a reuse (0
                //       picture(s), ...)"; (b) `m_pending->generation = 0;` deleted from discardHeldFrame ->
                //       "FAIL: and the new daemon's first picture is drawn after it (0 picture(s); ...)";
                //       (c) `forgetStaleImports(node);` added straight after the lambda at the top of
                //       updatePaintNode -> the first line, "(0 picture(s), ..., 1 lost; ...)" -- with ONE
                //       image this stayed green, which is why there are two; (d) in
                //       backend/kde/buffer_claims.cpp, the lifetime guard in BufferClaims::giveBack and the
                //       `m_entries.clear()` in daemonReplaced both deleted, the old daemon's claims kept and
                //       released to whoever owns the name -> "FAIL: and nothing is released to either
                //       daemon for it (0 covering it to the old, 1 to the new)". Via control_batch.sh,
                //       2026-10-05 on the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: the old daemon's reply is required to have been held and sent, for an
                //       image other than the one on the screen, and the new daemon's first reply held,
                //       before anything is graded.
                if (!drawNext(scene, stub, bus) || !drawNext(scene, stub, bus)) {
                    check(false, "the item draws both images of the first daemon");
                    return true;
                }
                const quint64 onScreen = stub.asks().back().served;
                stub.holdNextReply();
                announce(bus, stub.nextGeneration());
                if (!pumpUntil([&] { return stub.replyIsHeld(); }, 3000)) {
                    wallpaperx::testing::harnessFailure("the item's request to the first daemon never arrived to be held");
                    return true;
                }
                if (stub.asks().back().served == onScreen) {
                    wallpaperx::testing::harnessFailure("the held reply is for the image already on the screen");
                    return true;
                }
                const bool memoryLeftOut = !stub.asks().back().memorySent;
                // Releases of earlier pictures land after their fences, so what is counted is a release
                // covering the late reply's own hand-over, not every release after this point.
                const quint64 lateNumber = stub.asks().back().handover;
                const auto coveringLate = [&] {
                    return std::count_if(stub.releases().begin(), stub.releases().end(),
                                         [&](const ExportingStub::Release& r) { return r.handover >= lateNumber; });
                };
                const quint64 pictures0 = scene.pictures();
                const quint64 imports0 = scene.imports();
                const quint64 reused0 = scene.reused();
                const quint64 lost0 = scene.lost();
                stub.sendHeldReply();
                std::string why;
                if (!takenByItem(bus, why)) {  // no render pass runs
                    wallpaperx::testing::harnessFailure("the held reply could not be put in the item's hands: " + why);
                    return true;
                }

                SecondStub second(device, ring);
                QDBusConnection& busB = second.bus;
                ExportingStub& stubB = second.stub;
                stubB.holdServeNumber(1);
                QString newOwner;
                QDBusServiceWatcher watcher(QString::fromUtf8(kService), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForOwnerChange);
                QObject::connect(&watcher, &QDBusServiceWatcher::serviceOwnerChanged,
                                 [&newOwner](const QString&, const QString&, const QString& owner) { newOwner = owner; });
                if (!second.ok || !bus.unregisterService(QString::fromUtf8(kService)) ||
                    !busB.registerService(QString::fromUtf8(kService)) ||
                    !pumpUntil([&] { return newOwner == busB.baseService(); }, 3000) ||
                    !pumpUntil([&] { return stubB.replyIsHeld(); }, 3000)) {
                    wallpaperx::testing::harnessFailure("the name could not be moved to a second stub holding its first "
                                                        "reply");
                    return true;
                }
                const bool drawn = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return scene.pictures() > pictures0;
                    },
                    3000);
                pumpFor(100);
                check(drawn && scene.pictures() == pictures0 + 1 && scene.reused() == reused0 + 1 &&
                          scene.imports() == imports0 && scene.lost() == lost0,
                      "the reply in hand when the daemon was replaced is drawn, as a reuse (" +
                          std::to_string(scene.pictures() - pictures0) + " picture(s), " +
                          std::to_string(scene.reused() - reused0) + " reuse(s), " +
                          std::to_string(scene.imports() - imports0) + " import(s), " +
                          std::to_string(scene.lost() - lost0) + " lost; memory " +
                          (memoryLeftOut ? "left out" : "sent") + ")");
                check(coveringLate() == 0 && stubB.releases().empty(),
                      "and nothing is released to either daemon for it (" + std::to_string(coveringLate()) +
                          " covering it to the old, " + std::to_string(stubB.releases().size()) + " to the new)");
                const quint64 pictures1 = scene.pictures();
                stubB.sendHeldReply();
                const bool firstOfB = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return scene.pictures() > pictures1;
                    },
                    3000);
                check(firstOfB && claimsFrameShown(scene.item),
                      "and the new daemon's first picture is drawn after it (" + std::to_string(scene.pictures() - pictures1) +
                          " picture(s); reason [" + scene.reason().toStdString() + "])");
                return true;
            }

            case Phase::PendingMonitor:
            case Phase::PendingOffscreen: {
                // GOAL: S15.314 -- a reply in hand but not yet drawn is dropped when the item moves to
                //       another monitor or leaves the screen: it is never drawn, and its image is given
                //       back to the daemon that is still current, naming that reply's hand-over.
                // WHY-THIS-SHAPE: the reply is for an image the item has never been served, so its claim
                //       is the only one on that image and dropping it is what sends the release; the
                //       stub refuses everything after, so a picture drawn later can only be the dropped
                //       one. The offscreen variant comes back on the screen before counting, because a
                //       kept reply would be drawn by the first pass after it returns.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (pending-monitor) `forgetGenerations(
                //       Undrawn::Drop);` in setMonitorId made `Undrawn::Keep` -> "FAIL: the reply in hand
                //       is never drawn once the item moved monitor (1 picture(s) since)" and "FAIL: and its
                //       image is given back naming that reply's hand-over (named 0 for 2)";
                //       (pending-offscreen) `discardHeldFrame(Undrawn::Drop);` in releaseHeldImages made
                //       `Undrawn::Keep` -> "FAIL: the reply in hand is never drawn once the item left the
                //       screen (1 picture(s) since)" -- the release still goes, from the retired claim set,
                //       so only the drawing tells. Via control_batch.sh, 2026-10-05 on the NVIDIA entry.
                //       Reverted.
                const wallpaperx::OffscreenTarget* unserved = nullptr;
                for (const wallpaperx::OffscreenTarget* target : ring) {
                    const bool served = std::any_of(stub.asks().begin(), stub.asks().end(), [&](const ExportingStub::Ask& a) {
                        return a.served == target->bufferId;
                    });
                    if (!served) unserved = target;
                }
                if (unserved == nullptr) {
                    wallpaperx::testing::harnessFailure("every image of the ring was served during startup, so no image "
                                                        "has the reply's claim as its only one");
                    return true;
                }
                stub.holdNextReply();
                stub.serveNext(unserved);
                announce(bus, stub.nextGeneration());
                if (!pumpUntil([&] { return stub.replyIsHeld(); }, 3000)) {
                    wallpaperx::testing::harnessFailure("the item's request never arrived to be held");
                    return true;
                }
                const quint64 heldNumber = stub.asks().back().handover;
                const size_t releases0 = stub.releases().size();
                stub.refuseFurtherWith(QStringLiteral("io.wallpaperx.Error.NoFrame"));
                stub.sendHeldReply();
                std::string why;
                if (!takenByItem(bus, why)) {  // no render pass runs
                    wallpaperx::testing::harnessFailure("the held reply could not be put in the item's hands: " + why);
                    return true;
                }
                const quint64 pictures0 = scene.pictures();
                if (phase == Phase::PendingMonitor) {
                    scene.item->setProperty("monitorId", kSecondMonitor);
                } else {
                    scene.item->setVisible(false);
                }
                const auto releasedHeld = [&] {
                    for (size_t i = releases0; i < stub.releases().size(); ++i) {
                        if (stub.releases()[i].bufferId == unserved->bufferId) return stub.releases()[i].handover;
                    }
                    return quint64{0};
                };
                const bool released = pumpUntil([&] { return releasedHeld() != 0; }, 3000);
                if (phase == Phase::PendingOffscreen) {
                    scene.renderPass();
                    scene.item->setVisible(true);
                }
                for (int pass = 0; pass < 5; ++pass) {
                    scene.renderPass();
                    pumpFor(20);
                }
                check(scene.pictures() == pictures0,
                      std::string("the reply in hand is never drawn once the item ") +
                          (phase == Phase::PendingMonitor ? "moved monitor" : "left the screen") + " (" +
                          std::to_string(scene.pictures() - pictures0) + " picture(s) since)");
                check(released && releasedHeld() == heldNumber,
                      "and its image is given back naming that reply's hand-over (named " +
                          std::to_string(releasedHeld()) + " for " + std::to_string(heldNumber) + ")");
                return true;
            }

            case Phase::NoOwner: {
                // GOAL: S15.312 -- with the departed daemon heard leaving and its successor not yet heard
                //       arriving, a fetch the item is made to do sends nothing; once the successor is
                //       heard, the item asks it, and from then on every image the successor hands over is
                //       eventually given back: the wallpaper keeps drawing from a daemon that refuses any
                //       image still held. False for the rule that sent such a fetch to the well-known
                //       name: the successor answered it, the reply was credited to the departed daemon,
                //       its claim was never recorded, and that image stayed held for the session.
                // WHY-THIS-SHAPE: the successor stub keeps the daemon's ledger -- numbered hand-overs,
                //       held images never served, a freed claim announced -- with one image more than the
                //       item keeps imported, so a single leaked hold leaves it no image to serve and the
                //       drive stalls. The name is registered from another thread and the fetch forced by
                //       hiding and showing the item before this thread runs an event loop, so the item
                //       cannot have heard of the successor when it fetches.
                // CONTROL: in WallpaperXItem::fetchFrame (backend/kde/wallpaperx_item.cpp), the `if
                //       (asked.owner.empty()) {...}` early return deleted and the destination made the
                //       well-known name when the owner is empty -> "FAIL: the successor keeps serving
                //       pictures, none of its images leaking (2 of 12 drawn; S1#1 S2#2 X0 | held 1 2, on
                //       screen 2)": image 1 was served to the request sent before the item heard of the
                //       successor, never given back, and the drive stalled. Via control_batch.sh, re-run
                //       2026-10-06 on the NVIDIA entry. Reverted.
                // COVERAGE-ASSERT: the departure must be heard (the item says it is not running) before
                //       the successor registers, and twelve pictures -- two rounds of the ring -- are
                //       required.
                constexpr int kPictures = 12;
                const QString kNotRunning = QStringLiteral("WallpaperX is not running.");
                bus.unregisterObject(QString::fromUtf8(kPath));
                if (!bus.unregisterService(QString::fromUtf8(kService)) ||
                    !pumpUntil([&] { return scene.reason() == kNotRunning; }, 3000)) {
                    wallpaperx::testing::harnessFailure("the item never heard the first daemon leave");
                    return true;
                }
                SecondStub second(device, ring);
                QDBusConnection& busB = second.bus;
                ExportingStub& stubB = second.stub;
                stubB.respectHolds(busB);
                bool registered = false;
                if (second.ok) {
                    std::thread([&] { registered = busB.registerService(QString::fromUtf8(kService)); }).join();
                }
                if (!registered) {
                    wallpaperx::testing::harnessFailure("the second stub daemon could not take the name");
                    return true;
                }
                scene.item->setVisible(false);
                scene.item->setVisible(true);
                int drawn = 0;
                for (int i = 0; i < kPictures; ++i) {
                    const quint64 before = scene.pictures();
                    // The first picture is the item's own fetch on hearing the successor.
                    if (i > 0) announce(busB, stubB.nextGeneration());
                    const bool more = pumpUntil(
                        [&] {
                            scene.renderPass();
                            return scene.pictures() > before;
                        },
                        3000);
                    if (!more) break;
                    ++drawn;
                }
                pumpFor(300);
                std::string held;
                for (quint64 id : stubB.outstanding()) held += (held.empty() ? "" : " ") + std::to_string(id);
                quint64 onScreen = 0;
                for (const ExportingStub::Ask& ask : stubB.asks()) {
                    if (ask.served != 0) onScreen = ask.served;
                }
                check(drawn == kPictures && stubB.outstanding() == std::vector<quint64>{onScreen},
                      "the successor keeps serving pictures, none of its images leaking (" + std::to_string(drawn) +
                          " of " + std::to_string(kPictures) + " drawn; " + stubB.ledgerLog() + "| held " + held +
                          ", on screen " + std::to_string(onScreen) + ")");
                if (g_monitor != nullptr) {
                    check(g_monitor->calls("GetFrame") > 0, "the bus monitor saw this phase's frame requests");
                }
                return true;
            }

            case Phase::NoItem: {
                // GOAL: S15.315 -- a daemon replaced while the process has no wallpaper item is seen by the
                //       next item, which fetches from the new daemon. False for one watch per item: with
                //       no item alive nothing watched the name, and the next item asked the departed
                //       daemon's unique name for the rest of the session.
                // WHY-THIS-SHAPE: the first daemon's connection is closed outright, so its unique name is
                //       gone and a request addressed to it is refused rather than answered by a stub that
                //       is still listening. The change is waited for on a watcher of this process's own
                //       connection, which is the connection the plugin's watch is on.
                // CONTROL: `delete DaemonWatch::instance();` added to WallpaperXItem::~WallpaperXItem
                //       (backend/kde/wallpaperx_item.cpp), a watch that dies with the items -> "FAIL: an
                //       item made after the daemon was replaced with no item alive draws from the new
                //       daemon (0 served by it; reason [WallpaperX could not be reached.])" -- its request
                //       went to the departed daemon's unique name. Via control_batch.sh, 2026-10-05 on the
                //       NVIDIA entry. Reverted.
                scene.root.reset();
                scene.item = nullptr;
                scene.renderPass();
                pumpFor(100);
                QString newOwner;
                QDBusServiceWatcher watcher(QString::fromUtf8(kService), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForOwnerChange);
                QObject::connect(&watcher, &QDBusServiceWatcher::serviceOwnerChanged,
                                 [&newOwner](const QString&, const QString&, const QString& owner) { newOwner = owner; });
                bus.unregisterObject(QString::fromUtf8(kPath));
                (void)bus.unregisterService(QString::fromUtf8(kService));
                QDBusConnection::disconnectFromBus(QStringLiteral("wallpaperx-cache-stub-daemon"));
                SecondStub second(device, ring);
                QDBusConnection& busB = second.bus;
                ExportingStub& stubB = second.stub;
                if (!second.ok || !busB.registerService(QString::fromUtf8(kService)) ||
                    !pumpUntil([&] { return newOwner == busB.baseService(); }, 3000)) {
                    wallpaperx::testing::harnessFailure("the second stub daemon never became the name's owner");
                    return true;
                }
                pumpFor(50);
                scene.root.reset(scene.component->create());
                auto* again = qobject_cast<QQuickItem*>(scene.root.get());
                if (again == nullptr) {
                    wallpaperx::testing::harnessFailure("the second item could not be made");
                    return true;
                }
                again->setParentItem(scene.window.contentItem());
                scene.item = qobject_cast<QQuickItem*>(scene.root->property("rendererItem").value<QObject*>());
                if (scene.item == nullptr) {
                    wallpaperx::testing::harnessFailure("the second root exposed no rendererItem");
                    return true;
                }
                const bool drawn = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return stubB.served() > 0 && itemTexture(scene.item) != nullptr;
                    },
                    5000);
                check(drawn, "an item made after the daemon was replaced with no item alive draws from the new daemon (" +
                                 std::to_string(stubB.served()) + " served by it; reason [" +
                                 scene.reason().toStdString() + "])");
                return true;
            }

            case Phase::AskingMonitor: {
                // GOAL: S15.316 -- when the item moves to another monitor while a request for the old one
                //       is out, the old monitor's imports are freed once that reply lands (the clean-up
                //       waits for it, so a late picture could be drawn first), the picture on the screen
                //       stays; and S15.310 -- that reply, now for a monitor the item does not show, is
                //       given back naming its own hand-over, which is the newest of its image.
                // WHY-THIS-SHAPE: the whole ring is imported first, so there are kTargetDepth imports to
                //       free; the reply is held across the change and sent only after it. The stub
                //       refuses every later request, so no new monitor's picture is drawn.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) the `if (marksMovedSince(epoch)) {...}`
                //       block in fetchFrame's reply handler deleted -> "FAIL: a left monitor's imports are
                //       freed once the reply out at the change lands (0 of 4, the drawn one kept)"; (b) the
                //       body of WallpaperXItem::releaseFrameReply emptied, the reply dropped with the
                //       message -> "FAIL: and that reply's image is given back naming its hand-over (named 0
                //       for 6)". Via control_batch.sh, 2026-10-05 on the NVIDIA entry. Reverted.
                TextureWatch watch(&scene.window);
                if (!importWholeRing(scene, stub, bus, watch, imageCount)) return true;
                QSGTexture* drawnTexture = itemTexture(scene.item);
                stub.holdNextReply();
                announce(bus, stub.nextGeneration());
                if (!pumpUntil([&] { return stub.replyIsHeld(); }, 3000)) {
                    wallpaperx::testing::harnessFailure("the item's request never arrived to be held");
                    return true;
                }
                const quint64 heldImage = stub.asks().back().served;
                const quint64 heldNumber = stub.asks().back().handover;
                const size_t releases0 = stub.releases().size();
                stub.refuseFurtherWith(QStringLiteral("io.wallpaperx.Error.NoFrame"));
                scene.item->setProperty("monitorId", kSecondMonitor);
                for (int pass = 0; pass < 3; ++pass) {
                    scene.renderPass();
                    pumpFor(20);
                }
                stub.sendHeldReply();
                // Waited for, bounded: how soon the reply lands is the bus's business. A few passes more
                // after it, so a late picture or a late free of the drawn texture still shows.
                const bool freed = pumpUntil(
                    [&] {
                        scene.renderPass();
                        return watch.destroyed() >= imageCount - 1;
                    },
                    3000);
                for (int pass = 0; pass < 3; ++pass) {
                    scene.renderPass();
                    pumpFor(20);
                }
                check(freed && watch.destroyed() == imageCount - 1 && !watch.isDestroyed(drawnTexture) &&
                          itemTexture(scene.item) == drawnTexture,
                      "a left monitor's imports are freed once the reply out at the change lands (" +
                          std::to_string(watch.destroyed()) + " of " + std::to_string(imageCount - 1) +
                          ", the drawn one " + (watch.isDestroyed(drawnTexture) ? "freed" : "kept") + ")");
                // The release naming that reply's own hand-over, if one went: a release of an older
                // hand-over of the same image, superseded by this reply, may go first.
                const auto namedFor = [&] {
                    quint64 named = 0;
                    for (size_t i = releases0; i < stub.releases().size(); ++i) {
                        if (stub.releases()[i].bufferId == heldImage) named = std::max(named, stub.releases()[i].handover);
                    }
                    return named;
                };
                (void)pumpUntil([&] { return namedFor() != 0; }, 3000);
                check(namedFor() == heldNumber, "and that reply's image is given back naming its hand-over (named " +
                                                    std::to_string(namedFor()) + " for " + std::to_string(heldNumber) +
                                                    ")");
                return true;
            }

            case Phase::StartupNoDaemon:
                // Graded above, before any import is waited for: no daemon serves one.
                return true;

            case Phase::NotOpenGl:
                // Graded above, before any import is waited for: there is none on this scene graph.
                return true;
        }
        return true;
    });
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_import_cache_test";
    wallpaperx::testing::registerFrameReplyTypes();
    QGuiApplication app(argc, argv);

    const PhaseName* which = nullptr;
    for (const PhaseName& candidate : kPhases) {
        if (argc > 1 && std::strcmp(argv[1], candidate.name) == 0) which = &candidate;
    }
    // Before any window exists, which is all setGraphicsApi requires.
    QQuickWindow::setGraphicsApi(which != nullptr && which->phase == Phase::NotOpenGl
                                     ? QSGRendererInterface::Software
                                     : QSGRendererInterface::OpenGL);
    if (which == nullptr) {
        wallpaperx::testing::harnessFailure("no known phase was named on the command line");
        (void)wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
        return 1;
    }

    // Every connection this binary opens -- the item's, the stubs', the monitor -- is on the session
    // bus it was given, so it runs only on the private one.
    std::string whyNotPrivate;
    if (!wallpaperx::testing::onPrivateTestBus(whyNotPrivate)) {
        wallpaperx::testing::harnessFailure("this test runs only on the private bus isolated_run.sh starts: " +
                                            whyNotPrivate);
        (void)wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
        return 1;
    }

    // Before the item exists, so its first request is seen too.
    wallpaperx::testing::BusMonitor monitor;
    if (monitor.ok()) {
        g_monitor = &monitor;
    } else {
        // S15.311's cross-phase check below is graded by the monitor alone.
        notVerified("S15.311", "the private bus would not accept a monitor, so where requests and releases were "
                               "addressed cannot be read: " + monitor.why());
    }

    bool ok = false;
    try {
        ok = runPhase(*which);
    } catch (const std::exception& e) {
        wallpaperx::testing::harnessFailure(std::string("the harness could not build its preconditions: ") + e.what());
    }

    // GOAL: S15.311 -- no frame request and no release is ever addressed to the daemon's well-known
    //       name, in any phase: the well-known name is served by whichever daemon owns it when the bus
    //       routes the call, which can be one this item has not heard arrive, and a release reaching
    //       it can free an image that daemon handed to somebody else.
    // WHY-THIS-SHAPE: read off the bus monitor, the only party that sees a call's destination; the
    //       stubs own both names and answer both. Asked across every phase rather than staged once, so
    //       each phase's own traffic is evidence.
    // CONTROL: in WallpaperXItem::fetchFrame (backend/kde/wallpaperx_item.cpp), the `if
    //       (asked.owner.empty()) {...}` early return deleted and the destination made the
    //       well-known name when the owner is empty -> on item_cache_no-owner, "FAIL: no frame request
    //       or release was addressed to the daemon's well-known name (1 GetFrame(s), 0
    //       ReleaseBuffer(s) to it; 5 and 1 seen in all)" (re-run 2026-10-06 via control_batch.sh), and
    //       on item_cache_startup-no-daemon the same line with 1 once the one-unanswered guard is opened
    //       too (2026-10-06). The destination made the well-known name always -> on
    //       item_cache_unique-name the same line, "(5 GetFrame(s) ...)", 2026-10-05. On the NVIDIA entry.
    //       Reverted.
    // COVERAGE-ASSERT: how many requests the monitor saw addressed anywhere is printed beside it, and
    //       the count is read only once the monitor has seen a barrier the item's own connection sent
    //       after everything else it sent -- the bus routes one connection's messages in order -- and
    //       only while the monitor is still reading.
    if (g_monitor != nullptr) {
        std::string why;
        if (!monitorCaughtUp(monitor, why)) {
            wallpaperx::testing::harnessFailure(why);
            g_monitor = nullptr;
        }
    }
    if (g_monitor != nullptr) {
        const int frames = monitor.callsTo("GetFrame", kService);
        const int releases = monitor.callsTo("ReleaseBuffer", kService);
        std::printf("item_import_cache_test: the bus monitor saw %d GetFrame(s) and %d ReleaseBuffer(s), %d and %d of "
                    "them to the well-known name\n",
                    monitor.calls("GetFrame"), monitor.calls("ReleaseBuffer"), frames, releases);
        check(frames == 0 && releases == 0,
              "no frame request or release was addressed to the daemon's well-known name (" +
                  std::to_string(frames) + " GetFrame(s), " + std::to_string(releases) + " ReleaseBuffer(s) to it; " +
                  std::to_string(monitor.calls("GetFrame")) + " and " + std::to_string(monitor.calls("ReleaseBuffer")) +
                  " seen in all)");
        g_monitor = nullptr;
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
