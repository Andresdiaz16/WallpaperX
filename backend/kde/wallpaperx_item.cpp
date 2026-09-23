// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaperx_item.h"

#include "daemon/log_escape.h"
#include "daemon/ring_depth.h"

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
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>

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

// The daemon keeps one image per monitor free to render into, and bounds what one consumer may
// hold; a hand-over past either is refused rather than taken from whoever holds it. Not a failure
// of either side, and not permanent.
constexpr const char* kErrorNoFreeImage = "io.wallpaperx.Daemon.Error.NoFreeImage";

// How long a refused item with nothing on screen waits before asking again. Long enough that it
// is not a poll of the daemon, short enough that a desktop showing the plain backdrop recovers
// on its own once an image comes free.
constexpr int kRefusedRetryMs = 2000;

// A screen size reported before the daemon knows the screen's name. Ordinary while the app has
// not named it, and answered by the next report once it has.
constexpr const char* kErrorUnknownOutput = "io.wallpaperx.Daemon.Error.UnknownOutput";
constexpr const char* kErrorRenderFailed = "io.wallpaperx.Daemon.Error.RenderFailed";

// Long enough to cover a login's burst of layout and scale changes, short enough that a rotation
// is redrawn before anyone looks for it.
constexpr int kSizeReportSettleMs = 250;

// How long after handing the scene graph a frame this item waits before deciding the desktop is
// not drawing it. A pass normally follows within one display frame; a second is far past any sync
// that ran long, and it is what a wallpaper costs after a screen locks before it stops.
constexpr int kPaintDeadlineMs = 1000;

// How soon an item that believes it is not being drawn asks for a pass to find out whether it is
// again: the delay before a wallpaper starts moving after the screen comes back.
constexpr int kPaintProbeMs = 250;

// Where the doubling stops. A screen can stay dark for hours, and this is the wake-up rate that
// costs; two seconds is the longest a wallpaper should take to start moving again once it is back.
constexpr int kPaintProbeMaxMs = 2000;

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
// claims the daemon still holds, and nothing would ever give those images back.
QString g_daemonOwner;

// GUI thread. Called for every owner change of the daemon's bus name, departures included.
void noteDaemonOwner(const QString& owner) {
    if (owner == g_daemonOwner) return;
    g_daemonOwner = owner;
    BufferClaims::instance().daemonReplaced();
}

// Asks the bus who owns the daemon's name, once per process. The service watcher only reports
// CHANGES, so without this a plugin that starts beside a running daemon never learns its unique
// name -- which is the name every release has to be addressed to.
void askWhoOwnsTheDaemon() {
    static bool asked = false;
    if (asked) return;

    // On qApp, not on the item that asked: an item destroyed within the round trip would take the
    // answer with it, and the latch below would stop any other item from asking again.
    QCoreApplication* app = QCoreApplication::instance();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (app == nullptr || !bus.isConnected()) return;
    asked = true;

    QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("/org/freedesktop/DBus"),
                                                          QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("GetNameOwner"));
    request.setArguments({QString::fromUtf8(kService)});
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(request), app);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, app, [](QDBusPendingCallWatcher* done) {
        done->deleteLater();
        // Only to fill a blank. The watcher's own reports are newer by construction -- the bus
        // sends them after this reply -- so applying an answer over one would rewind the record.
        if (done->isError() || !g_daemonOwner.isEmpty()) return;
        noteDaemonOwner(done->reply().arguments().value(0).toString());
    });
}

// GUI thread, where the bus connection is.
//
// Addressed to the unique name that granted the claim, never to the well-known one: a unique
// name is never reused, so a release delayed past a daemon restart is dropped by the bus instead
// of freeing the SUCCESSOR's image of the same id under whoever is reading it. Empty only before
// this process has learned any owner, where the well-known name is all there is.
void sendReleaseBuffer(const std::string& owner, int monitorId, quint64 bufferId) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;

    const QString destination = owner.empty() ? QString::fromUtf8(kService) : QString::fromStdString(owner);
    QDBusMessage request = QDBusMessage::createMethodCall(destination, QString::fromUtf8(kPath),
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
            app, [owner = claim.owner, monitorId, bufferId] { sendReleaseBuffer(owner, monitorId, bufferId); },
            Qt::QueuedConnection);
    } catch (...) {
        // A release that never reaches the daemon costs the daemon an image until this process
        // leaves the bus: nothing is ever taken back from a consumer, so no claim expires on its
        // own. The ring is sized to survive one such consumer, not a stream of them.
    }
}

// A descriptor with one owner, so no path out of the parking below can lose one.
struct OwnedFd {
    int fd = -1;

    OwnedFd() = default;
    explicit OwnedFd(int owned) : fd(owned) {}
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    ~OwnedFd() {
        if (fd >= 0) ::close(fd);
    }

    int take() { return std::exchange(fd, -1); }
};

// One claim waiting on the descriptor that says this process's GPU has finished reading the image.
// GUI thread, because that is where the notifier's event loop and the bus connection are.
//
// It gives the claim back only when the fence signals, so until then the ledger still counts the
// import -- which is what makes a second live import of the same image, and a daemon replaced in
// the meantime, come out right without either being asked about here.
//
// The deadline bounds a fence that never signals: nothing else would ever free the claim, and the
// daemon takes none back, so that image would be out of the ring for the session. Orders of
// magnitude beyond the reads it covers, which are one desktop frame's worth.
constexpr int kFenceDeadlineMs = 5000;

