// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "config_store.h"
#include "config_writer.h"
#include "kept_rows.h"
#include "log_escape.h"
#include "vulkan/frame_export.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace wallpaperx {

class Device;
class OffscreenTargetManager;
struct OffscreenTarget;
class VideoPipeline;

// A request that was malformed rather than one that failed to render. Its own type so the
// DBus layer can answer with InvalidArgs instead of a generic failure.
struct InvalidRequest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A screen size reported under a connector name no monitor answers to. Its own type because the
// cure is elsewhere: the name has to be bound first.
struct UnknownOutput : InvalidRequest {
    using InvalidRequest::InvalidRequest;
};

// One consumer's claim on a monitor's images: the buffer ids it has been handed and has not
// given back, oldest first. A holder disappears when its last claim ends, so a monitor's list is
// exactly the consumers currently reading it.
struct BufferHold {
    std::string client;  // DBus unique name
    std::vector<uint64_t> buffers;
};

// What a monitor was last asked to display, and what came of it. `generation` is monotonic
// per monitor and never resets, so a consumer can tell a fresh frame from a repeat; `hasFrame`
// is separate because a failed render clears the flag without rewinding the counter.
struct MonitorStatus {
    std::string type;
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;
    bool hasFrame = false;

    // What the next render draws at: the size this monitor's screen reported, or the configured
    // default until one has.
    OutputSize targetSize;
    bool targetSizeReported = false;

    // Why the most recent request produced no frame, or empty if it produced one or is still
    // rendering. The only channel a failure has: SetWallpaper replies before the render runs.
    std::string lastError;

    // Whether that failure is the daemon's to answer for, as opposed to the image being
    // unreadable. Only the daemon's own failures make /health report a broken daemon.
    bool lastErrorIsDaemonFault = false;

    // A render has been accepted and no pass has run for it yet -- "the daemon owes an answer",
    // as against "this monitor has no picture", which is also true of a superseded render.
    bool renderOutstanding = false;

    // Renders refused since the last committed frame because every image on this monitor was
    // in a consumer's hands. Published because a producer quietly drawing less often than it
    // was asked to is otherwise indistinguishable from one that is keeping up.
    uint32_t deferredRenders = 0;

    // Hand-overs refused since the last one this monitor served. The published sign that a
    // consumer has stopped giving images back: the daemon then renders and commits as normal
    // while no consumer can fetch, which reads from every other field like a desktop that is
    // keeping up.
    uint32_t refusedHandovers = 0;

    // Who is reading this monitor's images. Published because a count of refusals says a
    // hand-over was declined but not who is holding what it wanted, and an image is only ever
    // given back by its holder -- so this is the whole diagnosis of a frozen screen.
    std::vector<BufferHold> holds;

    // What this monitor's screen last reported about being drawn, and whether anything is
    // collecting its pictures. A video advances only while `watched`, so these two are the
    // answer to "why has this wallpaper stopped moving".
    bool screenVisible = true;
    bool watched = false;
};

// True once a config read has been abandoned mid-call. That thread is still blocked in the
// kernel and may wake during static destruction, into library code being torn down, so main()
// ends the process rather than unwind past it.
bool configLoadAbandoned();

// The GPU the daemon resolved, once it has resolved one.
struct GpuStatus {
    bool initialized = false;
    std::string name;
    std::string descriptorTier;

    // Whether GetFrameFd's sync_fds carries a descriptor the consumer must wait on before it
    // samples the frame. The one thing a consumer cannot work out for itself: it sees an empty
    // array either because this daemon cannot produce one or because it chose not to, and the two
    // call for different reports to the user.
    bool frameSync = false;
};

// Pending is a real answer: nothing has been tried yet. Since restoreSaved() renders at
// startup, for shaders and gpu that describes a daemon with no saved config.
enum class HealthStatus { Pending, Ok, Fail };

const char* toString(HealthStatus status);

struct HealthCheck {
    HealthStatus status = HealthStatus::Pending;
    // Sanitized at the recording site: it can embed $WALLPAPERX_SHADER_DIR and reaches JSON.
    std::string detail;
};

// Recorded as the daemon tries its dependencies, never probed on demand -- stat()ing a path
// inside the health handler would let a hung mount wedge the endpoint.
struct DaemonHealth {
    HealthCheck shaders;
    HealthCheck gpu;

    // Deliberately absent from /health's checks: a bind failure means /health is answered by
    // whatever took the port, so there it could only ever read ok. GetStatus carries it,
    // because the bus name cannot be taken from a running daemon.
    HealthCheck configServer;

    // Whether the wallpaper choice will survive a restart. A failed save leaves SetWallpaper
    // succeeding, so this is the only channel that says the choice will be lost.
    HealthCheck persistence;
};

// One hand-over: the memory, the descriptor the consumer waits on before reading it, and which
// render it is. The two descriptors are produced together and are the caller's to close (or to
// hand to something that will).
//
// Paired here rather than inside ExportedFrame because only one of them is a property of the
// image: exportFrame() reads a target and submits nothing, while the acquire descriptor is a
// binary semaphore signalled per hand-over, since the first wait consumes it. Building both in one
// place is what stops a consumer being given memory with no way to acquire it.
struct FrameHandle {
    ExportedFrame frame;
    uint64_t generation = 0;

    // -1 when the GPU cannot export one, which leaves the consumer reading without the
    // queue-family acquire -- undefined, and what every consumer did before this existed.
    int acquireFd = -1;

    // Handed to a consumer this monitor's playing video has served before: one of many a second for
    // as long as it keeps reading. False for a consumer the session has not served, which is the
    // event worth recording.
    bool routine = false;
};

// What a type is drawn with. The single answer to "can this build draw this type" too: a
// renderable type is exactly one that names a drawer, so the render dispatch, the GetFrameFd
// reason and the GetStatus field cannot drift apart.
enum class Drawable { Still, Clip };

inline std::optional<Drawable> drawableType(const std::string& type) {
    if (type == "static") return Drawable::Still;
    if (type == "video") return Drawable::Clip;
    return std::nullopt;
}

inline bool isRenderableType(const std::string& type) { return drawableType(type).has_value(); }

// The path a client may act on: the bytes as stored, or nothing when they are not valid
// UTF-8 and so cannot be carried by JSON at all. Published beside the escaped `path`, which
// stays display-safe.
inline std::optional<std::string> literalPath(const std::string& path) {
    if (!isValidUtf8(path)) return std::nullopt;
    return path;
}

