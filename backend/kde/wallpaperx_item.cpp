// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaperx_item.h"

#include <QCoreApplication>
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

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wallpaperx {

namespace {

// The EGL driver's answer, not an item's. Cleared on a daemon restart.
std::atomic<bool> g_modifiersReported{false};

// Set when the daemon refused this process's report, cleared when the retry it earns is armed.
// Without it every consumer would re-report on every change, which is the poll this avoids.
std::atomic<bool> g_reportRefused{false};

// The set last reported, kept so a modifier that turns out not to import can be withdrawn.
// eglQueryDmaBufModifiersEXT is not the last word: NVIDIA advertises modifiers for this format
// that eglCreateImage then refuses for a Vulkan-exported buffer.
std::mutex g_reportedMutex;
std::vector<uint64_t> g_reportedModifiers;

void sendImportModifiers(QObject* context, quint32 drmFormat, const std::vector<uint64_t>& modifiers,
                         std::vector<uint64_t> restoreOnFailure);

constexpr const char* kService = "io.wallpaperx.Daemon";
constexpr const char* kPath = "/io/wallpaperx/Daemon";
constexpr const char* kInterface = "io.wallpaperx.Daemon";

// The daemon keeps one image per monitor free to render into, and refuses a hand-over that
// would take the last one. Not a failure of either side, and not permanent.
constexpr const char* kErrorNoFreeImage = "io.wallpaperx.Daemon.Error.NoFreeImage";

}  // namespace

namespace {

// Every descriptor a request still owns. The import contract is that a FAILED import leaves them
// with the caller, and a frame superseded before the render thread arrives was never offered to
// the importer at all -- so both cases end here. One helper because a sync descriptor forgotten on
// one of the three paths below leaks at exactly the same rate as the memory one, which is one per
// frame, which ends a login session.
void closeDescriptors(FrameImport::Request& request) {
    if (request.fd >= 0) ::close(request.fd);
    request.fd = -1;
    for (int& syncFd : request.syncFds) {
        if (syncFd >= 0) ::close(syncFd);
        syncFd = -1;
    }
}

// Disowns them instead, for the source of a move.
void disownDescriptors(FrameImport::Request& request) {
    request.fd = -1;
    for (int& syncFd : request.syncFds) syncFd = -1;
}

// Render thread, context current. A driver that imports memory but not semaphores must keep
// drawing wallpapers: the frame is then read without the producer's queue-family acquire, which is
// undefined and is what every consumer did before sync_fds carried anything. Dropped here rather
// than inside import(), which refuses a wait it cannot perform -- the choice to go without is the
// desktop's to make, not the importer's to make silently.
//
// Asked every frame, which costs a key check: supportsSync() caches the extension answers per GL
// context. Only the diagnostic is held to once per process.
void dropUnwaitableSync(FrameImport::Request& request) {
    if (request.syncFds.empty()) return;

    static std::once_flag reported;
    std::string whyNot;
    if (FrameImport::supportsSync(&whyNot)) return;
    std::call_once(reported, [&whyNot] {
        std::fprintf(stderr,
                     "wallpaperx: drawing frames without the producer's hand-over -- %s. The wallpaper still "
                     "shows; what is lost is the guarantee that the compositor reads it only after the daemon "
                     "has finished writing it.\n",
                     whyNot.c_str());
    });
    for (int syncFd : request.syncFds) {
        if (syncFd >= 0) ::close(syncFd);
    }
    request.syncFds.clear();
}

// Render thread, and there is one per window, so the record is shared under a lock.
//
// Once per distinct reason, not once per process: a failed import withdraws the modifier that
// caused it, and the layout the daemon picks instead can fail differently -- that second
// sentence is the one worth reading. Not "changed since last time" either, because two items on
// different monitors can fail alternately and that would log at display rate.
//
// Capped because one reason names the two GPU UUIDs, and the frame's half is whatever the daemon
// sent: without a bound, a peer that varies it defeats the once-per-reason rule.
void reportImportFailure(const std::string& reason) {
    constexpr std::size_t kMaxReasons = 8;
    static std::mutex mutex;
    static std::vector<std::string> reported;

    std::lock_guard<std::mutex> lock(mutex);
    if (reported.size() >= kMaxReasons) return;
    if (std::find(reported.begin(), reported.end(), reason) != reported.end()) return;
    reported.push_back(reason);

    std::fprintf(stderr, "wallpaperx: this frame could not be imported -- %s\n", reason.c_str());
    if (reported.size() == kMaxReasons) {
        std::fprintf(stderr, "wallpaperx: no further import failures will be reported\n");
    }
}

// GUI thread only. The daemon's unique bus name as last observed, so the several items in one
// plasmashell report a restart once between them: a spurious "the daemon changed" would forget
// claims the daemon still holds, and they would then linger until the hand-over cap expired them.
QString g_daemonOwner;

// GUI thread. Called for every owner change of the daemon's bus name, departures included.
void noteDaemonOwner(const QString& owner) {
    if (owner == g_daemonOwner) return;
    g_daemonOwner = owner;
    BufferClaims::instance().daemonReplaced();
}

// GUI thread, where the bus connection is.
void sendReleaseBuffer(int monitorId, quint64 bufferId) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;

    QDBusMessage request = QDBusMessage::createMethodCall(QString::fromUtf8(kService), QString::fromUtf8(kPath),
                                                          QString::fromUtf8(kInterface),
                                                          QStringLiteral("ReleaseBuffer"));
    request.setArguments({monitorId, bufferId});
    // The pending call is dropped rather than watched: the reply carries nothing, and the only
    // error is a daemon that has gone -- which has already freed every claim of ours.
    bus.asyncCall(request);
}

// Tells the daemon this process has stopped reading an image, if this was the last import of it.
//
// Runs on the render thread, from scene graph teardown and from a destructor, so it must neither
// block nor throw: an exception on either path is std::terminate inside plasmashell.
//
// Posted to qApp rather than to the item that fetched the frame, because the scene graph destroys
// a texture AFTER the item that imported it is gone, and that is exactly when this fires.
void releaseClaim(const BufferClaims::Claim& claim) noexcept {
    try {
        if (!BufferClaims::instance().giveBack(claim)) return;
        // Gone already: the process is on its way out, and leaving the bus frees every claim.
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) return;

        const int monitorId = claim.monitorId;
        const quint64 bufferId = claim.bufferId;
        QMetaObject::invokeMethod(
            app, [monitorId, bufferId] { sendReleaseBuffer(monitorId, bufferId); }, Qt::QueuedConnection);
    } catch (...) {
        // A release that never reaches the daemon costs promptness, not correctness: the claim
        // still expires at the hand-over cap, or when this process leaves the bus.
    }
}

}  // namespace

