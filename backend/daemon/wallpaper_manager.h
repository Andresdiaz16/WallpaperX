// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "config_store.h"
#include "config_writer.h"
#include "crash_record.h"
#include "kept_rows.h"
#include "log_escape.h"
#include "pause_mode.h"
#include "screen_focus.h"
#include "sound_setting.h"
// kTargetDepth and the two numbers behind it: separate because the KDE plugin sizes its import
// cache against them and cannot include this header.
#include "ring_depth.h"
#include "video/picture_skip.h"
#include "video/stream_position.h"
#include "vulkan/frame_export.h"
#include "wallpaper_properties.h"

#include <cstdint>
#include <filesystem>
#include <functional>
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

class AudioOutput;
class AudioTrack;
class Device;
class OffscreenTargetManager;
struct OffscreenTarget;
class VideoPipeline;

// A request that was malformed rather than one that failed to render. Its own type so the
// DBus layer can answer with InvalidArgs instead of a generic failure.
struct InvalidRequest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// This consumer will not be given an acquire timeline, and asking again will not change that while
// the daemon runs: the GPU cannot export one, or the caller has no bus name to key it on. It goes on
// waiting on per-picture sync_fds.
struct TimelineUnavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Every acquire-timeline slot is taken. Clears when a holder leaves the bus; until then the consumer
// waits on per-picture sync_fds and may ask again.
struct TimelineLimitReached : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// This consumer has been sent kMaxTimelineCopiesPerClient copies of its acquire timeline. Lasts as
// long as its connection does.
struct TimelineCopyLimitReached : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A screen size reported under a connector name no monitor answers to. Its own type because the
// cure is elsewhere: the name has to be bound first.
struct UnknownOutput : InvalidRequest {
    using InvalidRequest::InvalidRequest;
};

// One reply handing an image over that has not been given back and carried descriptors -- 1 or 2,
// depending on whether the consumer already had the memory and which acquire went with it. They
// count against the consumer until a release covers that reply.
struct HandoverCharge {
    uint64_t handover = 0;
    uint32_t descriptors = 0;
};

// One image a consumer holds. Every hand-over is numbered from one daemon-wide sequence, and a
// release names the newest one the consumer has read, giving back that one and every earlier one --
// never a hand-over whose reply was still on its way when the release was sent. The claim stands
// while any is outstanding, numbered above `released` up to `newest`, so `released < newest`.
struct HeldBuffer {
    uint64_t id = 0;
    uint64_t newest = 0;
    uint64_t released = 0;
    // Outstanding hand-overs that carried descriptors, oldest first. Each costs at least one, so
    // kMaxDescriptorsPerClient bounds the list.
    std::vector<HandoverCharge> charges;
};

// One recorded hand-over: the number its reply carries, and what an undo of it needs.
struct HandoverRecord {
    uint64_t number = 0;  // what the reply's `handover` field says, and a release names
    // The newest hand-over of this image to this caller before this one; 0 when this began the claim.
    uint64_t previous = 0;
};

// One consumer's claim on a monitor's images: the buffers it has been handed and has not given
// back, oldest first. A holder disappears when its last claim ends, so a monitor's list is
// exactly the consumers currently reading it.
struct BufferHold {
    std::string client;  // DBus unique name
    std::vector<HeldBuffer> buffers;
};

// GetAcquireTimeline refused because the caller already has its fill of descriptors not given
// back. `firstRefusal`: the first since it was last under the cap, and so worth a line.
struct DescriptorLimitReached : std::runtime_error {
    DescriptorLimitReached(const std::string& what, bool first) : std::runtime_error(what), firstRefusal(first) {}
    bool firstRefusal;
};

// What a monitor was last asked to display, and what came of it. `generation` is monotonic
// per monitor and never resets, so a consumer can tell a fresh frame from a repeat; `hasFrame`
// is separate because a failed render clears the flag without rewinding the counter.
struct MonitorStatus {
    std::string type;
    std::string path;
    WallpaperProperties properties;
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

// A monitor's pause setting and what it comes to now. Apart from MonitorStatus because a screen can
// be set to pause before any wallpaper is chosen for it.
struct PauseStatus {
    // Nothing for a setting a newer build wrote, which this one keeps and does not apply.
    std::optional<PauseMode> mode = PauseMode::Play;
    bool paused = false;
    // What the screen bound to this monitor last reported; None and false when it has not.
    ScreenFocus focus = ScreenFocus::None;
    bool windowMaximized = false;
};

// A monitor's sound setting and what the three levels come to on it now.
struct MonitorSoundStatus {
    // Nothing for a setting a newer build wrote, which this one keeps and treats as on.
    std::optional<bool> enabled = true;
    // Sound is on globally, on this screen and for its wallpaper, a window or the desktop on this
    // screen has the focus, and no window on it is maximized or fullscreen. Says nothing about
    // whether the wallpaper has sound to play.
    bool heard = false;
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

    // Whether GetFrame's sync_fds carries a descriptor the consumer must wait on before it
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
    // frame.fd is -1 when the consumer said it can still draw this image and the hand-over carries
    // an acquire; the rest of `frame` describes the image all the same.
    ExportedFrame frame;
    uint64_t generation = 0;

    // -1 when the GPU cannot export one, which leaves the consumer reading without the
    // queue-family acquire, which is undefined. Also -1 for a picture asked for on the caller's
    // acquire timeline, which is given `acquireValue` instead.
    int acquireFd = -1;

    // The value to wait for on the consumer's acquire timeline, already reached when this is
    // handed out. 0 when the consumer has none, which is what acquireFd is for.
    uint64_t acquireValue = 0;

    // The hand-over this reply records against the caller's claim on frame.bufferId; empty when no
    // claim was recorded, as for an unnamed caller. What withdrawHandover() takes back.
    std::optional<HandoverRecord> handover;
};

// What a type is drawn with. The single answer to "can this build draw this type" too: a
// renderable type is exactly one that names a drawer, so the render dispatch, the GetFrame
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
    TooManyDescriptors,  // there is a frame, but this consumer has kMaxDescriptorsPerClient
                         // descriptors it has not given back. Its own doing, and temporary: a
                         // release or leaving the bus ends them.
};

// The result of asking for a monitor's current frame: the frame, or the reason it was not
// handed over.
struct FrameExport {
    std::optional<FrameHandle> handle;

    // Both meaningful only when `handle` is empty. lastError is non-empty exactly when
    // reason is RenderFailed -- Unsupported deliberately does not carry one, because it
    // is not a failure; it carries `type` instead, the wallpaper type this build cannot draw.
    NoFrameReason reason = NoFrameReason::NeverRequested;
    std::string lastError;
    std::string type;