enum class NoFrameReason {
    NeverRequested,  // nothing has ever been set on this monitor
    NotRendered,     // a request is recorded but no frame has committed -- still queued
                     // or still rendering. Temporary by definition: retrying is correct.
    RenderFailed,    // the last render was attempted and failed; lastError says why
    Unsupported,     // the recorded type (shader) cannot be rendered by this build,
                     // so no amount of waiting or retrying will produce a frame
    NoFreeImage,     // there IS a frame, but no image may be handed over: it would leave the
                     // producer nowhere to draw, or this consumer already holds its fill.
                     // Temporary: a release frees the ring, and every release is announced with
                     // FrameReady.
};

// The result of asking for a monitor's current frame: the frame, or the reason it was not
// handed over.
struct FrameExport {
    std::optional<FrameHandle> handle;

    // Both meaningful only when `handle` is empty. lastError is non-empty exactly when
    // reason is RenderFailed -- Unsupported deliberately does not carry one, because it
    // is not a failure and the reason name already states the whole fact.
    NoFrameReason reason = NoFrameReason::NeverRequested;
    std::string lastError;

    // Whether this refusal is worth a log line: the first for THIS consumer, or the first in a
    // while. Per consumer, not per monitor, because a second consumer being refused is its own
    // diagnosis. Rate-limited rather than armed by a hand-over, because a consumer holding its fill
    // is served and refused alternately while a video plays. Meaningful only for NoFreeImage.
    bool firstRefusal = false;
};

// What renderPending() did, so the caller knows whether to announce a new frame.
struct RenderOutcome {
    bool rendered = false;
    uint64_t generation = 0;
};

// A frame that is already committed and should be announced again, because a claim on one of
// its monitor's images has just ended. A consumer refused for want of a free image has no other
// trigger to ask again: nothing new will be rendered for a monitor whose request is already
// serviced, so without this the refusal would stand until the user set another wallpaper.
struct FrameAvailable {
    int monitorId = 0;
    uint64_t generation = 0;
};

// What one round of the video clock did: the pictures it committed, each to be announced like a
// render's frame, the monitors owed a render again -- playback failed, or the session their
// pictures need was freed while nobody was looking -- and when it next has work. `nextDue` covers
// a session waiting to be freed as well as a picture waiting to be shown; no `nextDue` means
// neither is pending, and only a render or a wake can start one.
struct ClockRound {
    struct Shown {
        int monitorId = 0;
        uint64_t generation = 0;
    };
    std::vector<Shown> shown;

    // A failure the clock recorded leaves the monitor owing a render, since it is the only thing
    // that can open the file again. The caller posts these.
    std::vector<int> retry;

    std::optional<std::chrono::steady_clock::time_point> nextDue;
};

// What a report or a withdrawal did to the layouts in force.
//
// `changed` is not "toRender is non-empty": a change that condemns no monitor still has to be
// announced, because whether some other consumer's refused report would now be accepted depends
// on the set, not on whether anything needed redrawing.
struct ImportModifierUpdate {
    bool changed = false;
    std::vector<int> toRender;
};

// Owns the renderer for the daemon: one Vulkan Device, one OffscreenTargetManager keyed by
// monitor, and the per-monitor bookkeeping behind SetWallpaper/GetFrameFd. Request and render
// are separate phases because rendering is slow and the DBus event loop is single-threaded.
//
// Two locks, strictly ordered: m_gpuMutex before m_stateMutex, never the reverse.
//
//   m_gpuMutex   the Device, the offscreen targets, and all GPU submission; held for a whole
//                render. Also what makes export safe: it reads the Device's live-allocation
//                map and a target that a render's create/destroy can replace underneath it.
//   m_stateMutex the per-monitor bookkeeping only -- a map lookup or a string copy -- so
//                GetStatus never waits on a render.
class WallpaperManager {
public:
    // Bounds worst-case GPU memory: each successful render retains a ~9 MB exportable image
    // until the daemon exits, so an unbounded id space is an unbounded allocation any client
    // on the session bus can drive. Finite, not tight.
    static constexpr int kMaxMonitors = 32;

    // Bounds the import-modifier list a client may report. Mesa publishes tens per format, so
    // this is far above any real driver and exists only to keep a client from sizing daemon
    // state.
    static constexpr size_t kMaxImportModifiers = 512;

    // How many of a monitor's images one consumer may hold at once. Two, because a consumer
    // that has fetched a new frame goes on sampling the old one until the new one is on screen.
    //
    // A third hand-over is REFUSED, not made room for: the daemon has no evidence the consumer
    // has stopped reading its oldest image, and drawing into one it is still sampling is the
    // tearing this ring exists to prevent. What the cap buys is the depth below -- without a
    // per-consumer bound, no depth guarantees a second consumer anything.
    static constexpr size_t kMaxHeldPerClient = 2;

    // Consumers a monitor is sized for. Two: the desktop, and one auxiliary reader -- a
    // screenshot tool, a settings preview, a debugging fetch over busctl.
    static constexpr size_t kFullConsumers = 2;

    // Images per monitor: the producer's reserve, plus a full hold for each consumer sized for.
    // Nothing is ever taken back, so a consumer that fetches its fill and goes quiet keeps those
    // images for the life of the daemon; at depth three that one consumer left the desktop
    // refused forever, with no release coming to announce that it may ask again. A third silent
    // consumer cannot be survived at any depth for the same reason, which is where the
    // arithmetic stops paying: each one costs ~8 MB per monitor at 1080p.
    static constexpr uint32_t kTargetDepth = 1 + static_cast<uint32_t>(kFullConsumers * kMaxHeldPerClient);

    // How many consumers may hold images on one monitor. Bounds what peers can make the daemon
    // remember; a further one is refused like any other hand-over. Unlike the reserve it can
    // refuse while images are free -- four consumers sharing one image leave four unheld -- which
    // is the price of not evicting a holder to make room for the fifth.
    static constexpr size_t kMaxHoldersPerMonitor = 4;

    // Video wallpapers holding an open decode session at once: a ceiling on the video memory
    // decoders hold, not a hardware session limit. Monitor ids are the client's to choose and each
    // session holds a decoder and its picture pool sized by the stream, so kMaxMonitors is not a
    // usable bound.
    //
    // A ceiling, not an allowance: what the card can actually hold is decided by trying, since a
    // decoder's pool is ffmpeg's to size and no query predicts it. Room for a new session is made
    // by closing one that is not playing -- which includes one whose file held a single picture,
    // since it will never show another -- both when this cap is reached and when an open runs out
    // of memory. A playing video is never taken back, so a screen never freezes for another's sake,
    // and with nothing free to close the new wallpaper is refused and says so.
    static constexpr size_t kMaxVideoSessions = 8;

