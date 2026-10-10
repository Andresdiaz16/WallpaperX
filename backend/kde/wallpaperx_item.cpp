// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaperx_item.h"

#include "daemon/log_escape.h"
#include "daemon/ring_depth.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusServiceWatcher>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QOpenGLContext>
#include <QPointer>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
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

// This process has been sent as many descriptors as the daemon lets one caller hold without giving
// any back. Only this process can end it, so no announcement will follow: it backs off and asks
// again. The cap is 64 against this plugin's 4 per screen and 1, so reaching it means images not
// given back, or many re-fetches of one it is still showing that each carried a descriptor.
constexpr const char* kErrorTooManyDescriptors = "io.wallpaperx.Daemon.Error.TooManyDescriptors";

// Set when a refusal above has been reported, cleared by the next frame served, so a run of them
// is one line on stderr. Process-wide because the daemon counts per connection, not per item.
std::atomic<bool> g_descriptorRefusalReported{false};

// How long a refused item first waits before asking again. Long enough that it is not a poll of
// the daemon, short enough that a desktop showing the plain backdrop recovers on its own once an
// image comes free.
constexpr int kRefusedRetryMs = 2000;

// Where that wait stops doubling while the refusals go on: the longest a screen can lag an image
// coming free.
constexpr int kRefusedRetryMaxMs = 16000;

// How long the owner lookup first waits to ask again after a lost one. Short, because until one
// answers no item can fetch at all.
constexpr int kLookupRetryMs = 2000;

// Where a failed owner lookup's retry stops doubling. Short for the same reason; a lookup is one
// message to the bus itself, not to the daemon.
constexpr int kLookupRetryMaxMs = 8000;

// libdbus's "no timeout" (DBUS_TIMEOUT_INFINITE), which QtDBus passes through.
constexpr int kNoReplyTimeout = std::numeric_limits<int>::max();

// How long an item waits for a frame before it stops waiting and says the daemon could not be
// reached: QtDBus's default call timeout, the patience every other call on this bus gets.
constexpr int kFetchPatienceMs = 25000;

// `baseMs` doubled `times` times, but never past `capMs`.
int doubledWait(int baseMs, int times, int capMs) {
    int ms = baseMs;
    for (int i = 0; i < times && ms < capMs; ++i) ms *= 2;
    return std::min(ms, capMs);
}

// Where a window report's retry stops doubling: the longest a pause or resume can lag a daemon that
// has recovered, if no window changes meanwhile.
constexpr int kWindowRetryMaxMs = 60000;

// Where a size report's retry stops doubling: the longest a screen can stay drawn at the wrong size
// after a daemon that failed to take it has recovered.
constexpr int kSizeRetryMaxMs = 8000;

// A screen size reported before the daemon knows the screen's name. Ordinary while the app has
// not named it, and answered by the report the naming's ConfigChanged sends.
constexpr const char* kErrorUnknownOutput = "io.wallpaperx.Daemon.Error.UnknownOutput";

// What answers a refused size or window report. Absent and Unnamed end with an event: the daemon's
// arrival, or the ConfigChanged that naming the screen sends. Permanent cannot be cured by sending
// the same report again. Anything else -- a daemon too busy, a bus short of memory, an error this
// plugin does not know -- has nothing that announces its end, so it is retried.
enum class Refusal { Absent, Unnamed, Permanent, Retried };

Refusal classifyRefusal(const QString& name) {
    if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")) return Refusal::Absent;
    if (name == QLatin1String(kErrorUnknownOutput)) return Refusal::Unnamed;
    if (name == QLatin1String("org.freedesktop.DBus.Error.InvalidArgs") ||
        name == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod")) {
        return Refusal::Permanent;
    }
    return Refusal::Retried;
}

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

// Every descriptor a request still owns: a FAILED import leaves them with the caller, and a frame
// superseded before the render thread arrives never reached the importer. One helper, because a sync
// descriptor forgotten on any path leaks one per frame, as the memory one would.
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

// The acquire timeline as imported into one GL share group, which every context of the group waits
// on: the semaphore name belongs to the group, and NVIDIA refuses every semaphore import in the process
// once enough have been made, released ones included. An unshared context is a group of one.
//
// Every import the group made is kept until the group's last context goes, and none is deleted by
// hand: another context of the group may have a wait on it queued and not yet flushed, and no context
// can order a delete after another's command stream. Keeping them costs a semaphore name per daemon
// replacement and no imports, since released imports count against NVIDIA's limit too. A daemon's
// late pictures are waited on its own import for as long as the group lives.
struct GroupTimeline {
    struct Import {
        FrameImport::Timeline timeline;
        std::string owner;  // the unique name that granted it
    };
    // Held across the import and each wait: the value waited for is state of the shared semaphore.
    std::mutex mutex;
    std::vector<Import> imports;  // guarded by mutex; only grows
};
// A record exists exactly while its group's destroyed() is connected, so a group made later at the
// same address starts clean. Shared because the render thread uses it after dropping the list's lock;
// no context of the group can be current once the group is destroyed.
struct ShareGroupRecord {
    const QOpenGLContextGroup* group = nullptr;
    std::shared_ptr<GroupTimeline> timeline;
};
std::mutex g_shareGroupsMutex;                // never held across a driver call or a GroupTimeline's mutex
std::vector<ShareGroupRecord> g_shareGroups;  // guarded by g_shareGroupsMutex

// The record of `group`, made on first use. Render thread, a context of `group` current.
std::shared_ptr<GroupTimeline> groupTimeline(QOpenGLContextGroup* group) {
    std::lock_guard<std::mutex> lock(g_shareGroupsMutex);
    const auto found = std::find_if(g_shareGroups.begin(), g_shareGroups.end(),
                                    [group](const ShareGroupRecord& record) { return record.group == group; });
    if (found != g_shareGroups.end()) return found->timeline;
    g_shareGroups.push_back({group, std::make_shared<GroupTimeline>()});
    try {
        // Direct: emitted on whichever thread deletes the group, after its last context has gone and
        // taken the semaphore with it.
        QObject::connect(group, &QObject::destroyed, [group] {
            std::lock_guard<std::mutex> lock(g_shareGroupsMutex);
            std::erase_if(g_shareGroups, [group](const ShareGroupRecord& record) { return record.group == group; });
        });
    } catch (...) {
        g_shareGroups.pop_back();
        throw;
    }
    return g_shareGroups.back().timeline;
}

// Set by the first semaphore import the driver itself refuses. On NVIDIA none succeeds again anywhere
// in the process, so every context draws without the producer's acquire from then on -- a small risk
// of a torn or stale picture, chosen over a blank desktop.
std::atomic<bool> g_semaphoresRefused{false};

// Only for a refusal by glImportSemaphoreFdEXT itself: a flag set for anything else would give up the
// hand-over for the session over a failure that says nothing about the driver.
void noteSemaphoresRefused(const std::string& why) {
    if (g_semaphoresRefused.exchange(true)) return;
    std::fprintf(stderr,
                 "wallpaperx: the graphics driver refused a semaphore import (%s), and on this driver none succeeds "
                 "in this process again. The desktop now draws without waiting for the daemon's hand-over: the "
                 "wallpaper keeps showing, but may occasionally show a picture the daemon had not finished writing.\n",
                 why.c_str());
}

// Render thread, context current. A context that cannot wait -- no support, or an import refused --
// still draws, reading the frame without the producer's acquire, which is undefined. Here rather than
// in import(): going without is the desktop's choice, not the importer's to make silently. Checked
// per picture, a value too, since the item can have moved to another context since asking.
void dropUnwaitableSync(FrameImport::Request& request) {
    if (request.syncFds.empty() && request.timelineValue == 0) return;

    if (!g_semaphoresRefused.load()) {
        const bool value = request.timelineValue != 0;
        std::string whyNot;
        if (value ? FrameImport::supportsTimeline(&whyNot) : FrameImport::supportsSync(&whyNot)) return;
        // Per form: a value lands here only until the next fetch, and must not use up the other report.
        static std::once_flag reportedSync;
        static std::once_flag reportedValue;
        std::call_once(value ? reportedValue : reportedSync, [&whyNot] {
            std::fprintf(stderr,
                         "wallpaperx: drawing frames without the producer's hand-over -- %s. The wallpaper still "
                         "shows; what is lost is the guarantee that the compositor reads it only after the daemon "
                         "has finished writing it.\n",
                         whyNot.c_str());
        });
    }
    for (int syncFd : request.syncFds) {
        if (syncFd >= 0) ::close(syncFd);
    }
    request.syncFds.clear();
    request.timelineValue = 0;
}