    // Whether this refusal is worth a log line: the first for THIS consumer, or the first in a
    // while. Per consumer, not per monitor, because a second consumer being refused is its own
    // diagnosis. Rate-limited rather than armed by a hand-over, because a consumer holding its fill
    // is served and refused alternately while a video plays. For TooManyDescriptors: the first
    // since this consumer was last under the cap. Meaningful only for those two.
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
// monitor, and the per-monitor bookkeeping behind SetWallpaper/GetFrame. Request and render
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

    // How many consumers may hold images on one monitor. Bounds what peers can make the daemon
    // remember; a further one is refused like any other hand-over. Unlike the reserve it can
    // refuse while images are free -- four consumers sharing one image leave four unheld -- which
    // is the price of not evicting a holder to make room for the fifth.
    static constexpr size_t kMaxHoldersPerMonitor = 4;

    // Descriptors one consumer may have been sent and not given back: a frame reply's from when it
    // is sent until the claim on its image ends, a timeline's until the consumer leaves the bus.
    // dbus-broker keeps every descriptor of a reply its recipient has not read in its own file
    // table, one table for the whole bus and limited to 1024, and when that fills it disconnects
    // the sender of the next one -- this daemon, blanking every screen. So a consumer that stops
    // reading is refused here instead. A well-behaved one needs at most 4 per monitor and 1 for its
    // timeline, and none for a picture drawn into an image it already holds an import of.
    static constexpr uint32_t kMaxDescriptorsPerClient = 64;

    // What one holder's claims on one monitor count against kMaxDescriptorsPerClient: the
    // descriptors its replies actually carried.
    static uint32_t descriptorsOf(const BufferHold& hold);

    // Consumers holding an acquire timeline at once, departed ones not yet freed included. Bounds
    // what peers can make the daemon keep; one past it is told to ask again later, and meanwhile
    // uses sync_fds, which still works.
    static constexpr size_t kMaxAcquireTimelines = 8;

    // Copies of its acquire timeline one consumer is sent per connection, the first included. Bounds
    // the descriptors a consumer that keeps asking and never reads the replies can leave in the bus's
    // table, which kMaxDescriptorsPerClient does not: the timeline counts there once however often it
    // is sent. A well-behaved one asks once per daemon.
    static constexpr uint32_t kMaxTimelineCopiesPerClient = 4;

    // Requests one consumer may have waiting on the render worker at once: its share of the
    // worker's queue, so one consumer's burst cannot fill it and refuse everyone else. A
    // well-behaved one has one frame request out per screen and one timeline request, which this
    // covers up to the 15 screens kMaxDescriptorsPerClient does.
    static constexpr int kMaxQueuedRequestsPerClient = 16;

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

    // How long a decode session stays open after its monitor stops being watched. Stopping already
    // ends the cost of playing, so what this trades is the video memory a decoder and its picture
    // pool hold against the cost of coming back: a file open, a
    // stream probe, and a seek back to the picture it stopped on, which decodes up to a keyframe
    // interval before anything is shown. The last picture stays on screen throughout.
    //
    // Five seconds is past every transient -- a menu, a notification, a window animation, a screen
    // redrawn at a new size -- and far below the absences whose memory is worth having back: a
    // locked screen, a fullscreen window, a session switched away.
    static constexpr std::chrono::seconds kIdleSessionRelease{5};

    // The same while the monitor's pause setting stops it, whether or not it is also unwatched.
    // Longer, because a focus mode pauses and resumes with every change of window focus, and one
    // coming back inside this carries on from the open decoder at once.
    static constexpr std::chrono::seconds kPausedSessionRelease{30};

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

    // Reopens after the daemon ran short, each of which first evicts an idle session if there is
    // one. More than kPlaybackRetries because a real shortage clears as other screens release
    // theirs; bounded because a file whose own demand exceeds the device is reported as a shortage
    // every time.
    static constexpr uint32_t kShortageRetries = 10;

    // Fatal signals caught while a thread was working on one video's content -- decoding its
    // pictures or its sound, drawing it, or freeing it after its own failure -- before the next
    // start refuses it.
    // Counted across every stop, never for a death that names no video, and cleared when the video
    // is set again or has played kCleanPlayAfter. Two, so one crash that merely landed there -- a
    // driver fault while drawing it -- never costs a working wallpaper, while a file that kills
    // every run it plays in is stopped on the third start.
    static constexpr int kPlaybackDeaths = 2;

    // Pictures shown for this long clear a video's death count: a file that crashes at some picture
    // reaches it within one loop, and wallpaper loops are seconds to a few minutes. Measured in
    // pictures delivered, not time since it was set, so a video nobody watched earns nothing.
    static constexpr std::chrono::minutes kCleanPlayAfter{10};

    // The longest a Config may set any of the waits above to.
    static constexpr std::chrono::hours kMaxTiming{24};

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

        // Where the records of the wallpaper being applied and of the videos a run started with
        // live, so a start after a crash can refuse what the last runs did not survive. Empty
        // disables that refusal, and is the default for the same reason configFile's is:
        // production opts in via defaultStateDir(), so a test that says nothing about state
        // cannot leave a record in the developer's own directory for the real daemon's next
        // start to honour.
        std::filesystem::path stateDir;

        // The waits above, overridable so a test can run them in milliseconds; the constants are
        // what production gets. Each must be positive and at most kMaxTiming, or the constructor
        // throws std::invalid_argument: zero would make a monitor unwatched the moment it publishes
        // and lift the once-per-interval limit on resume credits. The ceiling is a sanity bound far
        // below the ~292 years at which adding one to now() would overflow; some bound is needed.
        std::chrono::steady_clock::duration unwatchedAfter = kUnwatchedAfter;
        std::chrono::steady_clock::duration idleSessionRelease = kIdleSessionRelease;
        std::chrono::steady_clock::duration pausedSessionRelease = kPausedSessionRelease;
        std::chrono::steady_clock::duration refusalLogInterval = kRefusalLogInterval;
        std::chrono::steady_clock::duration cleanPlayAfter = kCleanPlayAfter;