class PendingRelease : public QObject {
public:
    PendingRelease(const BufferClaims::Claim& claim, int fenceFd, QObject* parent)
        : QObject(parent), m_claim(claim), m_fence(fenceFd), m_signalled(fenceFd, QSocketNotifier::Read) {
        QObject::connect(&m_signalled, &QSocketNotifier::activated, this, [this] { release(); });
        m_deadline.setSingleShot(true);
        QObject::connect(&m_deadline, &QTimer::timeout, this, [this] {
            noteTimedOutFence();
            release();
        });
        m_deadline.start(kFenceDeadlineMs);
    }

private:
    // Both sources are silenced first: a signalled fence stays readable, so the notifier would
    // otherwise fire on every pass of the event loop until deleteLater() ran.
    void release() {
        m_signalled.setEnabled(false);
        m_deadline.stop();
        releaseClaim(m_claim);
        deleteLater();
    }

    // Once per process: this is a statement about the driver, which would otherwise repeat it
    // for every image the desktop ever shows.
    static void noteTimedOutFence() {
        static std::once_flag reported;
        std::call_once(reported, [] {
            std::fprintf(stderr,
                         "wallpaperx: a fence saying this desktop had finished reading an image never signalled "
                         "within %d ms, so the image is being given back anyway. The wallpaper still shows; what "
                         "is lost is the guarantee that the daemon redraws an image only after this desktop's "
                         "queued reads of it have finished.\n",
                         kFenceDeadlineMs);
        });
    }

    BufferClaims::Claim m_claim;
    // Declared before the notifier so it is destroyed after it: closing a descriptor a live
    // QSocketNotifier still watches leaves the notifier armed on whatever number is opened next.
    OwnedFd m_fence;
    QSocketNotifier m_signalled;
    QTimer m_deadline;
};

// Once per process: the reason is a property of the GL context, not of the image.
void noteUnfencedRelease(const std::string& whyNot) {
    static std::once_flag reported;
    std::call_once(reported, [&whyNot] {
        std::fprintf(stderr,
                     "wallpaperx: waiting for this desktop's GPU to go idle before telling the daemon an image is "
                     "free -- %s. The wallpaper still shows; what this costs is a stall on the render thread "
                     "every time an imported frame goes away.\n",
                     whyNot.c_str());
    });
}

// The same, for a claim whose image this process has been READING: the daemon may draw into it the
// moment it is told, and the commands sampling it can still be queued when the host is done with
// the texture. So the claim waits on a fence placed after those commands instead.
//
// Render thread, context current, and called where the texture died: the fence covers the commands
// issued before it, and a texture still alive can still be drawn with.
//
// Every path that cannot park the claim releases it now rather than dropping it -- a dropped claim
// leaves the ledger counting an import that no longer exists, so no later import of that image
// would be released either.
void releaseClaimWhenReadsFinish(const BufferClaims::Claim& claim) noexcept {
    try {
        if (!claim.held()) return;
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) {
            // On the way out, with no event loop left to watch a fence with.
            releaseClaim(claim);
            return;
        }

        std::string whyNot;
        OwnedFd fence{FrameImport::readsFinishedFd(&whyNot)};
        if (fence.fd < 0) {
            // Waited out here and now, on the thread with the context: deleting the GL objects
            // does not drain the commands that sampled them, and the daemon may draw into the
            // image the instant it is told. Blocking is the price of saying so truthfully.
            noteUnfencedRelease(whyNot);
            FrameImport::waitForReadsToFinish();
            releaseClaim(claim);
            return;
        }

        if (QMetaObject::invokeMethod(
                app, [claim, fd = fence.fd, app] { new PendingRelease(claim, fd, app); }, Qt::QueuedConnection)) {
            fence.take();  // the post owns it now, and PendingRelease after that
            return;
        }
        // Nothing will ever watch that fence, so this is the unfenced case with a descriptor in
        // hand: wait the reads out rather than tell the daemon early.
        FrameImport::waitForReadsToFinish();
        releaseClaim(claim);
    } catch (...) {
        // As releaseClaim: a release that never reaches the daemon costs that image until this
        // process leaves the bus.
    }
}

// A frame that was never imported, so nothing is reading it and the release needs no fence.
// Silent when the item has already given this image back on its way off the screen.
void releaseUnimported(const std::shared_ptr<ClaimSet>& claims, const BufferClaims::Claim& claim) noexcept {
    if (claims && claims->remove(claim)) releaseClaim(claim);
}

// Which of the daemon's images an import describes.
//
// The buffer id carries it: the daemon numbers images from a counter it never resets, so within
// one of its lifetimes an id is one image, and a ring it rebuilds gets fresh ids. Across lifetimes
// it is not enough -- ids start from one again -- which is what `daemon` answers, the same count
// the release ledger keys on. The shape rides along so an image that does not match the import
// held for it is imported again rather than drawn at the wrong size or layout.
struct ImportKey {
    uint64_t daemon = 0;
    int monitorId = -1;
    uint64_t bufferId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    uint32_t drmFormat = 0;
    uint64_t drmModifier = 0;
    FrameImport::HandleType handleType = FrameImport::HandleType::OpaqueFd;

    bool operator==(const ImportKey&) const = default;

    // A reply that granted no image names none, and two of those would be the same key without
    // being the same memory.
    bool identifies() const { return bufferId != 0; }

    // Whether `other` names an image of the same ring: every field but which image it is. Written
    // against the whole key rather than field by field, so a field added above is covered here
    // without being remembered.
    bool sameRingAs(const ImportKey& other) const {
        ImportKey mine = *this;
        mine.bufferId = other.bufferId;
        return mine == other;
    }
};

ImportKey importKeyFor(const BufferClaims::Claim& claim, const FrameImport::Request& request) {
    return ImportKey{claim.daemon,       claim.monitorId,     claim.bufferId,
                     request.width,      request.height,      request.memorySize,
                     request.drmFormat,  request.drmModifier, request.handleType};
}