// Render threads, one per window, hence the lock. Once per distinct reason: a failed import withdraws
// its modifier, and the layout chosen instead can fail differently. Not "changed since last time":
// two items failing alternately would log at display rate. Capped because a reason names the frame's
// GPU UUID, which is whatever the daemon sent, and a peer varying it would defeat the rule.
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

// Render threads, so atomic. Capped like reportImportFailure(): lostImportCount() says how often.
void reportImportLost() {
    constexpr unsigned kMaxReports = 8;
    static std::atomic<unsigned> reported{0};

    const unsigned n = reported.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > kMaxReports) return;
    std::fprintf(stderr,
                 "wallpaperx: a frame arrived without the memory of an image this screen had dropped since asking; "
                 "asking again without naming any\n");
    if (n == kMaxReports) std::fprintf(stderr, "wallpaperx: no further lost imports will be reported\n");
}

// This process's acquire timeline, as the daemon last answered. One per process: the daemon keys it
// on the bus connection every item shares, and each node imports it into its own context. Written by
// the GUI thread, read by render threads. `owner` is the unique name the request was addressed to,
// and only the current daemon's record is ever replaced. Until it is Held for the current daemon,
// fetches ask for sync_fds.
struct AcquireTimelineRecord {
    enum class State { NotAsked, Asking, Held, Refused };
    State state = State::NotAsked;
    std::string owner;
    int fd = -1;       // Held only; owned
    quint64 ask = 0;   // numbers the requests, so an answer to one that was superseded is dropped
    // NotAsked after a failed answer: not asked again before this, since each ask takes the
    // daemon's GPU lock and one comes with every binary picture.
    std::chrono::steady_clock::time_point retryAfter{};
};
std::mutex g_timelineMutex;
AcquireTimelineRecord g_timeline;  // guarded by g_timelineMutex

// Errors meaning this daemon will never hand this process a timeline. Anything else is asked about
// again after kRefusedRetryMs; until then pictures come with sync_fds, as after a lost answer.
bool timelineRefusalIsPermanent(const QString& name) {
    return name == QLatin1String("io.wallpaperx.Daemon.Error.Unsupported") ||
           name == QLatin1String("io.wallpaperx.Daemon.Error.TooManyTimelineCopies") ||
           name == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod") ||
           name == QLatin1String("org.freedesktop.DBus.Error.UnknownInterface");
}

// Whether the record already answers for `daemon`, or asked it too recently. Caller holds
// g_timelineMutex.
bool timelineSettledLocked(const std::string& daemon) {
    using State = AcquireTimelineRecord::State;
    if (g_timeline.owner != daemon) return false;
    return g_timeline.state != State::NotAsked || std::chrono::steady_clock::now() < g_timeline.retryAfter;
}

// GUI thread, the only one that knows which daemon is current. Asks `daemon` for its timeline if it
// still is, addressed to its unique name so the answer can only be that daemon's.
void askForAcquireTimeline(const std::string& daemon) {
    if (daemon.empty() || daemon != BufferClaims::instance().currentDaemon().owner) return;
    QCoreApplication* app = QCoreApplication::instance();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (app == nullptr || !bus.isConnected()) return;

    quint64 ask = 0;
    int stale = -1;
    {
        std::lock_guard<std::mutex> lock(g_timelineMutex);
        if (timelineSettledLocked(daemon)) return;
        stale = std::exchange(g_timeline.fd, -1);
        g_timeline.state = AcquireTimelineRecord::State::Asking;
        g_timeline.owner = daemon;
        ask = ++g_timeline.ask;
    }
    if (stale >= 0) ::close(stale);

    QDBusMessage request = QDBusMessage::createMethodCall(QString::fromStdString(daemon), QString::fromUtf8(kPath),
                                                          QString::fromUtf8(kInterface),
                                                          QStringLiteral("GetAcquireTimeline"));
    // Never timed out by QtDBus: the daemon charges this process for every descriptor it sends until
    // the process leaves the bus, so an answer dropped as late and asked for again is charged twice,
    // and enough of those reach the cap that refuses every picture. The call still ends when the
    // daemon leaves the bus; until then this daemon is not asked again, and pictures come with sync_fds.
    // On qApp, not on the item that asked: it may be gone by the answer, and the record would then
    // say Asking for good.
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(request, kNoReplyTimeout), app);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, app, [ask, daemon](QDBusPendingCallWatcher* done) {
        done->deleteLater();
        const QDBusMessage reply = done->reply();
        int fd = -1;
        if (!done->isError() && !reply.arguments().isEmpty()) {
            fd = ::fcntl(reply.arguments().at(0).value<QDBusUnixFileDescriptor>().fileDescriptor(), F_DUPFD_CLOEXEC,
                         0);
        }
        const bool permanent = done->isError() && timelineRefusalIsPermanent(done->error().name());
        int superseded = -1;
        {
            std::lock_guard<std::mutex> lock(g_timelineMutex);
            using State = AcquireTimelineRecord::State;
            if (g_timeline.ask != ask) {
                superseded = fd;
            } else if (fd >= 0) {
                superseded = std::exchange(g_timeline.fd, fd);
                g_timeline.state = State::Held;
                g_timeline.owner = daemon;
            } else if (permanent) {
                g_timeline.state = State::Refused;
                g_timeline.owner = daemon;
            } else {
                // Busy, a failed export, a daemon that has gone, or a descriptor that could not be taken.
                superseded = fd;
                g_timeline.state = State::NotAsked;
                g_timeline.retryAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRefusedRetryMs);
            }
        }
        if (superseded >= 0) ::close(superseded);
        if (permanent) {
            static std::once_flag reported;
            const std::string why = sanitizeForLog(done->error().message().toStdString());
            std::call_once(reported, [&why] {
                std::fprintf(stderr, "wallpaperx: waiting on a semaphore per picture -- the daemon has no acquire "
                                     "timeline for this desktop: %s\n",
                             why.c_str());
            });
        }
    });
}

// Render thread. Only a cheap early return: whether the record may be replaced is the GUI thread's
// to decide, since only it knows whether `sender` is still the daemon.
void wantAcquireTimeline(const std::string& sender) {
    if (sender.empty()) return;
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(g_timelineMutex);
        if (timelineSettledLocked(sender)) return;
    }
    QMetaObject::invokeMethod(app, [sender] { askForAcquireTimeline(sender); }, Qt::QueuedConnection);
}

// Whether this process holds the acquire timeline `owner` granted. Any thread.
bool holdsAcquireTimeline(const std::string& owner) {
    if (owner.empty()) return false;
    std::lock_guard<std::mutex> lock(g_timelineMutex);
    return g_timeline.state == AcquireTimelineRecord::State::Held && g_timeline.owner == owner;
}

// Render thread. A descriptor of its own for the timeline `sender` granted, or -1 when this process
// holds none from that name. Duplicated under the lock, so the GUI thread replacing the record
// cannot close the number between it being read and being imported.
int acquireTimelineFdFor(const std::string& sender) {
    if (sender.empty()) return -1;
    std::lock_guard<std::mutex> lock(g_timelineMutex);
    if (g_timeline.state != AcquireTimelineRecord::State::Held || g_timeline.owner != sender) return -1;
    return ::fcntl(g_timeline.fd, F_DUPFD_CLOEXEC, 0);
}

// Render thread, context current. Waits for `value` against `texture` on the acquire timeline `sender`
// granted, as imported into the current context's share group -- by the first picture in the group
// that needs it. Keyed on the unique name alone: the daemon keeps one timeline per connection, and this
// process has one. `importRefused` as for FrameImport::importTimeline(); `imports` counts attempts.
// Lock order: the group's mutex, then g_timelineMutex.
bool acquireOnGroupTimeline(const std::string& sender, uint64_t value, unsigned int texture,
                            std::atomic<quint64>& imports, std::string& failure, bool& importRefused) {
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        failure = "no current GL context to import the acquire timeline into";
        return false;
    }
    const std::shared_ptr<GroupTimeline> group = groupTimeline(context->shareGroup());
    std::lock_guard<std::mutex> lock(group->mutex);
    const auto grantedBySender = [&sender](const GroupTimeline::Import& held) { return held.owner == sender; };
    const auto granted = std::find_if(group->imports.begin(), group->imports.end(), grantedBySender);
    if (granted != group->imports.end()) return FrameImport::acquire(granted->timeline, value, texture, &failure);

    // The record is made, and room for it reserved, before the import, so nothing can throw between the
    // import and storing it.
    GroupTimeline::Import fresh{{}, sender};
    group->imports.reserve(group->imports.size() + 1);
    const int fd = acquireTimelineFdFor(sender);
    if (fd < 0) {
        // A descriptor that could not be duplicated, or the record replaced for a newer daemon while
        // this picture was in flight.
        failure = "no acquire timeline from the daemon that sent this frame";
        return false;
    }
    imports.fetch_add(1, std::memory_order_relaxed);
    fresh.timeline = FrameImport::importTimeline(fd, &failure, &importRefused);
    ::close(fd);
    if (fresh.timeline.semaphore == 0) return false;
    group->imports.push_back(std::move(fresh));
    return FrameImport::acquire(group->imports.back().timeline, value, texture, &failure);
}