    // How long a published picture may sit uncollected before the monitor stops counting as
    // watched. Measured from the announcement, never from wall time: a consumer can only collect
    // what has been announced, so "time since the last hand-over" would be the file's own frame
    // interval and a video with pictures seconds apart would call its own consumer gone.
    //
    // Two seconds is far past any scheduling hiccup on the consumer's side and short enough that a
    // desktop that has gone stops the decoder almost at once. A monitor wrongly called unwatched
    // costs one paused picture: the next hand-over starts it again.
    static constexpr std::chrono::seconds kUnwatchedAfter{2};

    // How long a decode session stays open after its monitor stops playing. Stopping already ends
    // the cost of playing, so what this trades is the video memory a decoder and its picture pool
    // hold against the cost of coming back, which is not only a file open, a stream probe and one
    // decode: a reopened file starts at its first picture, so a video that resumes after this plays
    // from the beginning rather than carrying on. The last picture stays on screen throughout.
    //
    // Five seconds is past every transient -- a menu, a notification, a window animation, a screen
    // redrawn at a new size -- and far below the absences whose memory is worth having back: a
    // locked screen, a fullscreen window, a session switched away.
    static constexpr std::chrono::seconds kIdleSessionRelease{5};

    // How often one consumer's refusal on one monitor is worth a line. A consumer that holds its
    // fill of a playing video is refused for every picture it does not release in time, so this is
    // the difference between one line and sixty a second. It is also how long a consumer must have
    // been quiet before another may take its slot in reportConsumer().
    static constexpr std::chrono::seconds kRefusalLogInterval{60};

    // Times the clock reopens one wallpaper after playback fails without a picture having been
    // shown in between. Three, because one retry can be lost to a fault that was genuinely
    // momentary, while three failures with nothing shown between them is a file that cannot get
    // past that point -- and each try costs a decoder open, so spending more buys no better
    // evidence. A file that fails on its second picture therefore settles in about a tenth of a
    // second instead of reopening tens of times a second for ever. Every picture shown restores
    // the budget, so a video that hiccups once an hour keeps recovering.
    static constexpr uint32_t kPlaybackRetries = 3;

    struct Config {
        // What a monitor is drawn at until its screen reports a size through setOutputSize().
        // Tests override it to keep software-rasterised runs fast.
        uint32_t width = 1920;
        uint32_t height = 1080;

        // Off by default: the validation layers cost real time and are not guaranteed
        // installed on a user's machine. Tests turn them on as their correctness oracle.
        bool enableValidation = false;

        // Where the compiled SPIR-V lives. Empty means "resolve at first use" --
        // $WALLPAPERX_SHADER_DIR if set, else the build-time default.
        std::string shaderDir;

        // Where wallpaper choices are persisted; empty disables persistence. Empty is the
        // default and production opts in via defaultConfigPath(), so a test that says nothing
        // about config cannot read or overwrite the developer's own.
        std::filesystem::path configFile;

        // Where the record of the wallpaper being applied lives, so a start after a crash
        // can refuse the one the last run did not survive. Empty disables that refusal, and
        // is the default for the same reason configFile's is: production opts in via
        // defaultStateDir(), so a test that says nothing about state cannot leave a record
        // in the developer's own directory for the real daemon's next start to honour.
        std::filesystem::path stateDir;
    };

    explicit WallpaperManager(Config config);
    ~WallpaperManager();

    WallpaperManager(const WallpaperManager&) = delete;
    WallpaperManager& operator=(const WallpaperManager&) = delete;

    // Records a request; GPU-free. Throws InvalidRequest for a malformed one, and everything
    // that can only fail on the GPU fails later in renderPending(). The caller must then post
    // the monitor to the render worker, types this build cannot draw included -- those still
    // need their target released.
    void requestWallpaper(const std::string& type, const std::string& path, int monitorId);

    // Records the connector name a monitor is known by -- "DP-3". Throws InvalidRequest for a
    // bad id or a name outputNameError() rejects, and std::runtime_error when the name did not
    // reach the config file: the binding stands for this run either way, and only the write
    // makes it outlive a restart. Idempotent; a call that changes nothing already on disk does
    // not write, since the write parks the calling thread.
    //
    // A name carries its screen's reported size, so binding one can change what two monitors are
    // drawn at. Those are recorded as owed a render and put in `redraw` before anything can throw;
    // the caller posts them on success and on a failed write alike, since the binding stands.
    void setMonitorOutput(int monitorId, const std::string& outputName, std::vector<int>& redraw);

    // Records the size the screen named `outputName` is drawn at. The monitor whose drawing size
    // that changes is recorded as owed a render and put in `redraw` before anything can throw; the
    // caller posts it on success and failure alike, since the size stands either way. Throws
    // InvalidRequest for a name or size the shared validators reject, and UnknownOutput for a name
    // no monitor answers to. Idempotent. The write is queued, not waited on -- a size lost to a
    // failed write costs one redraw after a restart, and a stalled disk must not delay this one.
    void setOutputSize(const std::string& outputName, uint32_t width, uint32_t height, std::vector<int>& redraw);

    // Records whether the screen named `outputName` is actually being drawn -- its desktop is not
    // behind a lock screen, a fullscreen window or another session. The daemon can see none of
    // that for itself, so a screen that never reports is taken to be drawn and behaves as it
    // always has. A video on a screen that is not drawn stops on the next round and keeps its last
    // picture; nothing a consumer does starts it again while that stands, so the caller wakes the
    // render worker whatever the answer. Throws InvalidRequest for a name the shared validator
    // rejects and UnknownOutput for one no monitor answers to. Idempotent: only the change from
    // covered to drawn credits the monitor with having been seen.
    void setOutputVisible(const std::string& outputName, bool visible);

