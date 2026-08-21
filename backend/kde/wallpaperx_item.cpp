// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaperx_item.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QQuickWindow>
#include <QRunnable>
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

bool WallpaperXItem::replyIsReadable(int fieldCount) { return fieldCount >= kFrameReplyFields; }

bool WallpaperXItem::worthFetching(Arriving generation, Held newestFetched) {
    if (generation.value == 0) return true;
    return generation.value > newestFetched.value;
}

bool WallpaperXItem::supersedesShown(Arriving generation, Held shown) {
    if (generation.value == 0) return true;
    return generation.value >= shown.value;
}

WallpaperXItem::WallpaperXItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    connectToDaemon();
}

WallpaperXItem::~WallpaperXItem() = default;

void WallpaperXItem::setMonitorId(int monitorId) {
    if (m_monitorId == monitorId) return;
    m_monitorId = monitorId;

    // Generations are per monitor, so the old screen's marks would reject the new screen's frames.
    m_fetchedGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.reset();
    }
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        m_shownGeneration = 0;
    }

    emit monitorIdChanged();
    // A screen that has just been identified may already have a frame waiting; without this the
    // desktop stays blank until the next time the wallpaper happens to change.
    if (m_monitorId >= 0) fetchFrame(0);
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
    // Messages are built by hand rather than through QDBusInterface, whose constructor blocks on
    // an Introspect round trip -- on the thread that runs the compositor's UI, from a QML item's
    // constructor. Nothing here waits on the bus.
    QDBusConnection::sessionBus().connect(QString::fromUtf8(kService), QString::fromUtf8(kPath),
                                          QString::fromUtf8(kInterface), QStringLiteral("FrameReady"), this,
                                          SLOT(onFrameReady(int, qulonglong)));
    // The item is created with the desktop, so the daemon may not own its name yet.
    auto* watcher = new QDBusServiceWatcher(QString::fromUtf8(kService), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString&, const QString& newOwner) {
                setDaemonConnected(!newOwner.isEmpty());
                if (newOwner.isEmpty()) {
                    setUnavailableReason(tr("WallpaperX is not running."));
                    return;
                }
                // A restarted daemon numbers from scratch.
                m_fetchedGeneration = 0;
                fetchFrame(0);
            });

    if (m_monitorId >= 0) fetchFrame(0);
}

void WallpaperXItem::onFrameReady(int monitorId, qulonglong generation) {
    if (monitorId != m_monitorId) return;
    fetchFrame(generation);
}

void WallpaperXItem::fetchFrame(quint64 generation) {
    if (m_monitorId < 0) return;

    // Already have this frame, or a newer one. Asking is not free: a bus round trip, an export
    // by the daemon, a dup() here. A signal for a frame already fetched is ordinary.
    //
    // Zero means "fetch whatever is there": it is what the two callers that are not signals
    // pass, when this item has just learned its monitor id or just connected, and neither knows
    // what generation the daemon is on.
    if (!worthFetching(Arriving{generation}, Held{m_fetchedGeneration})) return;

    // ASYNCHRONOUS, and this is not a preference. A blocking call here parks the thread that
    // runs the compositor's UI until the daemon answers -- and worse, the first fetch happens
    // while QML is still building this item, where the nested event loop a blocking call needs
    // never gets to run. Measured: the call never returned, the item drew nothing, and the
    // desktop had no way to say why. The daemon answering the same request over qdbus in
    // milliseconds is what located it.
    QDBusMessage request = QDBusMessage::createMethodCall(
        QString::fromUtf8(kService), QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
        QStringLiteral("GetFrameFd"));
    request.setArguments({m_monitorId});
    QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(request);
    auto* watcher = new QDBusPendingCallWatcher(pending, this);
    // The reply carries no monitor id of its own.
    const int requestedMonitor = m_monitorId;
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, requestedMonitor](QDBusPendingCallWatcher* done) {
                done->deleteLater();
                if (requestedMonitor != m_monitorId) return;
                takeFrameReply(done->reply());
            });
}