// Imports one item keeps: the daemon's own ring depth, shared rather than restated, because a
// cache shallower than the ring is refilled every time the ring comes round and nothing says so.
constexpr std::size_t kMaxImports = kTargetDepth;

// Hand-overs that may fail in a row before the desktop stops showing the last picture that worked
// and says why. An eighth of a second of video: past any blip, and short enough that a wallpaper
// stuck on one picture explains itself rather than looking healthy for the rest of the session.
constexpr int kToleratedHandOverFailures = 8;

// Whether this picture carries the producer's hand-over. Without one nothing tells the driver that
// the memory behind an import it already has holds a different picture now, so a kept import must
// not be drawn for it -- see the header's note on what a reuse costs.
bool carriesHandOver(const FrameImport::Request& request) {
    return std::any_of(request.syncFds.begin(), request.syncFds.end(), [](int syncFd) { return syncFd >= 0; });
}

// What stopped a picture reaching the screen. Named rather than read back out of the error text,
// because the three differ in what may be concluded from them: only the first says anything about
// the layout the daemon chose, and only the last leaves the previous picture still being drawn.
enum class Stopped {
    Nothing,
    Layout,    // the driver would not describe this memory
    Wrapping,  // it did, and the scene graph would not take the texture
    HandOver,  // the memory is imported; the producer's acquire could not be performed
};

// What the item hands the scene graph: the picture on the screen, the imports it can switch
// between without describing memory to the driver again, and the daemon's hold on the image it is
// sampling now.
//
// All of it is the render thread's, the destructor included -- the scene graph destroys its nodes
// there with a context current, which the GL frees need and the item's own destructor, on the GUI
// thread, cannot offer. It is also why the imports are kept here: an item leaving the screen asks
// for its node to be destroyed, so "holds nothing importable" and "has no node" are one fact.
class FrameNode : public QSGSimpleTextureNode {
public:
    // One image described to the driver once, with the scene graph's wrapper around it. The two
    // die together, so neither can outlive the other.
    class Import {
    public:
        Import(const ImportKey& image, const FrameImport::Result& result, QSGTexture* wrapper)
            : m_key(image), m_imported(result), m_texture(wrapper) {}
        ~Import() { free(); }
        Import(Import&& other) noexcept
            : m_key(other.m_key),
              m_imported(std::move(other.m_imported)),
              m_texture(std::exchange(other.m_texture, nullptr)) {}
        Import& operator=(Import&& other) noexcept {
            if (this != &other) {
                free();
                m_key = other.m_key;
                m_imported = std::move(other.m_imported);
                m_texture = std::exchange(other.m_texture, nullptr);
            }
            return *this;
        }
        Import(const Import&) = delete;
        Import& operator=(const Import&) = delete;

        const ImportKey& key() const { return m_key; }
        QSGTexture* texture() const { return m_texture; }
        // The GL name the daemon's acquire must be waited on against.
        unsigned int name() const { return m_imported.texture; }

    private:
        // The import goes before the wrapper, never after: from outside this plugin the scene
        // graph's texture destruction is the moment the import's fate can be observed, and
        // freeing afterwards would leave the GL objects alive past the only handle to them.
        //
        // A null wrapper means this was moved from, and then the import went with it.
        void free() {
            if (m_texture == nullptr) return;
            FrameImport::release(m_imported);
            delete m_texture;
            m_texture = nullptr;
        }

        ImportKey m_key;
        FrameImport::Result m_imported;
        QSGTexture* m_texture = nullptr;
    };

    FrameNode() {
        // An import outlives the picture it was made for, so the node must not free what it is
        // handed; m_imports is what owns every texture this node is ever given.
        setOwnsTexture(false);
        setFiltering(QSGTexture::Linear);
    }

    ~FrameNode() override {
        m_imports.clear();
        // Silent when the item gave this image back on its way off the screen: the same claim
        // released twice would free an image another reader in this process still holds.
        if (m_claims && m_claims->remove(m_claim)) releaseClaimWhenReadsFinish(m_claim);
    }

    // The import held for this image, or null. Reading only: present() is what moves the node.
    const Import* held(const ImportKey& key) const {
        if (!key.identifies()) return nullptr;
        const auto it = std::find_if(m_imports.begin(), m_imports.end(),
                                     [&key](const Import& import) { return import.key() == key; });
        return it == m_imports.end() ? nullptr : &*it;
    }

    // Takes over a fresh import of `key`, at the front, where present() will find it. Trimmed
    // straight away, because a picture whose hand-over then fails is never presented and the bound
    // would otherwise be enforced only by the pictures that succeed.
    void adopt(const ImportKey& key, const FrameImport::Result& imported, QSGTexture* texture) {
        m_imports.emplace(m_imports.begin(), key, imported, texture);
        forgetUnusable();
    }

    // Draws the import held for `key` and takes the daemon's hold on the picture in it.
    //
    // The hold it replaces is given back here rather than when its import is freed: the import
    // stays for the next picture drawn into the same image, and a hold kept that long would leave
    // the daemon with nowhere to draw. This is the first moment nothing of this item's still
    // samples the previous picture -- the commands that did were issued by earlier passes, which
    // is what the fence inside the release covers.
    void present(const ImportKey& key, const std::shared_ptr<ClaimSet>& claims,
                 const BufferClaims::Claim& claim) {
        const auto shown = std::find_if(m_imports.begin(), m_imports.end(),
                                        [&key](const Import& import) { return import.key() == key; });
        if (shown == m_imports.end()) {
            // The caller has just imported this key or found it here, so there is no such pass.
            // Were there one, nothing would be sampling this picture and the daemon would never
            // hear about the hold its caller has already given up -- so it hears about it now.
            releaseUnimported(claims, claim);
            return;
        }
        std::rotate(m_imports.begin(), shown, shown + 1);
        setTexture(m_imports.front().texture());
        markDirty(QSGNode::DirtyMaterial);

        const std::shared_ptr<ClaimSet> previousClaims = std::exchange(m_claims, claims);
        const BufferClaims::Claim previous = std::exchange(m_claim, claim);
        if (previousClaims && previousClaims->remove(previous)) releaseClaimWhenReadsFinish(previous);

        forgetUnusable();
    }