    // Records what the consumer named by `client` says it can import, and returns the monitors
    // to re-post so their images are rebuilt against it. GPU-free, like requestWallpaper: the
    // set is applied on the render thread at the next render.
    //
    // The daemon cannot answer this for itself -- Vulkan's importable bit describes Vulkan, and
    // the importer is EGL in another process -- so the consumer reports and the exporter
    // intersects. Frames already exported keep their layout until re-rendered, which is why
    // this returns work rather than doing it.
    //
    // `client` is the caller's DBus unique name. Several consumers may report at once and every
    // one of them has to be able to read the frame, so what takes effect is the intersection.
    // The name is also what forgetImportModifiers() later withdraws: a set that outlived its
    // reporter would narrow the exporter on behalf of a process that no longer exists.
    //
    // Throws InvalidRequest for a format this daemon does not export, an implausibly long list,
    // or a set that leaves no layout every reporting consumer can read. Idempotent: an
    // unchanged intersection reports no change and no monitors.
    ImportModifierUpdate setImportModifiers(const std::string& client, uint32_t drmFormat,
                                           const std::vector<uint64_t>& modifiers);

    // Drops what `client` reported, because it left the bus. Widening the intersection cannot
    // invalidate an image, but it does un-block a monitor the departed consumer's set left with
    // no frame at all -- and nothing else will ever retry it.
    ImportModifierUpdate forgetImportModifiers(const std::string& client);

    // Applies the config file's wallpapers, returning the monitor ids to post to the worker.
    // Call once. Writes nothing: a repair it computed is held for writeRepairedConfig().
    std::vector<int> restoreSaved();

    // Moves an unreadable config aside, or copies one whose rows this run rejected or is about
    // to rewrite, and records the outcome in the persistence health. Separate from the
    // constructor because it writes, and a duplicate instance must not touch the running
    // daemon's config: call once, after the bus name is held.
    void preserveUnreadableConfig();

    // Queues the repair restoreSaved() computed. Does not wait: the caller holds the bus name
    // and has no event loop yet, and the writer reports a failed write on stderr itself. Call
    // once, after preserveUnreadableConfig(), which copies the bytes this rewrite replaces.
    void writeRepairedConfig();

    // Renders the monitor's current request, on the render worker's thread and never the DBus
    // one. rendered=true only when the frame is still the one that was asked for: a request
    // arriving mid-render supersedes this one, and committing anyway would flag the monitor as
    // holding pixels of the previous wallpaper. Failures land in lastError; nobody is waiting.
    RenderOutcome renderPending(int monitorId);

    // Moves every playing video on by at most one picture, on the render worker's thread: decodes
    // the next picture of one whose picture is on screen, and shows one whose next picture is due.
    // A video plays only while its monitor is settled -- showing a frame of this wallpaper with no
    // render owed -- so a request or a resize stops it until that render has run. Never touches
    // requestSeq: a picture is not a render anyone asked for. Failures land in lastError, as a
    // render's do, and stop that video.
    ClockRound advanceVideo(std::chrono::steady_clock::time_point now);

    // Exports the monitor's most recent frame as a fresh fd. An empty handle means nothing was
    // handed over, with `reason` saying which state that is; a thrown exception means the
    // export itself went wrong.
    //
    // A successful export records `client` -- its DBus unique name -- as holding that image,
    // and no render draws into a held image. The claim ends at ReleaseBuffer or when the client
    // leaves the bus, and nowhere else.
    //
    // Every bound on the holds is a refusal, reported as NoFreeImage: the hand-over that would
    // take the producer's last free image, the one past kMaxHeldPerClient, the one from a
    // consumer past kMaxHoldersPerMonitor. Nothing may be taken back from a consumer that could
    // still be sampling it, so declining the request is the only bound that does not risk
    // drawing into an image being read.
    //
    // The default is for a caller that is not a bus peer, and it records no claim: there would
    // be no name to withdraw it under, and a claim nothing can end takes an image out of the
    // ring for the life of the daemon. A frame exported that way can be redrawn under its
    // reader.
    FrameExport exportCurrentFrame(int monitorId, const std::string& client = {});

    // Drops `client`'s claim on one image, because it says it has stopped reading it. Returns
    // the monitors whose deferred render should be retried now that an image may be free.
    // Silent about a claim it does not hold: a consumer releasing twice, or releasing one
    // already dropped for it, is not making an error. Throws InvalidRequest for a bad id.
    //
    // `announce`, when given, receives the monitors holding a frame a consumer can still read --
    // the freed image has to be published, or a consumer refused for want of one never learns it
    // may ask again. A monitor can be in both: a redraw at a new size is owed while the frame it
    // replaces is still served.
    std::vector<int> releaseBuffer(const std::string& client, int monitorId, uint64_t bufferId,
                                   std::vector<FrameAvailable>* announce = nullptr);

    // The same for every image on every monitor, because `client` left the bus. Without it a
    // consumer that crashed holding a frame would keep that image out of the ring forever --
    // and plasmashell restarts are routine, while this daemon does not.
    std::vector<int> forgetHeldBuffers(const std::string& client, std::vector<FrameAvailable>* announce = nullptr);

    // One consistent snapshot of everything GetStatus reports. Deliberately not two accessors:
    // separate lock acquisitions let a render commit in between, producing a reply that claims
    // no GPU beside a monitor that already has a frame.
    struct Snapshot {
        GpuStatus gpu;
        DaemonHealth health;
        std::map<int, MonitorStatus> monitors;

        // Connector name -> monitor id. Its own field: a named monitor need not have a
        // wallpaper.
        std::map<std::string, int> outputs;
    };
    Snapshot snapshot() const;

    // Private with a single friend rather than public: a claim any caller could revoke would
    // let a second server stamp its own bind failure over one that is serving.
private:
    friend class HttpServer;

    // Called from HttpServer's constructor with the bind result, from its serving thread
    // if the accept loop later dies, and from its registration on the way out.
    void recordConfigServer(bool listening, const std::string& error);

    // Retracts a live record on the way out, and only that. A bind failure or a dead accept
    // loop already named its own cause, and that diagnosis is the only place a port conflict
    // is reported at all.
    void recordConfigServerShutdown(const std::string& detail);

    bool tryAttachConfigServer();
    void detachConfigServer();

public:
    // The renderer's validation-warning counter, or null before a device exists. Shared so it
    // outlives this manager and its Device, which is when the layer emits its teardown
    // complaints. Not surfaced by GetStatus: both take m_gpuMutex, which a render holds for its
    // whole duration, so status would wait on an image decode.
    std::shared_ptr<const std::atomic<int>> validationCounter() const;
    bool validationLayerActive() const;

private:
    // What withHandover() decided. One value with three states rather than a flag beside a
    // maybe-filled list: `holds` is meaningless unless the hand-over was allowed, and only
    // Granted may be undone -- an image the client already held is one it is still reading.
    struct Handover {
        enum class Outcome { Granted, AlreadyHeld, Refused };

