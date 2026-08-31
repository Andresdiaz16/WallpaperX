// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The scene two tests stand WallpaperXItem up in: a stub daemon that serves one real exported
// frame, the QML that instantiates the plugin's own type, and the readers that ask what the item
// ended up with.
//
// Shared because both tests need the same precondition -- a frame genuinely imported and attached
// to a scene-graph node -- and reach it identically; only what they then do to it differs. Kept
// as one definition rather than two copies: a copy that drifts still compiles, still runs, and
// still reads as coverage, and the two copies had already lost two CONTROL annotations between
// them.

#include "kde/frame_import.h"
#include "renderers/static_renderer.h"
#include "support/test_image.h"
#include "tests/qt_frame_reply.h"
#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QQuickItem>
#include <QSGSimpleTextureNode>
#include <QString>
#include <QtQuick/private/qquickitem_p.h>

#include <EGL/egl.h>
#include <GL/gl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>

namespace wallpaperx::testing {

// The bus name, path and interface the plugin talks to. Literals rather than the daemon's own
// constants: what is under test is that the item speaks the wire the daemon declares, and taking
// both sides from one symbol would let a rename pass unnoticed.
inline constexpr const char* kService = "io.wallpaperx.Daemon";
inline constexpr const char* kPath = "/io/wallpaperx/Daemon";
inline constexpr const char* kInterface = "io.wallpaperx.Daemon";

// Not square and not a round number of pixels wide, for frame_export_probe's reason: a square
// target imports fine with width and height transposed.
inline constexpr uint32_t kWidth = 68;
inline constexpr uint32_t kHeight = 36;

// The monitor id the QML below asks for, and the one a test may move it to. Different, and
// neither is the -1 the item refuses to fetch for.
inline constexpr int kFirstMonitor = 0;
inline constexpr int kSecondMonitor = 1;

// Renders once into `target`, so it carries a PUBLISHED frame.
//
// Not decoration: the daemon only ever exports a frame a render published, and the acquire
// descriptor is a hand-over of that publication -- exportFrameAcquireFd refuses a target with
// nothing published, by design. A stub exporting an unrendered target would be serving the item a
// shape the daemon cannot produce, and the acquire half of its reply could not exist at all.
//
// The renderer is a local: targets.render() waits for the submission, so nothing of it is needed
// afterwards. `fixtureName` keeps each caller's PNG on a path of its own -- these binaries share
// WALLPAPERX_TEST_IMAGE_DIR.
inline void publishOneFrame(wallpaperx::Device& device, wallpaperx::OffscreenTargetManager& targets,
                            const wallpaperx::OffscreenTarget& target, const char* fixtureName) {
    const std::string imagePath =
        wallpaperx::testing::writeGradientPng(std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/" + fixtureName, 16, 16);
    wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);
    targets.render(renderer, target);
}

// Serves ONE real exported frame -- memory and acquire descriptor both -- and then refuses.
//
// The refusal is a barrier tests rest on. Making the item fetch again -- by moving it to another
// monitor id -- must not produce a second importable frame, or the next render pass would import
// it and set the "a node is drawing" fact back to true, masking the mutation a check exists to
// catch. A refusal reaches the item as a fetch reason and touches neither the node nor the
// texture.
class ExportingStub : public QDBusVirtualObject {
public:
    ExportingStub(wallpaperx::Device& device, const wallpaperx::OffscreenTarget& target)
        : m_device(device), m_target(target) {}

    QString introspect(const QString&) const override { return QString(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String(kInterface) || message.member() != QLatin1String("GetFrameFd")) {
            return false;
        }
        // Counted before anything here can go wrong, so "the item asked" and "the stub answered"
        // stay separate verdicts.
        ++m_requests;
        if (m_served > 0) {
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.NoFrame"),
                                                            QStringLiteral("this stub serves one frame per run")));
        }

