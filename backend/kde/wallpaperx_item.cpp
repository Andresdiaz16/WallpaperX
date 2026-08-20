// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaperx_item.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <unistd.h>

#include <utility>

namespace wallpaperx {

namespace {

constexpr const char* kService = "io.wallpaperx.Daemon";
constexpr const char* kPath = "/io/wallpaperx/Daemon";
constexpr const char* kInterface = "io.wallpaperx.Daemon";

}  // namespace

WallpaperXItem::PendingFrame::~PendingFrame() {
    // Nobody imported it, so nobody consumed the fd. The import contract is that a FAILED
    // import leaves the fd with the caller, and a frame that is superseded before the render
    // thread arrives was never offered to the importer at all.
    if (request.fd >= 0) ::close(request.fd);
}

WallpaperXItem::PendingFrame::PendingFrame(PendingFrame&& other) noexcept
    : request(other.request), generation(other.generation) {
    other.request.fd = -1;
}

WallpaperXItem::PendingFrame& WallpaperXItem::PendingFrame::operator=(PendingFrame&& other) noexcept {
    if (this != &other) {
        if (request.fd >= 0) ::close(request.fd);
        request = other.request;
        generation = other.generation;
        other.request.fd = -1;
    }
    return *this;
}

WallpaperXItem::WallpaperXItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    connectToDaemon();
}

WallpaperXItem::~WallpaperXItem() = default;

void WallpaperXItem::setMonitorId(int monitorId) {
    if (m_monitorId == monitorId) return;
    m_monitorId = monitorId;
    emit monitorIdChanged();
    // A screen that has just been identified may already have a frame waiting; without this the
    // desktop stays blank until the next time the wallpaper happens to change.
    if (m_daemonConnected && m_monitorId >= 0) fetchFrame(0);
}

void WallpaperXItem::setDaemonConnected(bool connected) {
    if (m_daemonConnected == connected) return;
    m_daemonConnected = connected;
    emit daemonConnectedChanged();
}

void WallpaperXItem::setUnavailableReason(const QString& reason) {
    if (m_unavailableReason == reason) return;
    m_unavailableReason = reason;
    emit unavailableReasonChanged();
}

void WallpaperXItem::connectToDaemon() {
    m_daemon = new QDBusInterface(QString::fromUtf8(kService), QString::fromUtf8(kPath),
                                 QString::fromUtf8(kInterface), QDBusConnection::sessionBus(), this);
    if (!m_daemon->isValid()) {
        setUnavailableReason(tr("WallpaperX is not running."));
        return;
    }
    QDBusConnection::sessionBus().connect(QString::fromUtf8(kService), QString::fromUtf8(kPath),
                                          QString::fromUtf8(kInterface), QStringLiteral("FrameReady"), this,
                                          SLOT(onFrameReady(int, qulonglong)));
    setDaemonConnected(true);
    if (m_monitorId >= 0) fetchFrame(0);
}

void WallpaperXItem::onFrameReady(int monitorId, qulonglong generation) {
    if (monitorId != m_monitorId) return;
    fetchFrame(generation);
}