        std::vector<BufferHold> holds;  // the claims that would be in force; empty if Refused
        Outcome outcome = Outcome::Refused;
    };

    // One consumer of one monitor, as both the refusal log and the hand-over log remember it. Two
    // times, because a slot is asked two different things: whether its holder is still there, and
    // whether a line about it is due again.
    struct ConsumerReport {
        std::string client;
        std::chrono::steady_clock::time_point heard;
        std::chrono::steady_clock::time_point logged;
    };

    // Whether `client` is worth a line now, recording it if so. At most kMaxHoldersPerMonitor
    // consumers are remembered, so peer-chosen bus names cannot size the list, and one that cannot
    // be given a slot is reported not at all -- which is what bounds the log when more consumers
    // than slots take turns on one monitor. Every call keeps the caller's slot, and a slot goes to
    // a newcomer only once its holder has been silent for kRefusalLogInterval or has left the bus.
    // The cost is deliberate: while four consumers keep reading one monitor, a fifth is never
    // named, which is one missing line rather than a stream of false ones.
    //
    // Periodic reports a consumer that already holds a slot again once per interval: a refusal
    // still happening is still worth saying. Once reports it only when it takes a slot, since
    // starting to read is a single event. The caller's lock is whichever one guards `reports`.
    enum class Repeat { Periodic, Once };
    static bool reportConsumer(std::vector<ConsumerReport>& reports, const std::string& client, Repeat repeat,
                               std::chrono::steady_clock::time_point now);

    struct MonitorState {
        std::string type;
        std::string path;

        // Counts accepted requests, where generation counts completed renders: the worker
        // snapshots requestSeq before rendering and refuses to commit if it has moved.
        uint64_t requestSeq = 0;

        // The requestSeq already rendered for, successfully or not: what makes renderPending()
        // idempotent per request. The worker's queue can legitimately hold a monitor twice, and
        // a second pass would burn a generation and re-announce a frame already imported.
        uint64_t servicedSeq = 0;

        // Counts the times this monitor was given a wallpaper, where requestSeq counts renders
        // owed: a resize or an import-modifier report bumps that one and leaves this alone. What
        // a retained decode session belongs to, since it may survive a re-post but never a new
        // wallpaper -- and comparing paths instead would call a file rewritten in place the same
        // wallpaper, leaving a decoder open on bytes that are gone.
        uint64_t wallpaperSeq = 0;

        uint64_t generation = 0;
        bool hasFrame = false;
        std::string lastError;
        bool lastErrorIsDaemonFault = false;

        // The saved request was not applied because the last run died applying it, and no pass
        // may decode it -- a re-post for any other reason must not either. Set only by
        // restoreSaved(), cleared only by requestWallpaper(): a client asking for this
        // wallpaper again is a different statement from a config being restored.
        bool quarantined = false;

        // Copied from the target at commit time, so status() can report the frame's size
        // without touching GPU state and therefore without waiting behind a render.
        uint32_t width = 0;
        uint32_t height = 0;

        // The memory layout the committed frame was built with, copied at commit time for the
        // same reason as the size: a bus handler must decide whether a new import-modifier
        // report invalidates this frame without taking the GPU lock. kDrmModifierInvalid means
        // the frame was exported as an opaque fd, which carries no layout for a report to
        // exclude. Meaningless unless hasFrame.
        uint64_t frameModifier = kDrmModifierInvalid;

        // Who is reading which of this monitor's images. The one authority on it: which images
        // a render may draw into is derived from this list rather than recorded a second time.
        // Emptied whenever the ring is rebuilt, since the ids then name images that are gone.
        std::vector<BufferHold> holds;

        // Renders refused for want of a free image since the last committed frame. Reset by a
        // commit, so it reads as how far behind this monitor is now rather than as a total.
        // Reads zero for as long as the reserve holds -- a non-zero count means a hand-over got
        // past exportCurrentFrame()'s refusal, so it is a report on that invariant and not on
        // the consumers. What a consumer holding images produces is refusedHandovers below.
        uint32_t deferredRenders = 0;

        // Hand-overs refused since the last one served. Reset by a hand-over rather than by a
        // commit, because what it reports on is the consumers and not the renders: a monitor
        // that keeps committing while this climbs is one whose images are not coming back.
        uint32_t refusedHandovers = 0;

        // When each consumer's refusal was last reported. Kept beside the count rather than derived
        // from it: the count is one number for the monitor, and which consumer was refused is what
        // the report is about.
        std::vector<ConsumerReport> refusalsReported;

        // The consumers this monitor's current video session has handed a picture to, so the log
        // records a consumer starting to read rather than every picture it takes; a single name
        // would not do, since two readers would each find the other there. About the session, and
        // emptied when one opens. Kept here and not on the session so a consumer leaving the bus
        // can be forgotten under this lock alone: the bus thread must never wait on m_gpuMutex.
        std::vector<ConsumerReport> served;

        // When this monitor published a picture that nothing has collected since, or nothing while
        // its newest picture is in a consumer's hands. The evidence that something is looking, and
        // deliberately anchored to the OLDEST uncollected picture: "time since the last hand-over"
        // is the file's own frame interval, which the scheduler lets reach kMaxPictureInterval, so
        // a slow video would have declared its own consumer gone between two pictures.
        //
        // A refusal does not clear it: a consumer that has stopped giving images back asks at
        // display rate and is refused every time, so counting a refusal as collection would keep a
        // frozen screen decoding at display rate.
        std::optional<std::chrono::steady_clock::time_point> uncollectedSince;

        // What a screen reporting it is being drawn again buys. It has to buy something, or a
        // stopped video would wait for a hand-over that only a picture can prompt -- but the report
        // is a peer's word, so it is worth one picture and no more: it lapses as soon as this
        // monitor publishes one, and whether the consumer then takes that picture is what says
        // whether playback goes on. It takes effect at most once per kUnwatchedAfter, since the
        // report is free to send; without that, a client that collects nothing would keep a decoder
        // running at the file's full rate by saying hidden and then shown again.
        //
        // It carries no deadline of its own: only a picture can spend it, and a monitor waiting on
        // a GPU that has just failed or on a decode session slot may take longer than any deadline
        // to publish one -- after which no further report would come, the screen already being
        // drawn. What bounds it instead is resumeCreditStands(): it counts only while this monitor
        // holds a video that can still publish that picture.
        struct ResumeCredit {
            // When it comes due. Never earlier than the report that granted it, and later than that
            // when the report landed inside the interval since the last one.
            std::optional<std::chrono::steady_clock::time_point> at;
            // This monitor's generation when it was granted; any picture since has spent it.
            uint64_t generation = 0;
        };
        ResumeCredit resumeCredit;

