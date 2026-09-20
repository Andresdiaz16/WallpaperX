// SPDX-License-Identifier: GPL-2.0-or-later

// G1: when the GL import of a fetched frame fails, WallpaperXItem publishes a non-empty
// unavailableReason and QML can read it.
//
// S12.11/S12.12: and the reply it fails on carries a non-empty sync_fds, so this is also where
// the plugin's receive path for the daemon's acquire descriptor is exercised. Two properties ride
// on that: a reply carrying one is READ rather than refused -- the refusal that shipped before
// this existed is what the published-reason equality check below now rules out -- and every
// descriptor such a reply carries is closed exactly once when the import fails, which is the
// contract that a FAILED import leaves them with the caller.
//
// The failure is manufactured entirely on the DAEMON side of the wire: a stub service on this
// run's private bus answers GetFrameFd with a fourcc the importer refuses to lay out (NV12), so
// FrameImport::import() fails before it creates any GPU object. Nothing in the plugin knows a
// test is running, and no production seam exists for this.
//
// Rendering goes through QQuickRenderControl rather than a shown window: updatePaintNode then runs
// on this thread at a point this file picks, so the reason appearing *during the import* is
// observable rather than raced against a scene-graph render thread. Nothing is displayed either
// way -- a render-control window has no native window behind it -- but the offscreen QPA platform
// still cannot be used, because it forces the software scene graph and the item imports only under
// OpenGL. A host that has no OpenGL platform skips, with the reason named.

#include "tests/qt_frame_reply.h"
#include "tests/test_report.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QUrl>
#include <QVariant>

#include <dirent.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::FramePlane;
using wallpaperx::testing::frameReplyFields;
using wallpaperx::testing::notVerified;

constexpr const char* kService = "io.wallpaperx.Daemon";
constexpr const char* kPath = "/io/wallpaperx/Daemon";
constexpr const char* kInterface = "io.wallpaperx.Daemon";

constexpr int kFrameWidth = 64;
constexpr int kFrameHeight = 64;

// NV12: a real fourcc, multi-planar YUV, and one FrameImport::import() refuses outright ("the
// daemon exported a pixel format this importer cannot lay out") before it touches the fd or the
// GPU. That refusal is what makes the failure deterministic on every driver. Measured with
// XRGB8888 instead, on the NVIDIA GL here, the import still failed -- glImportMemoryFdEXT rejects
// a plain memfd -- so this is the reliable lever, not the only one.
constexpr quint32 kUnimportableFourcc = 'N' | ('V' << 8) | ('1' << 16) | ('2' << 24);

// The two memfd names the stub answers with. Distinctive because the descriptor accounting below
// identifies the plugin's copies by reading /proc/self/fd, which is exact where a raw count of
// open descriptors would be measuring Qt's and the driver's traffic as well.
constexpr const char* kFrameFdName = "wallpaperx-stub-frame";
constexpr const char* kSyncFdName = "wallpaperx-stub-sync";

// How many descriptors in this process point at a memfd created under `name`. A memfd's
// /proc/self/fd link reads "/memfd:<name> (deleted)", so the count is of the plugin's duplicates
// and of nothing else -- every other fd this process holds is a socket, a device or a file.
int openMemfdCount(const char* name) {
    const std::string needle = std::string("/memfd:") + name;
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) return -1;
    int found = 0;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        char target[256];
        const std::string path = std::string("/proc/self/fd/") + entry->d_name;
        const ssize_t length = ::readlink(path.c_str(), target, sizeof(target) - 1);
        if (length <= 0) continue;
        target[length] = '\0';
        if (std::string(target).rfind(needle, 0) == 0) ++found;
    }
    ::closedir(dir);
    return found;
}

// What WallpaperXItem publishes when the import fails, copied from wallpaperx_item.cpp. Duplicated
// rather than shared: the plugin exports no such constant, and adding one would be test-only
// surface in a shipped header. A wording change in the item fails this file, deliberately -- the
// string is what the desktop shows the user.
constexpr const char* kImportFailedReason = "WallpaperX produced a frame this desktop could not import.";