    // Every import but the one on the screen. What the item asks for when the marks its frames were
    // fetched against stop meaning anything: no later hand-over can name any of these, and the one
    // being drawn is kept only because taking a picture off the desktop costs more than it does.
    //
    // The one path that frees imports without a picture arriving -- and the cases that need it, a
    // daemon that exited and a monitor this item left, are exactly the ones where none ever will.
    void forgetAllButDrawn() {
        std::erase_if(m_imports, [this](const Import& import) { return !isDrawn(import); });
    }

private:
    // Whether the node is sampling this import. Asked, never inferred from position: an import
    // taken in for a picture whose hand-over then failed sits at the front without being drawn,
    // and freeing what the node points at is a use-after-free inside the scene graph.
    bool isDrawn(const Import& import) const { return import.texture() == texture(); }

    // Drops every import that is neither on the screen, nor the one just taken in, nor one a later
    // hand-over could still name -- an image of the ring being drawn from, meaning same daemon
    // lifetime, same monitor, same size and layout.
    //
    // A daemon that has exited and a monitor this item no longer shows are the cases that matter:
    // their imports pin memory nothing will hand over again, and left to the count alone they
    // drain one per picture, which for a still wallpaper is never. A second entry under the front's
    // own key goes for another reason: two entries one lookup cannot tell apart is a state this
    // must not be left in. The count is the backstop for a ring rebuilt with everything but the
    // ids the same, and is walked from the least recently drawn.
    void forgetUnusable() {
        const ImportKey current = m_imports.front().key();
        for (std::size_t at = m_imports.size(); at > 1; --at) {
            const std::size_t index = at - 1;
            const Import& import = m_imports[index];
            if (isDrawn(import)) continue;
            const bool nameable = import.key() != current && import.key().sameRingAs(current);
            if (nameable && index < kMaxImports) continue;
            m_imports.erase(m_imports.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    // Most recently sampled first.
    std::vector<Import> m_imports;
    // The hold on the picture the node is sampling, and the set that says whether it is still this
    // node's to give back.
    std::shared_ptr<ClaimSet> m_claims;
    BufferClaims::Claim m_claim;
};

}  // namespace

WallpaperXItem::PendingFrame::~PendingFrame() {
    closeDescriptors(request);
    releaseUnimported(claims, claim);
}

// Moved, not copied. Request holds a std::vector, so copying it can throw -- and a throw out of
// a noexcept function is std::terminate, here inside plasmashell.
WallpaperXItem::PendingFrame::PendingFrame(PendingFrame&& other) noexcept
    : request(std::move(other.request)),
      generation(other.generation),
      claim(other.claim),
      claims(std::move(other.claims)) {
    disownDescriptors(other.request);
    other.claim = {};
}

WallpaperXItem::PendingFrame& WallpaperXItem::PendingFrame::operator=(PendingFrame&& other) noexcept {
    if (this != &other) {
        closeDescriptors(request);
        releaseUnimported(claims, claim);
        request = std::move(other.request);
        generation = other.generation;
        claim = other.claim;
        claims = std::move(other.claims);
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
    m_sizeReportTimer.setSingleShot(true);
    connect(&m_sizeReportTimer, &QTimer::timeout, this, &WallpaperXItem::reportSize);
    m_paintWatch.setSingleShot(true);
    connect(&m_paintWatch, &QTimer::timeout, this, &WallpaperXItem::checkPainting);
    connectToDaemon();
}

// A QML item can be destroyed while still parented and visible, and nothing else would ever tell
// the daemon what it was holding.
WallpaperXItem::~WallpaperXItem() { releaseHeldImages(); }

void WallpaperXItem::setMonitorId(int monitorId) {
    if (m_monitorId == monitorId) return;
    m_monitorId = monitorId;

    // QML sets this once the item is in the scene, so it is also the last point at which a scene
    // change this item never heard about would still leave it fetching nothing for ever.
    updateOnScreen();

    forgetGenerations();
    m_sizeReported.reset();
    scheduleSizeReport();

    emit monitorIdChanged();
    // A screen that has just been identified may already have a frame waiting; without this the
    // desktop stays blank until the next time the wallpaper happens to change.
    if (m_monitorId >= 0) fetchFrame(0);
}

void WallpaperXItem::setOutput(const QString& output) {
    if (m_output == output) return;
    m_output = output;
    // Both reports are filed under the screen's name, so neither says anything about the new one.
    m_sizeReported.reset();
    m_visibleReported.reset();
    emit outputChanged();
    scheduleSizeReport();
}

void WallpaperXItem::scheduleSizeReport() { m_sizeReportTimer.start(kSizeReportSettleMs); }

void WallpaperXItem::releaseHeldImages() {
    // The frame in hand goes with the claims, and before them: an image the daemon has been told
    // is free must not be left importable by the next pass. The one teardown both this and a
    // monitor change use, so the marks they invalidate cannot drift apart.
    discardHeldFrame();

    // Retired, not emptied: a claim is matched by which image it names, and ring slots hand the
    // same id out again, so a texture that dies after this must not find a claim taken since and
    // give back an image being displayed. Holders keep a reference to the set they were added to,
    // and this one will never be added to again.
    const std::shared_ptr<ClaimSet> retired = std::exchange(m_claims, std::make_shared<ClaimSet>());

    // Unfenced: this runs on the GUI thread with no graphics context, and the fence would be
    // placed by a pass that may never run for this item again. The cost is that the daemon may
    // redraw an image whose last queued read has not retired.
    for (const BufferClaims::Claim& claim : retired->removeAll()) releaseClaim(claim);
}

// The node is a live reader of an image the daemon now counts as free, so showing it again would
// sample memory the next picture is being drawn into -- and only a scene graph pass, with the
// context current, can free the texture that holds it.
void WallpaperXItem::dropShownFrame() {
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        m_shownGeneration = 0;
        m_nodeHasFrame = false;
        m_nodeRequest = NodeRequest::Destroy;
    }
    publishReason();
    update();
}

// Qt's answer to whether this item is on the screen. An item that is not has nothing to show and
// nothing to hold: it gives its images back, since the daemon keys a hold on this process and
// cannot tell that the item holding it is gone, so images it stops showing would stay out of the
// monitor's ring for the session and every later hand-over would be refused.
void WallpaperXItem::updateOnScreen() {
    const bool onScreen = window() != nullptr && isVisible();
    if (onScreen == m_onScreen) return;
    m_onScreen = onScreen;

    if (!m_onScreen) {
        m_paintWatch.stop();
        releaseHeldImages();
        dropShownFrame();
        reportVisible();
        return;
    }
    // Assumed to be painted until a frame handed over proves otherwise, so a wallpaper resumes
    // without waiting out an interval. The fetch is forced: the item gave up what it was showing
    // on the way out, and the daemon may have announced nothing meanwhile.
    m_painting = true;
    m_paintUnconfirmed = false;
    reportVisible();
    fetchFrame(0);
    // Not left to the dirty mark set on the way out: a window teardown between the two would drop
    // it, and nothing else asks for the pass that frees the node.
    update();
}

void WallpaperXItem::armPaintWatch() {
    if (m_paintWatch.isActive()) return;
    m_paintMark = m_paintCount.load(std::memory_order_relaxed);
    m_paintUnconfirmed = false;
    m_probeMs = kPaintProbeMs;
    m_paintWatch.start(m_painting ? kPaintDeadlineMs : m_probeMs);
    // What the deadline measures is a pass this item asked for. Without this, a stretch with no new
    // frame -- an item being refused images, which is exactly what a dark screen produces -- reads
    // as a desktop that has stopped drawing, and the daemon is told to stop a video the user is
    // watching.
    update();
}

// Whether the desktop drew anything this item handed it since the watch was armed. The only
// signal there is for a screen that is locked, covered by a fullscreen window or belongs to
// another session: the compositor stops asking for frames, so the scene graph stops running, and
// nothing else in this process is told why.
void WallpaperXItem::checkPainting() {
    const quint64 painted = m_paintCount.load(std::memory_order_relaxed);
    const bool drew = painted != m_paintMark;
    m_paintMark = painted;

    // One empty window is not evidence: a visible desktop can miss a pass for a second during a
    // reconfigure or a mode change, and telling the daemon then costs a visible freeze and, past
    // the idle release, the video's place in the file. So the first empty window only asks for
    // another pass; the answer stands when the window this code explicitly requested is empty too.
    if (drew) {
        m_painting = true;
        m_paintUnconfirmed = false;
    } else if (m_painting && !m_paintUnconfirmed) {
        m_paintUnconfirmed = true;
    } else {
        m_painting = false;
    }

    reportVisible();
    if (!m_onScreen) return;
    // Nothing left to watch until the next hand-over arms it again.
    if (m_painting && !m_paintUnconfirmed) return;

    // Asking for a pass is what makes the next check mean something, and it is free exactly while
    // this holds: a window the compositor is not driving does not render. Backed off all the same,
    // since that is an assumption about the compositor and a screen can stay dark all night.
    update();
    m_paintWatch.start(m_probeMs);
    m_probeMs = std::min(m_probeMs * 2, kPaintProbeMaxMs);
}

void WallpaperXItem::reportVisible() {
    const bool visible = m_onScreen && m_painting;
    if (m_output.isEmpty() || m_visibleReported == visible) return;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        // The watch is what asks again, and nothing else would: a screen whose report never landed
        // gets no pictures, and no pictures means no hand-over to arm it.
        armPaintWatch();
        return;
    }
    QDBusMessage request = QDBusMessage::createMethodCall(QString::fromUtf8(kService), QString::fromUtf8(kPath),
                                                          QString::fromUtf8(kInterface),
                                                          QStringLiteral("SetOutputVisible"));
    request.setArguments({m_output, visible});
    m_visibleReported = visible;

    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(request), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, visible](QDBusPendingCallWatcher* done) {
        done->deleteLater();
        // Nothing to undo after a success, or once a newer answer has replaced this one.
        if (!done->isError() || m_visibleReported != visible) return;
        m_visibleReported.reset();
        const QString name = done->error().name();
        if (name == QLatin1String(kErrorUnknownOutput)) {
            // The size report's success is what says the daemon can place this screen, and it is
            // not sent again while the size has not changed -- so the retry it already has for an
            // unnamed screen is re-opened here rather than left suppressed.
            m_sizeReported.reset();
            if (!m_sizeReportTimer.isActive()) m_sizeReportTimer.start(kRefusedRetryMs);
            return;
        }
        // No daemon: its arrival reports again, through the owner watcher.
        if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")) return;
        std::fprintf(stderr, "wallpaperx: the daemon refused this screen's visibility -- %s\n",
                     sanitizeForLog(done->error().message().toStdString()).c_str());
        // Anything else -- a busy daemon, a timeout -- has no other retry: the watch recomputes the
        // answer and sends it again, and stops once one is accepted.
        armPaintWatch();
    });
}

