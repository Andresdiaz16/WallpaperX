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
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QThread>

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

// Moved, not copied. Request holds a std::vector, so copying it can throw -- and a throw out of
// a noexcept function is std::terminate, here inside plasmashell.
WallpaperXItem::PendingFrame::PendingFrame(PendingFrame&& other) noexcept
    : request(std::move(other.request)), generation(other.generation) {
    other.request.fd = -1;
}

WallpaperXItem::PendingFrame& WallpaperXItem::PendingFrame::operator=(PendingFrame&& other) noexcept {
    if (this != &other) {
        if (request.fd >= 0) ::close(request.fd);
        request = std::move(other.request);
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

    forgetGenerations();

    emit monitorIdChanged();
    // A screen that has just been identified may already have a frame waiting; without this the
    // desktop stays blank until the next time the wallpaper happens to change.
    if (m_monitorId >= 0) fetchFrame(0);
}

// Every generation mark at once. Generations belong to one monitor and one daemon instance, so a
// change of either invalidates all of them -- resetting some and not the rest is what made a
// restarted daemon's frames read as older than the screen.
void WallpaperXItem::forgetGenerations() {
    m_fetchedGeneration = 0;
    m_retriedGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.reset();
    }
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        m_shownGeneration = 0;
        ++m_generationEpoch;
    }
    publishReason();
}

// The two producers write their own fact and never the published string, because a queued clear
// posted by one can otherwise land after the other has published a newer error and wipe it.
// Recomputing from both facts gives the same answer whatever order the hops arrive in.
void WallpaperXItem::setFetchReason(const QString& reason) {
    m_fetchReason = reason;
    publishReason();
}

void WallpaperXItem::setRenderReason(const QString& reason) {
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        m_renderReason = reason;
    }
    QMetaObject::invokeMethod(this, [this] { publishReason(); }, Qt::QueuedConnection);
}

void WallpaperXItem::publishReason() {
    QString render;
    bool shown = false;
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        render = m_renderReason;
        // Derived, never stored twice: a hand-maintained copy has to be cleared again on every
        // teardown path, and missing one leaves the desktop claiming a frame it no longer has.
        shown = m_nodeHasFrame;
    }
    if (m_frameShown != shown) {
        m_frameShown = shown;
        emit frameShownChanged();
    }
    // The fetch side wins: it describes the newer observation, and a frame that cannot be
    // imported matters less than not having reached the daemon at all.
    QString settled = m_fetchReason.isEmpty() ? render : m_fetchReason;
    // A missing subscription outlives any single frame, so a good frame must not clear it: the
    // wallpaper would be shown and then never change again, with nothing said.
    if (settled.isEmpty() && !m_subscribed) {
        settled = tr("This plugin could not subscribe to WallpaperX frame updates, so the wallpaper "
                     "will not change.");
    }
    if (m_unavailableReason == settled) return;
    m_unavailableReason = settled;
    emit unavailableReasonChanged();
}

bool WallpaperXItem::subscribeToFrameReady() {
    return QDBusConnection::sessionBus().connect(QString::fromUtf8(kService), QString::fromUtf8(kPath),
                                                 QString::fromUtf8(kInterface), QStringLiteral("FrameReady"), this,
                                                 SLOT(onFrameReady(int, qulonglong)));
}

