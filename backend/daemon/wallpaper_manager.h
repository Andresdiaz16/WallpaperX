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
// monitor, and the per-monitor bookkeeping behind SetWallpaper/GetFrameFd. Split into a
// request phase and a render phase, because rendering is slow and the DBus event loop is
// single-threaded.
//
// Thread-safe, under two locks with a strict order: m_gpuMutex may be taken before
// m_stateMutex, never the reverse.
//
//   m_gpuMutex   guards the Device and the offscreen targets, and serialises all GPU
//                submission. Held for the whole of a render. It is also what makes export
//                safe: OffscreenTarget's exportLayout/externalOwner are written through a
//                const reference by *both* render and export.
//   m_stateMutex guards the per-monitor bookkeeping only, where every operation is a map
//                lookup or a string copy, so GetStatus never waits on a render.
class WallpaperManager {
public:
    // Upper bound on monitor ids. A successful render retains a full-size exportable image
    // (~9 MB at 1080p) until the daemon exits, so an unbounded id space lets any client on the
    // session bus walk ids until the GPU allocator fails. The cap makes the worst case finite
    // rather than being tight.
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

    // Records a request. Fast and GPU-free. Throws InvalidRequest for a malformed request;
    // everything that can only fail on the GPU fails later, in renderPending().
    //
    // The caller must post the monitor to the render worker afterwards, including for
    // non-static types -- those still need their target released.
    void requestWallpaper(const std::string& type, const std::string& path, int monitorId);

    // Records the connector name a monitor is known by -- "DP-3". Throws InvalidRequest for
    // a bad id or a name outputNameError() rejects. No render and no GPU work.
    //
    // Idempotent, and a call changing nothing does not write: clients are invited to
    // re-assert names, and the write parks the calling thread. Throws std::runtime_error when
    // the name did not reach the config file -- the binding stands for this run regardless,
    // but only a write makes it survive a restart, and the caller is the only one who can say
    // so at the moment it happens.
    void setMonitorOutput(int monitorId, const std::string& outputName);

    // Applies the config file's wallpapers, returning the monitor ids to post to the worker.
    // Call once. Writes nothing: a repair it computed is held for writeRepairedConfig().
    std::vector<int> restoreSaved();

    // Moves an unreadable config aside, or copies one this run rejected rows from or is about
    // to rewrite, and updates the persistence health with the outcome. Separate from the
    // constructor because it writes: a duplicate instance would otherwise move the running
    // daemon's config before finding out it cannot run. Call once, after the bus name is held.
    // Until it runs, saving stays refused.
    void preserveUnreadableConfig();

    // Queues the repair restoreSaved() computed, if it computed one, and does not wait for the
    // write: the caller has claimed the bus name and has no event loop yet. A failed write is
    // reported on stderr by the writer itself. Call once, after preserveUnreadableConfig():
    // that call is what copies the bytes this rewrite replaces, and with no copy every write --
    // this one and every later save -- is refused until one is taken.
    void writeRepairedConfig();

    // Renders whatever request the monitor currently holds. Called on the render worker's
    // thread; never on the DBus thread.
    //
    // Returns rendered=true only when a frame was produced *and* is still the one that was
    // asked for: a request arriving mid-render supersedes this one, and committing it anyway
    // would flag the monitor as holding pixels from the previous wallpaper. Failures are
    // recorded in the monitor's lastError, not thrown -- nobody is waiting.
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
    // stays readable after this manager and its Device are destroyed, which is when the layer's
    // teardown complaints are emitted.
    //
    // NOT surfaced by GetStatus, and both take m_gpuMutex -- a render holds it for its whole
    // duration, so wiring them into status as-is would make status wait on an image decode.
    std::shared_ptr<const std::atomic<int>> validationCounter() const;
    bool validationLayerActive() const;

private:
    struct MonitorState {
        std::string type;
        std::string path;

        // Counts accepted requests, where generation counts completed renders: the worker
        // snapshots requestSeq before rendering and refuses to commit if it has moved.
        uint64_t requestSeq = 0;

        // The requestSeq a render pass has already been run for, successfully or not, which is
        // what makes renderPending() idempotent per request. The worker's queue can
        // legitimately hold a monitor twice, and a second pass would burn a generation number
        // and emit a second FrameReady for a frame the consumer already imported.
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

    // Displaced: the bind took the name off another monitor, or took another name off this one.
    enum class BindResult { Unchanged, Bound, Displaced };

    // Binds a name to a monitor, releasing any binding either side already had. Caller must
    // hold m_stateMutex and must have validated the name.
    BindResult bindOutputLocked(int monitorId, const std::string& outputName);

    // Hands `entries` to the config writer and returns it, or null when nothing was queued
    // (persistence disabled, an unread config, no writer thread). Must NOT be called holding
    // m_stateMutex: it takes that lock itself.
    std::shared_ptr<PersistWorker> queuePersist(const std::vector<PersistedWallpaper>& entries);