// Measured on this item, not on its QScreen: the item is what the frame is stretched over, and on
// Wayland a fractional scale belongs to the window while QScreen carries the output's integer one.
void WallpaperXItem::reportSize() {
    const QQuickWindow* host = window();
    if (m_output.isEmpty() || host == nullptr) return;
    const qreal ratio = host->effectiveDevicePixelRatio();
    const SizeReport report{m_output, QSize(qRound(width() * ratio), qRound(height() * ratio))};
    // Not laid out yet. The geometry change that lays it out reports.
    if (report.pixels.isEmpty() || m_sizeReported == report) return;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;
    QDBusMessage request = QDBusMessage::createMethodCall(QString::fromUtf8(kService), QString::fromUtf8(kPath),
                                                          QString::fromUtf8(kInterface),
                                                          QStringLiteral("SetOutputSize"));
    request.setArguments({report.output, static_cast<quint32>(report.pixels.width()),
                          static_cast<quint32>(report.pixels.height())});
    m_sizeReported = report;

    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(request), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, report](QDBusPendingCallWatcher* done) {
        done->deleteLater();
        if (!done->isError()) {
            // The daemon answers to this screen by name now, which is what a visibility report
            // needs and the only thing that says when it has one -- so this is where a report the
            // daemon could not place is sent again.
            reportVisible();
            return;
        }
        // Nothing to undo once a newer report has replaced this one.
        if (!(m_sizeReported == report)) return;
        m_sizeReported.reset();

        const QString name = done->error().name();
        // The daemon's arrival reports again, through the owner watcher.
        if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")) return;
        // Ordinary until the app names the screen, so not logged. This item exists only while
        // /config says the name is bound, so its retries end with it.
        const bool unnamed = name == QLatin1String(kErrorUnknownOutput);
        // Only what a resend can fix: an unnamed screen, a save that could not be queued, a daemon
        // too busy to answer. Anything else stands until the daemon or the screen changes, and
        // both of those report again on their own.
        const bool transient = unnamed || name == QLatin1String(kErrorRenderFailed) ||
                               name == QLatin1String("org.freedesktop.DBus.Error.NoReply") ||
                               name == QLatin1String("org.freedesktop.DBus.Error.Timeout");
        if (!unnamed) {
            std::fprintf(stderr, "wallpaperx: the daemon refused this screen's size -- %s\n",
                         sanitizeForLog(done->error().message().toStdString()).c_str());
        }
        // A trigger already pending goes sooner.
        if (transient && !m_sizeReportTimer.isActive()) m_sizeReportTimer.start(kRefusedRetryMs);
    });
}