// Answers GetFrameFd with a frame the importer must refuse. A virtual object rather than a slot on
// a QObject, because the reply's shape is the point: it must carry the daemon's ten fields in the
// daemon's order and types, and an adaptor would put Qt's marshalling of a C++ signature between
// this file and the wire. A shape the item cannot read shows up as a different published reason,
// which the equality check below catches.
class StubDaemon : public QDBusVirtualObject {
public:
    QString introspect(const QString&) const override { return QString(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String(kInterface) || message.member() != QLatin1String("GetFrameFd")) {
            return false;
        }
        // Counted before anything here can go wrong, so "the item asked" and "the stub answered"
        // stay separate verdicts: a fault in this file must not print as the item failing to fetch.
        ++m_requests;

        // A real, mappable fd: the item dups it, and a bad one would be refused by the reply parse
        // ("could not take hold of") long before the import is reached.
        const int fd = ::memfd_create(kFrameFdName, 0);
        if (fd < 0) {
            m_fdError = true;
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.ExportFailed"),
                                                            QStringLiteral("memfd_create failed in the stub")));
        }
        const quint64 memorySize = static_cast<quint64>(kFrameWidth) * kFrameHeight * 4;
        if (::ftruncate(fd, static_cast<off_t>(memorySize)) != 0) {
            m_fdError = true;
            ::close(fd);
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.ExportFailed"),
                                                            QStringLiteral("ftruncate failed in the stub")));
        }

        QDBusUnixFileDescriptor handle(fd);
        ::close(fd);  // QDBusUnixFileDescriptor took its own duplicate.

        // A memfd in the sync slot too, and deliberately not a real semaphore: what this file
        // grades about sync_fds is the RECEIVE path -- that a reply carrying one is read rather
        // than refused, and that its descriptor is accounted for when the import fails. The
        // acquire actually being waited on is item_import_release_test's, where the frame is one
        // the importer accepts and the descriptor comes from a real Vulkan export.
        const int syncFd = ::memfd_create(kSyncFdName, 0);
        if (syncFd < 0) {
            m_fdError = true;
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.ExportFailed"),
                                                            QStringLiteral("memfd_create failed in the stub")));
        }
        QList<QDBusUnixFileDescriptor> syncFds;
        syncFds.append(QDBusUnixFileDescriptor(syncFd));
        ::close(syncFd);

        QList<FramePlane> planes;
        planes.append(FramePlane{0, static_cast<quint64>(kFrameWidth) * 4});

        ++m_served;
        m_syncFdsSent += static_cast<int>(syncFds.size());
        return connection.send(message.createReply(
            frameReplyFields(handle, kFrameWidth, kFrameHeight, memorySize, /*handleType=*/0,
                             /*generation=*/1, kUnimportableFourcc, /*drmModifier=*/0, planes,
                             // Not this device, and it does not have to be: the fourcc this
                             // phase grades is refused before the importer looks at the GPU.
                             QByteArray(16, '\0'), /*bufferId=*/1, syncFds)));
    }

    int requests() const { return m_requests; }
    int served() const { return m_served; }
    int syncFdsSent() const { return m_syncFdsSent; }
    bool fdError() const { return m_fdError; }

private:
    int m_requests = 0;
    int m_served = 0;
    int m_syncFdsSent = 0;
    bool m_fdError = false;
};

