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
// talks to, its per-share-group timeline imports and its refused-import flag are process-wide and
// never reset.
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
//   refused-shared      one refusal stops imports in every context, shared or not (S15.439)
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
//   context-share       one import per share group: invalidated, new, returned to (S15.420);
//                       a sharing context imports nothing and its end deletes nothing (S15.436-S15.438);
//                       a group's destroyed() erases its record (S15.442)
//   shared-replaced     every earlier daemon's import stays in the group, none deleted by hand (S15.440, S15.441)
//   shared-threads      two render threads in one group: no glFinish under the group lock, waits flushed, a failed
//                       one too, before the other uses it (S15.443, S15.444)
//   shared-stuck        a context stuck on an unsignalled value imports a new daemon's timeline; the other
//                       render thread still gets through (S15.445)
//
// Every phase but refused-binary, refused-shared and stray-value needs a GL context that can wait on a Vulkan
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
#include <QThread>
#include <QUrl>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <thread>
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
    RefusedShared,
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
    SharedReplaced,
    SharedThreads,
    SharedStuck,
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
    {Phase::RefusedShared, "refused-shared",
     "S15.439 (one refused import stops semaphore imports in every context, said once)", false},
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
     "S15.420, S15.436-S15.438 and S15.442 (the timeline is imported once per GL share group and shared by its "
     "contexts)",
     true},
    {Phase::SharedReplaced, "shared-replaced",
     "S15.440 and S15.441 (a share group keeps every earlier daemon's import and deletes none by hand)", true},
    {Phase::SharedThreads, "shared-threads",
     "S15.443 and S15.444 (no glFinish under a share group's timeline lock; each wait on a group timeline, a failed "
     "one too, flushed before another thread's context uses it)",
     true},
    {Phase::SharedStuck, "shared-stuck",
     "S15.445 (a context stuck on a value never signalled does not hold up its share group's other render threads "
     "when it imports a new daemon's timeline)",
     true},
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

    QString connection;  // the name QtDBus keeps the connection under, one per daemon alive at once

    SuccessorDaemon(wallpaperx::Device& device, const std::vector<const wallpaperx::OffscreenTarget*>& ring,
                    const QString& connectionName = QStringLiteral("wallpaperx-timeline-b"))
        : bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName)),
          stub(device, ring),
          connection(connectionName) {
        stub.rotate();
        stub.serveTimelines(ExportingStub::Timeline::Grant);
        QObject::connect(&watcher, &QDBusServiceWatcher::serviceOwnerChanged,
                         [this](const QString&, const QString&, const QString& owner) { newOwner = owner; });
        ok = bus.isConnected() && bus.registerVirtualObject(QString::fromUtf8(kPath), &stub);
    }
    ~SuccessorDaemon() {
        bus.unregisterObject(QString::fromUtf8(kPath));
        (void)bus.unregisterService(QString::fromUtf8(kService));
        QDBusConnection::disconnectFromBus(connection);
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

// A new context in `shareWith`'s share group, adopted by a second scene graph in which `in` runs; the
// scene graph is invalidated after it. Returns the context owned and not current, the scene graph
// gone, or null after a reported harness failure. The scene graph's own contexts share nothing.
std::unique_ptr<QOpenGLContext> withSharingScene(QOpenGLContext* shareWith, const std::function<void(Scene&)>& in) {
    auto shared = std::make_unique<QOpenGLContext>();
    shared->setShareContext(shareWith);
    if (!shared->create() || shared->shareGroup() != shareWith->shareGroup()) {
        wallpaperx::testing::harnessFailure("no context sharing the scene's group could be made");
        return nullptr;
    }
    {
        Scene adopted;
        adopted.window.setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(shared.get()));
        adopted.window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
        if (!adopted.control.initialize()) {
            wallpaperx::testing::harnessFailure("the scene graph would not adopt the sharing context");
            return nullptr;
        }
        in(adopted);
        adopted.control.invalidate();
    }
    if (QOpenGLContext::currentContext() == shared.get()) shared->doneCurrent();
    return shared;
}

// A scene graph whose frames run on a QThread of its own, as under Qt's threaded render loop: the GUI
// thread polishes, then blocks while each call runs there, so it is blocked across every sync, which
// is QQuickRenderControl's contract for a render thread. A sync and its endFrame() are separate calls,
// so the test can hold this thread mid-frame while another scene runs. Joined on every exit.
struct ThreadedScene {
    QThread thread;
    QObject worker;  // on `thread`; calls are queued to it
    std::unique_ptr<QOpenGLContext> context;
    Scene scene;
    std::thread::id renderThread;
    bool up = false;

    ThreadedScene() {
        worker.moveToThread(&thread);
        thread.start();
    }
    ~ThreadedScene() {
        down();
        thread.quit();
        thread.wait();
    }
    ThreadedScene(const ThreadedScene&) = delete;
    ThreadedScene& operator=(const ThreadedScene&) = delete;

    void on(const std::function<void()>& fn) {
        QMetaObject::invokeMethod(&worker, [&fn] { fn(); }, Qt::BlockingQueuedConnection);
    }
    // `context` made (and shared) by the caller, on the GUI thread.
    bool bringUp() {
        scene.window.setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(context.get()));
        scene.window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
        scene.control.prepareThread(&thread);
        context->moveToThread(&thread);
        on([this] {
            up = scene.control.initialize();
            renderThread = std::this_thread::get_id();
        });
        return up;
    }
    void sync() {
        scene.control.polishItems();
        on([this] {
            scene.control.beginFrame();
            scene.control.sync();
        });
    }
    void end() {
        on([this] {
            wallpaperx::testing::gl_fault_detail::order(wallpaperx::testing::GlSemaphoreFaults::Call::FrameEnd, 0);
            scene.control.endFrame();
        });
    }
    void pass() {
        sync();
        end();
    }
    // `fn` queued to the render thread without waiting: the future is ready once it has run. For a call
    // that may block there, so the GUI thread can bound its wait and release what holds it. Later on()
    // calls queue behind it.
    std::shared_future<void> post(std::function<void()> fn) {
        auto ran = std::make_shared<std::promise<void>>();
        std::shared_future<void> done = ran->get_future().share();
        QMetaObject::invokeMethod(
            &worker,
            [fn = std::move(fn), ran] {
                try {
                    fn();
                    ran->set_value();
                } catch (...) {
                    ran->set_exception(std::current_exception());
                }
            },
            Qt::QueuedConnection);
        return done;
    }
    // sync() and end() without waiting; the GUI thread must not touch this scene until the future is ready.
    std::shared_future<void> syncLater() {
        scene.control.polishItems();
        return post([this] {
            scene.control.beginFrame();
            scene.control.sync();
        });
    }
    std::shared_future<void> endLater() {
        return post([this] {
            wallpaperx::testing::gl_fault_detail::order(wallpaperx::testing::GlSemaphoreFaults::Call::FrameEnd, 0);
            scene.control.endFrame();
        });
    }
    // The item gone and its node reaped in one more frame, the scene graph invalidated and the context
    // handed back, all before anything here is destroyed on the GUI thread.
    void down() {
        if (context == nullptr || context->thread() != &thread) return;
        if (up) {
            scene.root.reset();
            pass();
        }
        up = false;
        QThread* gui = QCoreApplication::instance()->thread();
        on([this, gui] {
            scene.control.invalidate();
            if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();
            context->moveToThread(gui);
        });
    }
};

// The plugin's item on `monitor` (the second by default), loaded into `into`; false after a reported
// harness failure.
bool loadSecondMonitorItem(Scene& into, const char* file, int monitor = wallpaperx::testing::kSecondMonitor) {
    into.engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    into.component = std::make_unique<QQmlComponent>(&into.engine);
    QString qml = wallpaperx::testing::cacheQmlSource();
    qml.replace(QStringLiteral("monitorId: 0"), QStringLiteral("monitorId: ") + QString::number(monitor));
    into.component->setData(qml.toUtf8(), QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                              QStringLiteral("/") + QString::fromUtf8(file)));
    into.root.reset(into.component->create());
    auto* root = qobject_cast<QQuickItem*>(into.root.get());
    if (root == nullptr) {
        wallpaperx::testing::harnessFailure(std::string("the item in ") + file + " could not be made");
        return false;
    }
    root->setParentItem(into.window.contentItem());
    into.item = qobject_cast<QQuickItem*>(into.root->property("rendererItem").value<QObject*>());
    if (into.item == nullptr) {
        wallpaperx::testing::harnessFailure(std::string("the item in ") + file + " exposed no rendererItem");
        return false;
    }
    return true;
}

// The receivers of `object`'s destroyed(): QObject::receivers is protected, and a pointer to it taken
// through a derived class is still a pointer to QObject's member, so no cast to a type `object` is not.
int destroyedReceivers(const QObject* object) {
    struct Peek : QObject {
        using QObject::receivers;
    };
    return (object->*&Peek::receivers)(SIGNAL(destroyed(QObject*)));
}

// One of gl_semaphore_fault.h's call records, read under its lock: its length, and the names in it
// from index `from` on.
using CallRecord = std::vector<wallpaperx::testing::GlSemaphoreFaults::NamedCall> wallpaperx::testing::GlSemaphoreFaults::*;
size_t recorded(CallRecord record) {
    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
    return (g_glSemaphoreFaults.*record).size();
}
std::vector<GLuint> namesSince(CallRecord record, size_t from) {
    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
    std::vector<GLuint> names;
    const auto& calls = g_glSemaphoreFaults.*record;
    for (size_t i = from; i < calls.size(); ++i) names.push_back(calls[i].name);
    return names;
}