        // The wallpaper whose file held a single picture: it is on screen and there is nothing
        // after it, so no clock advances it and no session is opened to play it. Keyed by
        // wallpaperSeq for playbackRetriesFor's reason, and held here rather than on the session
        // because the session is freed while the fact stays true.
        uint64_t parkedWallpaperSeq = 0;

        // Reopens the clock has spent on `playbackRetriesFor`, against kPlaybackRetries. Keyed by
        // the wallpaper rather than reset when one arrives: a count whose key names a wallpaper
        // this monitor no longer holds has nothing to say about the one it does, so no caller has
        // to remember to clear it.
        uint64_t playbackRetriesFor = 0;
        uint32_t playbackRetries = 0;
    };

    // Creates the Device and target manager on first use, so a machine with no working Vulkan
    // still starts and reports a clean error. Called with m_gpuMutex held.
    void ensureGpu();

    // Both called with m_gpuMutex held, and both drop the monitor's holds and frame when they
    // change which images exist: a claim on an image that has been freed names an id nothing
    // will ever hand out again.
    // ensureTargets() answers whether it replaced the images.
    bool ensureTargets(int monitorId, OutputSize size);
    void releaseTarget(int monitorId);

    // Whether this monitor already has images at `size`, which ensureTargets() then reuses.
    // Caller must hold m_gpuMutex.
    bool targetsFit(int monitorId, OutputSize size) const;

    // One attempt at renderPending()'s work. `superseded` means a newer request overtook it before
    // it committed. `rebuilt` is set when this attempt replaced the images. Caller must hold
    // m_gpuMutex.
    struct RenderPass {
        RenderOutcome outcome;
        bool superseded = false;
    };
    RenderPass renderPassLocked(int monitorId, bool& rebuilt);

    struct VideoSession;

    // Publishes the frame just drawn into `target`, returning its generation, or records why a
    // render or a picture produced none. Caller must hold m_stateMutex.
    uint64_t commitFrameLocked(MonitorState& state, const OffscreenTarget& target);
    static void recordFailureLocked(MonitorState& state, const std::string& error, bool daemonFault);

    // What a failed render or picture was: the message, and who answers for it. Built by
    // rethrowing, so every site sorts the same three kinds the same way rather than carrying its
    // own copy of the order. Call from inside a catch of std::exception.
    struct RenderFailure {
        std::string error;
        bool daemonFault = false;
        // The GPU itself, so the caller re-records GPU health -- under whichever lock it holds.
        bool gpuFault = false;
        // A host or device resource ran out: freeing something may make the same work possible.
        bool exhausted = false;
    };
    static RenderFailure classifyFailure();

    // The reopens this monitor's current wallpaper has spent, zeroed if the count was another
    // wallpaper's. Caller must hold m_stateMutex.
    static uint32_t& playbackBudgetLocked(MonitorState& state);

    // Records that the held picture was just drawn. One drawn later than its own interval -- a
    // stall, not jitter -- restarts the schedule from `now`, so a long stall is not followed by
    // pictures shown back to back.
    static void markShown(VideoSession& session, std::chrono::steady_clock::time_point now);

    // advanceVideo() for one session: when it next needs a round, or nothing while its monitor is
    // not playing. Appends to `round` what it showed and what it left owed. Caller must hold
    // m_gpuMutex.
    std::optional<std::chrono::steady_clock::time_point> advanceSessionLocked(
        int monitorId, std::chrono::steady_clock::time_point now, ClockRound& round);

    // The three entries to a monitor's retained decode session, all on the GPU thread with
    // m_gpuMutex held. A teardown re-enters ffmpeg and the driver, and writes no crash record for
    // it: the file has already opened, decoded and drawn by then, and a session is released
    // whenever a screen goes dark, so a record here would refuse a working wallpaper after a death
    // in a teardown its content did not cause. A caller that already holds one keeps it.
    void closeVideoSessionLocked(int monitorId);

    // Keeps the session only if a pass for this wallpaper would draw with it. Called before the
    // branches that draw or release a target, so no type and no failure leaves a decoder and its
    // picture pool open for a file nothing shows.
    void dropStaleVideoSessionLocked(int monitorId, std::optional<Drawable> kind, uint64_t wallpaperSeq);

    // The request whose committed frame a video of `wallpaperSeq` still shows, or nothing when it
    // shows something else: no frame, a render owed, another wallpaper since, or a file with one
    // picture. Caller must hold m_stateMutex.
    std::optional<uint64_t> currentSeqLocked(int monitorId, uint64_t wallpaperSeq) const;

    // The same, and watched: what the clock advances, what the idle release frees, and what decides
    // a session is worth reopening. Eviction deliberately asks the narrower pair instead --
    // currentSeqLocked() and screenDrawnLocked() -- because a consumer that has not fetched for a
    // moment must not cost its screen the decoder it is visibly playing. Caller must hold
    // m_stateMutex.
    std::optional<uint64_t> playingSeqLocked(int monitorId, uint64_t wallpaperSeq) const;

    // Whether anything can see this monitor: its screen is being drawn and nothing it published is
    // sitting uncollected. Reads the clock itself, so no caller can ask with a stale one. Caller
    // must hold m_stateMutex.
    bool watchedLocked(int monitorId, const MonitorState& state) const;

    // Whether this monitor's resume credit is unspent and could still be spent: it holds a frame of
    // a video that has more than one picture. Anything else -- a still, a one-picture file, a
    // failed render -- will never publish the picture that spends it, and would read as watched
    // for ever. Caller must hold m_stateMutex.
    static bool resumeCreditStands(const MonitorState& state);

    // Whether the screen bound to this monitor is being drawn, which is true of one that has never
    // said otherwise. Caller must hold m_stateMutex.
    bool screenDrawnLocked(int monitorId) const;

    // Records that a hand-over collected this monitor's newest picture, which is what makes it
    // watched. Takes m_stateMutex itself.
    void noteHandoverServed(int monitorId);

    // Posts the videos that would be playing if they had a decode session: one whose session was
    // freed while nobody was looking, and -- only when this round freed one, so a failure cannot
    // spin -- one that never got a session because every slot was taken. Caller must hold
    // m_gpuMutex and not m_stateMutex.
    void reopenVideosLocked(std::chrono::steady_clock::time_point now, bool freedSession, ClockRound& round);