        // Handed to the Device: runs the moment a GPU call first answers VK_ERROR_DEVICE_LOST, on
        // whichever thread made it, usually with the GPU lock held: the stop starts while the job
        // that saw the loss is still unwinding, which a hung or crashing driver may never let it
        // finish. Must not block or call into this manager.
        std::function<void()> onDeviceLost;
    };

    explicit WallpaperManager(Config config);
    ~WallpaperManager();

    WallpaperManager(const WallpaperManager&) = delete;
    WallpaperManager& operator=(const WallpaperManager&) = delete;

    // Records a request; GPU-free. Throws InvalidRequest for a malformed one, and everything
    // that can only fail on the GPU fails later in renderPending(). The caller must then post
    // the monitor to the render worker, types this build cannot draw included -- those still
    // need their target released. `properties` replace whatever the previous wallpaper had.
    void requestWallpaper(const std::string& type, const std::string& path, int monitorId,
                          const WallpaperProperties& properties = {});

    // Replaces the properties of the wallpaper this monitor holds, keeping the wallpaper: a video
    // plays on from the picture it is at. Throws InvalidRequest for a bad id, a set
    // propertiesError() refuses, or a monitor holding no wallpaper. Needs no render; the caller
    // wakes the render worker, whose next round times the waiting picture at the new speed. The
    // write is queued, not waited on, since a slider sends a stream of these. Idempotent.
    void setWallpaperProperties(int monitorId, const WallpaperProperties& properties);

    // Records the connector name a monitor is known by -- "DP-3". Throws InvalidRequest for a
    // bad id or a name outputNameError() rejects, and std::runtime_error when the name did not
    // reach the config file: the binding stands for this run either way, and only the write
    // makes it outlive a restart. Idempotent; a call that changes nothing already on disk does
    // not write, since the write parks the calling thread.
    //
    // A name carries its screen's reported size, so binding one can change what two monitors are
    // drawn at. Those are recorded as owed a render and put in `redraw` before anything can throw;
    // the caller posts them on success and on a failed write alike, since the binding stands.
    //
    // Returns whether the call moved a name: what /config answers changed. A throw leaves the
    // caller not knowing, since a failed write keeps the binding it made.
    bool setMonitorOutput(int monitorId, const std::string& outputName, std::vector<int>& redraw);

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
    // covered to drawn credits the monitor with having been seen. Returns whether the report
    // changed what was recorded for that screen.
    bool setOutputVisible(const std::string& outputName, bool visible);

    // Sets when this monitor's video stops on the picture it shows, by PauseMode name. Throws
    // InvalidRequest for a bad id or an unknown name. Survives every wallpaper change. All or
    // nothing up to the queued save, which is then waited on without an answer, as requestWallpaper()
    // does: a failed write is reported in persistence health. The caller wakes the render worker.
    // Idempotent.
    void setMonitorPause(int monitorId, const std::string& mode);

    // Records what the screen named `outputName` reports about its windows: what holds the focus
    // there, and whether a window there is maximized or fullscreen. Not persisted. A screen that has
    // never reported, or whose reporter `client` has since left the bus, is taken to have neither,
    // so the focus modes play until a report says otherwise. Throws as setOutputVisible() does. The
    // caller wakes the render worker. Idempotent.
    void setOutputFocus(const std::string& client, const std::string& outputName, ScreenFocus focus,
                        bool windowMaximized);

    // Drops every focus report `client` made, because it left the bus. Returns whether any went,
    // which may have ended a pause, so the caller wakes the render worker.
    bool forgetOutputFocus(const std::string& client);

    // Switches sound on or off for every screen, above each screen's own setting. Persisted, saved
    // and waited on as setMonitorPause() does, all or nothing up to the queued save. Returns whether
    // the setting changed. Idempotent.
    bool setSound(bool on);

    // The same for one monitor, below the global setting and above its wallpaper's `mute`. Survives
    // every wallpaper change. Throws InvalidRequest for a bad id.
    bool setMonitorSound(int monitorId, bool on);

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

    // Replaces the playing record the last run left with the counts restoreSaved() carried forward,
    // and holds it open until this manager is destroyed. Call once, after restoreSaved() and after
    // the bus name is held: it overwrites state a running daemon owns.
    void recordStartingVideos();

    // Stops counting deaths, because an orderly stop has begun; the counts stay. Call as the stop
    // starts, not at the end of teardown, which the stop deadline may cut short. Idempotent;
    // destruction does it too.
    void endPlayingRecord();

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

    // What asks the render worker for a clock round when something only a round acts on changes off
    // its thread -- a video's sound found to be there or gone. Set by the worker while it runs and
    // emptied before it stops; must not block, nor call into this manager. Any thread.
    void setClockWake(std::function<void()> wake);

    // Whether the device this manager built has been lost and Config::onDeviceLost, which the loss
    // has already called, is set: the process is stopping for it. Permanent: the device is built
    // once and never rebuilt. Without a handler a lost device only fails renders, which is what
    // in-process tests that inject a loss rely on. Render worker thread.
    bool stopsForLostDevice() const;

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
    // drawing into an image being read. One past kMaxDescriptorsPerClient is refused as
    // TooManyDescriptors, before any of those.
    //
    // The default is for a caller that is not a bus peer, and it records no claim: there would
    // be no name to withdraw it under, and a claim nothing can end takes an image out of the
    // ring for the life of the daemon. A frame exported that way can be redrawn under its
    // reader.
    //
    // `onTimeline` is the caller saying it can wait on its acquire timeline for this picture. The
    // picture carries a value only then, and only while `client` holds a live one; otherwise a new
    // binary semaphore. The consumer chooses per request, so a timeline it never received or could
    // not import cannot leave it with a value it has no way to wait on.
    //
    // `knownBuffers` are the images the caller says it can still draw. The memory descriptor is
    // left out only when the image handed over is one of them and the picture carries an acquire,
    // since waiting on that is what tells the caller's driver the memory holds a new picture. Asked
    // per request and never remembered: a consumer drops imports on its own, and only it knows.
    // Throws InvalidRequest past kMaxKnownBuffers.
    //
    // An export is not yet a delivery: a hand-over counts as someone collecting this monitor's
    // pictures -- what keeps a video playing -- only once its reply is out, so one that never
    // reaches its caller cannot keep a video decoding. The caller follows every export that
    // returns a handle with handoverDelivered() or withdrawHandover().
    //
    // A failed export takes its own hand-over back. When that ends the claim, the image is
    // reported as a release reports it: the renders now owed go into `toRender`, and `announce`,
    // when given, receives the frames to announce. Both are filled only on that path, before the
    // throw, so a caller must act on them in its catch.
    //
    // Once the export has succeeded nothing in here throws, so a claim recorded is always one whose
    // handle reaches the caller.
    FrameExport exportCurrentFrame(int monitorId, const std::string& client, bool onTimeline,
                                   const std::vector<uint64_t>& knownBuffers, std::vector<int>& toRender,
                                   std::vector<FrameAvailable>* announce = nullptr);

    // The hand-over exportCurrentFrame() made to `client` reached it, which is what counts as this
    // monitor's picture being collected. True when this monitor's playing video has served this
    // consumer before: one of many a second, not worth a log line. Call on the render worker,
    // before it runs another export.
    bool handoverDelivered(int monitorId, const std::string& client) noexcept;

    // Throws InvalidRequest for a list exportCurrentFrame() would refuse, so a bus handler can
    // refuse it before queueing.
    static void requireKnownBuffers(const std::vector<uint64_t>& knownBuffers);

    // Gives back `client`'s hand-overs of one image numbered up to `handover`, the newest it says it
    // has read and stopped reading. The claim ends only when that covers the newest hand-over made;
    // a later one, whose reply crossed the release, keeps it -- the consumer is about to read it.
    // Returns the monitors whose deferred render should be retried now that an image may be free.
    // Silent about a claim it does not hold: a consumer releasing twice, or releasing one
    // already dropped for it, is not making an error. Throws InvalidRequest for a bad monitor id
    // and for a hand-over number never issued.
    //
    // `announce`, when given, receives the monitors holding a frame a consumer can still read --
    // the freed image has to be published, or a consumer refused for want of one never learns it
    // may ask again. A monitor can be in both: a redraw at a new size is owed while the frame it
    // replaces is still served.
    std::vector<int> releaseBuffer(const std::string& client, int monitorId, uint64_t bufferId, uint64_t handover,
                                   std::vector<FrameAvailable>* announce = nullptr);

    // The same for every image on every monitor, because `client` left the bus. Without it a
    // consumer that crashed holding a frame would keep that image out of the ring forever --
    // and plasmashell restarts are routine, while this daemon does not.
    std::vector<int> forgetHeldBuffers(const std::string& client, std::vector<FrameAvailable>* announce = nullptr);

    // Takes back the hand-over exportCurrentFrame() recorded for `handle`, because its reply never
    // reached the bus: that reply's descriptors only, and the claim itself only if no earlier
    // hand-over is outstanding. Nothing if a release or departure ended the claim meanwhile. Returns and announces
    // as releaseBuffer() does when the claim ends.
    //
    // Call on the render worker, before it runs another export: the undo is exact only while no
    // other export can have ended and re-created this claim.
    std::vector<int> withdrawHandover(int monitorId, const std::string& client, const FrameHandle& handle,
                                      std::vector<FrameAvailable>* announce = nullptr);

    // A descriptor for `client`'s acquire timeline, created on its first request and kept until it
    // leaves the bus. It only makes the timeline available: a picture carries a value on it only
    // when the request for that picture asks. Each call is a new copy of the same timeline, and a
    // connection is sent at most kMaxTimelineCopiesPerClient of them, so a consumer keeps and
    // duplicates the one it has rather than asking again. The timeline counts as one descriptor
    // against kMaxDescriptorsPerClient until the consumer leaves, however often it is sent: no call
    // gives a copy back, so counting each would only ever grow. GPU work: call from the render
    // worker, and follow a descriptor that reached its caller with timelineCopyDelivered().
    // Throws TimelineUnavailable, TimelineLimitReached, TimelineCopyLimitReached,
    // DescriptorLimitReached, and GpuFailure when the driver fails.
    int acquireTimelineFd(const std::string& client);

    // The descriptor acquireTimelineFd() returned is out on the bus, which is what counts against
    // kMaxTimelineCopiesPerClient: a reply that failed to send leaves nothing in the bus's table.
    // Call on the render worker, before it runs another acquireTimelineFd(): the limit's check and
    // this count are one step only while nothing runs between them.
    void timelineCopyDelivered(const std::string& client) noexcept;

    // `client` has left the bus, so its timeline goes. Only marked here, under the state lock; the
    // GPU thread destroys it at its next export or registration. Posting a worker task per
    // departure instead would let short-lived bus clients fill the bounded queue and refuse frame
    // requests. Bus thread.
    void forgetAcquireTimeline(const std::string& client);

    // A request from `client` queued on the render worker can create state for it -- a hold, a
    // timeline -- after it has left the bus, and the departure that should end that state has then
    // already been handled. These close that window. admitRequest() runs on the bus thread before
    // the post; noteRequestDone() when an admitted request has been answered or refused, and
    // returns true when `client` left while it was queued and this was its last: the departure is
    // then applied again, over whatever the requests created. noteDeparted() runs with the
    // departure. Bounded by the worker's queue.
    //
    // The same count is the client's share of that queue: a request finding kMaxQueuedRequestsPerClient
    // already waiting is not admitted, and nothing is counted for it. FirstRefusal opens a run of
    // refusals, which ends after kRequestRefusalQuiet without one or when the client leaves.
    enum class Admission { Admitted, Refused, FirstRefusal };
    [[nodiscard]] Admission admitRequest(const std::string& client);
    [[nodiscard]] bool noteRequestDone(const std::string& client);
    void noteDeparted(const std::string& client);

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

        // Every monitor holding a wallpaper or a pause setting.
        std::map<int, PauseStatus> pause;

        // The global sound setting; nothing for a value a newer build wrote, applied as on.
        std::optional<bool> sound = true;

        // Every monitor holding a wallpaper or a sound setting.
        std::map<int, MonitorSoundStatus> monitorSound;
    };
    Snapshot snapshot() const;

    // Private with a single friend rather than public: a claim any caller could revoke would
    // let a second server stamp its own bind failure over one that is serving.