// GUI thread. Addressed to the unique name that granted the claim, never the well-known one: a unique
// name is never reused, so a release delayed past a daemon restart is dropped by the bus instead of
// freeing the SUCCESSOR's image of the same id under whoever is reading it.
void sendReleaseBuffer(const std::string& owner, int monitorId, quint64 bufferId, quint64 handover) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;

    QDBusMessage request = QDBusMessage::createMethodCall(QString::fromStdString(owner), QString::fromUtf8(kPath),
                                                          QString::fromUtf8(kInterface),
                                                          QStringLiteral("ReleaseBuffer"));
    request.setArguments({monitorId, bufferId, handover});
    // The pending call is dropped rather than watched: the reply carries nothing, and the only
    // error is a daemon that has gone -- which has already freed every claim of ours.
    bus.asyncCall(request);
}

// Tells the daemon this process has stopped reading an image, if this was the last import of it.
// Render thread, from scene graph teardown and from a destructor, so it must neither block nor
// throw: a throw there is std::terminate inside plasmashell. Posted to qApp, not to the item, because
// the scene graph destroys a texture AFTER the item that imported it is gone.
void releaseClaim(const BufferClaims::Claim& claim) noexcept {
    try {
        std::optional<BufferClaims::Release> release = BufferClaims::instance().giveBack(claim);
        if (!release) return;
        // Gone already: the process is on its way out, and leaving the bus frees every claim.
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) return;

        const int monitorId = claim.monitorId;
        const quint64 bufferId = claim.bufferId;
        QMetaObject::invokeMethod(
            app,
            [release = std::move(*release), monitorId, bufferId] {
                sendReleaseBuffer(release.owner, monitorId, bufferId, release.handover);
            },
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

// One claim waiting on the fence that says this process's GPU has finished reading the image. GUI
// thread, where the notifier's event loop and the bus are. Until the fence signals the ledger still
// counts the import, which is what makes a second live import of the image, or a daemon replaced
// meanwhile, come out right. The deadline bounds a fence that never signals: nothing else would free
// the claim, and that image would be out of the ring for the session.
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
// moment it is told, and commands sampling it can still be queued, so the claim waits on a fence
// placed after them. Render thread, context current, called where the texture died: the fence
// covers only commands issued before it. A path that cannot park the claim releases it now rather
// than dropping it -- a dropped claim leaves the ledger counting an import that no longer exists.
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

// Which of the daemon's images an import describes. Within one daemon lifetime the buffer id is one
// image, since the counter is never reset; across lifetimes ids start from one again, which is what
// `daemon` answers, as in the release ledger. The shape rides along so an image that does not match
// the import held for it is imported again rather than drawn at the wrong size or layout.
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
    return request.timelineValue != 0 ||
           std::any_of(request.syncFds.begin(), request.syncFds.end(), [](int syncFd) { return syncFd >= 0; });
}

// What stopped a picture reaching the screen. Named rather than read back out of the error text,
// because they differ in what may be concluded from them: only the first says anything about the
// layout the daemon chose, and only the last two leave the previous picture still being drawn.
enum class Stopped {
    Nothing,
    Layout,      // the driver would not describe this memory
    Wrapping,    // it did, and the scene graph would not take the texture
    HandOver,    // the memory is imported; the producer's acquire could not be performed
    ImportLost,  // the memory was left out for an import this picture cannot use after all: the
                 // node dropped it after the fetch named it, or the acquire had to be dropped.
                 // Nothing failed, so it is asked for again with the memory
};

// What the item hands the scene graph: the picture on the screen, the imports it can switch between
// without describing memory to the driver again, and the daemon's hold on the image it samples now.
// All of it is the render thread's, the destructor included -- the GL frees need the context the
// scene graph has current there. It is also why the imports are kept here: an item leaving the
// screen asks for its node to be destroyed, so "holds nothing importable" and "has no node" are one.
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
        // freeing afterwards would leave the GL objects alive past the only handle to them. A null
        // wrapper means this was moved from, and the import went with it.
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

    // Which images this node holds an import of, most recently drawn first.
    std::vector<ImportKey> heldKeys() const {
        std::vector<ImportKey> keys;
        keys.reserve(m_imports.size());
        for (const Import& import : m_imports) keys.push_back(import.key());
        return keys;
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

    // Draws the import held for `key` and takes the daemon's hold on the picture in it. The hold it
    // replaces is given back here, not when its import is freed: the import stays for the next
    // picture drawn into that image, and a hold kept that long leaves the daemon nowhere to draw.
    // Nothing of this item's samples the previous picture after this but commands earlier passes
    // issued, which the fence inside the release covers.
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

    // Every import but the one on the screen, for when the marks its frames were fetched against stop
    // meaning anything: no later hand-over can name these. The one path that frees imports without a
    // picture arriving, which a daemon that exited and a monitor this item left both need.
    void forgetAllButDrawn() {
        std::erase_if(m_imports, [this](const Import& import) { return !isDrawn(import); });
    }

private:
    // Whether the node is sampling this import. Asked, never inferred from position: an import
    // taken in for a picture whose hand-over then failed sits at the front without being drawn,
    // and freeing what the node points at is a use-after-free inside the scene graph.
    bool isDrawn(const Import& import) const { return import.texture() == texture(); }

    // Drops every import that is neither on the screen, nor the one just taken in, nor one a later
    // hand-over could still name -- an image of the ring being drawn from. Left to the count alone, a
    // gone daemon's or monitor's imports would drain one per picture, which for a still wallpaper is
    // never. A second entry under the front's key goes too: one lookup could not tell them apart. The
    // count is the backstop for a ring rebuilt with only new ids, walked from the least recently drawn.
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

// Who owns the daemon's bus name, watched once for the process and written to the ledger, which is
// the one record of it. Not per item: with no item alive a per-item watch sees nothing, so a daemon
// replaced while the desktop had no wallpaper item would stay recorded, and every request and
// release would go to a name that has left. GUI thread.
class DaemonWatch : public QObject {
    Q_OBJECT
public:
    // Made on first use, on qApp, and never destroyed before it. Null without a bus.
    static DaemonWatch* instance();

    // Whether no owner is known because asking the bus failed, rather than because the answer has
    // not come yet. Only meaningful in lifetime 0, which the first answer or change ends.
    static bool lookupFailed();

signals:
    // A new daemon lifetime has begun, owned by `owner` -- empty for nobody. The ledger records it
    // before this is emitted.
    void replaced(const QString& owner);
    // The bus answered who owned the name when the watch began, which begins the first lifetime.
    void named(const QString& owner);
    // Asking the bus who owns the name failed; it is asked again, later each time.
    void lookupLost();

private:
    explicit DaemonWatch(QObject* parent);
    void askWhoOwns();

    bool m_lookupFailed = false;
    // Asks lost so far, which the wait before the next ask grows with. The retry covers a lookup lost
    // while the owner stays put, which no change notice would report.
    int m_failedAsks = 0;
};

DaemonWatch* DaemonWatch::instance() {
    static QPointer<DaemonWatch> watch;
    if (watch) return watch;
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr || !QDBusConnection::sessionBus().isConnected()) return nullptr;
    watch = new DaemonWatch(app);
    return watch;
}

bool DaemonWatch::lookupFailed() {
    const DaemonWatch* watch = instance();
    return watch != nullptr && watch->m_lookupFailed && BufferClaims::instance().currentDaemon().lifetime == 0;
}

DaemonWatch::DaemonWatch(QObject* parent) : QObject(parent) {
    auto* watcher = new QDBusServiceWatcher(QString::fromUtf8(kService), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    // Every report is a change of owner, departures included, and each arrives here once.
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString&, const QString& owner) {
                BufferClaims::instance().daemonReplaced(owner.toStdString());
                emit replaced(owner);
            });
    // After the watcher: the bus sends its signals and the answer on one connection, in order, so a
    // change is either already in the answer or reported after it.
    askWhoOwns();
}