        wallpaperx::ExportedFrame frame;
        try {
            frame = wallpaperx::exportFrame(m_device, m_target);
        } catch (const std::exception& e) {
            m_exportError = e.what();
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.ExportFailed"),
                                                            QString::fromStdString(m_exportError)));
        }
        if (frame.fd < 0) {
            m_exportError = "exportFrame produced no descriptor";
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.ExportFailed"),
                                                            QString::fromStdString(m_exportError)));
        }

        QDBusUnixFileDescriptor handle(frame.fd);
        ::close(frame.fd);  // QDBusUnixFileDescriptor took its own duplicate.

        // The acquire descriptor, exported the way the daemon exports it: a fresh already-signalled
        // binary semaphore per hand-over. Unconditional rather than opt-in, because it is what the
        // daemon sends whenever it can, and a stub that served an easier reply than the real
        // producer does would leave the item's whole receive path graded against a shape nothing
        // produces.
        //
        // A driver that cannot export one sends none and says so, rather than failing the reply:
        // that is exactly what the daemon does, and a caller can then tell "this host has no
        // acquire" from "the plugin lost it".
        QList<QDBusUnixFileDescriptor> syncFds;
        try {
            const int acquireFd = wallpaperx::exportFrameAcquireFd(m_device, m_target);
            if (acquireFd >= 0) {
                syncFds.append(QDBusUnixFileDescriptor(acquireFd));
                ::close(acquireFd);
            }
        } catch (const std::exception& e) {
            m_acquireError = e.what();
        }
        m_acquireFdsSent += static_cast<int>(syncFds.size());

        QList<FramePlane> planes;
        for (const wallpaperx::ExportedFramePlane& plane : frame.planes) {
            planes.append(FramePlane{plane.offset, plane.rowPitch});
        }

        m_handleType = frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf ? 1 : 0;
        ++m_served;
        // Both taken from the export rather than invented: the importer refuses a frame whose
        // device is not the one this context runs on, and these tests import for real.
        return connection.send(message.createReply(
            frameReplyFields(handle, frame.width, frame.height, frame.memorySize, m_handleType,
                             /*generation=*/1, frame.drmFormat, frame.modifier, planes,
                             QByteArray(reinterpret_cast<const char*>(frame.deviceUuid.data()),
                                        static_cast<qsizetype>(frame.deviceUuid.size())),
                             frame.bufferId, syncFds)));
    }

    int requests() const { return m_requests; }
    int served() const { return m_served; }
    int handleType() const { return m_handleType; }
    const std::string& exportError() const { return m_exportError; }

    // How many acquire descriptors went out with the frames served. Read by the checks that are
    // ABOUT the acquire, so a stub that quietly stopped sending one turns them from a claim about
    // the plugin into a claim about nothing.
    int acquireFdsSent() const { return m_acquireFdsSent; }
    const std::string& acquireError() const { return m_acquireError; }

private:
    wallpaperx::Device& m_device;
    const wallpaperx::OffscreenTarget& m_target;
    int m_requests = 0;
    int m_served = 0;
    int m_handleType = -1;
    int m_acquireFdsSent = 0;
    std::string m_exportError;
    std::string m_acquireError;
};

// Whether the GL context must be on the same GPU as the exporter. Only an opaque fd is device
// bound; a dma_buf imports across devices. Asked of a real export, because the stub does not know
// its handle type until it has served a frame, which is after every caller's gate.
inline bool exportNeedsSameDevice(wallpaperx::Device& device, const wallpaperx::OffscreenTarget& target) {
    wallpaperx::ExportedFrame probe = wallpaperx::exportFrame(device, target);
    const bool needs = probe.type != wallpaperx::ExportedFrame::HandleType::DmaBuf;
    if (probe.fd >= 0) ::close(probe.fd);
    return needs;
}

// Runs the event loop until `done` or the deadline. Bounded, and the caller must have a branch
// for false: a run that never reached its precondition must say so rather than fall through.
inline bool pumpUntil(const std::function<bool()>& done, int limitMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > limitMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

inline QSGNode* itemPaintNode(QQuickItem* item) {
    return item == nullptr ? nullptr : QQuickItemPrivate::get(item)->paintNode;
}

inline QSGTexture* itemTexture(QQuickItem* item) {
    auto* node = static_cast<QSGSimpleTextureNode*>(itemPaintNode(item));
    return node == nullptr ? nullptr : node->texture();
}

inline bool claimsFrameShown(QQuickItem* item) { return item->property("frameShown").toBool(); }

// GL_DEVICE_UUID_EXT, read from whatever context is current. False when the context has no
// GL_EXT_memory_object -- which is also a context that could not have imported anything.
inline bool glDeviceUuid(std::array<uint8_t, 16>& out) {
    using GetUnsignedBytevFn = void (*)(GLenum, GLubyte*);
    auto getBytes = reinterpret_cast<GetUnsignedBytevFn>(eglGetProcAddress("glGetUnsignedBytevEXT"));
    if (getBytes == nullptr) return false;
    while (glGetError() != GL_NO_ERROR) {
    }
    constexpr GLenum kDeviceUuidExt = 0x9597;  // GL_DEVICE_UUID_EXT
    getBytes(kDeviceUuidExt, out.data());
    return glGetError() == GL_NO_ERROR;
}

// Instantiates the plugin's own registered type, so a module that fails to load or a type that
// stops resolving is a failure of the thing under test rather than of this scene.
inline QString qmlSource() {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: 68
    height: 36
    property Item rendererItem: renderer
    // Read only by the "never imported" skip, so that a run which could not set up says what the
    // item thought was wrong instead of naming nothing.
    property string publishedReason: renderer.unavailableReason

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
    }
}
)");
}

}  // namespace wallpaperx::testing