void WallpaperXItem::fetchFrame(quint64 generation) {
    if (m_daemon == nullptr || m_monitorId < 0) return;

    // Blocking, and deliberately on this thread: the render thread must never wait on the bus,
    // and the daemon answers this from its own GPU worker rather than its event loop, so a slow
    // render here does not stall the daemon's other callers either.
    QDBusReply<QDBusUnixFileDescriptor> reply;
    QDBusMessage call = m_daemon->call(QStringLiteral("GetFrameFd"), m_monitorId);
    if (call.type() != QDBusMessage::ReplyMessage) {
        // Not a failure of this item: no frame yet is the ordinary state before the user has
        // chosen a wallpaper, and the daemon says which case it is.
        setUnavailableReason(call.errorMessage().isEmpty() ? tr("WallpaperX has no frame for this screen yet.")
                                                           : call.errorMessage());
        return;
    }

    const QList<QVariant> args = call.arguments();
    // Ten fields, in the order GetFrameFd declares them. Checked rather than assumed: a daemon
    // from a newer build could answer with more, and reading past the end of a short reply is
    // the kind of crash that takes plasmashell with it.
    if (args.size() < 10) {
        setUnavailableReason(tr("WallpaperX answered with something this plugin cannot read."));
        return;
    }

    PendingFrame frame;
    frame.request.fd = ::dup(args.at(0).value<QDBusUnixFileDescriptor>().fileDescriptor());
    frame.request.width = args.at(1).toUInt();
    frame.request.height = args.at(2).toUInt();
    frame.request.memorySize = args.at(3).toULongLong();
    frame.request.handleType = args.at(4).toInt() == 1 ? FrameImport::HandleType::DmaBuf
                                                       : FrameImport::HandleType::OpaqueFd;
    frame.generation = args.at(6).toULongLong();
    frame.request.drmFormat = args.at(7).toUInt();
    frame.request.drmModifier = args.at(8).toULongLong();

    if (frame.request.fd < 0) {
        setUnavailableReason(tr("WallpaperX sent a frame this plugin could not take hold of."));
        return;
    }
    Q_UNUSED(generation);

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        // Replaced, not queued: an older frame waiting behind this one describes a state this
        // one already supersedes, and its destructor closes its fd.
        m_pending = std::move(frame);
    }
    setUnavailableReason(QString());
    update();
}

QSGNode* WallpaperXItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGSimpleTextureNode*>(old);

    // The scene graph can be running on Vulkan, Metal, D3D or software, and this import path is
    // OpenGL only. Checked every pass rather than once, because the window can change.
    QQuickWindow* host = window();
    if (host == nullptr || host->rendererInterface() == nullptr ||
        host->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
        setUnavailableReason(tr("This desktop is not running on OpenGL, which WallpaperX needs to show a frame."));
        delete node;
        return nullptr;
    }

    std::optional<PendingFrame> incoming;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        incoming = std::move(m_pending);
        m_pending.reset();
    }

    if (incoming.has_value()) {
        // Older than what is already on screen: several fetches can be in flight for one
        // monitor and they need not complete in order, so the newest frame wins by number
        // rather than by arrival. Its fd is closed by ~PendingFrame.
        if (incoming->generation != 0 && incoming->generation < m_shownGeneration) {
            incoming.reset();
        }
    }

    if (incoming.has_value()) {
        const FrameImport::Result imported = FrameImport::import(incoming->request);
        if (imported.texture == 0) {
            setUnavailableReason(tr("WallpaperX produced a frame this desktop could not import."));
        } else {
            // The fd was consumed by the import, so the pending frame must not close it.
            incoming->request.fd = -1;

            // Freed only now, with the new texture in hand: releasing the old one before the
            // node has been repointed leaves the scene graph holding a deleted texture for the
            // rest of this pass.
            const FrameImport::Result previous = m_imported;
            m_imported = imported;
            m_shownGeneration = incoming->generation;

            if (node == nullptr) {
                node = new QSGSimpleTextureNode();
                node->setOwnsTexture(true);
                node->setFiltering(QSGTexture::Linear);
            }
            QSGTexture* texture = QNativeInterface::QSGOpenGLTexture::fromNative(
                imported.texture, host, QSize(static_cast<int>(incoming->request.width),
                                              static_cast<int>(incoming->request.height)),
                QQuickWindow::TextureHasAlphaChannel);
            node->setTexture(texture);
            node->markDirty(QSGNode::DirtyMaterial);
            setUnavailableReason(QString());

            // After the swap, and on this thread with the context current -- the two conditions
            // S12.4 is about.
            FrameImport::release(previous);
        }
    }

    if (node == nullptr) return nullptr;
    node->setRect(boundingRect());
    return node;
}

void WallpaperXItem::releaseResources() {
    // The scene graph is taking the window away. The GL objects belong to that context, so they
    // go with it; holding the names would leave this item pointing at freed driver state.
    m_imported = FrameImport::Result{};
    m_shownGeneration = 0;
}

}  // namespace wallpaperx