    // Closes the least recently drawn session whose monitor is not playing, to make room for
    // another. False when every open session is playing, which is the only case a caller cannot
    // resolve. Caller must hold m_gpuMutex and not m_stateMutex.
    bool evictIdleVideoSessionLocked();

    // The session to draw this monitor's video with, opened and decoded into when it has none for
    // this wallpaper -- so a re-post costs the blit of the picture already held. Never advances a
    // session that exists: that is advanceVideo()'s alone. ensureGpu() and this pass's
    // dropStaleVideoSessionLocked() must have run: a session found here is reused without asking
    // which wallpaper it was opened for. Throws what VideoPipeline's constructor and decodeNext()
    // throw, retaining nothing when it does.
    VideoSession& videoSessionLocked(int monitorId, const std::string& path, uint64_t wallpaperSeq);

    // Drops every claim on a monitor whose images have just been freed or replaced, and the
    // frame they held: hasFrame promises an export that the images it names still exist. Takes
    // m_stateMutex itself, so the caller must hold only m_gpuMutex.
    void forgetImages(int monitorId);

    // The size the screen bound to this monitor reported, if it has. Caller must hold
    // m_stateMutex.
    std::optional<OutputSize> reportedSizeLocked(int monitorId) const;

    // What this monitor is drawn at: the reported size, else the configured default. Caller
    // must hold m_stateMutex.
    OutputSize targetSizeLocked(int monitorId) const;

    // Re-requests a monitor whose drawing size is no longer `before`, appending it to `toRender`.
    // Bumping requestSeq is what keeps a render already under way at the old size from
    // committing. Cannot throw once `toRender` has room for one more. Caller must hold
    // m_stateMutex.
    void redrawIfResizedLocked(int monitorId, OutputSize before, std::vector<int>& toRender);

    // Drops what names no monitor answers to reported, until no more than kMaxMonitors of each are
    // held. Caller must hold m_stateMutex.
    void pruneOutputReportsLocked();

    // Both of a hand-over's descriptors, or neither and a throw with the GPU health recorded.
    // Caller must hold m_gpuMutex, which is what keeps `target` alive across the export.
    FrameHandle exportHandle(const OffscreenTarget& target);

    // The images `monitorId` has handed out and not got back. Caller must hold m_stateMutex.
    std::vector<uint64_t> heldBuffersLocked(int monitorId) const;

    // The claims that would be in force if `client` were handed `bufferId`, or Refused when a
    // cap stands in the way. `client` must not be empty. Pure, and it only ever adds: its result
    // is both what the reserve is tested against and what is stored, so the admission test
    // cannot disagree with the bookkeeping, and no caller can end a claim through it.
    static Handover withHandover(std::vector<BufferHold> holds, const std::string& client, uint64_t bufferId);

    // Ends `client`'s claim on one image, reporting whether there was one to end. Caller must
    // hold m_stateMutex.
    static bool dropHoldLocked(MonitorState& state, const std::string& client, uint64_t bufferId);

    // Every id in `holds`, which nextUnheld() takes as its membership test. Duplicates are left
    // in: two consumers may hold the same image, and both claims are real.
    static std::vector<uint64_t> flattenHolds(const std::vector<BufferHold>& holds);

    // What an image coming free on `monitorId` is worth: a render if one is owed, an announcement
    // if a readable frame exists. Caller must hold m_stateMutex.
    void reportFreedImageLocked(int monitorId, const MonitorState& state, std::vector<int>& toRender,
                                std::vector<FrameAvailable>* announce) const;

    // Reads the config on a thread this can walk away from. Linux gives no way to bound a
    // filesystem call -- a hard network mount is uninterruptible by design -- so the only
    // way to stop a stalled read holding startup is to stop waiting for it.
    LoadResult loadWithDeadline() const;

    // Whether a live monitor holds a wallpaper for this id. Caller must hold m_stateMutex.
    bool liveHolds(int monitorId) const;

    // The live state the persistence side needs, and all it needs. Read under
    // m_stateMutex wherever a render or a bus call could be running beside it.
    LiveWallpapers liveWallpapers() const;

    // The persistence report, over that live state. Same locking rule.
    std::string keptDetail(const std::string& base) const;

    // Hands `entries` to the writer without waiting, and reports a refusal or a missing
    // thread as persistence health. Safe to call with no lock held.
    void queuePersist(const std::vector<PersistedWallpaper>& entries);

    // The same, then the writer's bounded wait. Returns whether THESE entries are known to
    // be on disk; on false, `whyNot` receives that write's own reason, sanitized. Takes no lock
    // of its own, but it parks the calling thread on the write for up to the writer's deadline,
    // which is why restoreSaved() hands its repair to queuePersist() instead.
    bool persist(const std::vector<PersistedWallpaper>& entries, std::string* whyNot = nullptr);

    // Folds the writer's answer, and the losses outliving it, into a health copy rather
    // than m_health: the writer's thread cannot touch this object.
    void applyPersistOutcomeLocked(DaemonHealth& health) const;

    // Displaced: the bind took the name off another monitor, or took another name off this one.
    enum class BindResult { Unchanged, Bound, Displaced };

    // Binds a name to a monitor, releasing any binding either side already had. Caller must
    // hold m_stateMutex and must have validated the name.
    BindResult bindOutputLocked(int monitorId, const std::string& outputName);


    const Config m_config;
    const ConfigStore m_store;

    // Owns getting choices onto disk and answers whether they got there. Declared before
    // the state it reports on, so it outlives every path that asks.
    ConfigWriter m_writer;

    // The rows this run could not apply, and the tally of what the config lost.
    KeptRows m_kept{kMaxMonitors};

    // Everything from here to the mutexes is guarded by m_stateMutex, except the load results
    // below, which are written before the event loop starts and only read after.

    // What load() returned, until restoreSaved() consumes it.
    std::vector<PersistedWallpaper> m_restore;

    // The repair restoreSaved() computed, until writeRepairedConfig() takes it out.
    std::optional<std::vector<PersistedWallpaper>> m_repair;

    // A copy of the pre-repair file exists. Written by preserveUnreadableConfig(), read by
    // writeRepairedConfig(); both run before the event loop.
    bool m_repairCopied = false;

    // A copy this run needed could not be taken, so the rewrite after it removed rows held
    // nowhere else. Sticky: a later write succeeding says nothing about what the first
    // destroyed, and health returning to ok is what hides it.
    bool m_preserveFailed = false;