WallpaperXItem::PendingFrame::~PendingFrame() {
    closeDescriptors(request);
    releaseClaim(claim);
}

// Moved, not copied. Request holds a std::vector, so copying it can throw -- and a throw out of
// a noexcept function is std::terminate, here inside plasmashell.
WallpaperXItem::PendingFrame::PendingFrame(PendingFrame&& other) noexcept
    : request(std::move(other.request)), generation(other.generation), claim(other.claim) {
    disownDescriptors(other.request);
    other.claim = {};
}

WallpaperXItem::PendingFrame& WallpaperXItem::PendingFrame::operator=(PendingFrame&& other) noexcept {
    if (this != &other) {
        closeDescriptors(request);
        releaseClaim(claim);
        request = std::move(other.request);
        generation = other.generation;
        claim = other.claim;
        disownDescriptors(other.request);
        other.claim = {};
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
    // Destroyed outside the lock: giving its claim back reaches the bus, and m_pendingMutex is
    // taken by the render thread on every pass.
    std::optional<PendingFrame> dropped;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        dropped = std::move(m_pending);
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

bool WallpaperXItem::subscribeToImportModifiersChanged() {
    return QDBusConnection::sessionBus().connect(QString::fromUtf8(kService), QString::fromUtf8(kPath),
                                                 QString::fromUtf8(kInterface),
                                                 QStringLiteral("ImportModifiersChanged"), this,
                                                 SLOT(onImportModifiersChanged()));
}

// The daemon says the layouts in force have changed. That only matters here if this process's
// own report was refused -- the refusal may have been for sharing no layout with a consumer that
// has since departed or widened, which is not something this process could have fixed by itself.
// Anything else is left alone: a report the daemon accepted stands, and re-sending it would be
// the per-frame retry this deliberately is not.
void WallpaperXItem::onImportModifiersChanged() {
    if (!g_reportRefused.exchange(false)) return;
    g_modifiersReported.store(false);
}

void WallpaperXItem::connectToDaemon() {
    // Messages are built by hand rather than through QDBusInterface, whose constructor blocks on
    // an Introspect round trip -- on the thread that runs the compositor's UI, from a QML item's
    // constructor. Nothing here waits on the bus.
    m_subscribed = subscribeToFrameReady();
    subscribeToImportModifiersChanged();
    // No early return on failure: an item that cannot hear FrameReady must still notice the
    // daemon arriving or leaving, and the watcher below is the only thing that can retry.
    if (!m_subscribed) publishReason();

    // The item is created with the desktop, so the daemon may not own its name yet.
    auto* watcher = new QDBusServiceWatcher(QString::fromUtf8(kService), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString&, const QString& newOwner) {
                // Before anything else, and on departures too: a claim outlives the daemon that
                // granted it, and the next daemon starts its buffer ids over from one.
                noteDaemonOwner(newOwner);
                if (newOwner.isEmpty()) {
                    setFetchReason(tr("WallpaperX is not running."));
                    return;
                }
                // Gated: an unconditional retry would add a duplicate match rule per restart,
                // which is one of the things that makes the subscription fail in the first place.
                if (!m_subscribed) m_subscribed = subscribeToFrameReady();
                g_modifiersReported.store(false);
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

// Zero beside no request in flight is the item genuinely having nothing: m_fetchedGeneration is
// cleared by everything that invalidates what it holds -- a monitor change, a daemon restart,
// leaving the scene.
void WallpaperXItem::ensureFrameRequested() {
    if (m_fetchInFlight || m_fetchedGeneration != 0) return;
    fetchFrame(0);
}

void WallpaperXItem::fetchFrame(quint64 generation) {
    if (m_monitorId < 0) return;

    // Already have this frame, or a newer one. Asking is not free: a bus round trip, an export
    // by the daemon, a dup() here. A signal for a frame already fetched is ordinary.
    //
    // Zero means "fetch whatever is there", and it forces: it is what every caller that is not a
    // FrameReady signal passes, having just discarded its marks and no idea what generation the
    // daemon is on. A caller that may already hold the answer wants ensureFrameRequested().
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
    if (g_modifiersReported.exchange(true)) return;

    std::string whyNot;
    const std::vector<uint64_t> modifiers = FrameImport::importableModifiers(drmFormat, &whyNot);
    if (modifiers.empty()) {
        // Stays latched: neither a driver without the query nor a GL context that is not
        // EGL-backed grows one while this process runs, and the daemon's unnegotiated default
        // is what it already uses.
        std::fprintf(stderr, "wallpaperx: not reporting import modifiers -- %s\n", whyNot.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_reportedMutex);
        g_reportedModifiers = modifiers;
    }
    // Stays latched even if the daemon refuses, and the record keeps the list that was sent.
    // Every refusal is a verdict on the list's contents -- a format this daemon does not export,
    // a length over its cap, or no layout in common with another consumer -- so re-sending the
    // same list gets the same answer, once per frame, forever. Recovery does not need a retry:
    // an import that actually fails narrows this record and sends a different list, which is a
    // question the daemon has not already answered.
    sendImportModifiers(this, drmFormat, modifiers, modifiers);
}

// Withdraws a modifier the daemon may still choose, after an import of it actually failed.
// Re-reporting the narrowed set is the whole mechanism: the daemon treats a changed set as a
// re-render, so the next frame arrives in a layout this driver has demonstrated it can take.
void WallpaperXItem::withdrawImportModifier(quint32 drmFormat, quint64 modifier) {
    std::vector<uint64_t> narrowed;
    std::vector<uint64_t> previous;
    {
        std::lock_guard<std::mutex> lock(g_reportedMutex);
        const auto it = std::find(g_reportedModifiers.begin(), g_reportedModifiers.end(), modifier);
        if (it == g_reportedModifiers.end()) return;
        // The daemon refuses an empty set, and rightly: "I can import nothing" is not a report.
        // Leaving the last one stands a better chance than withdrawing it.
        if (g_reportedModifiers.size() == 1) return;
        previous = g_reportedModifiers;
        g_reportedModifiers.erase(it);
        narrowed = g_reportedModifiers;
    }
    // Still latched on refusal: the daemon has an earlier report of ours that stands.
    sendImportModifiers(this, drmFormat, narrowed, std::move(previous));
}

namespace {

// `restoreOnFailure` is what this plugin's record should go back to if the daemon refuses. It
// matters because a caller narrows the record BEFORE sending: a refusal the caller never hears
// about leaves this plugin and the daemon disagreeing about what was reported, and the daemon
// then keeps choosing the very layout that failed to import.
void sendImportModifiers(QObject* context, quint32 drmFormat, const std::vector<uint64_t>& modifiers,
                         std::vector<uint64_t> restoreOnFailure) {
    // Sent from the GUI thread: the render thread owns no bus connection, and QDBusConnection
    // is not safe to use from it.
    QMetaObject::invokeMethod(
        context,
        [context, drmFormat, modifiers, restoreOnFailure = std::move(restoreOnFailure)] {
            QDBusConnection bus = QDBusConnection::sessionBus();
            if (!bus.isConnected()) {
                g_modifiersReported.store(false);  // retry on the next frame
                return;
            }
            QDBusMessage request = QDBusMessage::createMethodCall(
                QString::fromUtf8(kService), QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                QStringLiteral("SetImportModifiers"));
            QList<quint64> wire;
            wire.reserve(static_cast<qsizetype>(modifiers.size()));
            for (uint64_t modifier : modifiers) wire.append(modifier);
            request.setArguments({drmFormat, QVariant::fromValue(wire)});

            // Watched, not waited on: a success needs nothing here (the daemon answers by
            // re-rendering and announcing FrameReady, which this item already listens for), but
            // a refusal has to be undone. Blocking would park the desktop's UI.
            auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(request), context);
            QObject::connect(
                watcher, &QDBusPendingCallWatcher::finished, context,
                [modifiers, restoreOnFailure](QDBusPendingCallWatcher* done) {
                    done->deleteLater();
                    if (!done->isError()) return;

                    std::fprintf(stderr, "wallpaperx: the daemon refused the import modifier report -- %s\n",
                                 done->error().message().toUtf8().constData());
                    // Armed, not retried. The same list earns the same answer until the daemon
                    // says the layouts in force have changed.
                    g_reportRefused.store(true);
                    std::lock_guard<std::mutex> lock(g_reportedMutex);
                    // Only if nothing has moved since. A withdrawal that landed while this call
                    // was in flight must not be undone by an older one failing.
                    if (g_reportedModifiers == modifiers) g_reportedModifiers = restoreOnFailure;
                });
        },
        Qt::QueuedConnection);
}

}  // namespace

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
        // Refused so the daemon keeps somewhere to draw. Reported as no reason at all, because a
        // reason hides the wallpaper behind the backdrop -- and this item still has the frame it
        // is showing, which is more use than a message about a fetch that will succeed on the
        // next announcement. The daemon sends one whenever an image comes back.
        if (name == QLatin1String(kErrorNoFreeImage)) {
            setFetchReason(QString());
            return;
        }
        // Not a failure of this item: no frame yet is the ordinary state before the user has
        // chosen a wallpaper, and the daemon says which case it is.
        setFetchReason(call.errorMessage().isEmpty() ? tr("WallpaperX has no frame for this screen yet.")
                                                           : call.errorMessage());
        return;
    }

    const QList<QVariant> args = call.arguments();
    // Twelve fields, in the order GetFrameFd declares them. Checked rather than assumed: a daemon
    // from a newer build could answer with more, and reading past the end of a short reply is
    // the kind of crash that takes plasmashell with it.
    if (!replyIsReadable(static_cast<int>(args.size()))) {
        setFetchReason(tr("WallpaperX answered with something this plugin cannot read."));
        return;
    }

    PendingFrame frame;
    // Field 11, buffer_id: which image, where frame_generation says which render. Read and
    // claimed BEFORE the rest of the parse, because the daemon recorded the hold when it answered
    // -- so from here on every path that drops this frame owes it a release, and `frame` owning
    // the claim is what makes that automatic.
    frame.request.bufferId = args.at(11).toULongLong();
    frame.claim = BufferClaims::instance().take(m_monitorId, frame.request.bufferId);

    frame.request.fd = ::dup(args.at(0).value<QDBusUnixFileDescriptor>().fileDescriptor());
    frame.request.width = args.at(1).toUInt();
    frame.request.height = args.at(2).toUInt();
    frame.request.memorySize = args.at(3).toULongLong();
    frame.request.handleType = args.at(4).toInt() == 1 ? FrameImport::HandleType::DmaBuf
                                                       : FrameImport::HandleType::OpaqueFd;
    frame.generation = args.at(6).toULongLong();
    frame.request.drmFormat = args.at(7).toUInt();
    frame.request.drmModifier = args.at(8).toULongLong();

    // Field 5, sync_fds: what must be waited on before this frame is sampled. Read into the
    // request whatever the count -- ALL of them, not the first -- so a daemon that later sends
    // more than one is waited on completely rather than sampled early, which is the tearing the
    // old outright refusal of this field existed to prevent.
    //
    // Duplicated like the memory descriptor above: the reply's own descriptors close when this
    // message goes out of scope.
    const QDBusArgument syncFds = args.at(5).value<QDBusArgument>();
    bool syncTaken = true;
    syncFds.beginArray();
    while (!syncFds.atEnd()) {
        QDBusUnixFileDescriptor announced;
        syncFds >> announced;
        const int taken = ::dup(announced.fileDescriptor());
        if (taken < 0) {
            // Refused, not dropped. A wait this plugin was given and could not take hold of is a
            // frame it would sample before the producer's hand-over completed, and the array must
            // be read to its end regardless or the message stays half-parsed.
            syncTaken = false;
            continue;
        }
        frame.request.syncFds.push_back(taken);
    }
    syncFds.endArray();
    if (!syncTaken) {
        setFetchReason(tr("WallpaperX sent a frame this plugin could not take hold of."));
        return;
    }

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

    if (frame.request.fd < 0) {
        setFetchReason(tr("WallpaperX sent a frame this plugin could not take hold of."));
        return;
    }

    // Recorded from the REPLY, not from the signal that prompted the fetch: the daemon may have
    // rendered again between the two, and the reply describes the frame actually in hand.
    m_fetchedGeneration = frame.generation;

    std::optional<PendingFrame> superseded;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        // Replaced, not queued: an older frame waiting behind this one describes a state this
        // one already supersedes. Taken out rather than destroyed here, because its destructor
        // gives a claim back over the bus and this lock is on the render thread's path.
        superseded = std::exchange(m_pending, std::move(frame));
    }
    superseded.reset();
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
        dropUnwaitableSync(incoming->request);
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
            // A texture name with no error is not the importer's refusal: the import succeeded and
            // the scene graph would not wrap it, which carries no reason of its own.
            reportImportFailure(imported.texture == 0
                                    ? imported.error
                                    : std::string("the scene graph would not wrap the imported texture"));
            if (imported.texture != 0) FrameImport::release(imported);
            // A failed import outranks what EGL said it could take: this modifier demonstrably
            // does not work here, so stop the daemon choosing it again.
            if (incoming->request.handleType == FrameImport::HandleType::DmaBuf) {
                withdrawImportModifier(incoming->request.drmFormat, incoming->request.drmModifier);
            }
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
            //
            // The claim travels with the texture, since the texture is what is reading the
            // image -- and it outlives this item. Given back only after the GL objects are gone,
            // never before: telling the daemon first invites it to draw into memory this process
            // still has mapped.
            QObject::connect(
                texture, &QObject::destroyed, texture,
                [imported, claim = std::exchange(incoming->claim, BufferClaims::Claim{})] {
                    FrameImport::release(imported);
                    releaseClaim(claim);
                },
                Qt::DirectConnection);
            // Deletes the texture it replaces, so a QSGTexture* kept across a re-import is freed
            // memory -- and the allocator hands the same address back, which reads as no change.
            // Watch QObject::destroyed, never compare pointers.
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

// A still wallpaper sends no further FrameReady, so a released item must ask on its own. Not
// fetchFrame(0), which forces: QML sets the monitor id moments before the item joins a scene, and
// forcing here made every item ask the daemon twice for the frame it was already getting.
void WallpaperXItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change != ItemSceneChange || value.window == nullptr) return;
    ensureFrameRequested();
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