void DaemonWatch::askWhoOwns() {
    QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("/org/freedesktop/DBus"),
                                                          QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("GetNameOwner"));
    request.setArguments({QString::fromUtf8(kService)});
    // QtDBus's default timeout, every time: a bus slow at login is still answering, and timing an
    // ask out would only throw the answer away and tell every screen the daemon could not be reached.
    auto* call = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(request), this);
    connect(call, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* done) {
        done->deleteLater();
        QString owner;
        if (!done->isError()) {
            owner = done->reply().arguments().value(0).toString();
        } else if (done->error().name() != QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")) {
            // Lost rather than answered. Asked again, since until some answer arrives no request
            // can be addressed; a change reported meanwhile answers it instead.
            m_lookupFailed = true;
            emit lookupLost();
            QTimer::singleShot(doubledWait(kLookupRetryMs, m_failedAsks, kLookupRetryMaxMs), this, [this] {
                if (BufferClaims::instance().currentDaemon().lifetime == 0) askWhoOwns();
            });
            ++m_failedAsks;
            return;
        }
        if (BufferClaims::instance().daemonNamed(owner.toStdString())) emit named(owner);
    });
}

namespace {

// From any sender, unlike FrameReady: naming the service makes QtDBus resolve its owner before the
// rule is in place. A forged one costs a /config read, never more than one at a time, and a size
// report where one is owed. Not retried: for a new receiver connect() is false only on a disconnected
// bus, never for a rule the bus refuses.
void watchConfigChanged(QObject* receiver, const char* slot) {
    if (!QDBusConnection::sessionBus().connect(QString(), QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                                               QStringLiteral("ConfigChanged"), receiver, slot)) {
        std::fprintf(stderr, "wallpaperx: could not subscribe to WallpaperX config changes -- %s\n",
                     sanitizeForLog(QDBusConnection::sessionBus().lastError().message().toStdString()).c_str());
    }
}

}  // namespace

ConfigWatch::ConfigWatch(QObject* parent) : QObject(parent) {
    watchConfigChanged(this, SIGNAL(configChanged()));
    if (DaemonWatch* watch = DaemonWatch::instance()) {
        // A departure too: the screen then says the daemon is not running.
        connect(watch, &DaemonWatch::replaced, this, &ConfigWatch::configChanged);
        // A daemon that took its name before the watch's rule reached the bus is heard of only here,
        // and main.qml's first read may have gone before its port was open. An empty answer is a
        // daemon still absent, which that read already reported.
        connect(watch, &DaemonWatch::named, this, [this](const QString& owner) {
            if (!owner.isEmpty()) emit configChanged();
        });
    }
}

bool ConfigWatch::daemonAbsent() const {
    const BufferClaims::Daemon daemon = BufferClaims::instance().currentDaemon();
    return daemon.lifetime != 0 && daemon.owner.empty();
}

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
      claims(std::move(other.claims)),
      sender(std::move(other.sender)) {
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
        sender = std::move(other.sender);
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

WallpaperXItem::WallpaperXItem(QQuickItem* parent)
    : QQuickItem(parent), m_windowRetryMs(kRefusedRetryMs) {
    setFlag(ItemHasContents, true);
    m_sizeReportTimer.setSingleShot(true);
    connect(&m_sizeReportTimer, &QTimer::timeout, this, &WallpaperXItem::reportSize);
    m_paintWatch.setSingleShot(true);
    connect(&m_paintWatch, &QTimer::timeout, this, &WallpaperXItem::checkPainting);
    m_windowReportTimer.setSingleShot(true);
    connect(&m_windowReportTimer, &QTimer::timeout, this, &WallpaperXItem::reportWindows);
    m_backoffTimer.setSingleShot(true);
    connect(&m_backoffTimer, &QTimer::timeout, this, &WallpaperXItem::reopenFetchAfterBackoff);
    m_noImageRetryTimer.setSingleShot(true);
    connect(&m_noImageRetryTimer, &QTimer::timeout, this, [this] {
        if (!m_frameShown) fetchFrame(0);
    });
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

    forgetGenerations(Undrawn::Drop);
    // A new id can mean the daemon moved this name to another monitor, and a move drops what the
    // name reported. The id is read back from the daemon, so the move is done: sent now, as
    // setOutput does, since the size report has failures it never retries.
    m_sizeReported.reset();
    m_visibleReported.reset();
    m_windowsReported.reset();
    restartWindowRetry();
    scheduleSizeReport();
    reportVisible();
    reportWindows();

    emit monitorIdChanged();
    // A busy image ring was the old monitor's; a descriptor back-off is the whole process's and
    // runs on, the fetch below queued behind it.
    abandonNoImageRun();
    // A screen that has just been identified may already have a frame waiting; without this the
    // desktop stays blank until the next time the wallpaper happens to change.
    if (m_monitorId >= 0) fetchFrame(0);
}

void WallpaperXItem::setOutput(const QString& output) {
    if (m_output == output) return;
    m_output = output;
    // Every report is filed under the screen's name, so none says anything about the new one. The
    // window record carries the name, so it never matches a report for the new one.
    m_sizeReported.reset();
    m_visibleReported.reset();
    emit outputChanged();
    scheduleSizeReport();
    // Not after the size report, which waits to settle: the daemon places these by name alone, and
    // a video stays stopped until it hears this screen is visible. An unknown-name refusal reopens
    // the size report, which the screen's naming sends.
    reportVisible();
    restartWindowRetry();
    reportWindows();
}

void WallpaperXItem::setWindowFocused(bool focused) {
    if (m_windows.focused == focused) return;
    m_windows.focused = focused;
    emit windowFocusedChanged();
    m_windowReportTimer.start(0);
}

void WallpaperXItem::setWindowMaximized(bool maximized) {
    if (m_windows.maximized == maximized) return;
    m_windows.maximized = maximized;
    emit windowMaximizedChanged();
    m_windowReportTimer.start(0);
}

void WallpaperXItem::scheduleSizeReport() { m_sizeReportTimer.start(kSizeReportSettleMs); }

void WallpaperXItem::releaseHeldImages() {
    // The frame in hand goes with the claims, and before them: an image the daemon has been told
    // is free must not be left importable by the next pass. The one teardown both this and a
    // monitor change use, so the marks they invalidate cannot drift apart.
    discardHeldFrame(Undrawn::Drop);

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
        m_knownImports.clear();
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
        // Off the screen it stops speaking for the screen's windows, so another item for the name
        // may report over this one meanwhile; coming back sends the state even if it is unchanged.
        m_windowsReported.reset();
        restartWindowRetry();
        // A busy image ring is no concern off the screen; a descriptor back-off is the whole
        // process's and runs on, the fetch on return queued behind it.
        abandonNoImageRun();
        return;
    }
    // Assumed to be painted until a frame handed over proves otherwise, so a wallpaper resumes
    // without waiting out an interval. The fetch is forced: the item gave up what it was showing
    // on the way out, and the daemon may have announced nothing meanwhile.
    m_painting = true;
    m_paintUnconfirmed = false;
    reportVisible();
    reportWindows();
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
            // not sent again while the size has not changed -- so it is reopened here, to go with
            // the ConfigChanged that naming the screen sends.
            m_sizeReported.reset();
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

void WallpaperXItem::reportWindows() {
    const WindowReport report{m_output, m_windows};
    // Off the screen it does not speak for the screen's windows: another item for the same name
    // may be showing it.
    if (!m_onScreen || m_output.isEmpty() || m_windowsReported == report) return;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;
    QDBusMessage request = QDBusMessage::createMethodCall(QString::fromUtf8(kService), QString::fromUtf8(kPath),
                                                          QString::fromUtf8(kInterface),
                                                          QStringLiteral("SetOutputFocus"));
    request.setArguments({report.output, report.windows.focused, report.windows.maximized});
    m_windowsReported = report;

    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(request), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, report](QDBusPendingCallWatcher* done) {
        done->deleteLater();
        if (!done->isError()) {
            m_windowRetryMs = kRefusedRetryMs;
            return;
        }
        if (m_windowsReported != report) return;
        m_windowsReported.reset();
        const QString name = done->error().name();
        const Refusal refusal = classifyRefusal(name);
        // No daemon, or one older than this call: a new owner reports again, and an old daemon has
        // no pause setting for the report to serve. Neither is logged -- this follows every focus
        // change.
        if (refusal == Refusal::Absent || name == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod")) {
            return;
        }
        // Not logged, and resent through the size report as visibility is: its success is the only
        // sign the daemon knows the name again.
        if (refusal == Refusal::Unnamed) {
            m_sizeReported.reset();
            return;
        }
        // A refusal that does not clear repeats with every retry, so a run of them is logged once.
        if (refusal == Refusal::Permanent || m_windowRetryMs == kRefusedRetryMs) {
            std::fprintf(stderr, "wallpaperx: the daemon refused this screen's window state -- %s\n",
                         sanitizeForLog(done->error().message().toStdString()).c_str());
        }
        // Its own retry, backed off. Not the size report's: the daemon can go on accepting that while
        // refusing this, which would resend both every interval for the session.
        if (refusal == Refusal::Retried) {
            m_windowReportTimer.start(m_windowRetryMs);
            m_windowRetryMs = std::min(m_windowRetryMs * 2, kWindowRetryMaxMs);
        }
    });
}