void WallpaperXItem::connectToDaemon() {
    // Messages are built by hand rather than through QDBusInterface, whose constructor blocks on
    // an Introspect round trip -- on the thread that runs the compositor's UI, from a QML item's
    // constructor. Nothing here waits on the bus.
    m_subscribed = subscribeToFrameReady();
    // No early return on failure: an item that cannot hear FrameReady must still notice the
    // daemon arriving or leaving, and the watcher below is the only thing that can retry.
    if (!m_subscribed) publishReason();

    // The item is created with the desktop, so the daemon may not own its name yet.
    auto* watcher = new QDBusServiceWatcher(QString::fromUtf8(kService), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString&, const QString& newOwner) {
                if (newOwner.isEmpty()) {
                    setFetchReason(tr("WallpaperX is not running."));
                    return;
                }
                // Gated: an unconditional retry would add a duplicate match rule per restart,
                // which is one of the things that makes the subscription fail in the first place.
                if (!m_subscribed) m_subscribed = subscribeToFrameReady();
                // A restarted daemon numbers from scratch, so every mark is meaningless now.
                forgetGenerations();
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

    // The mark above only moves when a reply lands, so it cannot suppress a burst that arrives
    // while one is out. One trailing fetch subsumes them, and it carries the highest generation
    // announced rather than 0 -- 0 forces, so a reply that already brought the newest frame
    // would otherwise be followed by a redundant export and a second import of the same image.
    if (m_fetchInFlight) {
        if (!m_queuedFetch) {
            m_queuedFetch = generation;
        } else if (*m_queuedFetch == 0 || generation == 0) {
            m_queuedFetch = 0;  // a forced fetch stays forced
        } else {
            m_queuedFetch = std::max(*m_queuedFetch, generation);
        }
        return;
    }

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
    // Checked before asking, not after: with no bus, asyncCall hands back a null pending call
    // whose watcher never emits anything, so the in-flight latch below would never clear and
    // this item would stop fetching for the rest of the session. Reported rather than inlined --
    // answering the reply here would make the fetch synchronous for every caller.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        setFetchReason(tr("WallpaperX could not be reached."));
        return;
    }
    QDBusPendingCall pending = bus.asyncCall(request);
    m_fetchInFlight = true;
    auto* watcher = new QDBusPendingCallWatcher(pending, this);
    // The reply carries no monitor id of its own.
    const int requestedMonitor = m_monitorId;
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, requestedMonitor](QDBusPendingCallWatcher* done) {
                done->deleteLater();
                // Cleared before any early return, or one dropped reply stops fetching for good.
                m_fetchInFlight = false;
                const std::optional<quint64> queued = std::exchange(m_queuedFetch, std::nullopt);
                if (requestedMonitor == m_monitorId) takeFrameReply(done->reply());
                if (queued) fetchFrame(*queued);
            });
}

// Render thread. The set belongs to the process's EGL driver, not to an item -- plasmashell can
// hold one WallpaperXItem per screen and they all import through the same driver -- so it is
// queried and sent once, and every later caller returns immediately.
void WallpaperXItem::reportImportModifiersOnce(quint32 drmFormat) {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true)) return;

    std::string whyNot;
    const std::vector<uint64_t> modifiers = FrameImport::importableModifiers(drmFormat, &whyNot);
    if (modifiers.empty()) {
        // Not an error and not retried: a driver with no such query has none to report, and the
        // daemon's unnegotiated default is what it already uses.
        std::fprintf(stderr, "wallpaperx: not reporting import modifiers -- %s\n", whyNot.c_str());
        return;
    }

    // Sent from the GUI thread: this one owns no bus connection, and QDBusConnection is not
    // safe to use from the scene graph's thread.
    QMetaObject::invokeMethod(
        this,
        [drmFormat, modifiers] {
            QDBusConnection bus = QDBusConnection::sessionBus();
            if (!bus.isConnected()) return;
            QDBusMessage request = QDBusMessage::createMethodCall(
                QString::fromUtf8(kService), QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                QStringLiteral("SetImportModifiers"));
            QList<quint64> wire;
            wire.reserve(static_cast<qsizetype>(modifiers.size()));
            for (uint64_t modifier : modifiers) wire.append(modifier);
            request.setArguments({drmFormat, QVariant::fromValue(wire)});
            // Fire and forget: the daemon answers by re-rendering and announcing FrameReady,
            // which this item already listens for. Waiting here would park the desktop's UI.
            bus.asyncCall(request);
        },
        Qt::QueuedConnection);
}