    // What the load found, held for preserveUnreadableConfig() to act on later.
    bool m_loadPreservable = false;
    int m_loadMalformed = 0;
    std::string m_loadError;

    // Order matters: m_gpuMutex may be taken before m_stateMutex, never the reverse.
    mutable std::mutex m_gpuMutex;
    mutable std::mutex m_stateMutex;

    // Guarded by m_gpuMutex. Declaration order is destruction order reversed: targets
    // hold a Device&, so the Device must outlive them.
    std::unique_ptr<Device> m_device;
    std::unique_ptr<OffscreenTargetManager> m_targets;
    std::string m_resolvedShaderDir;

    // One monitor's open video file, looping: the decoder, its picture pool, one converted picture
    // and where that picture stands against the clock. Held so a re-post -- a screen reporting its
    // size, a consumer reporting its layouts -- draws the held picture instead of rebuilding a
    // decode session for it. A resize keeps it: nothing in a session is sized to the target, since
    // the blit that shows the picture scales to whatever extent it is given.
    //
    // Guarded by m_gpuMutex and not m_stateMutex, which is why it cannot live in MonitorState: a
    // pipeline is GPU-thread-only, destruction included, while MonitorState is read by bus
    // handlers. Declared after the Device and the targets so it is destroyed before them, which is
    // also what frees the pipelines at shutdown -- on the thread ~WallpaperManager adopts, and
    // before the Device they were built on. Holds at most kMaxVideoSessions entries.
    struct VideoSession {
        uint64_t wallpaperSeq = 0;
        // Which draw this session was last wanted for, against m_videoSessionUse. Eviction picks
        // the smallest.
        uint64_t lastUsed = 0;
        // The file this pipeline opened, which its teardown is attributed to. Not a copy of the
        // monitor's path: that names what the monitor has been asked for since.
        std::string path;
        std::unique_ptr<VideoPipeline> pipeline;

        // The held picture: the first, never drawn; the one on screen, whose successor is decoded
        // next; or that successor, waiting for `due`. `timeline` is a picture's place on the file's
        // own schedule, which the next is timed from; `due` is when it may be drawn, which the cap
        // holds at least kMinPictureInterval after the previous picture's `due` but which never
        // moves the schedule. Measured from `due` and not from the draw, so wake-up latency is not
        // charged to the next picture. A pass drawing a waiting picture shows it early and leaves
        // the schedule where it was.
        struct Undrawn {};
        struct OnScreen {
            std::chrono::steady_clock::time_point timeline;
            std::chrono::steady_clock::time_point due;
        };
        struct Waiting {
            std::chrono::steady_clock::time_point due;
            std::chrono::steady_clock::time_point timeline;
        };
        std::variant<Undrawn, OnScreen, Waiting> clock;

        // When the clock first found this session's monitor not playing, or nothing while it is.
        // The whole off switch: a video nobody can see stops at once and keeps its picture, and
        // this is what says how long it has been stopped, whatever stopped it.
        std::optional<std::chrono::steady_clock::time_point> idleSince;
    };
    std::map<int, VideoSession> m_videoSessions;

    // Counts draws that wanted a session, so the least recently wanted one can be told from the
    // rest. Guarded by m_gpuMutex with the map it orders.
    uint64_t m_videoSessionUse = 0;

    // Guarded by m_stateMutex. m_gpuStatus is a copy of what the Device reported,
    // published once at initialisation, so status() can answer without touching the
    // Device and therefore without waiting for a render to finish.
    std::map<int, MonitorState> m_monitors;

    // Connector name -> monitor id, and the only authority on which is which. Guarded by
    // m_stateMutex.
    std::map<std::string, int> m_outputs;

    // Connector name -> the size that screen last reported. Kept apart from m_outputs so a size
    // outlives its name being unbound: the plugin that sent it cannot tell the name was unbound
    // and bound again, so it never sends it twice. Guarded by m_stateMutex.
    std::map<std::string, OutputSize> m_outputSizes;

    // The screens that reported they are not being drawn -- behind a lock screen, a fullscreen
    // window, or a session switched away. By name for the same reason a size is, and not on the
    // monitor: a screen can be named before a wallpaper is chosen for it, and a report dropped
    // there would be answered with success and never sent again. Absent means drawn, which is what
    // a screen that has never reported is taken to be. Guarded by m_stateMutex.
    std::set<std::string> m_outputsNotDrawn;

    // What each consumer reported it can import, keyed by its DBus unique name, and the
    // intersection currently in force. Per-consumer because the daemon must satisfy all of
    // them at once, and because a set has to die with the process that reported it -- one
    // left behind by a crashed consumer would narrow the exporter for its replacement.
    //
    // Guarded by m_stateMutex; the render thread reads and clears the flag under m_gpuMutex
    // first, so the two locks keep their declared order.
    std::map<std::string, std::vector<uint64_t>> m_importModifiersByClient;
    std::vector<uint64_t> m_importModifiers;
    bool m_importModifiersPending = false;

    // The monitors the pending set invalidates, chosen when it was accepted rather than when
    // it is applied: only these lose their images and re-render. Guarded by m_stateMutex.
    std::vector<int> m_importModifiersInvalidated;

    // Caller must hold m_stateMutex. Empty when no consumer has reported, which is what
    // "unconstrained" means to the exporter.
    std::vector<uint64_t> intersectImportModifiersLocked() const;

    // Whether this monitor's committed frame can still be read under the set in force. Caller
    // must hold m_stateMutex.
    bool frameStillReadableLocked(const MonitorState& state) const;

    // Records `modifiers` as the effective set and marks the monitors it invalidates. Caller
    // must hold m_stateMutex.
    ImportModifierUpdate adoptImportModifiersLocked(std::vector<uint64_t> modifiers);

    // Hands a newly reported import-modifier set to the GPU side and drops the images built
    // against the old one. Render thread, m_gpuMutex held, before any target is created.
    void applyImportModifiersLocked();

    // Member pointer picks the check, so a caller cannot record into a field it did not
    // mean and the sanitizing happens in one place.
    void recordHealth(HealthCheck DaemonHealth::*which, HealthStatus status, const std::string& detail);
    // Caller must hold m_stateMutex, which is not recursive.
    void recordHealthLocked(HealthCheck DaemonHealth::*which, HealthStatus status, const std::string& detail);

    GpuStatus m_gpuStatus;
    DaemonHealth m_health;
    bool m_configServerAttached = false;
};

}