// Stopped, not left to run: the retry it holds was paced for the old run, and each caller resends
// on its own -- directly, through the size report's success, or on coming back on the screen.
void WallpaperXItem::restartWindowRetry() {
    m_windowReportTimer.stop();
    m_windowRetryMs = kRefusedRetryMs;
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
            m_sizeRefusals.reset();
            // The daemon answers to this screen by name now, which is what a visibility report
            // needs and the only thing that says when it has one -- so this is where a report the
            // daemon could not place is sent again.
            reportVisible();
            reportWindows();
            return;
        }
        // Nothing to undo once a newer report has replaced this one.
        if (!(m_sizeReported == report)) return;

        const Refusal refusal = classifyRefusal(done->error().name());
        // Not logged: an unnamed screen is ordinary until the app runs.
        if (refusal == Refusal::Absent || refusal == Refusal::Unnamed) {
            m_sizeReported.reset();
            return;
        }
        const bool retried = refusal == Refusal::Retried;
        const int times = retried && m_sizeRefusals && m_sizeRefusals->report == report ? m_sizeRefusals->times + 1 : 0;
        // A run of retries is logged once: each would repeat the line.
        if (times == 0) {
            std::fprintf(stderr, "wallpaperx: the daemon refused this screen's size -- %s\n",
                         sanitizeForLog(done->error().message().toStdString()).c_str());
        }
        // Refused again while the size and the daemon stay as they are, so the record stays: only a
        // change of either sends it.
        if (!retried) return;
        m_sizeReported.reset();
        m_sizeRefusals = SizeRefusals{report, times};
        // A trigger already pending goes sooner.
        if (!m_sizeReportTimer.isActive()) {
            m_sizeReportTimer.start(doubledWait(kRefusedRetryMs, times, kSizeRetryMaxMs));
        }
    });
}

// Every generation mark at once, and the frame they describe. Generations belong to one monitor
// and one daemon instance, so a change of either invalidates all of them. The epoch bump is what
// makes a pass that is importing right now discard its write-back.
void WallpaperXItem::discardHeldFrame(Undrawn undrawn) {
    m_fetchedGeneration = 0;
    m_retriedGeneration = 0;
    // Destroyed outside the lock: giving its claim back reaches the bus, and m_pendingMutex is
    // taken by the render thread on every pass.
    std::optional<PendingFrame> dropped;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (undrawn == Undrawn::Drop) {
            dropped = std::move(m_pending);
            m_pending.reset();
        } else if (m_pending) {
            // Kept, it is drawn after the marks are gone, and its number counts in the replaced
            // daemon's numbering: written back it would make the successor's frames read as older.
            m_pending->generation = 0;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        m_shownGeneration = 0;
        ++m_generationEpoch;
        // The imports go with the marks: what invalidates a generation is a daemon or a monitor
        // change, and neither will ever hand over those images again -- except in the reply to a
        // request already out, whose arrival asks for this instead, or the kept frame, which the
        // pass draws before acting on it. Only the render thread can free them. Never a downgrade
        // -- an image already given back must still stop being shown.
        if (m_fetch != Fetch::Asking && m_nodeRequest == NodeRequest::None) {
            m_nodeRequest = NodeRequest::ForgetStaleImports;
        }
    }
}

void WallpaperXItem::forgetGenerations(Undrawn undrawn) {
    discardHeldFrame(undrawn);
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
    watchConfigChanged(this, SLOT(onConfigChanged()));
    // No early return on failure: an item that cannot hear FrameReady must still notice the
    // daemon arriving or leaving, and the watcher below is the only thing that can retry.
    if (!m_subscribed) publishReason();

    // The item is created with the desktop, so the daemon may not own its name yet.
    if (DaemonWatch* watch = DaemonWatch::instance()) {
        // The first owner known. Nothing has been asked before it -- fetchFrame() waits for it. The
        // reports need nothing here: a report needs an output, which comes after this constructor
        // put the watch's rule on the bus, so a daemon that refused one as absent arrives as a change.
        connect(watch, &DaemonWatch::named, this, [this](const QString& owner) {
            if (owner.isEmpty()) {
                setFetchReason(tr("WallpaperX is not running."));
                return;
            }
            ensureFrameRequested();
        });
        // Nothing can be asked until the bus answers, so this is the only reason a screen gets.
        connect(watch, &DaemonWatch::lookupLost, this, [this] {
            if (DaemonWatch::lookupFailed()) setFetchReason(tr("WallpaperX could not be reached."));
        });
        connect(watch, &DaemonWatch::replaced, this, [this](const QString& newOwner) {
            m_sizeReported.reset();
            // The old daemon's refusals: the new one's first is logged and retried at the first delay.
            m_sizeRefusals.reset();
            // A new daemon takes every screen to be drawn until it is told otherwise, and one
            // that has gone remembers nothing.
            m_visibleReported.reset();
            // Window reports live in the daemon's memory only.
            m_windowsReported.reset();
            restartWindowRetry();
            // A refusal was the old daemon's to make. What was queued behind it is covered by the
            // forced fetch below, or by the next arrival's.
            abandonRefusals();
            // On departures too, and before the return below: a daemon numbers its images from
            // scratch, so every mark this item holds is meaningless either way -- and one that
            // never comes back is the case where no later frame would clear them or the
            // imports they describe. A picture already in is still this screen's, asked of the
            // daemon that drew it, so it is drawn.
            forgetGenerations(Undrawn::Keep);
            // Nothing is sent on a departure: it could only be refused as ServiceUnknown, and
            // the cleared records above make the next arrival send both reports.
            if (newOwner.isEmpty()) {
                setFetchReason(tr("WallpaperX is not running."));
                return;
            }
            scheduleSizeReport();
            // Sent now, as setOutput does: the size report has failures it never retries, and a
            // name the new daemon does not know yet is refused as UnknownOutput, sent again once
            // ConfigChanged says it has been named.
            // Visibility only from the screen, as reportWindows() gates itself: an item off it
            // still holds the name, and its "not visible" would override a copy showing it.
            if (m_onScreen) reportVisible();
            reportWindows();
            // Gated: QtDBus refuses a repeat of a connection already held, and that false would
            // clear m_subscribed and put the "could not subscribe" message over a working wallpaper.
            if (!m_subscribed) m_subscribed = subscribeToFrameReady();
            g_modifiersReported.store(false);
            fetchFrame(0);
        });
    }

    if (m_monitorId >= 0) fetchFrame(0);
}

// A size already reported is not sent again, and one waiting to settle or to be retried goes then.
void WallpaperXItem::onConfigChanged() {
    if (!m_sizeReportTimer.isActive()) reportSize();
}

void WallpaperXItem::onFrameReady(int monitorId, qulonglong generation) {
    if (monitorId != m_monitorId) return;
    fetchFrame(generation);
}

// Zero beside no request in flight is the item genuinely having nothing: m_fetchedGeneration is
// cleared by everything that invalidates what it holds -- a monitor change, a daemon restart,
// leaving the scene.
void WallpaperXItem::ensureFrameRequested() {
    if (m_fetch != Fetch::Idle || m_fetchedGeneration != 0) return;
    fetchFrame(0);
}

bool WallpaperXItem::fetchOnTimeline(const BufferClaims::Daemon& daemon) {
    if (!holdsAcquireTimeline(daemon.owner)) return false;
    std::lock_guard<std::mutex> lock(m_importedMutex);
    return m_contextWaitsOnTimeline && m_timelineRefusedBy != daemon.owner;
}

std::vector<quint64> WallpaperXItem::importsToDeclare(const BufferClaims::Daemon& daemon) {
    std::vector<quint64> declared;
    if (m_declareNoImports) return declared;
    std::lock_guard<std::mutex> lock(m_importedMutex);
    for (const KnownImport& known : m_knownImports) {
        if (declared.size() == kMaxKnownBuffers) break;
        if (known.daemon == daemon.lifetime && known.monitorId == m_monitorId) declared.push_back(known.bufferId);
    }
    return declared;
}