    // queuePersist(), then a bounded wait for the write to land. The wait is what lets the
    // reply to SetWallpaper mean the choice is on disk -- but only when it ends because the
    // write landed: a wait that runs out leaves the write in flight and records persistence as
    // unsettled instead. Same locking rule.
    //
    // Returns whether THESE entries are known to be on disk -- true when persistence is
    // disabled, since durability was never promised then. A caller that must answer for its own
    // write asks this rather than the persistence health, which is an aggregate and carries
    // failures belonging to other rows. On false, `whyNot` receives that write's own reason:
    // the health field does not hold it, because a completed write's error is composed into a
    // caller's copy at snapshot time and recorded nowhere.
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

    // THE answer to "does the file hold what live state holds, and if not, why" -- one
    // derivation, read by everything that needs it: the health field, the reply to
    // SetMonitorOutput, and the idempotent re-assert shortcut.
    //
    // It is derived rather than stored because it cannot be stored: a completed write's result
    // lives in the worker, which outlives this object and must never write into it. What went
    // wrong before was not the derivation but that there were four of them -- one per caller,
    // each consulting a different subset of a null pointer, three bools and the worker's four
    // progress fields, so callers disagreed about the same write. Every defect of that shape
    // came back in a new costume until they were made one function.
    struct PersistOutcome {
        // No write is outstanding, so `onDisk` describes the file as it now stands rather than
        // as it stood before work that has not finished.
        bool settled = true;
        // The file holds what live state holds. True before anything is asked of the file: an
        // unchanged binding then came out of the config, so the file already agrees.
        bool onDisk = true;
        // Why not, when !onDisk. Raw; sanitized where it is recorded or thrown.
        std::string why;
    };

    // Caller must hold m_stateMutex; takes the worker's lock, which is the established order.
    PersistOutcome persistOutcomeLocked() const;

    // Whether a write is in flight and older than the deadline -- a stalled filesystem, which
    // earns its own wording. Same locking rule.
    bool stalledWriteLocked() const;

    // Folds that outcome, and the losses that outlive it, into a health copy. Caller must hold
    // m_stateMutex. Not recorded into m_health, because the writer cannot touch this object.
    void applyPersistOutcomeLocked(DaemonHealth& health) const;

    const Config m_config;
    const ConfigStore m_store;

    // What load() returned, until restoreSaved() consumes it.
    std::vector<PersistedWallpaper> m_restore;

    // Entries restoreSaved() could not apply, carried so a save does not delete them.
    // Guarded by m_stateMutex.
    std::vector<PersistedWallpaper> m_unapplied;

    // The repair restoreSaved() computed, until writeRepairedConfig() takes it out.
    // Guarded by m_stateMutex.
    std::optional<std::vector<PersistedWallpaper>> m_repair;

    // The file still holds content this run could not read and could not move aside, so
    // persist() must not replace it. Written only by the constructor, read afterwards.
    bool m_configUnread = false;

    // A copy of the pre-repair file exists, so writeRepairedConfig() may replace it. Written by
    // preserveUnreadableConfig(), read by writeRepairedConfig(); both run before the event loop.
    bool m_repairCopied = false;

    // A copy this run needed could not be taken, so the rewrite that follows removed rows held
    // nowhere else. Sticky for the life of the process: a later write succeeding says nothing
    // about what the first one destroyed, and health flipping back to ok is what hid it. Set by
    // preserveUnreadableConfig() before the event loop; read under m_stateMutex.
    bool m_preserveFailed = false;

    // Raw, unsanitized; sanitized once when recorded. Written by the constructor, by
    // preserveUnreadableConfig() and by restoreSaved(), all before the event loop starts, and by
    // requestWallpaper() under m_stateMutex once per kept row it displaces; read under
    // m_stateMutex.
    std::string m_persistenceNote;

    // Started on the first save rather than at construction, so a daemon that never sets a
    // wallpaper never spawns it. Detached, never joined: see PersistWorker. Guarded by
    // m_stateMutex; non-null means a thread is servicing it.
    std::shared_ptr<PersistWorker> m_persist;

    // Why the last save never reached the writer at all -- no thread, or a refusal decided
    // before one exists. Empty when the last save was handed over. This is the half of the
    // outcome the worker cannot know about; persistOutcomeLocked() joins it to the half only
    // the worker knows. Guarded by m_stateMutex.
    std::string m_notWritten;

    // Entries no build can apply, dropped rather than kept, counted so the loss is reported at
    // all. Guarded by m_stateMutex. Name-only rows counted apart: the report these feed is the
    // user's notice that a picture did not come back, and a name never was one.
    int m_droppedEntries = 0;
    int m_droppedNames = 0;

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