private:
    friend class HttpServer;

    // Called from HttpServer's constructor with the bind result, from its serving thread
    // if the accept loop later dies, and from its registration on the way out. `detail` names
    // the port bound, or why none is.
    void recordConfigServer(bool listening, const std::string& detail);

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
    // maybe-filled list: `holds` is meaningless unless the hand-over was allowed.
    struct Handover {
        enum class Outcome { Granted, AlreadyHeld, Refused };

        std::vector<BufferHold> holds;  // the claims that would be in force; empty if Refused
        Outcome outcome = Outcome::Refused;
        uint64_t previous = 0;  // the claim's newest hand-over before this one, if AlreadyHeld
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
    // a newcomer only once its holder has been silent for `interval` or has left the bus.
    // The cost is deliberate: while four consumers keep reading one monitor, a fifth is never
    // named, which is one missing line rather than a stream of false ones.
    //
    // Periodic reports a consumer that already holds a slot again once per interval: a refusal
    // still happening is still worth saying. Once reports it only when it takes a slot, since
    // starting to read is a single event. The caller's lock is whichever one guards `reports`.
    enum class Repeat { Periodic, Once };
    static bool reportConsumer(std::vector<ConsumerReport>& reports, const std::string& client, Repeat repeat,
                               std::chrono::steady_clock::time_point now,
                               std::chrono::steady_clock::duration interval);

    // Who answers for a failure: the file, the daemon, or the daemon for want of a resource that
    // closing a session can give back -- memory, a decoder, a descriptor.
    enum class Fault { File, Daemon, Shortage };

    struct MonitorState {
        std::string type;
        std::string path;
        // Changed without a new request, so a playing video keeps its session through a change.
        WallpaperProperties properties;

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
        Fault lastErrorFault = Fault::File;  // meaningless while lastError is empty

        // The saved request was not applied because the last run died applying it, or because
        // kPlaybackDeaths deaths were caught while playing it, and no pass may decode it -- a
        // re-post for any other reason must not either. Set only by restoreSaved(), cleared only
        // by requestWallpaper(): a client asking for this wallpaper again is a different statement
        // from a config being restored.
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
        // whether playback goes on. It takes effect at most once per unwatchedAfter, since the
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

        // Where the wallpaper `forSeq` names stood when its session was released, so the session
        // that replaces it starts on the picture that stayed on screen. Held here because the
        // session is gone; spent by the next attempt to open one for this monitor, whatever it plays.
        struct ReleasedAt {
            uint64_t forSeq = 0;
            StreamPosition shown;
        };
        std::optional<ReleasedAt> releasedAt;

        // The wallpaper whose file held a single picture: it is on screen and there is nothing
        // after it, so no clock advances it and no session is opened to play it. Keyed by
        // wallpaperSeq for ReopenBudget's reason, and held here rather than on the session
        // because the session is freed while the fact stays true.
        uint64_t parkedWallpaperSeq = 0;
        // Why the parked wallpaper `forSeq` names gave no sound when it was wanted, so its released
        // session is not opened again for nothing. Silent: the file has none, and it is never opened
        // again for it. Failed: what failed may not fail again, so it is opened again while the
        // reopen budget lasts, and the record is spent by that reopen. Keyed as parkedWallpaperSeq is.
        struct NoSound {
            enum class Why { Silent, Failed };
            uint64_t forSeq = 0;
            Why why = Why::Silent;
        };
        NoSound noSound;

        // Reopens the clock has spent on the wallpaper `forSeq` names. Keyed by the wallpaper
        // rather than reset when one arrives: a count whose key names a wallpaper this monitor no
        // longer holds has nothing to say about the one it does, so no caller has to remember to
        // clear it.
        struct ReopenBudget {
            uint64_t forSeq = 0;
            uint32_t failures = 0;   // against kPlaybackRetries
            uint32_t shortages = 0;  // against kShortageRetries, and never the file's

            // Spends one reopen after `failure` from the allowance it belongs to; false once spent.
            bool spend(Fault failure);
        };
        ReopenBudget reopens;

        // How long the wallpaper `forSeq` names has played in this run: the gaps between consecutive
        // clock pictures, each drawn on time. `lastShown` is dropped whenever play stops -- the
        // screen reported hidden, the clock finding it not playing, a failure, a new session -- so
        // no stop, however short, adds anything. `cleared` once it reached Config::cleanPlayAfter
        // and its death count was cleared.
        struct CleanPlay {
            uint64_t forSeq = 0;
            std::chrono::steady_clock::duration played{};
            std::optional<std::chrono::steady_clock::time_point> lastShown;
            bool cleared = false;
        };
        CleanPlay cleanPlay;
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
    static void recordFailureLocked(MonitorState& state, const std::string& error, Fault fault);

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

        Fault fault() const { return exhausted ? Fault::Shortage : daemonFault ? Fault::Daemon : Fault::File; }
    };
    static RenderFailure classifyFailure();

    // The reopens this monitor's current wallpaper has spent, zeroed if the count was another
    // wallpaper's. Caller must hold m_stateMutex.
    static MonitorState::ReopenBudget& playbackBudgetLocked(MonitorState& state);

    // Records that the held picture was just drawn. One drawn more than its own interval at
    // `speed` after it was due -- a stall, not jitter -- restarts the schedule from `now`, so a long stall is not
    // followed by pictures shown back to back.
    static void markShown(VideoSession& session, std::chrono::steady_clock::time_point now, double speed);

    // Credits `state`'s wallpaper with the play since its last clock picture, for the picture shown at
    // `now` -- nothing when that one was not `onTime`. True exactly once per wallpaper, when that play
    // first reaches Config::cleanPlayAfter. Caller must hold m_stateMutex.
    bool creditCleanPlayLocked(MonitorState& state, std::chrono::steady_clock::time_point now, bool onTime) const;

    // advanceVideo() for one session: when it next needs a round, or nothing while its monitor is
    // not playing. Appends to `round` what it showed and what it left owed. A decode dropping pictures
    // on the way to its next one stops at `catchUpUntil`, having dropped at least one, and carries on
    // next round. Caller must hold m_gpuMutex.
    std::optional<std::chrono::steady_clock::time_point> advanceSessionLocked(
        int monitorId, std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::time_point catchUpUntil, ClockRound& round);

    // A CatchingUp session caught up in one go, for a pass that must draw now. A failure that gives the
    // position up closes the session and opens the file again at its first picture; any other closes it
    // and is rethrown. Throws what videoSessionLocked() throws.
    VideoSession& caughtUpSessionLocked(int monitorId, VideoSession& session, const std::string& path,
                                        uint64_t wallpaperSeq);

    // Whether `failure`, met by a CatchingUp session on its way to its saved position, gives that
    // position up so the file plays from its first picture: a content error, since the position may be
    // what failed, or a shortage, since only an open runs under retryAfterEvictionLocked(). Logs when
    // it does. The caller has closed the session; the open that follows starts at the first picture,
    // the position having been spent by the one before.
    static bool givesUpPosition(int monitorId, const RenderFailure& failure);

    // The four entries to a monitor's retained decode session, all on the GPU thread with
    // m_gpuMutex held. A routine teardown -- a screen gone dark, an eviction, a wallpaper replaced --
    // re-enters ffmpeg and the driver but blames nothing: it is not the file's content running, so
    // a death in it must not refuse a working wallpaper. A caller that holds a record keeps it.
    void closeVideoSessionLocked(int monitorId);

    // The same for a session freed only to have its memory back, which the next session for the
    // same wallpaper resumes from: it records the picture on screen as MonitorState::releasedAt.
    // Takes m_stateMutex.
    void releaseVideoSessionLocked(int monitorId);

    // The same after `failure` broke the session, named to the fatal-signal handler only when the
    // failure was the file's: freeing a pipeline on a device that has just failed can itself crash
    // the driver, and that death says nothing about the video.
    void closeFailedVideoSessionLocked(int monitorId, Fault failure);

    // Keeps the session only if a pass for this wallpaper would draw with it. Called before the
    // branches that draw or release a target, so no type and no failure leaves a decoder and its
    // picture pool open for a file nothing shows.
    void dropStaleVideoSessionLocked(int monitorId, std::optional<Drawable> kind, uint64_t wallpaperSeq);

    // The request whose committed frame a video of `wallpaperSeq` still shows, or nothing when it
    // shows something else: no frame, a render owed, another wallpaper since, or a file with one
    // picture. Caller must hold m_stateMutex.
    std::optional<uint64_t> currentSeqLocked(int monitorId, uint64_t wallpaperSeq) const;

    // The same, watched and not paused: what the clock advances, and what decides a session is worth
    // reopening. Eviction deliberately asks the narrower pair instead -- currentSeqLocked() and
    // screenDrawnLocked() -- because a consumer that has not fetched for a moment must not cost its
    // screen the decoder it is visibly playing. Caller must hold m_stateMutex.
    std::optional<uint64_t> playingSeqLocked(int monitorId, uint64_t wallpaperSeq) const;

    // Whether this monitor's pause setting, against what its screen last reported, stops its video
    // now. Caller must hold m_stateMutex.
    bool pausedLocked(int monitorId) const;

    // MonitorSoundStatus::heard for this monitor. Caller must hold m_stateMutex.
    bool soundHeardLocked(int monitorId) const;

    // Ends the clean-play stretch of a monitor that is paused now. Called by every change that can
    // pause one, so a pause and its end landing between two clock rounds is not counted as played.
    // Caller must hold m_stateMutex.
    void endPlayIfPausedLocked(int monitorId);

    // Whether anything can see this monitor: its screen is being drawn and nothing it published is
    // sitting uncollected. Reads the clock itself, so no caller can ask with a stale one. Caller
    // must hold m_stateMutex.
    bool watchedLocked(int monitorId, const MonitorState& state) const;
    // When watchedLocked() turns false with nothing but time passing; nothing when time alone never
    // turns it. Caller must hold m_stateMutex.
    std::optional<std::chrono::steady_clock::time_point> unwatchedAtLocked(const MonitorState& state) const;

    // Whether this monitor's resume credit is unspent and could still be spent: it holds a frame of
    // a video that has more than one picture. Anything else -- a still, a one-picture file, a
    // failed render -- will never publish the picture that spends it, and would read as watched
    // for ever. Caller must hold m_stateMutex.
    static bool resumeCreditStands(const MonitorState& state);

    // Whether the screen bound to this monitor is being drawn, which is true of one that has never
    // said otherwise. Caller must hold m_stateMutex.
    bool screenDrawnLocked(int monitorId) const;

    // Records that a hand-over delivered to `client` collected this monitor's newest picture, which
    // is what makes it watched, and returns handoverDelivered()'s answer. Nothing for an unnamed
    // caller: a diagnostic poll no claim can be charged to must not keep a video decoding. Caller
    // must hold m_gpuMutex and not m_stateMutex.
    bool recordDeliveredLocked(int monitorId, const std::string& client) noexcept;

    // Posts the videos that would be playing if they had a decode session: one whose session was
    // freed while nobody was looking, and -- only when this round freed one, so a failure cannot
    // spin -- one that never got a session because every slot was taken. Caller must hold
    // m_gpuMutex and not m_stateMutex.
    void reopenVideosLocked(std::chrono::steady_clock::time_point now, bool freedSession, ClockRound& round);

    // Closes the least recently drawn session whose monitor is not playing, to make room for
    // another. False when every open session is playing, which is the only case a caller cannot
    // resolve. Caller must hold m_gpuMutex and not m_stateMutex.
    bool evictIdleVideoSessionLocked();

    // Runs `attempt`, and when it fails for want of something a closed session gives back -- memory,
    // a decoder, the file descriptor its video holds -- closes an idle one and runs it once more.
    // What the second attempt throws, or the first when nothing could be closed, propagates. Caller
    // must hold m_gpuMutex and not m_stateMutex.
    template <typename Attempt>
    void retryAfterEvictionLocked(Attempt&& attempt);

    // The session to draw this monitor's video with, opened and decoded into when it has none for
    // this wallpaper -- so a re-post costs the blit of the picture already held. One opened where a
    // released session stood is left CatchingUp, with nothing decoded. Never advances a
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

    // The memory, unless `sendMemory` is false, and its acquire -- a value on `timeline` when the
    // consumer has one, a new binary semaphore otherwise -- or neither and a throw with the GPU
    // health recorded. Caller must hold m_gpuMutex, which is what keeps `target` and `timeline`
    // alive across the export.
    FrameHandle exportHandle(const OffscreenTarget& target, AcquireTimeline* timeline, bool sendMemory);

    // The images `monitorId` has handed out and not got back. Caller must hold m_stateMutex.
    std::vector<uint64_t> heldBuffersLocked(int monitorId) const;

    // The claims that would be in force if `client` were handed `bufferId` as hand-over `handover`, in
    // a reply carrying `descriptors`, or Refused when a cap stands in the way. `client` must not be
    // empty, and `handover` must be newer than every number recorded. Pure, and it only ever adds:
    // its result is both what the reserve is tested against and what is stored, so the admission
    // test cannot disagree with the bookkeeping, and no caller can end a claim through it.
    static Handover withHandover(std::vector<BufferHold> holds, const std::string& client, uint64_t bufferId,
                                 uint64_t handover, uint32_t descriptors);

    // Gives back `client`'s hand-overs of `bufferId` numbered up to `handover`, as releaseBuffer()
    // describes. Reports whether that ended the claim. Caller must hold m_stateMutex.
    static bool releaseHandoversLocked(MonitorState& state, const std::string& client, uint64_t bufferId,
                                       uint64_t handover);

    // Takes back `record`, for a reply that was never sent: the claim ends if no earlier hand-over
    // is outstanding, and otherwise only its charge goes -- an image the client already held is one
    // it is still reading. Nothing if a release ended the claim meanwhile. Reports whether the claim
    // ended. Caller must hold m_stateMutex.
    static bool undoHandoverLocked(MonitorState& state, const std::string& client, uint64_t bufferId,
                                   const HandoverRecord& record);

    // Descriptors `client` has been sent and not given back, as kMaxDescriptorsPerClient counts
    // them. Caller must hold m_stateMutex.
    uint32_t descriptorsInFlightLocked(const std::string& client) const;

    // Whether `client` would pass kMaxDescriptorsPerClient with `more`. When it would, it is recorded
    // as refused and the result says whether that starts a new episode; when not, the episode
    // ends. Caller must hold m_stateMutex.
    struct DescriptorAdmission {
        bool admitted = true;
        bool firstRefusal = false;
    };
    DescriptorAdmission admitDescriptorsLocked(const std::string& client, uint32_t more);

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

    // What the next save writes: the kept rows merged with live state, and the root settings. Caller
    // must hold m_stateMutex.
    PersistedConfig configToWriteLocked() const;

    // What committing a persisted setting did. Skipped: unchanged and known to be on disk, so nothing
    // was queued. Rewritten: unchanged, but queued, since the file is not known to hold it.
    enum class SettingCommit { Skipped, Rewritten, Changed };
    // Puts `next` in a persisted setting and queues the save, all or nothing: a save that cannot be
    // queued puts the old value back and rethrows. The root's setting is text, empty for absent; a
    // monitor's is an entry in `settings`, nothing for absent. Caller must hold m_stateMutex, and
    // waits for the write after releasing it unless Skipped.
    SettingCommit commitSettingLocked(std::string& setting, std::string next);
    SettingCommit commitSettingLocked(std::map<int, std::string>& settings, int monitorId,
                                      std::optional<std::string> next);
    // Both through this: `exchange` swaps the old and new values, so calling it again undoes it.
    template <class Exchange>
    SettingCommit commitLocked(bool changed, Exchange exchange);

    // The persistence report, over that live state. Same locking rule.
    std::string keptDetail(const std::string& base) const;

    // Hands `entries` to the writer without waiting; a refusal or a missing thread reaches
    // persistence health through the writer's outcome. Never blocks, so a change can be queued
    // under m_stateMutex and undone there if this throws: nothing is handed over in that case.
    void queuePersist(PersistedConfig config);

    // The writer's bounded wait for what was last queued. Returns whether it is known to be on
    // disk; on false, `whyNot` receives that write's own reason, sanitized. Parks the calling
    // thread for up to the writer's deadline, so never under m_stateMutex -- and never at
    // startup, which is why restoreSaved()'s repair is only queued.
    bool awaitPersisted(std::string& whyNot);
    // The same wait with no answer, for a caller that has already committed its change and so
    // must not throw.
    void awaitPersisted() noexcept;

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
    std::optional<PersistedConfig> m_repair;

    // The counts restoreSaved() carried forward, until recordStartingVideos() puts them on disk.
    // Written and read before the event loop starts.
    std::vector<PlayingVideo> m_startingVideos;
    PlayingRecord m_playing;

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

    // Each consumer's acquire timeline, by DBus unique name. Split between the two locks: the list
    // and `departed` are m_stateMutex's, so the bus thread can mark a departure without touching the
    // GPU; the timeline object is created, signalled and destroyed only under m_gpuMutex. An entry is
    // removed only under both, which is what keeps a pointer the GPU thread holds valid. After
    // m_device, which the timelines reference.
    struct ConsumerTimeline {
        std::string client;
        std::unique_ptr<AcquireTimeline> timeline;
        bool departed = false;
        uint32_t copiesSent = 0;
    };
    std::vector<ConsumerTimeline> m_acquireTimelines;

    // Consumers refused by kMaxDescriptorsPerClient and not back under it since, so each run of
    // refusals is logged once. Entries go when the consumer is next admitted or leaves the bus.
    // Guarded by m_stateMutex.
    std::set<std::string> m_descriptorRefusals;

    // The number of the latest hand-over recorded, across every monitor and consumer; 0 before the
    // first. Guarded by m_stateMutex.
    uint64_t m_lastHandover = 0;

    // The live timeline `client` registered, or null. Caller holds m_gpuMutex and not m_stateMutex.
    AcquireTimeline* liveAcquireTimeline(const std::string& client);
    // Its entry, or null. Caller holds m_stateMutex; the pointer stays valid while m_gpuMutex is held.
    ConsumerTimeline* liveTimelineEntryLocked(const std::string& client);

    // Destroys the timelines of consumers that have left. Caller holds m_gpuMutex and not
    // m_stateMutex: a timeline whose last signal did not finish waits for it.
    void reapDepartedTimelinesLocked();

    // See admitRequest(). Guarded by m_stateMutex. An entry lives while its client has a request
    // admitted and unanswered, or is in a run of refusals and still on the bus.
    struct QueuedRequests {
        int count = 0;
        bool departed = false;
        std::optional<std::chrono::steady_clock::time_point> lastRefusal;
        bool refusing(std::chrono::steady_clock::time_point now) const;
    };
    // A run of refusals ends after this long without one.
    static constexpr std::chrono::seconds kRequestRefusalQuiet{10};
    std::map<std::string, QueuedRequests> m_queuedRequests;
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
        // What names this session to the fatal-signal handler while it is being worked on. On the
        // heap so its bytes stay put when the session is moved: the handler holds their address.
        std::unique_ptr<const std::string> blame;
        std::unique_ptr<VideoPipeline> pipeline;
        // The MonitorState::releasedAt this session was opened at: the picture on screen until the
        // session draws one of its own.
        std::optional<StreamPosition> openedAt;

        // The held picture: the first, never drawn; the one on screen, whose successor is decoded
        // next; or that successor, waiting for `due`. `timeline` is a picture's place on the file's
        // own schedule, which the next is timed from; `due` is when it may be drawn, which the cap
        // holds at least kMinPictureInterval after the previous picture's `due` but which never
        // moves the schedule. Measured from `due` and not from the draw, so wake-up latency is not
        // charged to the next picture. A pass drawing a waiting picture shows it early and leaves
        // the schedule where it was.
        //
        // CatchingUp is a session opened at `openedAt` that has not yet decoded the picture there. It
        // holds no picture: the monitor's frame is the one on screen when the session it replaces was
        // released, which is that same picture. Clock rounds decode toward it a slice at a time.
        struct CatchingUp {};
        struct Undrawn {};
        struct OnScreen {
            std::chrono::steady_clock::time_point timeline;
            std::chrono::steady_clock::time_point due;
        };
        // Keeps what is left of its distance from the picture on screen in the file's own time,
        // running at `speed` from `from`. A speed change re-times the rest of the wait and keeps
        // the part already waited, so the picture neither leaves at the old rate nor becomes due
        // at a moment that had already passed when the speed changed.
        struct Waiting {
            OnScreen previous;
            std::chrono::steady_clock::time_point from;
            std::chrono::nanoseconds left{};
            double speed = 1.0;
            // Where the file is, on the pass of the picture on screen, when the wait ends. Not the
            // waiting picture's own time when that picture starts a new pass. Empty when the picture
            // on screen has no time.
            std::optional<std::chrono::nanoseconds> reaches;
            // When a round first left a picture the sound had played past still held, its slice
            // spent: the screen has been kept on its picture since, to land on the sound rather than
            // run through late ones. Ends when the picture is no longer behind, or is drawn.
            std::optional<std::chrono::steady_clock::time_point> catchingUpSince;

            // Re-times the rest of the wait from `now` when `speed` differs. A picture already
            // due stays due where it was.
            void retime(std::chrono::steady_clock::time_point now, double newSpeed);
            OnScreen planned() const;
            // Where the waiting picture stands once drawn at `now`, `interval` being its own at the
            // current speed.
            OnScreen shownAt(std::chrono::steady_clock::time_point now, std::chrono::nanoseconds interval) const;
        };
        std::variant<Undrawn, OnScreen, Waiting, CatchingUp> clock;

        // When the clock first found this session's monitor not playing, or nothing while it is.
        // The whole off switch: a video nobody can see stops at once and keeps its picture, and
        // this is what says how long it has been stopped, whatever stopped it. Which release delay
        // runs from it is decided each round by what stops it then.
        std::optional<std::chrono::steady_clock::time_point> idleSince;

        // The file's sound, made the first time its screen is heard and kept, silent, while it is
        // not. Its anchor is the one record of where the sound was told to be.
        std::unique_ptr<AudioTrack> audio;
        // Where every pass through the file starts and ends in its own time, from the first loop on,
        // for a pass long enough that its sound is handed to the next ahead of time.
        struct Pass {
            std::chrono::nanoseconds first{};
            std::chrono::nanoseconds end{};
        };
        std::optional<Pass> pass;
        // The sound's last clock reading -- the file's time `file` heard at `at` -- and the track
        // request it was read for: where the sound is known to be while its clock is missing. A newer
        // request makes it no one's.
        struct Heard {
            uint64_t request = 0;
            std::chrono::nanoseconds file{};
            std::chrono::steady_clock::time_point at;
        };
        std::optional<Heard> heard;

        // How long pictures take to decode against the time they are shown for, averaged over several so
        // that one slow decode -- a key picture, a wait on the GPU queue, a stalled read -- is not taken
        // for a file that cannot keep up; no state is entered before its average holds kSettled samples.
        // Each is entered at 1 and left under kKeepsUp, so a file decoding at about its own rate does
        // not flip between two every few pictures.
        struct DecodeLoad {
            struct Average {
                double value = 0;
                uint32_t samples = 0;  // up to kSettled
                void add(double load);
            };
            // Against how long each picture is shown: decoding every picture, a skipped one taken to cost
            // what a decoded one does.
            Average everyPicture;
            // Against the time shown since the picture before: the decoder as it runs, restarted when it
            // starts skipping.
            Average asRun;
            enum class State {
                Keeps,   // every picture decodes in real time
                Skips,   // only with unreferenced pictures skipped, and then keeps pace
                Behind,  // skipping, and not yet shown to keep pace: the pictures keep their own clock
            };
            State state = State::Keeps;
            static constexpr double kKeepsUp = 0.9;
            // One averaging window: the samples an average holds before it moves the state.
            static constexpr uint32_t kSettled = 8;
            void add(std::chrono::nanoseconds took, std::chrono::nanoseconds shownFor, std::chrono::nanoseconds covered);
        };
        DecodeLoad decodeLoad;
        // What the decoder may leave out: the pictures nothing references while decoding them all
        // cannot keep up.
        PictureSkip skip() const {
            return decodeLoad.state == DecodeLoad::State::Keeps ? PictureSkip::Nothing : PictureSkip::Unreferenced;
        }

        // The file's average rate is above the picture-rate cap, so its pictures play slower than its
        // sound would: it plays silent until sound can be stretched. Set once, at the open.
        bool slowedByCap = false;
    };
    // Before the output, whose thread calls it until the output is destroyed.
    std::mutex m_clockWakeMutex;
    std::function<void()> m_clockWake;
    // Before the sessions, whose tracks it must outlive.
    std::unique_ptr<AudioOutput> m_audio;
    std::map<int, VideoSession> m_videoSessions;

    // What a session's monitor lets it do, the reasons its pictures stand still told apart where its
    // sound treats them differently: a render owed redraws the same video and leaves the sound be,
    // and a file whose one picture is on screen plays its sound alone. Stopped is everything else:
    // paused, unwatched, no frame, or another wallpaper set. Caller holds m_stateMutex.
    enum class Motion { Playing, RenderOwed, Parked, Stopped };
    Motion motionLocked(int monitorId, uint64_t wallpaperSeq) const;

    // What a session's sound should do this round. `heard` is soundHeardLocked().
    struct SoundWanted {
        bool heard = false;
        double speed = 1.0;
        float volume = 1.0f;
        // Whatever the file: any speed but its own would need the sound stretched, which nothing
        // here does yet. At volume 0 nothing is heard, so nothing is decoded or kept open for it.
        bool audible() const { return heard && speed == 1.0 && volume > 0.0f; }
    };
    // Whether the session's sound is to be heard, given what its monitor wants.
    static bool soundWanted(const VideoSession& session, const SoundWanted& wanted);
    // The session's track, made if need be and at the wanted volume, or null -- having stopped any
    // -- when its sound is not to be heard. Throws only on making a track. Caller holds m_gpuMutex
    // and not m_stateMutex.
    AudioTrack* heardTrackLocked(int monitorId, VideoSession& session, const SoundWanted& wanted);
    // What steering the sound made of the waiting picture.
    struct SoundPace {
        // The sound's clock timed it this round, so once the sound has played past it, it is dropped.
        bool timesPicture = false;
        // A round wanted before the picture is due, to hand the sound to the next pass.
        std::optional<std::chrono::steady_clock::time_point> roundAt;
    };
    // Whether the waiting picture's catch-up hold has run out with the pictures still behind the sound.
    enum class CatchUp { Chasing, HoldRunOut };
    // Starts a playing session's sound on the picture schedule `waiting` belongs to, hands it to each
    // new pass, or silences it. While the sound is heard playing, `waiting` is moved to it, so the
    // pictures follow the sound card; a sound that falls behind the pictures is moved to them instead,
    // as is one too far ahead of them once `catchUp` says they failed to reach it or no clock lets
    // them chase it, judged while it is not heard playing from where it was last heard. A file that
    // cannot decode in real time even skipping is left to drift until its next pass.
    // Never fails the session: sound is lost before a picture is. Caller holds m_gpuMutex and not
    // m_stateMutex.
    SoundPace steerSoundLocked(int monitorId, VideoSession& session, VideoSession::Waiting& waiting,
                               const SoundWanted& wanted, std::chrono::steady_clock::time_point now,
                               CatchUp catchUp) noexcept;
    // Plays a parked session's sound on its own, looping, when it would be heard, and returns its
    // track; null, with the sound stopped, otherwise. Caller holds m_gpuMutex and not m_stateMutex.
    AudioTrack* loopParkedSoundLocked(int monitorId, VideoSession& session, const SoundWanted& wanted,
                                      std::chrono::steady_clock::time_point now) noexcept;

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

    // Monitor id -> its pause setting as the config row carries it, JSON text; absent means play.
    // Text so a setting a newer build wrote is kept as read -- pauseModeFromJson() says which mode
    // it is, if any. Guarded by m_stateMutex.
    std::map<int, std::string> m_pauseSettings;

    // The global sound setting as the config root carries it, and each monitor's as its row does:
    // JSON text, absent or empty meaning on, kept as read for pause's reason (sound_setting.h).
    // Guarded by m_stateMutex.
    std::string m_sound;
    std::map<int, std::string> m_soundSettings;

    // What each screen reported about its windows, by name for m_outputsNotDrawn's reason, and who
    // reported it, so the report dies with its reporter. Only a report of some focus or a maximized
    // window is held: absent means neither. Every key is a bound name -- bindOutputLocked() drops a
    // name's report with its binding. Guarded by m_stateMutex.
    struct FocusReport {
        std::string reporter;
        ScreenFocus focus = ScreenFocus::None;
        bool windowMaximized = false;
    };
    std::map<std::string, FocusReport> m_outputFocus;

    // The report for the screen bound to this monitor, or null. Caller must hold m_stateMutex.
    const FocusReport* focusReportLocked(int monitorId) const;

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