// Every generation mark at once, and the frame they describe. Generations belong to one monitor
// and one daemon instance, so a change of either invalidates all of them -- resetting some and not
// the rest is what made a restarted daemon's frames read as older than the screen. The epoch bump
// is what makes a pass that is importing right now discard its write-back.
void WallpaperXItem::discardHeldFrame() {
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
        // The imports go with the marks: what invalidates a generation is a daemon or a monitor
        // change, and neither will ever hand over those images again. Only the render thread can
        // free them. Never a downgrade -- an image already given back must still stop being shown.
        if (m_nodeRequest == NodeRequest::None) m_nodeRequest = NodeRequest::ForgetStaleImports;
    }
}

void WallpaperXItem::forgetGenerations() {
    discardHeldFrame();
    publishReason();
    // The request above is only a request; without a pass, an item whose daemon never comes back
    // keeps a dead one's ring imported for the session. Not in discardHeldFrame(), which the
    // destructor reaches through releaseHeldImages().
    update();
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
                m_sizeReported.reset();
                // A new daemon takes every screen to be drawn until it is told otherwise, and one
                // that has gone remembers nothing.
                m_visibleReported.reset();
                // On departures too, and before the return below: a daemon numbers its images from
                // scratch, so every mark this item holds is meaningless either way -- and one that
                // never comes back is the case where no later frame would clear them or the
                // imports they describe.
                forgetGenerations();
                if (newOwner.isEmpty()) {
                    setFetchReason(tr("WallpaperX is not running."));
                    return;
                }
                scheduleSizeReport();
                // Gated: an unconditional retry would add a duplicate match rule per restart,
                // which is one of the things that makes the subscription fail in the first place.
                if (!m_subscribed) m_subscribed = subscribeToFrameReady();
                g_modifiersReported.store(false);
                fetchFrame(0);
            });
    // After the watcher, so an owner change between the two is reported by it rather than lost:
    // the bus sends its signals and this reply on one connection, in order.
    askWhoOwnsTheDaemon();

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
    // Nothing to do with a frame while this item is not on the screen, and every one fetched would
    // take an image out of the daemon's ring for a wallpaper nobody is looking at.
    if (m_monitorId < 0 || !m_onScreen) return;

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
                // A reply for a monitor this item has since stopped showing is still a frame the
                // daemon recorded a hold for, so it is given back rather than dropped with the
                // message. Nothing else would: the daemon takes no claim back on its own.
                if (requestedMonitor == m_monitorId) {
                    takeFrameReply(requestedMonitor, done->reply());
                } else {
                    releaseFrameReply(requestedMonitor, done->reply());
                }
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

                    // Whatever answered on the bus wrote this text; the journal is a terminal.
                    const std::string refusal = sanitizeForLog(done->error().message().toStdString());
                    std::fprintf(stderr, "wallpaperx: the daemon refused the import modifier report -- %s\n",
                                 refusal.c_str());
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