bool WallpaperXItem::marksMovedSince(quint64 epoch) {
    std::lock_guard<std::mutex> lock(m_importedMutex);
    return m_generationEpoch != epoch;
}

// The mark fetchFrame() checks only moves when a reply lands, so it cannot suppress a burst that
// arrives while one is out. One trailing fetch subsumes them, and it carries the highest generation
// announced rather than 0 -- 0 forces, so a reply that already brought the newest frame would
// otherwise be followed by a redundant export and a second import of the same image.
void WallpaperXItem::queueFetch(quint64 generation) {
    if (!m_queuedFetch) {
        m_queuedFetch = generation;
    } else if (*m_queuedFetch == 0 || generation == 0) {
        m_queuedFetch = 0;
    } else {
        m_queuedFetch = std::max(*m_queuedFetch, generation);
    }
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

    if (m_fetch != Fetch::Idle) {
        queueFetch(generation);
        return;
    }

    // ASYNCHRONOUS, and this is not a preference. A blocking call parks the thread that runs the
    // compositor's UI until the daemon answers -- and the first fetch happens while QML is still
    // building this item, where the nested event loop a blocking call needs never runs: it never
    // returns.
    //
    // Checked before asking, not after: with no bus, asyncCall hands back a null pending call
    // whose watcher never emits anything, so the in-flight latch below would never clear and
    // this item would stop fetching for the rest of the session. Reported rather than inlined --
    // answering the reply here would make the fetch synchronous for every caller.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        setFetchReason(tr("WallpaperX could not be reached."));
        return;
    }

    // Only ever to the daemon's unique name, and the reply labelled with the daemon lifetime it was
    // asked in, since QtDBus leaves a reply's sender blank: a value is only valid on the timeline of
    // the daemon that sent it, an image id only names an import of that daemon's, and a claim may
    // only be released to it. The well-known name would be served by whichever daemon owns it when
    // the bus routes the call, which can be one this item has not yet heard arrive. With no owner
    // known there is nobody to ask; DaemonWatch's signals ask again once there is.
    const BufferClaims::Daemon asked = BufferClaims::instance().currentDaemon();
    if (asked.owner.empty()) {
        if (asked.lifetime != 0) {
            setFetchReason(tr("WallpaperX is not running."));
        } else if (DaemonWatch::lookupFailed()) {
            setFetchReason(tr("WallpaperX could not be reached."));
        }
        return;
    }
    if (m_unansweredLifetime == asked.lifetime) {
        queueFetch(generation);
        return;
    }
    const bool onTimeline = fetchOnTimeline(asked);
    std::vector<quint64> declared = importsToDeclare(asked);
    QDBusMessage request =
        QDBusMessage::createMethodCall(QString::fromStdString(asked.owner), QString::fromUtf8(kPath),
                                       QString::fromUtf8(kInterface), QStringLiteral("GetFrame"));
    QList<quint64> wire;
    wire.reserve(static_cast<qsizetype>(declared.size()));
    for (const quint64 id : declared) wire.append(id);
    request.setArguments({m_monitorId, QVariant::fromValue(wire), onTimeline});
    // Never timed out by QtDBus, which would drop a reply arriving late -- and the daemon recorded a
    // hand-over for it that only reading the reply lets this process give back. The call still ends
    // when the daemon leaves the bus. kFetchPatienceMs stands in for the timeout below.
    QDBusPendingCall pending = bus.asyncCall(request, kNoReplyTimeout);
    m_fetch = Fetch::Asking;
    m_declareNoImports = false;
    // On qApp, not on this item: a reply that outlives the item still records a hand-over, and only
    // taking it lets a release name it.
    QCoreApplication* app = QCoreApplication::instance();
    auto* watcher = new QDBusPendingCallWatcher(pending, app != nullptr ? static_cast<QObject*>(app) : this);
    // The reply carries no monitor id of its own.
    const int requestedMonitor = m_monitorId;
    quint64 epoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_importedMutex);
        epoch = m_generationEpoch;
    }
    const auto answered = [this, generation, requestedMonitor, asked, onTimeline, epoch,
                           declared = std::move(declared)](const QDBusMessage& reply) {
        if (reply.type() == QDBusMessage::ReplyMessage) {
            g_descriptorRefusalReported.store(false);
            endRefusalRuns();
        }
        // Refused until this process gives descriptors back, which asking again cannot
        // change: the latch stays shut for a backoff, and the refused fetch and what is
        // announced meanwhile queue behind it as they would behind a fetch in flight. Only
        // the current daemon's refusal: a replaced one's says nothing about the daemon asked next.
        // Off the screen too, since the refusal is the whole process's.
        const bool backOff = reply.type() == QDBusMessage::ErrorMessage &&
                             reply.errorName() == QLatin1String(kErrorTooManyDescriptors) &&
                             asked.lifetime == BufferClaims::instance().currentDaemon().lifetime;
        // Set before any early return, or one dropped reply stops fetching for good.
        m_fetch = backOff ? Fetch::BackingOff : Fetch::Idle;
        std::optional<quint64> queued;
        if (!backOff) {
            queued = std::exchange(m_queuedFetch, std::nullopt);
        } else if (requestedMonitor == m_monitorId) {
            queueFetch(generation);
        }
        // A reply for a monitor this item has since stopped showing is still a frame the
        // daemon recorded a hold for, so it is given back rather than dropped with the
        // message. Nothing else would: the daemon takes no claim back on its own.
        if (requestedMonitor == m_monitorId) {
            takeFrameReply(requestedMonitor, reply, asked, onTimeline, declared);
        } else {
            releaseFrameReply(requestedMonitor, reply, asked.lifetime);
        }
        // The forget the change of daemon or monitor left to this reply, now that it is in:
        // after its picture, which the pass draws first.
        if (marksMovedSince(epoch)) {
            {
                std::lock_guard<std::mutex> lock(m_importedMutex);
                if (m_nodeRequest == NodeRequest::None) m_nodeRequest = NodeRequest::ForgetStaleImports;
            }
            update();
        }
        if (backOff) {
            const int delay = doubledWait(kRefusedRetryMs, m_descriptorRefusals++, kRefusedRetryMaxMs);
            if (!g_descriptorRefusalReported.exchange(true)) {
                const std::string why = sanitizeForLog(reply.errorMessage().toStdString());
                std::fprintf(stderr,
                             "wallpaperx: the daemon is refusing this process frames, asking again after %d ms "
                             "and later each time: %s\n",
                             delay, why.c_str());
            }
            m_backoffTimer.start(delay);
            return;
        }
        if (queued) fetchFrame(*queued);
    };
    // Past this the item stops waiting: the call is answered as though the daemon could not be
    // reached, and the reply, if it ever comes, is only given back -- so the fetch it was is owed
    // again, and asked once that lifetime answers. Stopped by the reply, so an inactive timer is
    // one that fired.
    auto* patience = new QTimer(watcher);
    patience->setSingleShot(true);
    connect(patience, &QTimer::timeout, this, [this, answered, generation, requestedMonitor, daemon = asked.lifetime] {
        m_unansweredLifetime = daemon;
        if (requestedMonitor == m_monitorId) queueFetch(generation);
        answered(QDBusMessage::createError(QDBusError::NoReply, QStringLiteral("WallpaperX did not answer in time")));
    });
    patience->start(kFetchPatienceMs);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, answered, patience, requestedMonitor, daemon = asked.lifetime](QDBusPendingCallWatcher* done) {
                done->deleteLater();
                if (!patience->isActive()) {
                    releaseFrameReply(requestedMonitor, done->reply(), daemon);
                    if (m_unansweredLifetime != daemon) return;
                    m_unansweredLifetime = 0;
                    // This call's own fetch, and what was asked for while it held the daemon up.
                    if (m_fetch == Fetch::Idle) {
                        if (const std::optional<quint64> queued = std::exchange(m_queuedFetch, std::nullopt)) {
                            fetchFrame(*queued);
                        }
                    }
                    return;
                }
                patience->stop();
                answered(done->reply());
            });
    // The connections above are gone with the item, so a reply that arrives after it is given back here.
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
            [item = QPointer<WallpaperXItem>(this), requestedMonitor,
             daemon = asked.lifetime](QDBusPendingCallWatcher* done) {
                if (item) return;
                done->deleteLater();
                releaseFrameReply(requestedMonitor, done->reply(), daemon);
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

// One reader of the buffer id and hand-over fields, so the callers cannot disagree about them.
BufferClaims::Claim WallpaperXItem::takeGranted(int monitorId, const QDBusMessage& call, quint64 daemon) {
    if (call.type() != QDBusMessage::ReplyMessage) return {};
    const QList<QVariant> args = call.arguments();
    if (!replyIsReadable(static_cast<int>(args.size()))) return {};
    const quint64 bufferId = args.at(kBufferIdField).toULongLong();
    BufferClaims::Taken taken =
        BufferClaims::instance().take(daemon, monitorId, bufferId, args.at(kHandoverField).toULongLong());
    if (taken.superseded) sendReleaseBuffer(taken.superseded->owner, monitorId, bufferId, taken.superseded->handover);
    return taken.claim;
}

// A reply this item can no longer use -- its monitor changed, or the item went, while the fetch was
// out. The daemon recorded the hold when it answered, so the claim is taken and handed straight
// back; dropping the message instead pins that image until this process leaves the bus. Reports
// nothing: the reason it would publish describes a screen this item has stopped showing.
void WallpaperXItem::releaseFrameReply(int monitorId, const QDBusMessage& call, quint64 daemon) {
    releaseClaim(takeGranted(monitorId, call, daemon));
}

void WallpaperXItem::takeFrameReply(int monitorId, const QDBusMessage& call, const BufferClaims::Daemon& from,
                                    bool askedForValue, const std::vector<quint64>& declared) {
    if (call.type() != QDBusMessage::ReplyMessage) {
        // A replaced daemon's error says nothing about the one now asked, and the change of daemon
        // has already set its own reason and asked the new one.
        if (from.lifetime != BufferClaims::instance().currentDaemon().lifetime) return;
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
        // Nothing is armed here: fetchFrame()'s latch stays shut for the backoff and retries the
        // refused fetch when it opens. What is on screen stays.
        if (name == QLatin1String(kErrorTooManyDescriptors)) {
            armPaintWatch();
            setFetchReason(m_frameShown ? QString() : tr("WallpaperX has refused this desktop more frames for now."));
            return;
        }
        // Not a failure of this item: no frame yet is the ordinary state before the user has
        // chosen a wallpaper, and the daemon says which case it is.
        setFetchReason(call.errorMessage().isEmpty() ? tr("WallpaperX has no frame for this screen yet.")
                                                           : call.errorMessage());
        return;
    }

    const QList<QVariant> args = call.arguments();
    // kFrameReplyFields fields, in the order GetFrame declares them. Checked rather than assumed: a
    // daemon from a newer build could answer with more, and reading past the end of a short reply
    // is the kind of crash that takes plasmashell with it.
    if (!replyIsReadable(static_cast<int>(args.size()))) {
        setFetchReason(tr("WallpaperX answered with something this plugin cannot read."));
        return;
    }

    PendingFrame frame;
    // Field 11, buffer_id: which image, where frame_generation says which render. Read and
    // claimed BEFORE the rest of the parse, because the daemon recorded the hold when it answered
    // -- so from here on every path that drops this frame owes it a release, and `frame` owning
    // the claim is what makes that automatic.
    frame.claim = takeGranted(monitorId, call, from.lifetime);
    frame.request.bufferId = frame.claim.bufferId;
    frame.sender = from.owner;
    // Registered before anything else can fail, for the same reason the claim is taken first: from
    // here the set is what owes this image back, whichever path drops it.
    frame.claims = m_claims;
    m_claims->add(frame.claim);

    // Left the screen while the request was out. The hold the daemon recorded is given straight
    // back by `frame` going out of scope, since nothing else ends it.
    if (!m_onScreen) return;

    // Field 0, memory_fd: the memory, or nothing when the request named this image as one the node
    // can draw. Read to its end like sync_fds below, or the message stays half-parsed.
    const QDBusArgument memoryFds = args.at(0).value<QDBusArgument>();
    int memoryFdCount = 0;
    bool memoryTaken = true;
    memoryFds.beginArray();
    while (!memoryFds.atEnd()) {
        QDBusUnixFileDescriptor announced;
        memoryFds >> announced;
        if (++memoryFdCount > 1) continue;
        frame.request.fd = ::dup(announced.fileDescriptor());
        memoryTaken = frame.request.fd >= 0;
    }
    memoryFds.endArray();
    // Memory left out for an image this request did not name, or two memories for one image: a
    // daemon not keeping the protocol. Not asked again -- that would ask the same daemon the same
    // question -- and the next announcement fetches as usual.
    if (memoryFdCount > 1 ||
        (memoryFdCount == 0 &&
         std::find(declared.begin(), declared.end(), frame.request.bufferId) == declared.end())) {
        static std::once_flag reported;
        std::call_once(reported, [memoryFdCount] {
            std::fprintf(stderr, "wallpaperx: frame not shown -- %s\n",
                         memoryFdCount > 1 ? "the daemon sent more than one memory descriptor for one image"
                                           : "the daemon left out the memory of an image this desktop did not "
                                             "say it holds");
        });
        setFetchReason(tr("WallpaperX answered with something this plugin cannot read."));
        return;
    }

    frame.request.width = args.at(1).toUInt();
    frame.request.height = args.at(2).toUInt();
    frame.request.memorySize = args.at(3).toULongLong();
    frame.request.handleType = args.at(4).toInt() == 1 ? FrameImport::HandleType::DmaBuf
                                                       : FrameImport::HandleType::OpaqueFd;
    // Not known for a replaced daemon's reply: it counts in a numbering the marks no longer hold, and
    // written back it would make the successor's frames read as older than the screen.
    const bool replaced = from.lifetime != BufferClaims::instance().currentDaemon().lifetime;
    frame.generation = replaced ? 0 : args.at(6).toULongLong();
    frame.request.drmFormat = args.at(7).toUInt();
    frame.request.drmModifier = args.at(8).toULongLong();

    // Field 5, sync_fds: what must be waited on before this frame is sampled. ALL of them, not the
    // first, so a daemon that sends more than one is waited on completely rather than sampled early.
    // Duplicated like the memory descriptor: the reply's own close with this message.
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

    // Field 12, acquire_value, the other form of the same hand-over: a value on this process's
    // acquire timeline. Kept only when sync_fds is empty -- one picture, one hand-over, and the
    // descriptors already in hand win over a value from a reply that sent both. Read only from a
    // reply to a request that asked for one: only its `sender` is known exact enough to pair a value
    // with.
    if (askedForValue && frame.request.syncFds.empty()) {
        frame.request.timelineValue = args.at(kAcquireValueField).toULongLong();
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

    if (!memoryTaken) {
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

    // What the next fetch may name as already imported, from the node as this pass leaves it. Only
    // what a picture could be drawn from again: a context that cannot wait drops the acquire, and
    // without one an import is never reused.
    const auto publishKnownImports = [this](const FrameNode* current) {
        std::vector<KnownImport> known;
        if (current != nullptr && FrameImport::supportsSync() && !g_semaphoresRefused.load()) {
            for (const ImportKey& key : current->heldKeys()) {
                if (key.identifies()) known.push_back(KnownImport{key.daemon, key.monitorId, key.bufferId});
            }
        }
        std::lock_guard<std::mutex> lock(m_importedMutex);
        m_knownImports = std::move(known);
    };

    // What the GUI thread has asked of the node, and the only place it can be done: freeing a
    // texture or an import needs the context this pass has.
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
    }
    // After this pass's picture, which can be a replaced daemon's last, drawn from one of these.
    const auto forgetStaleImports = [request](FrameNode* current) {
        if (request == NodeRequest::ForgetStaleImports && current != nullptr) current->forgetAllButDrawn();
    };

    // Never null here: this runs only in a sync of the item's own window, and Qt 6 documents
    // rendererInterface() as never null.
    QQuickWindow* host = window();

    // The scene graph can be running on Vulkan, Metal, D3D or software, and this import path is
    // OpenGL only. Checked every pass rather than once, because the window can change.
    if (host->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
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
            m_knownImports.clear();
        }
        // After the frame is gone, so the publish it posts reads both facts settled.
        setRenderReason(tr("This desktop is not running on OpenGL, which WallpaperX needs to show a frame."));
        return nullptr;
    }

    // Counted past the backend check, and only there: what the GUI thread is waiting on is a pass
    // this item could have presented in, and on a scene graph that cannot import at all every
    // forced pass would otherwise read as a desktop drawing the wallpaper it can never show.
    m_paintCount.fetch_add(1, std::memory_order_relaxed);

    // The epoch is read BEFORE the frame is claimed, not after. forgetGenerations() clears or
    // unnumbers m_pending and bumps the epoch under two separate locks, in that order, so an epoch read
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
        // on it is the one thing that tells the driver the memory holds something else now. A
        // picture arriving without one is described to the driver afresh.
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
        } else if (incoming->request.fd < 0) {
            stopped = Stopped::ImportLost;
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

        // What the next fetch may ask for. A picture that brought sync_fds is one this process could
        // have had a value for instead; asked once per daemon, and the pictures before the answer
        // lose nothing.
        const bool contextWaits = FrameImport::supportsTimeline() && !g_semaphoresRefused.load();
        std::string refusedBy;
        {
            std::lock_guard<std::mutex> lock(m_importedMutex);
            m_contextWaitsOnTimeline = contextWaits;
            refusedBy = m_timelineRefusedBy;
        }
        if (contextWaits && !incoming->request.syncFds.empty()) wantAcquireTimeline(incoming->sender);

        // A value asked for before this item gave up on that daemon's timeline. Not drawn, since
        // reading it unwaited in a context that can wait is undefined, and not counted as a failed
        // hand-over, since none was tried: the fetch that replaces it asks for sync_fds.
        if (stopped == Stopped::Nothing && incoming->request.timelineValue != 0 && refusedBy == incoming->sender) {
            stopped = Stopped::HandOver;
            failure = "this item no longer waits on that daemon's acquire timeline";
        }

        // The hand-over, in the form the daemon sent: a value, waited on through the share group's
        // import of that daemon's timeline, or sync_fds, each imported as a semaphore of its own. A refused
        // import draws this picture unwaited, since its memory imported fine.
        if (stopped == Stopped::Nothing) {
            bool handedOver = false;
            bool importRefused = false;
            if (incoming->request.timelineValue != 0) {
                handedOver = acquireOnGroupTimeline(incoming->sender, incoming->request.timelineValue, importedTexture,
                                                    m_semaphoreImportCount, failure, importRefused);
                if (!handedOver && !importRefused) {
                    // Any other failure to import or wait on that daemon's timeline, transient ones
                    // included: this item asks it for sync_fds from now on.
                    std::lock_guard<std::mutex> lock(m_importedMutex);
                    m_timelineRefusedBy = incoming->sender;
                }
            } else {
                const auto semaphores = std::count_if(incoming->request.syncFds.begin(), incoming->request.syncFds.end(),
                                                      [](int syncFd) { return syncFd >= 0; });
                m_semaphoreImportCount.fetch_add(static_cast<quint64>(semaphores), std::memory_order_relaxed);
                handedOver = FrameImport::acquire(incoming->request, importedTexture, &failure, &importRefused);
            }
            if (importRefused) {
                noteSemaphoresRefused(failure);
                handedOver = true;
            }
            if (!handedOver) {
                stopped = Stopped::HandOver;
                ++m_handOversFailed;
            }
        }

        // A picture that could not be handed over leaves the node drawing the last one that was,
        // imports and all: a frame on screen outranks a message about a fetch that failed. Bounded,
        // because that tolerance is for a blip; a hand-over that has stopped working would otherwise
        // freeze the wallpaper while the item went on reporting that it draws normally.
        const bool stillDrawing = stopped == Stopped::HandOver && m_handOversFailed <= kToleratedHandOverFailures &&
                                  node != nullptr && node->texture() != nullptr;

        if (stopped == Stopped::ImportLost) {
            // Not drawn, and not reported as a failure: the picture on the screen stays, and the claim
            // goes back with `incoming`. Asked for again on the item's thread, epoch-guarded for the
            // failure retry's reason below; the fetch that names nothing cannot be answered this way.
            m_lostImportCount.fetch_add(1, std::memory_order_relaxed);
            reportImportLost();
            const quint64 lost = incoming->generation;
            QMetaObject::invokeMethod(
                this,
                [this, epoch, lost] {
                    emit importCountsChanged();
                    {
                        std::lock_guard<std::mutex> lock(m_importedMutex);
                        if (m_generationEpoch != epoch) return;
                    }
                    // A newer reply has landed since, and it is drawn or retried on its own.
                    if (m_fetchedGeneration > lost) return;
                    m_declareNoImports = true;
                    m_fetchedGeneration = 0;
                    fetchFrame(0);
                },
                Qt::QueuedConnection);
        } else if (stopped != Stopped::Nothing) {
            reportImportFailure(failure);
            // Only a refusal to describe the memory is evidence about the layout. A hand-over that
            // could not be performed says nothing about it -- the wait is the same whichever
            // modifier the frame arrived in -- and withdrawing on one would take a layout this
            // driver demonstrably imports out of the daemon's hands for the session.
            if (stopped == Stopped::Layout && incoming->request.handleType == FrameImport::HandleType::DmaBuf) {
                withdrawImportModifier(incoming->request.drmFormat, incoming->request.drmModifier);
            }
            if (!stillDrawing) {
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
                // After the frame is gone, so the publish it posts reads both facts settled.
                setRenderReason(tr("WallpaperX produced a frame this desktop could not import."));
            }
            // Asked for once more. A still wallpaper sends no further FrameReady, so without this
            // a transient failure is permanent. The whole decision runs on the item's own thread,
            // marks included -- this branch is the render thread's.
            const quint64 failed = incoming->generation;
            QMetaObject::invokeMethod(
                this,
                [this, failed, epoch] {
                    // Before the guard: a failed hand-over still moved semaphoreImportCount.
                    emit importCountsChanged();
                    // Epoch-guarded like the mark above, and for the same reason: a monitor
                    // change or daemon restart can land between the post and its delivery, and
                    // recording a retry for a generation that has since been discarded burns the
                    // one retry a later, real failure would need.
                    {
                        std::lock_guard<std::mutex> lock(m_importedMutex);
                        if (m_generationEpoch != epoch) return;
                    }
                    if (m_retriedGeneration == failed || m_fetchedGeneration > failed) return;
                    m_retriedGeneration = failed;
                    m_fetchedGeneration = 0;
                    fetchFrame(0);
                },
                Qt::QueuedConnection);
        } else {
            // Both counted at the one event they describe, a picture drawn, so their sum is what this
            // item put on the screen and the split is how much of it needed an import of its own.
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
                m_renderReason.clear();
            }
            // What a drawn picture owes this item's own thread. The retry mark is cleared so "retried
            // once" means once per failure: the same generation can be re-fetched later and must get
            // its retry again. One hop for all of it, since each is a queued event per video frame.
            QMetaObject::invokeMethod(
                this,
                [this] {
                    m_retriedGeneration = 0;
                    emit importCountsChanged();
                    publishReason();
                },
                Qt::QueuedConnection);
        }
    }

    forgetStaleImports(node);
    publishKnownImports(node);
    if (node == nullptr) return nullptr;
    node->setRect(boundingRect());
    return node;
}