// Pumps the event loop until `done`, or gives up. Returns whether it happened -- every caller
// branches on it, so a stall can never be read as data.
bool pumpUntil(const std::function<bool()>& done, int limitMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > limitMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

// Pumps for a fixed span, waiting for nothing. Separate from pumpUntil so that no call site
// discards a "it never happened" answer.
void pumpFor(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

QString qmlSource() {
    // publishedReason is a BINDING, not a read: it updates only if the item emits
    // unavailableReasonChanged, so a property that changes silently leaves it stale.
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: 64
    height: 64
    property string publishedReason: renderer.unavailableReason
    property Item rendererItem: renderer

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
    }
}
)");
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_import_failure_test";

    wallpaperx::testing::registerFrameReplyTypes();

    // Set before the window exists: the item refuses to import at all unless the scene graph
    // reports OpenGL, and Qt picks the API at first window creation.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);

    QDBusConnection bus = QDBusConnection::sessionBus();
    StubDaemon stub;
    if (!bus.isConnected() || !bus.registerService(QString::fromUtf8(kService)) ||
        !bus.registerVirtualObject(QString::fromUtf8(kPath), &stub)) {
        notVerified("G1", "no session bus this run could own io.wallpaperx.Daemon on: " +
                              bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.setGeometry(0, 0, kFrameWidth, kFrameHeight);

    // The scene graph makes its own OpenGL context here. Whether it can is a property of the
    // host's Qt platform -- the offscreen one forces the software adaptation, which refuses the
    // RHI outright -- so a failure is a named skip, not a verdict on the item.
    // CONTROL: run with QT_QPA_PLATFORM=offscreen -> "NOT VERIFIED -- G1 could not be exercised:
    //       no OpenGL scene graph could be brought up under the offscreen platform", exit 77.
    //       Observed 2026-08-21.
    if (!control.initialize() || window.rendererInterface() == nullptr ||
        window.rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
        notVerified("G1", std::string("no OpenGL scene graph could be brought up under the ") +
                              QGuiApplication::platformName().toStdString() +
                              " platform, so a real import could not be attempted");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    QQmlComponent component(&engine);
    component.setData(qmlSource().toUtf8(), QUrl::fromLocalFile(QCoreApplication::applicationDirPath() +
                                                                QStringLiteral("/item_import_failure.qml")));
    std::unique_ptr<QObject> root(component.create());
    auto* rootItem = qobject_cast<QQuickItem*>(root.get());
    if (rootItem == nullptr) {
        for (const QQmlError& error : component.errors()) {
            std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
        }
    }
    // The module and the type are the plugin's own, so a load failure here is production's, not
    // this harness's -- red, and every check below is gated on it.
    // CONTROL: the import path replaced with a nonexistent directory -> `qml: ... module
    //       "com.wallpaperx" is not installed` and "FAIL: the com.wallpaperx module loads and
    //       WallpaperXRenderer instantiates". Observed 2026-08-21. Reverted.
    check(rootItem != nullptr, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");

    if (rootItem != nullptr) {
        rootItem->setParentItem(window.contentItem());

        // sync() is the pass that calls updatePaintNode, which is where the import happens. render()
        // is left out: this window has no render target, and drawing establishes nothing the reason
        // string does not.
        const auto syncOnce = [&control] {
            control.polishItems();
            control.beginFrame();
            control.sync();
            control.endFrame();
        };
        // One pass before the frame arrives, so the item is fully in the scene and the only thing
        // left to happen is the import.
        syncOnce();

        // GOAL: the item asks the daemon for a frame at all. Not G1, but G1 is meaningless without
        //       it -- an item that never fetched would show an empty reason for a second reason.
        // WHY-THIS-SHAPE: bounded, with an explicit failure branch, and everything below is gated
        //       on it. Falling through on the timeout would let a never-fetching item be graded on
        //       whatever state it was left in.
        // CONTROL: `if (m_monitorId < 0 || !m_onScreen) return;` widened to `m_monitorId < 1000` in
        //       WallpaperXItem::fetchFrame in backend/kde/wallpaperx_item.cpp
        //       -> "FAIL: the item asked the stub daemon for a frame", after 5s, and the checks
        //       below did not run. Observed 2026-08-21; re-run 2026-09-20 under the Stage E4
        //       spelling of that guard, which now also tests whether the item is on the screen.
        //       Reverted.
        const bool asked = pumpUntil([&stub] { return stub.requests() > 0; }, 5000);
        check(asked, "the item asked the stub daemon for a frame");
        // A stub that could not build its reply is this file's fault, not the plugin's, and is
        // counted on the harness channel so the summary sends a reader to the right place.
        // CONTROL: ftruncate given -1 -> "FAIL (harness): the stub could not allocate the frame fd
        //       it answers with" and "1 fault(s) in this test file, not in the plugin", with no
        //       check reported against the item. Observed 2026-08-21. Reverted.
        if (stub.fdError()) {
            wallpaperx::testing::harnessFailure("the stub could not allocate the frame fd it answers with");
        }

        if (asked && stub.served() > 0) {
            // GOAL: the reply path itself publishes no reason -- so a non-empty reason after the
            //       next render is attributable to the import and to nothing else.
            // WHY-THIS-SHAPE: this is the anti-vacuity half. unavailableReason is non-empty for
            //       five other causes (no subscription, daemon gone, short reply, a reply whose
            //       descriptors could not be taken hold of, a fd that could not be duplicated),
            //       and a check that only looked for "non-empty at
            //       the end" would pass on any of them. Reading it here is safe because nothing
            //       renders between the reply and this line: every sync in this file is explicit.
            //       The settle is a best effort -- had the reply not landed yet, this check would
            //       be true without being informative, and the equality check below is what still
            //       refuses a reason from any other cause.
            // CONTROL: the `setFetchReason(QString())` at the end of
            //       WallpaperXItem::takeFrameReply in backend/kde/wallpaperx_item.cpp replaced
            //       with a non-empty string -> "FAIL: no reason is published by the fetch path
            //       itself, before any import runs". Re-run 2026-09-06: the reason string is no
            //       longer written by one setter, so the mutation now names setFetchReason, and
            //       it is NOT alone -- the fetch side outranks the render side in
            //       publishReason(), so "the published reason is the one for an import that
            //       failed" goes red printing the injected string, and the descriptor check
            //       below goes red with one frame and one sync descriptor still open. Reverted.
            pumpFor(200);
            check(root->property("publishedReason").toString().isEmpty(),
                  "no reason is published by the fetch path itself, before any import runs");

            // GOAL (G1): a failed import publishes a non-empty unavailableReason, and QML reads it.
            // WHY-THIS-SHAPE: render-then-poll, because updatePaintNode is the only place the
            //       import happens and the reason is delivered to QML through a NOTIFY. The
            //       obvious simpler form -- one render, then read -- misses the queued property
            //       change on any pass where the item defers its update, and its failure branch
            //       would be indistinguishable from a real empty reason. The loop cannot pass by
            //       timing out: expiry leaves `reasonAppeared` false, which fails the check.
            // CONTROL: on the texture == 0 branch of WallpaperXItem::updatePaintNode in
            //       backend/kde/wallpaperx_item.cpp, the `setRenderReason(tr("WallpaperX
            //       produced a frame this desktop could not import."))` call replaced with
            //       `setRenderReason(QString())` -> "FAIL: a failed import publishes a non-empty
            //       unavailableReason, readable from QML" plus the next check, printing
            //       "published reason was: []", after the full 5s. Re-run 2026-09-06: the branch
            //       no longer calls setUnavailableReason, which the earlier annotation named and
            //       which no longer exists. Reverted.
            // COVERAGE-ASSERT: the check above -- reason empty before this loop -- and this one
            //       together pin the transition to a sync pass. A count of passes was tried and
            //       dropped: it cannot fail, since the loop increments it beside the very call it
            //       would be claiming to have made.
            const bool reasonAppeared = pumpUntil(
                [&] {
                    syncOnce();
                    return !root->property("publishedReason").toString().isEmpty();
                },
                5000);
            check(reasonAppeared, "a failed import publishes a non-empty unavailableReason, readable from QML");

            // GOAL: the reason names the import, rather than some other unavailability. Since the
            //       reply this failed on carries a non-empty sync_fds, this is also S12.11: a
            //       frame that must be waited on is READ and imported, not refused on sight.
            // WHY-THIS-SHAPE: equality against the shipped string, not `contains("import")`: the
            //       looser form also matches the short-reply and could-not-take-hold messages,
            //       and this test exists to distinguish exactly those. The stub sending a sync
            //       descriptor is what makes it grade S12.11 as well; without one the reply never
            //       enters the branch that reads the field.
            // CONTROL: the texture == 0 branch of WallpaperXItem::updatePaintNode in
            //       backend/kde/wallpaperx_item.cpp made to publish the could-not-take-hold-of
            //       message instead -> "FAIL: the published reason is the one for an import that
            //       failed", alone, and the diagnostic below named the wrong string. Re-run
            //       2026-08-31 -- the sync-fd message this control used to name no longer exists
            //       in production. Reverted.
            // CONTROL (S12.11): the pre-fix refusal restored in WallpaperXItem::takeFrameReply
            //       (backend/kde/wallpaperx_item.cpp) -- `setFetchReason(...must be waited on...)`
            //       and return, for a non-empty sync_fds -> "FAIL: the published reason is the
            //       one for an import that failed", printing "published reason was: [WallpaperX
            //       sent a frame that must be waited on...]". Observed 2026-08-31. Reverted.
            // COVERAGE-ASSERT: the stub's sync-descriptor count is asserted below, so a reply that
            //       silently stopped carrying one turns this back into a test of the fourcc alone
            //       and says so, rather than going quiet.
            const QString published = root->property("publishedReason").toString();
            check(published == QLatin1String(kImportFailedReason),
                  "the published reason is the one for an import that failed");
            if (published != QLatin1String(kImportFailedReason)) {
                std::fprintf(stderr, "  published reason was: [%s]\n", qPrintable(published));
            }

            // GOAL: the string QML holds is the string the property returns -- i.e. the change was
            //       announced. False if unavailableReason is written without emitting
            //       unavailableReasonChanged: the desktop then keeps showing the previous state.
            // WHY-THIS-SHAPE: the binding above is compared against a direct meta-object read of
            //       the same property. Reading the property alone cannot see a missing NOTIFY.
            // CONTROL: `emit unavailableReasonChanged();` deleted from
            //       WallpaperXItem::publishReason in backend/kde/wallpaperx_item.cpp -> "FAIL:
            //       the QML binding holds the same string the property returns", with the two
            //       checks above red as well because the binding never updated. Re-run
            //       2026-09-06: the signal is emitted from publishReason(), which recomputes the
            //       string from the fetch and render facts; setUnavailableReason, which the
            //       earlier annotation named, no longer exists. Reverted.
            auto* renderer = qobject_cast<QQuickItem*>(root->property("rendererItem").value<QObject*>());
            check(renderer != nullptr, "the QML scene exposes the renderer item");
            if (renderer != nullptr) {
                check(renderer->property("unavailableReason").toString() == published,
                      "the QML binding holds the same string the property returns");
            }

            // The anti-vacuity half of the two sync_fds claims above and below: both are about
            // what the plugin does with a descriptor it was sent, and both hold trivially if
            // nothing was sent.
            check(stub.syncFdsSent() == stub.served(),
                  "the stub sent one sync descriptor with every frame it served");

            // GOAL (S12.12): every descriptor a reply carries is closed exactly once when the
            //       import fails -- the sync descriptors as well as the memory one. False if a
            //       refused frame leaks one per fetch, which on a still wallpaper is one per
            //       scene change and ends a login session.
            // WHY-THIS-SHAPE: the plugin's copies are identified by reading /proc/self/fd and
            //       matching the stub's memfd names, not by counting open descriptors before and
            //       after. A raw count is measuring Qt's and the GL driver's own traffic too, so
            //       it would need a tolerance, and a tolerance is exactly what hides a one-per-
            //       frame leak. Matching the name makes the expected count zero with nothing to
            //       tune. The item's own copies are the only ones left by now: the stub closes
            //       its originals in handleMessage and the reply message that carried them has
            //       long since been destroyed.
            // CONTROL: closeDescriptors in backend/kde/wallpaperx_item.cpp cut back to
            //       `if (request.fd >= 0) ::close(request.fd);` -- the shape it had before
            //       sync_fds carried anything -> "FAIL: a refused frame leaves no descriptor of
            //       the reply open, the sync ones included", printing "3 sync and 0 frame
            //       descriptor(s) still open after the failed import". Observed 2026-08-31.
            //       Reverted.
            pumpFor(200);
            const int leakedSync = openMemfdCount(kSyncFdName);
            const int leakedFrame = openMemfdCount(kFrameFdName);
            if (leakedSync < 0 || leakedFrame < 0) {
                // Reading /proc is this file's instrument, so its failure is a harness fault and
                // must not print as the plugin leaking.
                wallpaperx::testing::harnessFailure("/proc/self/fd could not be read, so no descriptor accounting "
                                                    "was done");
            } else {
                check(leakedSync == 0 && leakedFrame == 0,
                      "a refused frame leaves no descriptor of the reply open, the sync ones included");
                if (leakedSync != 0 || leakedFrame != 0) {
                    std::fprintf(stderr, "  %d sync and %d frame descriptor(s) still open after the failed import\n",
                                 leakedSync, leakedFrame);
                }
            }
        }
    }

    // The node owns the import and freeing it needs the context, but destroying the item only
    // QUEUES the node -- and QQuickRenderControl makes the context current only between
    // beginFrame() and endFrame(). So one more frame runs the queued deletion, and invalidate()
    // releases the scene graph's own resources. Today's import always fails and no texture
    // exists; this is what keeps that true when a success case is added.
    root.reset();
    control.beginFrame();
    control.sync();
    control.endFrame();
    control.invalidate();

    const bool checksRed = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
    if (checksRed || harnessRed) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
