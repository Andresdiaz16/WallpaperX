// SPDX-License-Identifier: GPL-2.0-or-later

// G1: when the GL import of a fetched frame fails, WallpaperXItem publishes a non-empty
// unavailableReason and QML can read it.
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

#include "tests/test_report.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
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

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace wpxtest {

// The per-plane (offset, rowPitch) struct GetFrameFd's tenth field carries. Declared here so the
// stub's reply marshals with the same signature the daemon's does.
struct Plane {
    quint64 offset = 0;
    quint64 pitch = 0;
};

QDBusArgument& operator<<(QDBusArgument& arg, const Plane& plane) {
    arg.beginStructure();
    arg << plane.offset << plane.pitch;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, Plane& plane) {
    arg.beginStructure();
    arg >> plane.offset >> plane.pitch;
    arg.endStructure();
    return arg;
}

}  // namespace wpxtest

Q_DECLARE_METATYPE(wpxtest::Plane)

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;
using wpxtest::Plane;

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
        const int fd = ::memfd_create("wallpaperx-stub-frame", 0);
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

        QList<Plane> planes;
        planes.append(Plane{0, static_cast<quint64>(kFrameWidth) * 4});

        const QList<QVariant> fields = {
            QVariant::fromValue(handle),
            QVariant::fromValue(static_cast<quint32>(kFrameWidth)),
            QVariant::fromValue(static_cast<quint32>(kFrameHeight)),
            QVariant::fromValue(memorySize),
            QVariant::fromValue(0),  // handle_type: opaque fd
            QVariant::fromValue(QList<QDBusUnixFileDescriptor>{}),
            QVariant::fromValue(static_cast<quint64>(1)),  // frame_generation
            QVariant::fromValue(kUnimportableFourcc),
            QVariant::fromValue(static_cast<quint64>(0)),  // drm_modifier: LINEAR
            QVariant::fromValue(planes)};

        ++m_served;
        return connection.send(message.createReply(fields));
    }

    int requests() const { return m_requests; }
    int served() const { return m_served; }
    bool fdError() const { return m_fdError; }

private:
    int m_requests = 0;
    int m_served = 0;
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

    qDBusRegisterMetaType<Plane>();
    qDBusRegisterMetaType<QList<Plane>>();
    qDBusRegisterMetaType<QList<QDBusUnixFileDescriptor>>();

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
        // CONTROL: `if (m_monitorId < 0) return;` widened to `< 1000` in
        //       WallpaperXItem::fetchFrame in backend/kde/wallpaperx_item.cpp
        //       -> "FAIL: the item asked the stub daemon for a frame", after 5s, and the checks
        //       below did not run. Observed 2026-08-21. Reverted.
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
            //       five other causes (no subscription, daemon gone, short reply, sync fds, a fd
            //       that could not be duplicated), and a check that only looked for "non-empty at
            //       the end" would pass on any of them. Reading it here is safe because nothing
            //       renders between the reply and this line: every sync in this file is explicit.
            //       The settle is a best effort -- had the reply not landed yet, this check would
            //       be true without being informative, and the equality check below is what still
            //       refuses a reason from any other cause.
            // CONTROL: the `setUnavailableReason(QString())` at the end of
            //       WallpaperXItem::takeFrameReply in backend/kde/wallpaperx_item.cpp
            //       replaced with a non-empty string -> "FAIL: no
            //       reason is published by the fetch path itself, before any import runs", alone,
            //       with the import checks below still green. Observed 2026-08-21. Reverted.
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
            //       backend/kde/wallpaperx_item.cpp, the import
            //       message replaced with `setUnavailableReason(QString())` -> "FAIL: a failed
            //       import publishes a non-empty unavailableReason, readable from QML" plus the
            //       next check, after the full 5s. Observed 2026-08-21. Reverted.
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

            // GOAL: the reason names the import, rather than some other unavailability.
            // WHY-THIS-SHAPE: equality against the shipped string, not `contains("import")`: the
            //       looser form also matches the sync-fd and short-reply messages, and this test
            //       exists to distinguish exactly those.
            // CONTROL: the texture == 0 branch of WallpaperXItem::updatePaintNode in
            //       backend/kde/wallpaperx_item.cpp made to publish the sync-fd message
            //       instead -> "FAIL: the
            //       published reason is the one for an import that failed", alone, and the
            //       diagnostic below named the wrong string. Observed 2026-08-21. Reverted.
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
            //       WallpaperXItem::setUnavailableReason in backend/kde/wallpaperx_item.cpp
            //       -> "FAIL: the QML binding holds the same
            //       string the property returns", with the two checks above red as well because
            //       the binding never updated. Observed 2026-08-21. Reverted.
            auto* renderer = qobject_cast<QQuickItem*>(root->property("rendererItem").value<QObject*>());
            check(renderer != nullptr, "the QML scene exposes the renderer item");
            if (renderer != nullptr) {
                check(renderer->property("unavailableReason").toString() == published,
                      "the QML binding holds the same string the property returns");
            }
        }
    }

    // Before the render control tears its context down: the node owns the import, and freeing it
    // needs that context.
    root.reset();

    const bool checksRed = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the plugin");
    if (checksRed || harnessRed) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
