// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "config_store.h"
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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wallpaperx {

class Device;
class OffscreenTargetManager;
struct OffscreenTarget;

// A request that was malformed rather than one that failed to render. Its own type so the
// DBus layer can answer with InvalidArgs instead of a generic failure.
struct InvalidRequest : std::runtime_error {
    using std::runtime_error::runtime_error;
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

    // Why the most recent request produced no frame, or empty if it produced one or is still
    // rendering. The only channel a failure has: SetWallpaper replies before the render runs.
    std::string lastError;

    // Whether that failure is the daemon's to answer for, as opposed to the image being
    // unreadable. Only the daemon's own failures make /health report a broken daemon.
    bool lastErrorIsDaemonFault = false;

    // A render has been accepted and no pass has run for it yet -- "the daemon owes an answer",
    // as against "this monitor has no picture", which is also true of a superseded render.
    bool renderOutstanding = false;
};

// The config writer's thread and everything it touches. Held by shared_ptr and deliberately
// free of any reference back to the manager: a write on a stalled mount cannot be interrupted,
// so the thread must be abandonable, and abandoning one that could reach the manager would be
// a use-after-free waiting for the mount to recover.
struct PersistWorker {
    std::mutex mutex;

    // wake: the worker waiting for something to write. settled: whoever queued it waiting
    // for the write to land, which is what keeps a healthy save synchronous.
    std::condition_variable wake;
    std::condition_variable settled;

    // Set when a wait ran out, cleared when the filesystem answers again. Stops every later
    // save paying the same deadline for an answer this one already has.
    bool stalled = false;

    // One slot, not a queue: bus handlers run one at a time, so the newest snapshot already
    // describes everything the older ones would have written.
    std::optional<std::vector<PersistedWallpaper>> pending;

    // Set when a snapshot is handed over, cleared when the write returns. A value that stays
    // put is what tells the health report the filesystem has stopped answering.
    std::optional<std::chrono::steady_clock::time_point> writingSince;

    bool haveResult = false;
    SaveResult result;
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

// An exported frame plus the generation counter of the render that produced it.
// The caller owns frame.fd and must close it (or hand it to something that will).
struct FrameHandle {
    ExportedFrame frame;
    uint64_t generation = 0;
};

// The single answer to "can this build draw this type", so the render dispatch, the
// GetFrameFd reason and the GetStatus field cannot drift apart.
inline bool isRenderableType(const std::string& type) { return type == "static"; }

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
    Unsupported,     // the recorded type (video, shader) cannot be rendered by this build,
                     // so no amount of waiting or retrying will produce a frame
};

// The result of asking for a monitor's current frame: the frame, or the reason there
// isn't one.
struct FrameExport {
    std::optional<FrameHandle> handle;

    // Both meaningful only when `handle` is empty. lastError is non-empty exactly when
    // reason is RenderFailed -- Unsupported deliberately does not carry one, because it
    // is not a failure and the reason name already states the whole fact.
    NoFrameReason reason = NoFrameReason::NeverRequested;
    std::string lastError;
};

// What renderPending() did, so the caller knows whether to announce a new frame.
struct RenderOutcome {
    bool rendered = false;
    uint64_t generation = 0;
};

// Owns the renderer for the daemon: one Vulkan Device, one OffscreenTargetManager keyed by
// monitor, and the per-monitor bookkeeping behind SetWallpaper/GetFrameFd. Request and render
// are separate phases because rendering is slow and the DBus event loop is single-threaded.
//
// Two locks, strictly ordered: m_gpuMutex before m_stateMutex, never the reverse.
//
//   m_gpuMutex   the Device, the offscreen targets, and all GPU submission; held for a whole
//                render. Also what makes export safe: OffscreenTarget's exportLayout and
//                externalOwner are written through a const reference by render AND export.
//   m_stateMutex the per-monitor bookkeeping only -- a map lookup or a string copy -- so
//                GetStatus never waits on a render.
class WallpaperManager {
public:
    // Bounds worst-case GPU memory: each successful render retains a ~9 MB exportable image
    // until the daemon exits, so an unbounded id space is an unbounded allocation any client
    // on the session bus can drive. Finite, not tight.
    static constexpr int kMaxMonitors = 32;

    struct Config {
        // Placeholder geometry: no source of real monitor dimensions exists until the app
        // supplies them via KScreen (Step 15). Tests override it to keep software-rasterised
        // runs fast.
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
    };

    explicit WallpaperManager(Config config);
    ~WallpaperManager();

    WallpaperManager(const WallpaperManager&) = delete;
    WallpaperManager& operator=(const WallpaperManager&) = delete;

    // Records a request; GPU-free. Throws InvalidRequest for a malformed one, and everything
    // that can only fail on the GPU fails later in renderPending(). The caller must then post
    // the monitor to the render worker, non-static types included -- those still need their
    // target released.
    void requestWallpaper(const std::string& type, const std::string& path, int monitorId);

    // Records the connector name a monitor is known by -- "DP-3". Throws InvalidRequest for a
    // bad id or a name outputNameError() rejects, and std::runtime_error when the name did not
    // reach the config file: the binding stands for this run either way, and only the write
    // makes it outlive a restart. Idempotent; a call that changes nothing already on disk does
    // not write, since the write parks the calling thread.
    void setMonitorOutput(int monitorId, const std::string& outputName);

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

    // Exports the monitor's most recent frame as a fresh fd. An empty handle means there is
    // nothing to export, with `reason` saying which state that is; a thrown exception means
    // the export itself went wrong.
    FrameExport exportCurrentFrame(int monitorId);

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

        uint64_t generation = 0;
        bool hasFrame = false;
        std::string lastError;
        bool lastErrorIsDaemonFault = false;

        // Copied from the target at commit time, so status() can report the frame's size
        // without touching GPU state and therefore without waiting behind a render.
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // Creates the Device and target manager on first use, so a machine with no working Vulkan
    // still starts and reports a clean error. Called with m_gpuMutex held.
    void ensureGpu();

    // Both called with m_gpuMutex held.
    const OffscreenTarget& acquireTarget(int monitorId);
    void releaseTarget(int monitorId);

    // Reads the config on a thread this can walk away from. Linux gives no way to bound a
    // filesystem call -- a hard network mount is uninterruptible by design -- so the only
    // way to stop a stalled read holding startup is to stop waiting for it.
    LoadResult loadWithDeadline() const;

    // Snapshots the monitors worth persisting. Caller must hold m_stateMutex.
    std::vector<PersistedWallpaper> persistableLocked() const;

    // Clears the wallpaper half of a kept row, answering whether there was one to clear -- the
    // caller reports the loss, and only when there was one. Takes the row, not an id: one caller
    // holds a copy that is not in m_unapplied yet. Says nothing about `output`: a lost name is a
    // different loss, counted separately.
    static bool dropKeptWallpaper(PersistedWallpaper& row);

    // Whether a live monitor holds a wallpaper for this id, which is what decides that a kept
    // row for the same id will not survive the next save. Caller must hold m_stateMutex.
    bool liveHoldsLocked(int monitorId) const;

    // Displaced: the bind took the name off another monitor, or took another name off this one.
    enum class BindResult { Unchanged, Bound, Displaced };

    // Binds a name to a monitor, releasing any binding either side already had. Caller must
    // hold m_stateMutex and must have validated the name.
    BindResult bindOutputLocked(int monitorId, const std::string& outputName);

    // Hands `entries` to the config writer and returns it, or null when nothing was queued
    // (persistence disabled, an unread config, no writer thread). Must NOT be called holding
    // m_stateMutex: it takes that lock itself.
    std::shared_ptr<PersistWorker> queuePersist(const std::vector<PersistedWallpaper>& entries);

    // queuePersist(), then a bounded wait: a wait that runs out leaves the write in flight and
    // records persistence unsettled. Same locking rule.
    //
    // Returns whether THESE entries are known to be on disk, true when persistence is disabled
    // since durability was never promised. A caller answering for its own write asks this
    // rather than the persistence health, which is an aggregate carrying other rows' failures.
    // On false, `whyNot` receives that write's own reason.
    bool persist(const std::vector<PersistedWallpaper>& entries, std::string* whyNot = nullptr);

    // Facts about the config that outlive the operation that discovered them -- a copy of an
    // unreadable file, entries kept for a newer build. Every persistence detail is rebuilt
    // through these, or the next save would report only itself and drop the pointer to the copy.
    void addPersistenceNote(const std::string& note);
    std::string persistenceDetail(const std::string& base) const;

    // Copies the config to the first free <file>.bad[.N] in the state directory, or beside the
    // config when there is no usable state directory, and removes the original when `move`.
    // Returns the name used, or empty with `why` naming every directory tried. Numbered because
    // one name meant the second unreadable config had nowhere to go, and so was destroyed.
    std::string preserveConfigTo(bool move, std::string& why) const;


    // Spawns the config writer on first use, and leaves m_persist null when the thread could
    // not be created. Caller must hold m_stateMutex: m_persist is published here and read
    // under that lock.
    void startPersistWorkerLocked();

    // THE answer to "does the file hold what live state holds, and if not, why". Derived rather
    // than stored, since a completed write's result lives in the worker, which outlives this
    // object. One derivation for every caller, so no two can disagree about one write.
    struct PersistOutcome {
        bool settled = true;  // nothing outstanding, so onDisk describes the file as it stands
        bool onDisk = true;   // true before anything is asked of the file: it already agrees
        std::string why;      // when !onDisk. Raw; sanitized where recorded or thrown
    };

    // Caller must hold m_stateMutex; takes the worker's lock, which is the established order.
    PersistOutcome persistOutcomeLocked() const;

    // A write in flight past the deadline -- a stalled filesystem, which earns its own wording.
    bool stalledWriteLocked() const;

    // Folds that outcome, and the losses outliving it, into a health copy rather than m_health:
    // the writer cannot touch this object. Caller must hold m_stateMutex.
    void applyPersistOutcomeLocked(DaemonHealth& health) const;

    const Config m_config;
    const ConfigStore m_store;

    // Everything from here to the mutexes is guarded by m_stateMutex, except the load results
    // below, which are written before the event loop starts and only read after.

    // What load() returned, until restoreSaved() consumes it.
    std::vector<PersistedWallpaper> m_restore;

    // Entries restoreSaved() could not apply, carried so a save does not delete them.
    std::vector<PersistedWallpaper> m_unapplied;

    // The repair restoreSaved() computed, until writeRepairedConfig() takes it out.
    std::optional<std::vector<PersistedWallpaper>> m_repair;

    // The file holds content this run could not read and could not move aside, so persist()
    // must not replace it. Written only by the constructor.
    bool m_configUnread = false;

    // A copy of the pre-repair file exists. Written by preserveUnreadableConfig(), read by
    // writeRepairedConfig(); both run before the event loop.
    bool m_repairCopied = false;

    // A copy this run needed could not be taken, so the rewrite after it removed rows held
    // nowhere else. Sticky: a later write succeeding says nothing about what the first
    // destroyed, and health returning to ok is what hides it.
    bool m_preserveFailed = false;

    // Raw; sanitized once when recorded. Written by the constructor, preserveUnreadableConfig()
    // and restoreSaved() before the event loop, and by requestWallpaper() under m_stateMutex
    // once per kept row it displaces.
    std::string m_persistenceNote;

    // Started on the first save, so a daemon that never sets a wallpaper never spawns it.
    // Detached, never joined: see PersistWorker. Non-null means a thread is servicing it.
    std::shared_ptr<PersistWorker> m_persist;

    // Why the last save never reached the writer -- no thread, or a refusal decided before one
    // exists. Empty once a save is handed over. The half of the outcome the worker cannot know.
    std::string m_notWritten;

    // Entries no build can apply, dropped rather than kept, counted so the loss is reported at
    // all. Name-only rows counted apart: this report is the user's notice that a picture did
    // not come back, and a name never was one.
    int m_droppedEntries = 0;
    int m_droppedNames = 0;

    // Counted, not written into m_persistenceNote: these two are the only losses a client can
    // cause repeatedly, and the note is appended to for the life of the process while the
    // detail is truncated at its tail -- so a run of them would crowd out everything after.
    int m_replacedKept = 0;
    int m_takenOverNames = 0;

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

    // Guarded by m_stateMutex. m_gpuStatus is a copy of what the Device reported,
    // published once at initialisation, so status() can answer without touching the
    // Device and therefore without waiting for a render to finish.
    std::map<int, MonitorState> m_monitors;

    // Connector name -> monitor id, and the only authority on which is which. Guarded by
    // m_stateMutex.
    std::map<std::string, int> m_outputs;
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