void WallpaperXItem::takeFrameReply(const QDBusMessage& call) {
    // The reply is what settles it: only an unknown service means the daemon is not there.
    const bool absent = call.type() == QDBusMessage::ErrorMessage &&
                        call.errorName() == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown");
    setDaemonConnected(!absent);

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
    if (!replyIsReadable(static_cast<int>(args.size()))) {
        setUnavailableReason(tr("WallpaperX answered with something this plugin cannot read."));
        return;
    }

    // Field 5, sync_fds. The daemon sends none, and that is correct rather than unfinished: it
    // waits on a fence before answering, so the frame is complete by the time the reply leaves.
    // Refused rather than ignored if that ever changes -- a fence this plugin does not wait on
    // means sampling a half-drawn frame, which shows as tearing that nothing here explains.
    const QDBusArgument syncFds = args.at(5).value<QDBusArgument>();
    int syncFdCount = 0;
    syncFds.beginArray();
    while (!syncFds.atEnd()) {
        QDBusUnixFileDescriptor ignored;
        syncFds >> ignored;
        ++syncFdCount;
    }
    syncFds.endArray();
    if (syncFdCount > 0) {
        setUnavailableReason(tr("WallpaperX sent a frame that must be waited on, which this plugin cannot do yet."));
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

    // Field 9, the per-plane offsets and pitches. Only the dma_buf path needs them, but they are
    // read for both: an opaque frame simply carries none, and reading conditionally would make
    // the parse depend on a field the parse has just read.
    const QDBusArgument planes = args.at(9).value<QDBusArgument>();
    planes.beginArray();
    while (!planes.atEnd()) {
        quint64 offset = 0;
        quint64 pitch = 0;
        planes.beginStructure();
        planes >> offset >> pitch;
        planes.endStructure();
        frame.request.planes.emplace_back(offset, pitch);
    }
    planes.endArray();

    if (frame.request.fd < 0) {
        setUnavailableReason(tr("WallpaperX sent a frame this plugin could not take hold of."));
        return;
    }

    // Recorded from the REPLY, not from the signal that prompted the fetch: the daemon may have
    // rendered again between the two, and the reply describes the frame actually in hand.
    m_fetchedGeneration = frame.generation;

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

    quint64 shown = 0;
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        shown = m_shownGeneration;
    }
    // Dropped if it is older than what is on screen; its fd is closed by ~PendingFrame.
    if (incoming.has_value() && !supersedesShown(Arriving{incoming->generation}, Held{shown})) {
        incoming.reset();
    }

    if (incoming.has_value()) {
        const FrameImport::Result imported = FrameImport::import(incoming->request);
        if (imported.texture == 0) {
            setUnavailableReason(tr("WallpaperX produced a frame this desktop could not import."));
        } else {
            // Freed only now, with the new texture in hand: releasing the old one before the
            // node has been repointed leaves the scene graph holding a deleted texture for the
            // rest of this pass.
            FrameImport::Result previous;
            {
                std::lock_guard<std::mutex> lock(m_importedMutex);
                previous = m_imported;
                m_imported = imported;
                m_shownGeneration = incoming->generation;
            }

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

// A still wallpaper sends no further FrameReady, so a released item must ask on its own.
void WallpaperXItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change != ItemSceneChange || value.window == nullptr) return;
    if (m_monitorId < 0) return;
    fetchFrame(0);
}

void WallpaperXItem::releaseResources() {
    // Called when this item leaves its window, which is NOT necessarily when the context dies:
    // an item moved between windows leaves a live context behind, and simply forgetting the
    // names there leaks a texture and a memory object every time.
    //
    // They cannot be deleted here either -- this runs on the GUI thread and the context belongs
    // to the render thread. scheduleRenderJob is the way across: the job runs on the render
    // thread with the context current, which is the same pair of conditions updatePaintNode
    // satisfies. It takes a copy, so nothing outlives this object.

    // Above the early return, so an item holding nothing drops the mark too.
    m_fetchedGeneration = 0;

    FrameImport::Result doomed;
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        if (m_imported.texture == 0 && m_imported.memoryObject == 0 && m_imported.eglImage == nullptr) return;
        doomed = m_imported;
        m_imported = FrameImport::Result{};
        m_shownGeneration = 0;
    }

    QQuickWindow* host = window();

    if (host == nullptr) {
        // No window to schedule against, so the context this belonged to is already gone and
        // the driver freed its objects with it. Nothing to do but drop the names, which the
        // assignment above has done.
        return;
    }

    class ReleaseJob : public QRunnable {
    public:
        explicit ReleaseJob(FrameImport::Result imported) : m_imported(imported) {}
        void run() override { FrameImport::release(m_imported); }

    private:
        FrameImport::Result m_imported;
    };
    host->scheduleRenderJob(new ReleaseJob(doomed), QQuickWindow::NoStage);
}

}  // namespace wallpaperx