// A refused fetch with nothing on screen asks again on its own, once at a time: the announcement
// that would otherwise wake it is sent when a consumer releases an image, and a consumer that has
// stopped releasing sends none. Armed only while this item is on the screen showing nothing, so a
// desktop with a wallpaper on it never polls.
void WallpaperXItem::retryWhileNothingIsShown() {
    if (!m_onScreen || m_noImageRetryTimer.isActive()) return;
    m_noImageRetryTimer.start(doubledWait(kRefusedRetryMs, m_noImageRefusals++, kRefusedRetryMaxMs));
}

// The end of a TooManyDescriptors backoff: the refused fetch and what was announced meanwhile are
// fetched now. Nothing is queued only when the item is off the screen or has no monitor, where a
// fetch could not be asked anyway; whatever ends that state fetches on its own.
void WallpaperXItem::reopenFetchAfterBackoff() {
    if (m_fetch != Fetch::BackingOff) return;
    m_fetch = Fetch::Idle;
    if (const std::optional<quint64> queued = std::exchange(m_queuedFetch, std::nullopt)) fetchFrame(*queued);
}

void WallpaperXItem::endRefusalRuns() {
    m_noImageRefusals = 0;
    m_descriptorRefusals = 0;
}

void WallpaperXItem::abandonNoImageRun() {
    m_noImageRefusals = 0;
    m_noImageRetryTimer.stop();
}

// The fetch queued behind a back-off is dropped, not sent: every caller forces a fetch of its own
// once there is something to ask for.
void WallpaperXItem::abandonRefusals() {
    abandonNoImageRun();
    m_descriptorRefusals = 0;
    if (m_fetch == Fetch::BackingOff) {
        m_backoffTimer.stop();
        m_fetch = Fetch::Idle;
        m_queuedFetch.reset();
    }
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
        m_knownImports.clear();
        ++m_generationEpoch;
    }
    // Scoped above, because this takes the same non-recursive mutex.
    publishReason();
}

}  // namespace wallpaperx

#include "wallpaperx_item.moc"