void WallpaperXItem::takeFrameReply(const QDBusMessage& call) {
    if (call.type() != QDBusMessage::ReplyMessage) {
        // A bus error is jargon about transport ("The name io.wallpaperx.Daemon was not provided
        // by any .service files"), so it is answered in this plugin's own words. Only the
        // daemon's own errors carry text worth putting on a desktop.
        const QString name = call.errorName();
        if (name.startsWith(QLatin1String("org.freedesktop.DBus.Error."))) {
            setFetchReason(name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
                                     ? tr("WallpaperX is not running.")
                                     : tr("WallpaperX could not be reached."));
            return;
        }
        // Not a failure of this item: no frame yet is the ordinary state before the user has
        // chosen a wallpaper, and the daemon says which case it is.
        setFetchReason(call.errorMessage().isEmpty() ? tr("WallpaperX has no frame for this screen yet.")
                                                           : call.errorMessage());
        return;
    }

    const QList<QVariant> args = call.arguments();
    // Eleven fields, in the order GetFrameFd declares them. Checked rather than assumed: a daemon
    // from a newer build could answer with more, and reading past the end of a short reply is
    // the kind of crash that takes plasmashell with it.
    if (!replyIsReadable(static_cast<int>(args.size()))) {
        setFetchReason(tr("WallpaperX answered with something this plugin cannot read."));
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
        setFetchReason(tr("WallpaperX sent a frame that must be waited on, which this plugin cannot do yet."));
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

    // Field 10, device_uuid: which GPU allocated the memory. Validated by the importer, the
    // only place that knows which device the import would run on.
    const QByteArray uuid = args.at(10).toByteArray();
    frame.request.deviceUuid.assign(uuid.begin(), uuid.end());

    // Field 11, buffer_id: which image, where frame_generation says which render. Carried so
    // the import can be keyed on it once Stage F keeps imported buffers instead of redoing one
    // per frame; nothing reads it yet.
    frame.request.bufferId = args.at(11).toULongLong();

    if (frame.request.fd < 0) {
        setFetchReason(tr("WallpaperX sent a frame this plugin could not take hold of."));
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
    setFetchReason(QString());
    update();
}

QSGNode* WallpaperXItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGSimpleTextureNode*>(old);

    // No window or no interface is transient -- a reparent in progress -- so nothing is reported
    // and nothing is torn down; the next sync pass has one.
    QQuickWindow* host = window();
    if (host == nullptr || host->rendererInterface() == nullptr) return old;

    // The scene graph can be running on Vulkan, Metal, D3D or software, and this import path is
    // OpenGL only. Checked every pass rather than once, because the window can change.
    if (host->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
        setRenderReason(tr("This desktop is not running on OpenGL, which WallpaperX needs to show a frame."));
        // The release runs, and must. Reaching here means no OpenGL context is current, so its
        // glDeleteTextures and glDeleteMemoryObjectsEXT are silent no-ops -- but the GL context
        // that owned those names is gone and took them with it. Its eglDestroyImageKHR is the
        // part that matters: an EGLImage belongs to the display, not the context, needs no
        // current context, and nothing else destroys it. Cancelling the release to avoid the two
        // no-ops would pin the daemon's dma_buf for the life of the session.
        delete node;
        {
            std::lock_guard<std::mutex> lock(m_importedMutex);
            m_shownGeneration = 0;
            m_nodeHasFrame = false;
        }
        return nullptr;
    }

    // The epoch is read BEFORE the frame is claimed, not after. forgetGenerations() clears
    // m_pending and bumps the epoch under two separate locks, in that order, so an epoch read
    // afterwards can be the new one while the frame in hand is the old monitor's -- and the
    // write-back guard would then accept a mark that outlived the frames it describes.
    quint64 shown = 0;
    quint64 epoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        shown = m_shownGeneration;
        epoch = m_generationEpoch;
    }

    std::optional<PendingFrame> incoming;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        incoming = std::move(m_pending);
        m_pending.reset();
    }
    // Dropped if it is older than what is on screen; its fd is closed by ~PendingFrame.
    if (incoming.has_value() && !supersedesShown(Arriving{incoming->generation}, Held{shown})) {
        incoming.reset();
    }

    if (incoming.has_value()) {
        // Before the import, deliberately. If the daemon chose a layout this driver cannot take,
        // the import below fails -- and reporting only on success would mean the one case
        // negotiation exists for never reaches the daemon.
        if (incoming->request.handleType == FrameImport::HandleType::DmaBuf) {
            reportImportModifiersOnce(incoming->request.drmFormat);
        }
        const FrameImport::Result imported = FrameImport::import(incoming->request);
        // Null means nothing took ownership of the import, so it is freed by hand here.
        QSGTexture* texture =
            imported.texture == 0
                ? nullptr
                // No alpha flag: every fourcc the daemon can export is an X variant, so the alpha
                // bytes are undefined and the scene graph must not blend against them.
                : QNativeInterface::QSGOpenGLTexture::fromNative(
                      imported.texture, host,
                      QSize(static_cast<int>(incoming->request.width), static_cast<int>(incoming->request.height)),
                      {});
        if (texture == nullptr) {
            if (imported.texture != 0) FrameImport::release(imported);
            setRenderReason(tr("WallpaperX produced a frame this desktop could not import."));
            // The node goes too: keeping one would leave the desktop showing a frame while the
            // item reports it is drawing nothing.
            delete node;
            node = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_importedMutex);
                m_shownGeneration = 0;
                m_nodeHasFrame = false;
            }
            // Asked for once more. A still wallpaper sends no further FrameReady, so without this
            // a transient failure is permanent. The whole decision runs on the item's own thread,
            // marks included -- this branch is the render thread's.
            const quint64 failed = incoming->generation;
            QMetaObject::invokeMethod(
                this,
                [this, failed, epoch] {
                    // Epoch-guarded like the mark above, and for the same reason: a monitor
                    // change or daemon restart can land between the post and its delivery, and
                    // recording a retry for a generation that has since been discarded burns the
                    // one retry a later, real failure would need.
                    {
                        std::lock_guard<std::mutex> lock(m_importedMutex);
                        if (m_generationEpoch != epoch) return;
                    }
                    if (m_retriedGeneration == failed) return;
                    m_retriedGeneration = failed;
                    m_fetchedGeneration = 0;
                    fetchFrame(0);
                },
                Qt::QueuedConnection);
        } else {
            // Built only now: a node handed back without a texture is a null dereference inside
            // the scene graph.
            if (node == nullptr) {
                node = new QSGSimpleTextureNode();
                // Load-bearing for the release below: the node destroying its texture is what
                // frees the import.
                node->setOwnsTexture(true);
                node->setFiltering(QSGTexture::Linear);
            }
            // Tied to the texture rather than scheduled as a render job: a NoStage job posted
            // against an unexposed window is dropped without running, and by then this is the
            // only reference left. Node teardown and setTexture both run on the render thread
            // with the context current, which release needs.
            QObject::connect(
                texture, &QObject::destroyed, texture, [imported] { FrameImport::release(imported); },
                Qt::DirectConnection);
            node->setTexture(texture);
            node->markDirty(QSGNode::DirtyMaterial);
            {
                std::lock_guard<std::mutex> lock(m_importedMutex);
                // Only if nothing invalidated the marks while the import ran. The lock cannot be
                // held across GL work, so a monitor change, a daemon restart or a detach can land
                // in that gap -- and writing this number back afterwards would judge the next
                // screen's, or the next daemon's, frames against a number that outlived them.
                if (m_generationEpoch == epoch) m_shownGeneration = incoming->generation;
                // Not epoch-guarded, unlike the mark above: this says a node is drawing, which is
                // true whatever the epoch did while the import ran.
                m_nodeHasFrame = true;
            }
            // Cleared so "retried once" means once per failure, not once per session: the same
            // generation can be re-fetched later (a window change re-asks) and must be allowed
            // its retry again if that import fails too.
            QMetaObject::invokeMethod(
                this,
                [this] { m_retriedGeneration = 0; },
                Qt::QueuedConnection);
            setRenderReason(QString());
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

// The imported handles are the scene graph's to free -- Qt's own exemption for resources its
// nodes already manage -- so all this drops is the marks, which no longer describe anything.
void WallpaperXItem::releaseResources() {
    m_fetchedGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        m_shownGeneration = 0;
        m_nodeHasFrame = false;
        ++m_generationEpoch;
    }
    // Scoped above, because this takes the same non-recursive mutex.
    publishReason();
}

}  // namespace wallpaperx