std::string listNames(const std::vector<GLuint>& names) {
    std::string out;
    for (GLuint name : names) out += (out.empty() ? "" : " ") + std::to_string(name);
    return out.empty() ? "none" : out;
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
            case Phase::RefusedShared:
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
            case Phase::SharedStuck:
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
                // CONTROL: in backend/kde/wallpaperx_item.cpp, (a) acquireOnGroupTimeline's `grantedBySender`
                //       made to return false, so every value imports -> "FAIL: a picture on the timeline
                //       costs no semaphore import (20 over 20 pictures)" (via control_run.sh, 2026-10-10, on
                //       the NVIDIA opaque-fd and dma_buf entries both); (b) WallpaperXItem::fetchOnTimeline made to return false -> "FAIL: the
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
                //       blue where yellow was drawn; a value, through the kept import" (2026-09-23, NVIDIA).
                //       And acquireOnGroupTimeline's `grantedBySender` made to return false -> the same line
                //       "(0 of 4)", every colour right and each step "NOT a value through a kept import": the
                //       coverage half reddening alone (via control_run.sh, 2026-10-10, NVIDIA). Reverted.
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
                // GOAL: S13.223 -- once the driver refuses a semaphore import -- the timeline's, or a
                //       picture's binary one -- the process draws without the producer's hand-over from
                //       then on: one stderr line when it trips, every picture after it drawn, no
                //       semaphore import attempted again, and the wallpaper never blanks.
                // WHY-THIS-SHAPE: the refusal is the driver's own, from a descriptor that is no
                //       semaphore payload, so the flag is set by what sets it on a desktop. After the trip
                //       every binary picture, on both entries, carries a descriptor, so "no import attempted"
                //       means dropped, not absent. That descriptor is another unusable one, not a real
                //       semaphore: on NVIDIA one refused import makes the stub's own vkCreateSemaphore fail
                //       in this process, so "Real" would send none and leave nothing to drop. Served and
                //       drawn are counted apart, because a failed picture is replaced by the item's re-fetch.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, via control_run.sh, 2026-10-10, NVIDIA, each
                //       on both entries, reverted: (a) `g_semaphoresRefused.exchange(true)` in
                //       noteSemaphoresRefused made a plain load(), so the flag is never set -> "FAIL: the
                //       refusal is said once, on stderr (16 line(s))" (14 on refused-binary) and "FAIL: after
                //       the refusal no semaphore import is attempted again (10 over 10 pictures)", and on
                //       refused-timeline the coverage line "(0 over 10 pictures)": still asked on the timeline; (b)
                //       `handedOver = true;` deleted after it in updatePaintNode -> "FAIL: every picture
                //       served is drawn, the one that tripped it included (17 served, 16 drawn; ...)"; (c)
                //       dropUnwaitableSync's `if (!g_semaphoresRefused.load())` made `if (true)`, so the
                //       binary path ignores the flag -> refused-timeline: "FAIL: after the refusal no
                //       semaphore import is attempted again (10 over 10 pictures)".
                // COVERAGE-ASSERT: the marker printed at the start must be in the capture, and every
                //       picture after the trip must have carried a descriptor.
                const int served0 = stub.served();
                const quint64 pictures0 = scene.pictures();
                if (phase == Phase::RefusedBinary) stub.sendAcquireFrom(served0 + 3, ExportingStub::Acquire::Unusable);
                // The trip happens inside the drive: the timeline's first value, or the third picture.
                const Drive run = drive(scene, stub, bus, 6);
                const quint64 semaphoresAfterTrip = scene.semaphoreImports();
                stub.sendAcquireFrom(stub.served() + 1, ExportingStub::Acquire::Unusable);
                const int unusable0 = stub.unusableAcquiresSent();
                const Drive after = drive(scene, stub, bus, 10);
                const int unusableAfter = stub.unusableAcquiresSent() - unusable0;
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
                          std::to_string(after.drawn) + " pictures)");
                check(after.drawn == 10 && unusableAfter >= after.drawn,
                      "every picture after the refusal carried a semaphore descriptor (" + std::to_string(unusableAfter) +
                          " over " + std::to_string(after.drawn) + " pictures)");
                if (phase == Phase::RefusedTimeline) {
                    check(stub.timelinesGranted() >= 1, "the refused descriptor was handed over as the timeline");
                }
                return true;
            }

            case Phase::RefusedShared: {
                // GOAL: S15.439 -- one refusal stops semaphore imports in the whole process: after a binary
                //       import is refused in the scene's context, a new unshared context (a share group of its
                //       own) and a new context sharing the scene's group each draw every picture and attempt
                //       no semaphore import, and the refusal is said once.
                // WHY-THIS-SHAPE: every picture from the trip on carries an unusable descriptor (a memfd),
                //       not a real semaphore: on NVIDIA one refused import makes the stub's own
                //       vkCreateSemaphore fail in this process, so "Real" would send nothing to drop. Each
                //       context made by the test after the trip, so none could have been marked before it;
                //       the item is moved into each, as in context-share.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, `std::atomic<bool> g_semaphoresRefused` replaced
                //       by a flag kept per current context (same load()/exchange()), via control_run.sh,
                //       2026-10-10, NVIDIA, reverted -> "FAIL: a refusal is said once for the process (3
                //       line(s))", "FAIL: an unshared context made after the refusal imports no semaphore and
                //       draws (4 drawn, 1 import(s))" and "FAIL: a context sharing the refusing one's group
                //       imports no semaphore and draws (4 drawn, 1 import(s); ...)".
                // COVERAGE-ASSERT: the unshared context is in a group of its own, the sharing one in the
                //       scene's, and every picture drawn in each carried a descriptor.
                const int served0 = stub.served();
                stub.sendAcquireFrom(served0 + 2, ExportingStub::Acquire::Unusable);
                const Drive run = drive(scene, stub, bus, 5);
                const quint64 importsAfterTrip = scene.semaphoreImports();
                QOpenGLContext* first = nullptr;
                scene.renderPass([&] { first = QOpenGLContext::currentContext(); });
                if (first == nullptr) {
                    wallpaperx::testing::harnessFailure("the scene graph left no context current during its pass");
                    return true;
                }
                auto* rootItem = qobject_cast<QQuickItem*>(scene.root.get());
                // Pictures drawn in `in` after the item moved there; `carried`: of the descriptors sent then.
                const auto visitIn = [&](Scene& in, int pictures, int& carried) {
                    const int unusable0 = stub.unusableAcquiresSent();
                    in.item = scene.item;  // not owned: the first scene's root still is
                    rootItem->setParentItem(in.window.contentItem());
                    pumpFor(20);
                    const Drive there = drive(in, stub, bus, pictures);
                    carried = stub.unusableAcquiresSent() - unusable0;
                    rootItem->setParentItem(scene.window.contentItem());
                    in.renderPass();
                    pumpFor(20);
                    return there;
                };
                Drive unshared;
                int unsharedCarried = 0;
                bool unsharedOwnGroup = false;
                {
                    Scene other;
                    other.window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
                    if (!other.control.initialize()) {
                        wallpaperx::testing::harnessFailure("a new scene graph could not be brought up");
                        return true;
                    }
                    QOpenGLContext* context = nullptr;
                    other.renderPass([&] { context = QOpenGLContext::currentContext(); });
                    unsharedOwnGroup = context != nullptr && context != first &&
                                       context->shareGroup() != first->shareGroup();
                    unshared = visitIn(other, 4, unsharedCarried);
                    other.control.invalidate();
                }
                const quint64 importsUnshared = scene.semaphoreImports() - importsAfterTrip;
                Drive sharing;
                int sharingCarried = 0;
                if (withSharingScene(first, [&](Scene& adopted) { sharing = visitIn(adopted, 4, sharingCarried); }) ==
                    nullptr) {
                    return true;
                }
                const quint64 importsSharing = scene.semaphoreImports() - importsAfterTrip - importsUnshared;
                const Drive back = drive(scene, stub, bus, 3);
                const quint64 importsBack = scene.semaphoreImports() - importsAfterTrip - importsUnshared - importsSharing;
                const int lines = tripLines();
                std::printf("item_timeline_test: refused-shared: %d drawn through the trip; unshared context (own group: "
                            "%s) %d drawn, %d descriptor(s), %llu import(s); sharing context %d drawn, %d descriptor(s), "
                            "%llu import(s); back %d drawn, %llu import(s); %d refusal line(s)\n",
                            run.drawn, unsharedOwnGroup ? "yes" : "no", unshared.drawn, unsharedCarried,
                            static_cast<unsigned long long>(importsUnshared), sharing.drawn, sharingCarried,
                            static_cast<unsigned long long>(importsSharing), back.drawn,
                            static_cast<unsigned long long>(importsBack), lines);
                std::fflush(stdout);
                if (!unsharedOwnGroup || unsharedCarried < unshared.drawn || sharingCarried < sharing.drawn ||
                    stub.unusableAcquiresSent() == 0) {
                    wallpaperx::testing::harnessFailure("a refusal followed by pictures carrying descriptors in an "
                                                        "unshared and a sharing context was not staged");
                    return true;
                }
                check(captureWorked() && lines == 1,
                      "a refusal is said once for the process (" + std::to_string(lines) + " line(s))");
                check(!run.stalled && !unshared.stalled && unshared.drawn == 4 && importsUnshared == 0,
                      "an unshared context made after the refusal imports no semaphore and draws (" +
                          std::to_string(unshared.drawn) + " drawn, " + std::to_string(importsUnshared) + " import(s))");
                check(!sharing.stalled && sharing.drawn == 4 && importsSharing == 0 && !back.stalled &&
                          back.drawn == 3 && importsBack == 0 && drawingCleanly(scene),
                      "a context sharing the refusing one's group imports no semaphore and draws (" +
                          std::to_string(sharing.drawn) + " drawn, " + std::to_string(importsSharing) +
                          " import(s); back " + std::to_string(back.drawn) + " drawn, " + std::to_string(importsBack) +
                          " import(s); " + describe(scene) + ")");
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
                //       (d) acquireOnGroupTimeline's `grantedBySender` made to ignore the owner (any sender
                //       matches a held import; via control_run.sh, 2026-10-10, NVIDIA, restart entry) -> "FAIL: and
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
                // CONTROL: in acquireOnGroupTimeline (backend/kde/wallpaperx_item.cpp), `grantedBySender` made
                //       to return false, so every value imports, via control_run.sh, 2026-10-10, NVIDIA -> "FAIL: every
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
                // CONTROL: acquireOnGroupTimeline's `grantedBySender` made to ignore the owner
                //       (backend/kde/wallpaperx_item.cpp), via control_run.sh, 2026-10-10, NVIDIA ->
                //       "FAIL: the new daemon's timeline is imported once and its values are waited on (2
                //       import(s) for 2 binary picture(s); 8 of 8 steady pictures as values, 8 wait(s); ...)".
                //       Reverted. The rebuilds under the new daemon are red only under the always-import
                //       control above ("0 import(s), 20 not waited"), the process having refused by then.
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
                // CONTROL: in ~FrameNode (backend/kde/wallpaperx_item.cpp) every share group's record cleared
                //       (`g_shareGroups.clear()` under g_shareGroupsMutex), via control_run.sh, 2026-10-10,
                //       NVIDIA -> "FAIL: after the scene graph is invalidated in the same context, the import is
                //       kept and every value waited (1 import(s), 6 value(s), 6 wait(s); ...)". Reverted.
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

                // GOAL: S15.442 -- a share group's record is tied to the group's destroyed(): its first import
                //       connects exactly one receiver there, and that receiver erases the group's record, so
                //       the next value picture in the group imports the timeline again. False if the record
                //       outlives the group, which a later group at the same address would inherit.
                // WHY-THIS-SHAPE: Qt allocates groups, so a later group at a freed address cannot be staged:
                //       the six-context loop below gets one in some runs and none in others. The group's real
                //       destruction leaves nothing to observe, so destroyed() is emitted on a live group of the
                //       test's own; only then can a fresh import show the record went. Emitted only when the
                //       group had no receiver before its first import, so nothing of Qt's sees it. Before the
                //       six-context loop, so a stale record there cannot have spoilt the item's timeline first.
                // CONTROL: in groupTimeline (backend/kde/wallpaperx_item.cpp), via control_run.sh, 2026-10-10,
                //       NVIDIA, reverted: (a) the QObject::connect to destroyed() deleted -> "FAIL: a group's
                //       first import connects one receiver to its destroyed() (0 receiver(s), 1 import(s))" and
                //       "FAIL: the group's destroyed() erases its record: its next value imports again (0
                //       import(s))"; (b) the erase_if predicate made never to match -> the second line alone.
                // COVERAGE-ASSERT: the group is not the scene's, had no receiver before, and both visits drew
                //       three pictures with at least one value (else reported as not staged).
                {
                    QOpenGLContext* own = nullptr;
                    scene.renderPass([&] { own = QOpenGLContext::currentContext(); });
                    Scene other;
                    other.window.setGeometry(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
                    if (!other.control.initialize()) {
                        wallpaperx::testing::harnessFailure("a new scene graph could not be brought up");
                        return true;
                    }
                    QOpenGLContext* context = nullptr;
                    other.renderPass([&] { context = QOpenGLContext::currentContext(); });
                    QOpenGLContextGroup* group = context == nullptr ? nullptr : context->shareGroup();
                    if (own == nullptr || group == nullptr || group == own->shareGroup()) {
                        wallpaperx::testing::harnessFailure("the new scene graph's context is not in a group of its own");
                        other.control.invalidate();
                        return true;
                    }
                    // A staging failure skips this block's checks but not the phase, so the loops below still
                    // grade whatever broke it.
                    const int receivers0 = destroyedReceivers(group);
                    const Visit first = visit(other);
                    const int receivers1 = destroyedReceivers(group);
                    if (receivers0 != 0 || first.there.stalled || first.there.drawn != 3 || first.values < 1) {
                        wallpaperx::testing::harnessFailure(
                            "a fresh group with no destroyed() receivers, visited with value pictures, was not staged (" +
                            std::to_string(receivers0) + " receiver(s) before; " + std::to_string(first.there.drawn) +
                            " drawn, " + std::to_string(first.values) + " value(s))");
                    } else {
                        check(first.importsThere == 1 && receivers1 == 1,
                              "a group's first import connects one receiver to its destroyed() (" +
                                  std::to_string(receivers1) + " receiver(s), " + std::to_string(first.importsThere) +
                                  " import(s))");
                        Q_EMIT group->destroyed(group);
                        const Visit second = visit(other);
                        std::printf("item_timeline_test: destroyed() on a live group: receivers %d -> %d; first visit "
                                    "%llu import(s), after the signal %llu import(s), %d drawn, %d value(s)\n",
                                    receivers0, receivers1, static_cast<unsigned long long>(first.importsThere),
                                    static_cast<unsigned long long>(second.importsThere), second.there.drawn,
                                    second.values);
                        std::fflush(stdout);
                        if (second.there.stalled || second.there.drawn != 3 || second.values < 1) {
                            wallpaperx::testing::harnessFailure("the visit after the signal drew no value picture, so "
                                                                "the erased record was not graded");
                        } else {
                            check(second.importsThere == 1,
                                  "the group's destroyed() erases its record: its next value imports again (" +
                                      std::to_string(second.importsThere) + " import(s))");
                        }
                    }
                    other.control.invalidate();
                }

                // GOAL: S15.420 -- each new unshared GL context (a share group of its own) imports the
                //       timeline exactly once and waits on the values drawn in it; moving the item back to
                //       a group that already holds the import costs nothing.
                // WHY-THIS-SHAPE: several contexts, each destroyed after its visit, so a record keyed by
                //       anything shared across groups (or not keyed at all) shows as a zero, and one
                //       re-made per picture as a three. Values and waits are summed over the visit,
                //       because which of its pictures lands in which window is the item's to decide.
                // CONTROL: in backend/kde/wallpaperx_item.cpp via control_run.sh, 2026-10-10, NVIDIA, each
                //       reverted: (a) groupTimeline's lookup made to match any record -> "FAIL: each new
                //       context imports the timeline once and waits on its values (6 of 6 wrong; first:
                //       context 0: 3 drawn, 3 import(s) there, 2 back; 0 value(s), 5 wait(s))" -- the first
                //       group's semaphore name fails its wait, and the item falls back to binary imports;
                //       (b) acquireOnGroupTimeline's `grantedBySender` made to return false -> "FAIL: moving
                //       back to the first context costs no import (6 of 6 wrong; ... 2 back ...)", with the
                //       invalidation check above and every S15.436/S15.438/S15.442 check red too.
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

                // GOAL: S15.436 -- a context that shares the scene's GL share group waits on the group's one
                //       timeline import: drawing value pictures there creates no semaphore and imports nothing,
                //       and every value drawn is waited on.
                // GOAL: S15.437 -- destroying a sharing context (not current, as the scene graph's teardown
                //       leaves it) deletes no semaphore: the import belongs to the group, which lives on.
                // GOAL: S15.438 -- after that destruction the sibling keeps waiting on the same import: its
                //       next three value pictures import nothing and are each waited on. (The group keeps every
                //       daemon's import while it lives: S15.440-441, graded on item_timeline_shared-replaced.)
                // WHY-THIS-SHAPE: the test makes the sharing context itself (setShareContext on the scene's own)
                //       and hands it to a second scene graph; the scene graph's own contexts share nothing.
                //       Graded on glCreateSemaphoresNV and glDeleteSemaphoresEXT calls (gl_semaphore_fault.h)
                //       and the import counter, not glIsSemaphoreEXT: a name freed and handed out again reads
                //       as alive. Three cycles, so a record re-made per context shows on the second one too.
                // CONTROL: in backend/kde/wallpaperx_item.cpp, via control_run.sh, 2026-10-10, NVIDIA, each
                //       reverted: (a) acquireOnGroupTimeline keying the record by the context
                //       (`reinterpret_cast<QOpenGLContextGroup*>(context)`) -> S15.436 red, "(cycle 0: 1
                //       semaphore(s) created, 1 import(s) there, 0 back; ...)" in all three cycles, with only
                //       S15.442's two checks red beside it; (b) re-run against `group->imports` via
                //       control_batch.sh: every import in `group->imports` deleted (glDeleteSemaphoresEXT,
                //       resolved through eglGetProcAddress) and the list cleared from each drawing context's
                //       aboutToBeDestroyed -> S15.437 "(cycle 0: 1 delete(s))" and
                //       S15.438 "(cycle 0: 1 import(s), 3 value(s), 3 wait(s); ...)" in all three cycles;
                //       (c) the group's record erased from each context's aboutToBeDestroyed -> S15.438 alone
                //       red, "(cycle 0: 1 import(s), ...)" in all three cycles.
                // COVERAGE-ASSERT: every cycle's sharing context is in the scene's group, drew 3 pictures there,
                //       and was not current when destroyed (else reported as not staged).
                QOpenGLContext* sibling = nullptr;
                scene.renderPass([&] { sibling = QOpenGLContext::currentContext(); });
                if (sibling == nullptr) {
                    wallpaperx::testing::harnessFailure("the scene graph left no context current during its pass");
                    return true;
                }
                const auto created = [] { return recorded(&wallpaperx::testing::GlSemaphoreFaults::timelinesCreated); };
                const auto deletedTotal = [] {
                    return recorded(&wallpaperx::testing::GlSemaphoreFaults::semaphoresDeleted);
                };
                struct Sharing {
                    std::unique_ptr<QOpenGLContext> context;
                    size_t timelines = 0;  // semaphores created while it was visited
                    Visit visit;
                };
                const auto visitSharing = [&] {
                    Sharing out;
                    out.context = withSharingScene(sibling, [&](Scene& adopted) {
                        const size_t created0 = created();
                        out.visit = visit(adopted);
                        out.timelines = created() - created0;
                    });
                    return out;
                };
                for (int cycle = 0; cycle < 3; ++cycle) {
                    Sharing made = visitSharing();
                    if (made.context == nullptr) return true;
                    const Visit& v = made.visit;
                    QOpenGLContext* shared = made.context.get();
                    bool currentAtDestroy = false;
                    QObject::connect(shared, &QOpenGLContext::aboutToBeDestroyed,
                                     [&] { currentAtDestroy = QOpenGLContext::currentContext() == shared; });
                    const size_t deleted0 = deletedTotal();
                    made.context.reset();
                    const size_t deletedAtDestroy = deletedTotal() - deleted0;
                    const quint64 semaphores0 = scene.semaphoreImports();
                    const int waits0 = g_glSemaphoreFaults.waitsSeen.load();
                    const int values0 = stub.valuesSent();
                    const Drive after = drive(scene, stub, bus, 3);
                    const quint64 importsAfter = scene.semaphoreImports() - semaphores0;
                    const int valuesAfter = stub.valuesSent() - values0;
                    const int waitsAfter = g_glSemaphoreFaults.waitsSeen.load() - waits0;
                    std::printf("item_timeline_test: cycle %d: sharing context: %zu semaphore(s) created, %llu import(s), "
                                "%d drawn, %d value(s), %d wait(s); destroyed while current: %s, %zu delete(s) then; "
                                "the sibling after: %llu import(s), %d value(s), %d wait(s)\n",
                                cycle, made.timelines, static_cast<unsigned long long>(v.importsThere), v.there.drawn,
                                v.values, v.waits, currentAtDestroy ? "yes" : "no", deletedAtDestroy,
                                static_cast<unsigned long long>(importsAfter), valuesAfter, waitsAfter);
                    std::fflush(stdout);
                    if (v.there.stalled || v.there.drawn != 3 || currentAtDestroy) {
                        wallpaperx::testing::harnessFailure("the sharing context did not draw three pictures and go "
                                                            "while not current, so nothing here was staged");
                        return true;
                    }
                    check(made.timelines == 0 && v.importsThere == 0 && v.importsBack == 0 && v.values >= 3 &&
                              v.waits >= v.values,
                          "a context sharing the group waits on the group's import and makes none (cycle " +
                              std::to_string(cycle) + ": " + std::to_string(made.timelines) + " semaphore(s) created, " +
                              std::to_string(v.importsThere) + " import(s) there, " + std::to_string(v.importsBack) +
                              " back; " + std::to_string(v.values) + " value(s), " + std::to_string(v.waits) +
                              " wait(s))");
                    check(deletedAtDestroy == 0, "destroying a sharing context deletes no semaphore (cycle " +
                                                     std::to_string(cycle) + ": " + std::to_string(deletedAtDestroy) +
                                                     " delete(s))");
                    check(!after.stalled && importsAfter == 0 && valuesAfter == 3 && waitsAfter >= 3 &&
                              drawingCleanly(scene),
                          "the sibling goes on waiting on the same import after a sharing context goes (cycle " +
                              std::to_string(cycle) + ": " + std::to_string(importsAfter) + " import(s), " +
                              std::to_string(valuesAfter) + " value(s), " + std::to_string(waitsAfter) + " wait(s); " +
                              describe(scene) + ")");
                }

                const int lines = tripLines();
                check(captureWorked() && lines == 0, "no semaphore import was refused (" + std::to_string(lines) +
                                                         " line(s))");
                return true;
            }

            case Phase::SharedReplaced: {
                // Three items in one share group: X, the phase's item, in the scene's context, and Y and Z
                // on the second monitor, each in a context the test makes to share it. Y's and Z's requests
                // to daemon A are held while daemon B takes the name and X draws B's value pictures; one
                // held reply is let go; daemon C takes the name and X draws C's; then the other is let go.
                // Graded on the names glWaitSemaphoreEXT and glDeleteSemaphoresEXT were given
                // (gl_semaphore_fault.h), against the names glCreateSemaphoresNV handed back: A, B, C.
                if (!reachTimeline(6)) {
                    check(false, "the item reaches the timeline within six pictures");
                    return true;
                }
                QOpenGLContext* sibling = nullptr;
                scene.renderPass([&] { sibling = QOpenGLContext::currentContext(); });
                if (sibling == nullptr) {
                    wallpaperx::testing::harnessFailure("the scene graph left no context current during its pass");
                    return true;
                }
                using NamedCall = wallpaperx::testing::GlSemaphoreFaults::NamedCall;
                const auto timelineCreated = [](size_t index) -> NamedCall {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    const auto& created = g_glSemaphoreFaults.timelinesCreated;
                    return index < created.size() ? created[index] : NamedCall{0, EGL_NO_CONTEXT};
                };
                // Deletes of the timeline's name from its creation on.
                const auto deletesOf = [](const NamedCall& timeline) {
                    int count = 0;
                    for (GLuint deleted :
                         namesSince(&wallpaperx::testing::GlSemaphoreFaults::semaphoresDeleted, timeline.deletesBefore)) {
                        if (deleted == timeline.name) ++count;
                    }
                    return count;
                };
                const auto onlyName = [](const std::vector<GLuint>& names, GLuint name) {
                    return std::all_of(names.begin(), names.end(), [name](GLuint n) { return n == name; });
                };
                const auto staged = [](bool ok, const std::string& what) {
                    if (!ok) wallpaperx::testing::harnessFailure("not staged: " + what);
                    return ok;
                };
                const auto announceY = [&] { announce(bus, stub.nextGeneration(), wallpaperx::testing::kSecondMonitor); };
                struct DropItem {
                    Scene& scene;
                    ~DropItem() {
                        scene.root.reset();
                        scene.renderPass();
                    }
                };
                // Puts an item on the second monitor into `ys` and draws value pictures on A's timeline there.
                const auto onSecondMonitor = [&](Scene& ys, const std::string& which) {
                    if (!loadSecondMonitorItem(ys, "item_timeline_y.qml")) return false;
                    (void)pumpUntil([&] { ys.renderPass(); return itemTexture(ys.item) != nullptr; }, 10000);
                    if (!staged(itemTexture(ys.item) != nullptr && settleQuiet(ys, stub),
                                which + " never drew A's picture and went quiet")) {
                        return false;
                    }
                    int values = 0;
                    for (int i = 0; i < 8 && values < 2; ++i) {
                        const int values0 = stub.valuesSent();
                        const quint64 before = ys.pictures();
                        announceY();
                        if (!pumpUntil([&] { ys.renderPass(); return ys.pictures() > before; }, 5000)) break;
                        if (stub.valuesSent() > values0) ++values;
                    }
                    return staged(values > 0, which + " drew no value picture on A's timeline");
                };
                const auto inYZ = [&](Scene& ys, Scene& zs) {
                    if (!staged(settleQuiet(ys, stub) && settleQuiet(zs, stub), "Y and Z did not go quiet")) return;
                    // Both requests held from one announcement: a second one while the first is held would
                    // leave that item a newer generation to fetch from B straight after its late picture.
                    const int onTimeline0 = stub.onTimelineRequests();
                    stub.holdEveryReply(true);
                    announceY();
                    const bool bothAsked = pumpUntil([&] { return stub.heldReplies() >= 2; }, 3000);
                    pumpFor(100);
                    stub.holdEveryReply(false);
                    if (!staged(bothAsked && stub.heldReplies() == 2 && stub.onTimelineRequests() - onTimeline0 == 2,
                                "Y's and Z's requests to A were not both held on A's timeline (" +
                                    std::to_string(stub.heldReplies()) + " held)")) {
                        return;
                    }

                    SuccessorDaemon successor(device, ring);
                    if (!staged(successor.ok && successor.takeNameFrom(bus), "daemon B never took the name")) return;
                    pumpFor(50);
                    (void)pumpUntil([&] { scene.renderPass(); return false; }, 200);
                    (void)settleQuiet(scene, successor.stub);
                    const Drive reachB = drive(scene, successor.stub, successor.bus, 8,
                                               [&] { return successor.stub.valuesSent() > 0; });
                    (void)drive(scene, successor.stub, successor.bus, 1);
                    if (!staged(!reachB.stalled && successor.stub.valuesSent() > 0 &&
                                    recorded(&wallpaperx::testing::GlSemaphoreFaults::timelinesCreated) == 2,
                                "the first item never drew B's value pictures on a second timeline")) {
                        return;
                    }
                    const NamedCall a = timelineCreated(0);
                    const NamedCall b = timelineCreated(1);

                    // GOAL: S15.440 -- an earlier daemon's late value, drawn in another context of the group
                    //       after B's first value picture there, is waited on A's import: no import, and no
                    //       wait on any other semaphore (B's included).
                    // WHY-THIS-SHAPE: graded by the waited name, not pictures or a wait count: without A's
                    //       import the late hand-over fails and the re-fetch from B is drawn and waited on, so
                    //       both counters still rise. Both Y and Z are rendered: which held reply is oldest
                    //       depends on which item asked first, so the one that draws is the one graded.
                    // CONTROL: in acquireOnGroupTimeline (backend/kde/wallpaperx_item.cpp), via control_batch.sh,
                    //       2026-10-10, NVIDIA, reverted: a found import waited on as the group's newest ->
                    //       "FAIL: A's late value in a sibling context after B is waited on A's import (A is 1,
                    //       waited on 2, 0 import(s); ...)"; the entry then timed out at exit, the GPU waiting
                    //       on a value B's timeline never reaches.
                    // COVERAGE-ASSERT: a late picture drawn with at least one wait, else not staged.
                    size_t waited0 = recorded(&wallpaperx::testing::GlSemaphoreFaults::semaphoresWaited);
                    const quint64 imports0 = ys.semaphoreImports() + zs.semaphoreImports();
                    const quint64 yPictures0 = ys.pictures();
                    const quint64 zPictures0 = zs.pictures();
                    (void)stub.sendOldestHeld();
                    bool yDrew = false;
                    bool zDrew = false;
                    (void)pumpUntil(
                        [&] {
                            ys.renderPass();
                            zs.renderPass();
                            yDrew = ys.pictures() > yPictures0;
                            zDrew = zs.pictures() > zPictures0;
                            return yDrew || zDrew;
                        },
                        3000);
                    Scene* const early = yDrew == zDrew ? nullptr : yDrew ? &ys : &zs;
                    std::vector<GLuint> waited = namesSince(&wallpaperx::testing::GlSemaphoreFaults::semaphoresWaited, waited0);
                    quint64 imports = ys.semaphoreImports() + zs.semaphoreImports() - imports0;
                    std::printf("item_timeline_test: shared-replaced: timelines A %u, B %u; the late picture after B "
                                "drawn by %s, waited on [%s], %llu import(s)\n",
                                a.name, b.name, early == &ys ? "Y" : early == &zs ? "Z" : "neither",
                                listNames(waited).c_str(), static_cast<unsigned long long>(imports));
                    std::fflush(stdout);
                    if (!staged(early != nullptr && !waited.empty(),
                                "not exactly one of Y and Z drew a late picture after B, with a wait")) {
                        return;
                    }
                    check(onlyName(waited, a.name) && imports == 0 && drawingCleanly(*early),
                          "A's late value in a sibling context after B is waited on A's import (A is " +
                              std::to_string(a.name) + ", waited on " + listNames(waited) + ", " +
                              std::to_string(imports) + " import(s); " + describe(*early) + ")");

                    SuccessorDaemon third(device, ring, QStringLiteral("wallpaperx-timeline-c"));
                    if (!staged(third.ok && third.takeNameFrom(successor.bus), "daemon C never took the name")) return;
                    pumpFor(50);
                    (void)pumpUntil([&] { scene.renderPass(); return false; }, 200);
                    (void)settleQuiet(scene, third.stub);
                    const Drive reachC =
                        drive(scene, third.stub, third.bus, 8, [&] { return third.stub.valuesSent() > 0; });
                    (void)drive(scene, third.stub, third.bus, 1);
                    if (!staged(!reachC.stalled && third.stub.valuesSent() > 0 &&
                                    recorded(&wallpaperx::testing::GlSemaphoreFaults::timelinesCreated) == 3,
                                "the first item never drew C's value pictures on a third timeline")) {
                        return;
                    }
                    const NamedCall c = timelineCreated(2);

                    // GOAL: S15.440 -- the same after two replacements: A's late value, let go after C's
                    //       first value picture in the group, is waited on A's import, with no import.
                    // WHY-THIS-SHAPE: as above; only the item still held is rendered, so the other's own
                    //       fetches from B or C cannot put their waits in the window.
                    // CONTROL: in acquireOnGroupTimeline (backend/kde/wallpaperx_item.cpp), via control_batch.sh,
                    //       2026-10-10, NVIDIA, reverted: the oldest import deleted (glDeleteSemaphoresEXT, resolved
                    //       through eglGetProcAddress) and erased when a third is appended -> "FAIL: A's late value in a sibling context after B and C is waited on
                    //       A's import (A is 1, waited on 3, 0 import(s); ...)", the check above staying green.
                    //       The newest-import break above turns this red too ("waited on 3").
                    // COVERAGE-ASSERT: a late picture drawn with at least one wait, else not staged.
                    Scene& late = early == &ys ? zs : ys;
                    waited0 = recorded(&wallpaperx::testing::GlSemaphoreFaults::semaphoresWaited);
                    const quint64 lateImports0 = late.semaphoreImports();
                    const quint64 latePictures0 = late.pictures();
                    (void)stub.sendOldestHeld();
                    const bool lateDrawn =
                        pumpUntil([&] { late.renderPass(); return late.pictures() > latePictures0; }, 3000);
                    waited = namesSince(&wallpaperx::testing::GlSemaphoreFaults::semaphoresWaited, waited0);
                    imports = late.semaphoreImports() - lateImports0;
                    std::printf("item_timeline_test: shared-replaced: timeline C %u; the late picture after C drawn %s, "
                                "waited on [%s], %llu import(s)\n",
                                c.name, lateDrawn ? "yes" : "no", listNames(waited).c_str(),
                                static_cast<unsigned long long>(imports));
                    std::fflush(stdout);
                    if (!staged(lateDrawn && !waited.empty(), "the late picture after C was not drawn with a wait")) return;
                    check(onlyName(waited, a.name) && imports == 0 && drawingCleanly(late),
                          "A's late value in a sibling context after B and C is waited on A's import (A is " +
                              std::to_string(a.name) + ", waited on " + listNames(waited) + ", " +
                              std::to_string(imports) + " import(s); " + describe(late) + ")");

                    // GOAL: S15.441 -- no group timeline is deleted by hand: through B's and C's first imports
                    //       and both late pictures, A, B and C are each deleted 0 times.
                    // WHY-THIS-SHAPE: each name counted from its own creation (deletesBefore, taken under the
                    //       recorder's lock), not from a fixed point: the binary per-picture semaphores are
                    //       deleted too, and one may have held B's or C's name before it became a timeline.
                    //       A name cannot be handed out again while its timeline lives, so any delete of it
                    //       from creation on means the timeline was deleted.
                    // CONTROL: the oldest-import break above -> "FAIL: no group timeline is deleted by hand (A
                    //       deleted 1, B 0, C 0 time(s) since each was made)".
                    const int aDeleted = deletesOf(a);
                    const int bDeleted = deletesOf(b);
                    const int cDeleted = deletesOf(c);
                    check(aDeleted == 0 && bDeleted == 0 && cDeleted == 0,
                          "no group timeline is deleted by hand (A deleted " + std::to_string(aDeleted) + ", B " +
                              std::to_string(bDeleted) + ", C " + std::to_string(cDeleted) +
                              " time(s) since each was made)");
                };
                const auto inY = [&](Scene& ys) {
                    DropItem dropY{ys};
                    if (!onSecondMonitor(ys, "Y")) return;
                    if (withSharingScene(sibling, [&](Scene& zs) {
                            DropItem dropZ{zs};
                            if (onSecondMonitor(zs, "Z")) inYZ(ys, zs);
                        }) == nullptr) {
                        return;
                    }
                };
                if (withSharingScene(sibling, inY) == nullptr) return true;
                const int lines = tripLines();
                check(captureWorked() && lines == 0, "no semaphore import was refused (" + std::to_string(lines) +
                                                         " line(s))");
                return true;
            }

            case Phase::SharedThreads: {
                // Items P and Q on the second monitor, each in a scene graph on a render thread of its own,
                // in a share group the test makes (not the phase scene's, which is not drawn here). Each
                // round P syncs and is held mid-frame while Q runs a whole frame, then P ends its frame, so
                // Q's calls land after P's wait and before anything Qt does at P's endFrame(). Graded on
                // the recorded order of semaphore calls and of glFlush/glFinish (gl_semaphore_fault.h).
                using Faults = wallpaperx::testing::GlSemaphoreFaults;
                ThreadedScene p;
                ThreadedScene q;
                p.context = std::make_unique<QOpenGLContext>();
                q.context = std::make_unique<QOpenGLContext>();
                q.context->setShareContext(p.context.get());
                if (!p.context->create() || !q.context->create() ||
                    q.context->shareGroup() != p.context->shareGroup()) {
                    wallpaperx::testing::harnessFailure("two contexts in one share group could not be made");
                    return true;
                }
                if (!p.bringUp() || !q.bringUp()) {
                    wallpaperx::testing::harnessFailure("a scene graph would not come up on a render thread of its own");
                    return true;
                }
                if (!loadSecondMonitorItem(p.scene, "item_timeline_p.qml") ||
                    !loadSecondMonitorItem(q.scene, "item_timeline_q.qml")) {
                    return true;
                }
                struct StopRecording {
                    ~StopRecording() { g_glSemaphoreFaults.recordOrder.store(false); }
                } stopRecording;
                {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    g_glSemaphoreFaults.callOrder.clear();
                }
                g_glSemaphoreFaults.recordOrder.store(true);
                if (!pumpUntil(
                        [&] {
                            p.pass();
                            q.pass();
                            return p.scene.pictures() > 0 && q.scene.pictures() > 0;
                        },
                        10000)) {
                    wallpaperx::testing::harnessFailure("the items on the render threads never drew a first picture");
                    return true;
                }
                // GOAL: the flush and finish checks below grade the plugin only where the recorder can see
                //       its glFlush and glFinish at all.
                // WHY-THIS-SHAPE: a resolution flag, not "finishes > 0": the plugin is meant to call no
                //       glFinish, so a count would turn the finish control into "not verified".
                // CONTROL: eglGetProcAddress in gl_semaphore_fault.h made to return nullptr for glFlush and
                //       glFinish, as a stack without core names there (Qt then uses dlsym), by hand, 2026-10-10,
                //       NVIDIA, reverted -> "NOT VERIFIED -- ... glFlush and glFinish did not resolve through
                //       eglGetProcAddress", a skip; with this gate also disabled the same run blamed the plugin:
                //       "FAIL: a wait on a group timeline is flushed ... (33 of 33 not ...)" and the failed-wait FAIL.
                if (!g_glSemaphoreFaults.flushWrapped.load() || !g_glSemaphoreFaults.finishWrapped.load()) {
                    notVerified(kGoal, "glFlush and glFinish did not resolve through eglGetProcAddress (Qt then "
                                       "looks them up with dlsym), so the recorder cannot see the plugin's flushes");
                    return true;
                }

                constexpr int kRounds = 16;
                for (int r = 0; r < kRounds; ++r) {
                    const quint64 p0 = p.scene.pictures();
                    const quint64 q0 = q.scene.pictures();
                    const int served0 = stub.served();
                    announce(bus, stub.nextGeneration(), wallpaperx::testing::kSecondMonitor);
                    (void)pumpUntil([&] { return stub.served() >= served0 + 2; }, 3000);
                    pumpFor(50);
                    p.sync();
                    q.pass();
                    p.end();
                    if (!pumpUntil(
                            [&] {
                                if (p.scene.pictures() > p0 && q.scene.pictures() > q0) return true;
                                p.pass();
                                q.pass();
                                return false;
                            },
                            5000)) {
                        wallpaperx::testing::harnessFailure("round " + std::to_string(r) +
                                                            ": the two items did not both draw the announced picture");
                        return true;
                    }
                }
                // One more round in which P's wait fails after its value was set; disarmed before Q runs.
                size_t faultMark = 0;
                {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    faultMark = g_glSemaphoreFaults.callOrder.size();
                }
                const int failed0 = g_glSemaphoreFaults.waitsFailed.load();
                {
                    const int served0 = stub.served();
                    announce(bus, stub.nextGeneration(), wallpaperx::testing::kSecondMonitor);
                    (void)pumpUntil([&] { return stub.served() >= served0 + 2; }, 3000);
                    pumpFor(50);
                    g_glSemaphoreFaults.failWaits.store(1);
                    p.sync();
                    g_glSemaphoreFaults.failWaits.store(0);
                    q.pass();
                    p.end();
                }
                const int faultsFired = g_glSemaphoreFaults.waitsFailed.load() - failed0;
                g_glSemaphoreFaults.recordOrder.store(false);
                std::vector<Faults::OrderedCall> calls;
                {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    calls = g_glSemaphoreFaults.callOrder;
                }

                // Which calls name a live timeline of this group: from its glCreateSemaphoresNV on one of
                // the two render threads until a delete of it.
                const auto ours = [&](const Faults::OrderedCall& c) {
                    return c.thread == p.renderThread || c.thread == q.renderThread;
                };
                std::vector<bool> onTimeline(calls.size(), false);
                {
                    std::set<GLuint> live;
                    for (size_t i = 0; i < calls.size(); ++i) {
                        if (!ours(calls[i])) continue;
                        if (calls[i].call == Faults::Call::CreateTimeline) live.insert(calls[i].name);
                        if (calls[i].call == Faults::Call::Delete) live.erase(calls[i].name);
                        onTimeline[i] = calls[i].name != 0 && live.count(calls[i].name) != 0;
                    }
                }
                // The first call after `i` by the other render thread on the same timeline that sets a value
                // (or, with `waits`, also waits); calls.size() when there is none.
                const auto nextUseElsewhere = [&](size_t i, bool waits) {
                    for (size_t k = i + 1; k < calls.size(); ++k) {
                        const Faults::OrderedCall& c = calls[k];
                        if (!ours(c) || c.thread == calls[i].thread || !onTimeline[k] || c.name != calls[i].name) continue;
                        if (c.call == Faults::Call::SetValue || (waits && c.call == Faults::Call::Wait)) return k;
                    }
                    return calls.size();
                };
                const auto sameThreadBetween = [&](size_t i, size_t j, Faults::Call call) {
                    for (size_t k = i + 1; k < j; ++k) {
                        if (calls[k].thread == calls[i].thread && calls[k].call == call) return true;
                    }
                    return false;
                };
                const auto who = [&](std::thread::id t) { return t == p.renderThread ? "P" : "Q"; };

                std::set<GLuint> imported;
                int importsGraded = 0;
                // Each import or value-set on a group timeline opens a section the plugin holds the group lock
                // across; on its thread the next glFlush closes it (waitOn flushes before the lock goes).
                int sectionsGraded = 0;
                int importSections = 0;
                int sectionsFinished = 0;
                std::string firstFinished;
                int waitsGraded = 0;
                // Graded waits whose sibling's set came before the waiting thread ended its frame: only there
                // can nothing of Qt's (it flushes at endFrame()) stand in for the plugin's flush.
                int waitsTight = 0;
                int waitsUnflushed = 0;
                std::string firstUnflushed;
                for (size_t i = 0; i < calls.size(); ++i) {
                    if (!onTimeline[i] || !ours(calls[i])) continue;
                    if (calls[i].call == Faults::Call::Import || calls[i].call == Faults::Call::SetValue) {
                        ++sectionsGraded;
                        if (calls[i].call == Faults::Call::Import) ++importSections;
                        for (size_t k = i + 1; k < calls.size(); ++k) {
                            if (calls[k].thread != calls[i].thread) continue;
                            if (calls[k].call == Faults::Call::Flush) break;
                            if (calls[k].call != Faults::Call::Finish) continue;
                            if (++sectionsFinished == 1) {
                                firstFinished = std::string(who(calls[i].thread)) +
                                                (calls[i].call == Faults::Call::Import ? " imported " : " set a value on ") +
                                                std::to_string(calls[i].name) + " at call " + std::to_string(i) +
                                                ", glFinish at call " + std::to_string(k);
                            }
                            break;
                        }
                    }
                    if (calls[i].call == Faults::Call::Import) {
                        imported.insert(calls[i].name);
                        if (nextUseElsewhere(i, true) != calls.size()) ++importsGraded;
                    } else if (calls[i].call == Faults::Call::Wait) {
                        const size_t j = nextUseElsewhere(i, false);
                        if (j == calls.size()) continue;
                        ++waitsGraded;
                        if (!sameThreadBetween(i, j, Faults::Call::FrameEnd)) ++waitsTight;
                        if (!sameThreadBetween(i, j, Faults::Call::Flush)) {
                            if (++waitsUnflushed == 1) {
                                firstUnflushed = std::string(who(calls[i].thread)) + " waited on " +
                                                 std::to_string(calls[i].name) + " at call " + std::to_string(i) +
                                                 ", " + who(calls[j].thread) + " set a value at call " +
                                                 std::to_string(j);
                            }
                        }
                    }
                }
                const GLuint shared = imported.size() == 1 ? *imported.begin() : 0;
                int pWaits = 0;
                int qWaits = 0;
                int flushes = 0;
                int finishes = 0;
                for (size_t i = 0; i < calls.size(); ++i) {
                    if (ours(calls[i]) && calls[i].call == Faults::Call::Flush) ++flushes;
                    if (ours(calls[i]) && calls[i].call == Faults::Call::Finish) ++finishes;
                    if (calls[i].call != Faults::Call::Wait || calls[i].name != shared || !onTimeline[i]) continue;
                    if (calls[i].thread == p.renderThread) ++pWaits;
                    if (calls[i].thread == q.renderThread) ++qWaits;
                }
                // P's wait made to fail: its first from the fault round on. Graded up to whichever comes first of
                // P ending its frame (Qt flushes there) and Q's next value-set or wait on that name.
                size_t faulted = calls.size();
                for (size_t i = faultMark; i < calls.size(); ++i) {
                    if (calls[i].thread == p.renderThread && calls[i].call == Faults::Call::Wait) {
                        faulted = i;
                        break;
                    }
                }
                bool faultSetFirst = false;
                bool faultFlushed = false;
                size_t faultBound = calls.size();
                std::string faultBoundBy = "the end of the record";
                if (faulted != calls.size()) {
                    for (size_t k = faultMark; k < faulted; ++k) {
                        faultSetFirst = faultSetFirst || (calls[k].thread == p.renderThread &&
                                                          calls[k].call == Faults::Call::SetValue &&
                                                          calls[k].name == calls[faulted].name);
                    }
                    for (size_t k = faulted + 1; k < calls.size() && faultBound == calls.size(); ++k) {
                        const Faults::OrderedCall& c = calls[k];
                        if (c.thread == p.renderThread && c.call == Faults::Call::FrameEnd) {
                            faultBound = k;
                            faultBoundBy = "P ended its frame";
                        } else if (c.thread == q.renderThread && c.name == calls[faulted].name &&
                                   (c.call == Faults::Call::SetValue || c.call == Faults::Call::Wait)) {
                            faultBound = k;
                            faultBoundBy = "Q used the name";
                        }
                    }
                    faultFlushed = sameThreadBetween(faulted, faultBound, Faults::Call::Flush);
                }
                std::printf("item_timeline_test: shared-threads: %zu call(s) recorded, %zu timeline import(s) (shared "
                            "name %u); waits on it: P %d, Q %d; %d glFlush, %d glFinish on the render threads; locked "
                            "sections graded %d (%d imports, %d imports used by the other thread), with a glFinish %d; "
                            "waits graded %d, %d inside the waiter's frame, unflushed %d; failed wait: %d fired, at call "
                            "%zu, value set first %d, bounded at call %zu by %s, flushed %d\n",
                            calls.size(), imported.size(), shared, pWaits, qWaits, flushes, finishes, sectionsGraded,
                            importSections, importsGraded, sectionsFinished, waitsGraded, waitsTight, waitsUnflushed,
                            faultsFired, faulted, faultSetFirst ? 1 : 0, faultBound, faultBoundBy.c_str(),
                            faultFlushed ? 1 : 0);
                std::fflush(stdout);

                // GOAL: S15.443 -- the plugin calls no glFinish while it holds a share group's timeline lock: on
                //       the thread that imported a group timeline or set a value on one, no glFinish comes before
                //       that thread's next glFlush, which ends the locked section.
                // WHY-THIS-SHAPE: graded on the recorded call order, not on a stall: a glFinish only blocks when
                //       an earlier wait of the context is stuck, which this phase does not stage (shared-stuck
                //       does, on behaviour). Qt makes no glFinish on these threads, so a Finish in the section
                //       can only be the plugin's.
                // CONTROL: in acquireOnGroupTimeline (backend/kde/wallpaperx_item.cpp), via control_run.sh,
                //       2026-10-10, NVIDIA, reverted: gl->glFinish() put back after importTimeline -> "FAIL: the
                //       plugin calls no glFinish while it holds a share group's timeline lock (1 of 35 sections had
                //       one; first: P imported 1 at call 15, glFinish at call 16)".
                // COVERAGE-ASSERT: at least one import section and three sections in all graded, else not staged.
                // GOAL: S15.443 -- every wait on a group timeline is followed on the waiting thread by glFlush
                //       before the other thread sets a value on that name.
                // WHY-THIS-SHAPE: the sibling's set must land before the waiter's endFrame(), where Qt flushes
                //       anyway; left to the scheduler it lands after, and the check passes without the plugin's
                //       flush (16 of the 33 graded waits here are such). Hence P held mid-frame, and a FrameEnd
                //       mark recorded by the render thread itself, not a position the test assumes.
                // CONTROL: in acquireOnGroupTimeline's waitOn (backend/kde/wallpaperx_item.cpp), via
                //       control_run.sh, 2026-10-10, NVIDIA, reverted: the glFlush deleted -> "FAIL: a wait on a
                //       group timeline is flushed on its thread before the other thread sets a value (17 of 33
                //       not; first: P waited on 1 at call 21, Q set a value at call 22)".
                // COVERAGE-ASSERT: both threads waited on the shared name, and at least three graded waits had
                //       the sibling's set before the waiter's FrameEnd, else not staged. Controlled by ending P's
                //       frame before Q's pass: "0 inside the waiter's frame" and the not-staged line.
                const bool staged = shared != 0 && pWaits > 0 && qWaits > 0 && importsGraded > 0 && waitsTight >= 3 &&
                                    importSections > 0 && sectionsGraded >= 3;
                if (!staged) {
                    wallpaperx::testing::harnessFailure(
                        "not staged: one timeline import used by both render threads, and at least three waits whose "
                        "sibling set a value inside the waiter's frame, were not recorded (see the line above)");
                    return true;
                }
                check(sectionsFinished == 0,
                      "the plugin calls no glFinish while it holds a share group's timeline lock (" +
                          std::to_string(sectionsFinished) + " of " + std::to_string(sectionsGraded) +
                          " sections had one; first: " + firstFinished + ")");
                check(waitsUnflushed == 0,
                      "a wait on a group timeline is flushed on its thread before the other thread sets a value (" +
                          std::to_string(waitsUnflushed) + " of " + std::to_string(waitsGraded) + " not; first: " +
                          firstUnflushed + ")");

                // GOAL: S15.444 -- a wait on a group timeline that fails after its value was set is still
                //       flushed on its thread before the lock goes: before that thread ends its frame and before
                //       the other thread's next value-set or wait on the name.
                // WHY-THIS-SHAPE: bounded by P's FrameEnd as well as by Q's use: after a failed wait the plugin
                //       may stop using the timeline, so Q's use alone could leave no bound and Qt's endFrame()
                //       flush would then pass for the plugin's. The fault is armed only across P's sync.
                // CONTROL: in acquireOnGroupTimeline's waitOn (backend/kde/wallpaperx_item.cpp), via
                //       control_run.sh, 2026-10-10, NVIDIA, reverted: glFlush only when acquire returned true ->
                //       "FAIL: a failed wait on a group timeline is flushed on its thread before Q used the name
                //       (wait at call 177, bound at call 178)".
                // COVERAGE-ASSERT: exactly one wait failed, P's, on the shared name, with P's value-set on that
                //       name before it, else not staged.
                if (faultsFired != 1 || faulted == calls.size() || calls[faulted].name != shared || !faultSetFirst) {
                    wallpaperx::testing::harnessFailure(
                        "not staged: P's wait on the shared timeline, after setting its value, did not fail exactly once "
                        "(see the failed-wait part of the line above)");
                } else {
                    check(faultFlushed, "a failed wait on a group timeline is flushed on its thread before " +
                                            faultBoundBy + " (wait at call " + std::to_string(faulted) +
                                            ", bound at call " + std::to_string(faultBound) + ")");
                }
                const int lines = tripLines();
                check(captureWorked() && lines == 0, "no semaphore import was refused (" + std::to_string(lines) +
                                                         " line(s))");
                return true;
            }

            case Phase::SharedStuck: {
                // P (monitor 1) and Q (monitor 2) on render threads of their own, one share group, both on A's
                // own timeline. P then waits on a value A holds back on the GPU (the gate), B takes the name, and
                // both items are handed a value on B's timeline. P's frame imports B's timeline into the group
                // while its context is stuck; Q's frame must still get through. B serves from a Vulkan device of
                // its own: the gate holds A's queue, and B's every value is waited for on its device.
                using Faults = wallpaperx::testing::GlSemaphoreFaults;
                constexpr int kThirdMonitor = 2;
                constexpr int kSiblingBoundMs = 3000;
                wallpaperx::Device deviceB(/*enableValidation=*/true);
                grade.watch(deviceB);
                wallpaperx::OffscreenTargetManager targetsB(deviceB);
                std::vector<const wallpaperx::OffscreenTarget*> ringB;
                for (size_t i = 0; i < 2; ++i) {
                    const wallpaperx::OffscreenTarget& target = targetsB.create(static_cast<int>(20 + i), kWidth, kHeight);
                    publishSolidFrame(deviceB, targetsB, target, "item_timeline_stuck_" + std::to_string(i) + ".png",
                                      kPalette[i]);
                    ringB.push_back(&target);
                }
                ThreadedScene p;
                ThreadedScene q;
                p.context = std::make_unique<QOpenGLContext>();
                q.context = std::make_unique<QOpenGLContext>();
                q.context->setShareContext(p.context.get());
                if (!p.context->create() || !q.context->create() ||
                    q.context->shareGroup() != p.context->shareGroup()) {
                    wallpaperx::testing::harnessFailure("two contexts in one share group could not be made");
                    return true;
                }
                if (!p.bringUp() || !q.bringUp()) {
                    wallpaperx::testing::harnessFailure("a scene graph would not come up on a render thread of its own");
                    return true;
                }
                if (!loadSecondMonitorItem(p.scene, "item_timeline_p.qml") ||
                    !loadSecondMonitorItem(q.scene, "item_timeline_q.qml", kThirdMonitor)) {
                    return true;
                }
                // Destroyed before P and Q, whose teardown runs a frame on each: the held value is reached first.
                struct ReleaseLate {
                    ExportingStub& stub;
                    ~ReleaseLate() {
                        std::string ignored;
                        (void)stub.signalLateValue(ignored);
                    }
                } releaseLate{stub};
                struct StopRecording {
                    ~StopRecording() { g_glSemaphoreFaults.recordOrder.store(false); }
                } stopRecording;
                {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    g_glSemaphoreFaults.callOrder.clear();
                }
                g_glSemaphoreFaults.recordOrder.store(true);
                // The first recorded call from `from` on by `thread` of kind `call` (on `name` unless 0); npos if none.
                const auto findCall = [](size_t from, std::thread::id thread, Faults::Call call, GLuint name) {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    const std::vector<Faults::OrderedCall>& calls = g_glSemaphoreFaults.callOrder;
                    for (size_t i = from; i < calls.size(); ++i) {
                        if (calls[i].thread == thread && calls[i].call == call && (name == 0 || calls[i].name == name)) {
                            return i;
                        }
                    }
                    return std::string::npos;
                };
                const auto recordedSoFar = [] {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    return g_glSemaphoreFaults.callOrder.size();
                };
                const auto readyWithin = [](const std::shared_future<void>& done, int ms) {
                    return done.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
                };
                // The latest request `from` received for `monitor` was answered on its timeline: a value went out.
                const auto lastAnsweredOnTimeline = [](const ExportingStub& from, int monitor) {
                    const std::vector<ExportingStub::Ask>& asks = from.asks();
                    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
                        if (it->monitorId == monitor) return it->onTimeline && it->served != 0;
                    }
                    return false;
                };

                // Both items on A's timeline, through the group's one import of it.
                const auto announceBoth = [&](ExportingStub& from, QDBusConnection& on) {
                    const quint64 generation = from.nextGeneration();
                    announce(on, generation, wallpaperx::testing::kSecondMonitor);
                    announce(on, generation, kThirdMonitor);
                };
                bool onA = false;
                for (int round = 0; round < 12 && !onA; ++round) {
                    const int served0 = stub.served();
                    announceBoth(stub, bus);
                    (void)pumpUntil([&] { return stub.served() >= served0 + 2; }, 3000);
                    pumpFor(50);
                    p.pass();
                    q.pass();
                    onA = lastAnsweredOnTimeline(stub, wallpaperx::testing::kSecondMonitor) &&
                          lastAnsweredOnTimeline(stub, kThirdMonitor);
                }
                const size_t aImport = findCall(0, p.renderThread, Faults::Call::Import, 0) != std::string::npos
                                           ? findCall(0, p.renderThread, Faults::Call::Import, 0)
                                           : findCall(0, q.renderThread, Faults::Call::Import, 0);
                GLuint nameA = 0;
                if (aImport != std::string::npos) {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    nameA = g_glSemaphoreFaults.callOrder[aImport].name;
                }
                if (!onA || nameA == 0) {
                    wallpaperx::testing::harnessFailure("not staged: P and Q never both drew a value picture on A's "
                                                        "timeline through a group import");
                    return true;
                }
                pumpFor(200);

                // P waits on a value A has submitted behind its gate: held on the GPU until released.
                const size_t lateMark = recordedSoFar();
                const int lateServed0 = stub.served();
                stub.lateValueNext({});
                announce(bus, stub.nextGeneration(), wallpaperx::testing::kSecondMonitor);
                const bool lateOut = pumpUntil([&] { return stub.lateValueSent(); }, 3000);
                pumpFor(50);
                if (!lateOut || stub.served() != lateServed0 + 1 ||
                    !lastAnsweredOnTimeline(stub, wallpaperx::testing::kSecondMonitor)) {
                    wallpaperx::testing::harnessFailure("not staged: the held value did not go to P alone (" +
                                                        std::to_string(stub.served() - lateServed0) + " served; " +
                                                        stub.acquireError() + ")");
                    return true;
                }
                if (!readyWithin(p.syncLater(), 3000) || !readyWithin(p.endLater(), 3000)) {
                    wallpaperx::testing::harnessFailure("P's frame with the held value did not return within 3 s");
                    return true;
                }
                const size_t lateSet = findCall(lateMark, p.renderThread, Faults::Call::SetValue, nameA);
                const size_t lateWait = findCall(lateMark, p.renderThread, Faults::Call::Wait, nameA);
                if (lateSet == std::string::npos || lateWait == std::string::npos ||
                    g_glSemaphoreFaults.waitsFailed.load() != 0) {
                    wallpaperx::testing::harnessFailure("not staged: P did not wait on A's timeline for the held value");
                    return true;
                }

                // B takes the name, and both items hold a reply carrying a value on B's timeline, unsynced. Only
                // Q draws meanwhile, and only binary pictures: a drawn one is what asks B for its timeline.
                SuccessorDaemon successor(deviceB, ringB);
                if (!successor.ok || !successor.takeNameFrom(bus)) {
                    wallpaperx::testing::harnessFailure("not staged: daemon B never took the name");
                    return true;
                }
                bool onB = false;
                for (int round = 0; round < 12 && !onB; ++round) {
                    const int served0 = successor.stub.served();
                    announceBoth(successor.stub, successor.bus);
                    (void)pumpUntil([&] { return successor.stub.served() >= served0 + 2; }, 3000);
                    pumpFor(100);
                    const bool qValued = lastAnsweredOnTimeline(successor.stub, kThirdMonitor);
                    onB = successor.stub.timelinesGranted() > 0 && qValued &&
                          lastAnsweredOnTimeline(successor.stub, wallpaperx::testing::kSecondMonitor);
                    if (!qValued) q.pass();
                }
                if (!onB) {
                    std::string asked;
                    for (const ExportingStub::Ask& a : successor.stub.asks()) {
                        asked += " m" + std::to_string(a.monitorId) + (a.onTimeline ? "/t" : "/b") +
                                 (a.served != 0 ? "+" : "-");
                    }
                    std::fprintf(stderr, "  B: %d timeline ask(s), %d granted; requests:%s\n",
                                 successor.stub.timelineAsks(), successor.stub.timelinesGranted(), asked.c_str());
                    wallpaperx::testing::harnessFailure("not staged: B never answered both items with a value");
                    return true;
                }

                // P's frame imports B's timeline into the group; once that import is recorded, Q's frame runs.
                const size_t importMark = recordedSoFar();
                QElapsedTimer clock;
                clock.start();
                const std::shared_future<void> pFrame = p.syncLater();
                size_t bImport = std::string::npos;
                while (clock.elapsed() < 3000) {
                    bImport = findCall(importMark, p.renderThread, Faults::Call::Import, 0);
                    if (bImport != std::string::npos || readyWithin(pFrame, 0)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (bImport == std::string::npos) bImport = findCall(importMark, p.renderThread, Faults::Call::Import, 0);
                const qint64 qStart = clock.elapsed();
                const std::shared_future<void> qFrame =
                    bImport != std::string::npos ? q.syncLater() : std::shared_future<void>();
                const bool qThrough = qFrame.valid() && readyWithin(qFrame, kSiblingBoundMs);
                const qint64 qMs = clock.elapsed() - qStart;
                const bool pReturned = readyWithin(pFrame, 0);
                const qint64 pMs = clock.elapsed();
                const bool stillHeld = stub.lateValuePending();
                std::string why;
                const bool released = stub.signalLateValue(why);
                const bool drained = readyWithin(pFrame, 10000) && (!qFrame.valid() || readyWithin(qFrame, 10000));
                if (!drained) {
                    wallpaperx::testing::harnessFailure("the frames did not return within 10 s of the held value being "
                                                        "released (" + why + ")");
                    return true;
                }
                p.end();
                if (qFrame.valid()) q.end();
                GLuint nameB = 0;
                if (bImport != std::string::npos) {
                    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
                    nameB = g_glSemaphoreFaults.callOrder[bImport].name;
                }
                const bool qOnB = nameB != 0 && findCall(importMark, q.renderThread, Faults::Call::SetValue, nameB) !=
                                                    std::string::npos;
                std::printf("item_timeline_test: shared-stuck: A's timeline %u, P's held wait at call %zu; B's timeline "
                            "%u imported by P at call %zu; Q's frame %s after %lld ms (bound %d), set a value on B's "
                            "timeline %d; P's frame %s by then (%lld ms); value still held %d, released %d\n",
                            nameA, lateWait, nameB, bImport, qThrough ? "returned" : "had not returned",
                            static_cast<long long>(qMs), kSiblingBoundMs, qOnB ? 1 : 0,
                            pReturned ? "had returned" : "had not returned", static_cast<long long>(pMs),
                            stillHeld ? 1 : 0, released ? 1 : 0);
                std::fflush(stdout);

                // GOAL: S15.445 -- a render thread acquiring a picture on its share group's timeline gets through
                //       within a bound while a sibling context of the group, whose earlier wait is on a value
                //       never signalled, imports a new daemon's timeline into the group.
                // WHY-THIS-SHAPE: the stuck value is held on the GPU behind a gate the test opens, so the bound
                //       ends the wait, never a hang: Q's frame is waited for 3 s, then the value is released and
                //       both frames drain. Q's frame runs only after P's import is recorded, so P is inside the
                //       group lock or past it, never before it. A check on P's frame time would grade Qt and the
                //       driver; the property is only that nothing under the lock blocks.
                // CONTROL: in acquireOnGroupTimeline (backend/kde/wallpaperx_item.cpp), via control_run.sh,
                //       2026-10-10, NVIDIA, reverted: gl->glFinish() put back after importTimeline -> "FAIL: a
                //       render thread gets through while a sibling context of its share group, stuck on a value
                //       never signalled, imports a new daemon's timeline (Q's frame had not returned after 3000
                //       ms)"; the entry still ended in 5 s, the value released.
                // COVERAGE-ASSERT: P imported B's timeline, the value was still held when Q was graded, and Q set
                //       a value on B's timeline in that frame, else not staged.
                if (bImport == std::string::npos || !stillHeld || !released || !qOnB) {
                    wallpaperx::testing::harnessFailure(
                        "not staged: P importing B's timeline while held, then Q acquiring on it, was not recorded "
                        "(see the line above)");
                    return true;
                }
                check(qThrough, "a render thread gets through while a sibling context of its share group, stuck on a "
                                "value never signalled, imports a new daemon's timeline (Q's frame " +
                                    std::string(qThrough ? "returned after " + std::to_string(qMs) + " ms"
                                                         : "had not returned after " + std::to_string(qMs) + " ms") +
                                    ")");
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
