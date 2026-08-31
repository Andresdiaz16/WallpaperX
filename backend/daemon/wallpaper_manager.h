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

        // The memory layout the committed frame was built with, copied at commit time for the
        // same reason as the size: a bus handler must decide whether a new import-modifier
        // report invalidates this frame without taking the GPU lock. kDrmModifierInvalid means
        // the frame was exported as an opaque fd, which carries no layout for a report to
        // exclude. Meaningless unless hasFrame.
        uint64_t frameModifier = kDrmModifierInvalid;
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

    // Guarded by m_stateMutex. m_gpuStatus is a copy of what the Device reported,
    // published once at initialisation, so status() can answer without touching the
    // Device and therefore without waiting for a render to finish.
    std::map<int, MonitorState> m_monitors;

    // Connector name -> monitor id, and the only authority on which is which. Guarded by
    // m_stateMutex.
    std::map<std::string, int> m_outputs;

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