// Which image a GetFrameFd reply granted, or 0 for a reply that granted none. One reader of
// field 11, so the two callers below cannot disagree about where the buffer id is.
quint64 WallpaperXItem::grantedBufferId(const QDBusMessage& call) {
    if (call.type() != QDBusMessage::ReplyMessage) return 0;
    const QList<QVariant> args = call.arguments();
    if (!replyIsReadable(static_cast<int>(args.size()))) return 0;
    return args.at(11).toULongLong();
}

// A reply this item can no longer use -- its monitor changed while the fetch was out. The daemon
// recorded the hold when it answered, so the claim is taken and handed straight back; dropping
// the message instead pins that image until this process leaves the bus. Reports nothing: the
// reason it would publish describes a screen this item has stopped showing.
void WallpaperXItem::releaseFrameReply(int monitorId, const QDBusMessage& call) {
    releaseClaim(BufferClaims::instance().take(monitorId, grantedBufferId(call), g_daemonOwner.toStdString()));
}

void WallpaperXItem::takeFrameReply(int monitorId, const QDBusMessage& call) {
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
        // Refused so the daemon keeps somewhere to draw, or because this item already holds as
        // many images as one consumer may. While a frame is on screen nothing is reported: a
        // reason hides the wallpaper behind the backdrop, and the frame being shown is more use
        // than a message about a fetch the next announcement will serve.
        //
        // An item with nothing on screen is the opposite case. It has no frame to protect, and
        // the announcement it would otherwise wait for is sent when some consumer releases an
        // image -- which a consumer that has stopped releasing never does.
        if (name == QLatin1String(kErrorNoFreeImage)) {
            // A refusal is a hand-over the daemon answered, so it says as much about whether the
            // desktop is drawing as a served one does -- and it is what this item gets once the
            // screen goes dark, because a scene graph that has stopped running never destroys a
            // texture and so never frees an image to be handed the next one.
            armPaintWatch();
            if (m_frameShown) {
                setFetchReason(QString());
            } else {
                setFetchReason(tr("WallpaperX has no image free for this screen yet."));
                retryWhileNothingIsShown();
            }
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
    frame.request.bufferId = grantedBufferId(call);
    frame.claim = BufferClaims::instance().take(monitorId, frame.request.bufferId, g_daemonOwner.toStdString());
    // Registered before anything else can fail, for the same reason the claim is taken first: from
    // here the set is what owes this image back, whichever path drops it.
    frame.claims = m_claims;
    m_claims->add(frame.claim);

    // Left the screen while the request was out. The hold the daemon recorded is given straight
    // back by `frame` going out of scope, since nothing else ends it.
    if (!m_onScreen) return;

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
    // A hand-over is what makes "is this desktop drawing what I give it" a question with an answer.
    armPaintWatch();
}

QSGNode* WallpaperXItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* node = static_cast<FrameNode*>(old);

    // What the GUI thread has asked of the node, and the only place it can be done: freeing a
    // texture or an import needs the context this pass has. Taken before the window checks, so a
    // pass that returns early does not leave the request standing for the next one to act on late.
    NodeRequest request = NodeRequest::None;
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        request = std::exchange(m_nodeRequest, NodeRequest::None);
        // Cleared here too: a pass that was already importing when the drop was asked for writes
        // "a node is drawing" back afterwards, and this is the pass that makes that untrue.
        if (request == NodeRequest::Destroy) {
            m_shownGeneration = 0;
            m_nodeHasFrame = false;
        }
    }
    if (request == NodeRequest::Destroy) {
        delete node;
        node = nullptr;
        // The render thread must not emit, so the published reason is recomputed on the item's own.
        QMetaObject::invokeMethod(this, [this] { publishReason(); }, Qt::QueuedConnection);
    } else if (request == NodeRequest::ForgetStaleImports && node != nullptr) {
        node->forgetAllButDrawn();
    }

    // No window or no interface is transient -- a reparent in progress -- so nothing is reported
    // and nothing is torn down; the next sync pass has one.
    QQuickWindow* host = window();
    if (host == nullptr || host->rendererInterface() == nullptr) return node;

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

    // Counted past the backend check, and only there: what the GUI thread is waiting on is a pass
    // this item could have presented in, and on a scene graph that cannot import at all every
    // forced pass would otherwise read as a desktop drawing the wallpaper it can never show.
    m_paintCount.fetch_add(1, std::memory_order_relaxed);

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

        const ImportKey key = importKeyFor(incoming->claim, incoming->request);
        // An import is drawn again only for a picture that brings the producer's hand-over: waiting
        // on it is the one thing that tells the driver the memory holds something else now, and
        // neither extension promises anything without it. A picture arriving without one is
        // described to the driver afresh, as every picture was before imports were kept.
        const FrameNode::Import* reused =
            node != nullptr && carriesHandOver(incoming->request) ? node->held(key) : nullptr;

        Stopped stopped = Stopped::Nothing;
        std::string failure;
        unsigned int importedTexture = 0;  // the GL name this picture's hand-over is waited on against
        if (reused != nullptr) {
            // The memory descriptor goes unused: the import it would produce is the one held.
            if (incoming->request.fd >= 0) ::close(incoming->request.fd);
            incoming->request.fd = -1;
            importedTexture = reused->name();
        } else {
            // import() is handed no hand-over to perform, which is what makes its failure
            // readable: doing both halves it answers one error for two questions, and only one of
            // them is a verdict on the layout. Put back on the line after -- ~PendingFrame is what
            // closes them, and nothing between the two throws.
            std::vector<int> handOver;
            handOver.swap(incoming->request.syncFds);
            const FrameImport::Result memory = FrameImport::import(incoming->request);
            handOver.swap(incoming->request.syncFds);

            // Null means nothing took ownership of the import, so it is freed by hand below.
            // No alpha flag: every fourcc the daemon can export is an X variant, so the alpha
            // bytes are undefined and the scene graph must not blend against them.
            QSGTexture* texture =
                memory.texture == 0
                    ? nullptr
                    : QNativeInterface::QSGOpenGLTexture::fromNative(
                          memory.texture, host,
                          QSize(static_cast<int>(incoming->request.width), static_cast<int>(incoming->request.height)),
                          {});
            if (memory.texture == 0) {
                stopped = Stopped::Layout;
                failure = memory.error;
            } else if (texture == nullptr) {
                // The import succeeded and the scene graph would not take it, which is Qt's answer
                // and not the driver's, and carries no reason of its own.
                stopped = Stopped::Wrapping;
                failure = "the scene graph would not wrap the imported texture";
                FrameImport::release(memory);
            } else {
                // Built only now: a node handed back without a texture is a null dereference
                // inside the scene graph.
                if (node == nullptr) node = new FrameNode();
                node->adopt(key, memory, texture);
                importedTexture = memory.texture;
            }
        }

        // The hand-over, per picture, whichever way its memory got here.
        if (stopped == Stopped::Nothing && !FrameImport::acquire(incoming->request, importedTexture, &failure)) {
            stopped = Stopped::HandOver;
            ++m_handOversFailed;
        }

        // A picture that could not be handed over leaves the node drawing the last one that was, so
        // the item neither blanks nor says it is drawing nothing -- this file's rule elsewhere is
        // that a frame on screen outranks a message about a fetch that failed. Its imports stay
        // with it: every one is still good.
        //
        // Bounded, because that tolerance is for a blip. A hand-over that has stopped working
        // altogether would otherwise freeze the wallpaper on one picture while the item went on
        // reporting that it is drawing normally, which is the one state nothing else here can tell
        // the user about.
        const bool stillDrawing = stopped == Stopped::HandOver && m_handOversFailed <= kToleratedHandOverFailures &&
                                  node != nullptr && node->texture() != nullptr;

        if (stopped != Stopped::Nothing) {
            reportImportFailure(failure);
            // Only a refusal to describe the memory is evidence about the layout. A hand-over that
            // could not be performed says nothing about it -- the wait is the same whichever
            // modifier the frame arrived in -- and withdrawing on one would take a layout this
            // driver demonstrably imports out of the daemon's hands for the session.
            if (stopped == Stopped::Layout && incoming->request.handleType == FrameImport::HandleType::DmaBuf) {
                withdrawImportModifier(incoming->request.drmFormat, incoming->request.drmModifier);
            }
            if (!stillDrawing) {
                setRenderReason(tr("WallpaperX produced a frame this desktop could not import."));
                // The node goes too, and with it every import it held: keeping one would leave the
                // desktop showing a frame while the item reports it is drawing nothing, and a
                // layout the failure has just renegotiated makes those imports worthless anyway.
                delete node;
                node = nullptr;
                {
                    std::lock_guard<std::mutex> lock(m_importedMutex);
                    m_shownGeneration = 0;
                    m_nodeHasFrame = false;
                }
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
            // Both counted at the one event they describe -- a picture drawn -- so their sum is
            // what this item put on the screen and the split is how much of it needed an import of
            // its own. Counted at the top of either branch they would count attempts, and no
            // relation between them would be assertable.
            (reused == nullptr ? m_importCount : m_reusedImportCount).fetch_add(1, std::memory_order_relaxed);
            // The run of failed hand-overs is over: this is the picture that proves it.
            m_handOversFailed = 0;
            // The claim goes to the node, which is what samples the image, and is taken out of the
            // frame so ~PendingFrame does not give back an image about to be drawn. The node holds
            // it only until the next picture is sampled -- the import it belongs to is kept, the
            // daemon's permission to draw is not.
            node->present(key, incoming->claims, std::exchange(incoming->claim, BufferClaims::Claim{}));
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
            // What a drawn picture owes this item's own thread. The retry mark is cleared so
            // "retried once" means once per failure, not once per session: the same generation can
            // be re-fetched later (a window change re-asks) and must be allowed its retry again if
            // that import fails too. The counts are announced here rather than from a hop of their
            // own, which would be a second queued event per video frame; the render thread may not
            // emit at all.
            QMetaObject::invokeMethod(
                this,
                [this] {
                    m_retriedGeneration = 0;
                    emit importCountsChanged();
                },
                Qt::QueuedConnection);
            setRenderReason(QString());
        }
    }

    if (node == nullptr) return nullptr;
    node->setRect(boundingRect());
    return node;
}

// A refused fetch with nothing on screen asks again on its own, once at a time: the announcement
// that would otherwise wake it is sent when a consumer releases an image, and a consumer that has
// stopped releasing sends none. Armed only while this item is showing nothing, so a desktop with
// a wallpaper on it never polls.
void WallpaperXItem::retryWhileNothingIsShown() {
    if (m_retryQueued) return;
    m_retryQueued = true;
    QTimer::singleShot(kRefusedRetryMs, this, [this] {
        m_retryQueued = false;
        if (m_frameShown) return;
        fetchFrame(0);
    });
}

// A still wallpaper sends no further FrameReady, so a released item must ask on its own. Not
// fetchFrame(0), which forces: QML sets the monitor id moments before the item joins a scene, so
// a fetch for this screen is usually already out and forcing would ask for the same frame twice.
void WallpaperXItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemDevicePixelRatioHasChanged) {
        scheduleSizeReport();
        return;
    }
    if (change == ItemVisibleHasChanged) {
        updateOnScreen();
        return;
    }
    if (change != ItemSceneChange) return;
    updateOnScreen();
    if (value.window == nullptr) return;
    ensureFrameRequested();
    scheduleSizeReport();
}

void WallpaperXItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) scheduleSizeReport();
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
