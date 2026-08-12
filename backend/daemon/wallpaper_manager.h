#pragma once

#include "config_store.h"
#include "log_escape.h"
#include "vulkan/frame_export.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace wallpaperx {

class Device;
class OffscreenTargetManager;
struct OffscreenTarget;

// A request that was malformed rather than one that failed to render. Its own type
// so the DBus layer can answer with InvalidArgs instead of a generic failure --
// "you asked for something that cannot exist" and "I could not do what you asked"
// are different bugs on the client's side of the wire.
struct InvalidRequest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// What a monitor was last asked to display, and what came of it. `generation` is
// monotonic per monitor and never resets -- a consumer uses it to tell a fresh frame
// from a repeat, so reusing a number for different pixels would defeat it. `hasFrame`
// is therefore separate: a request that failed, or one for a type this build cannot
// render yet, leaves the counter alone and clears the flag rather than rewinding.
struct MonitorStatus {
    std::string type;
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;
    bool hasFrame = false;

    // Why the most recent request produced no frame, or empty if it produced one or is
    // still rendering. Rendering is asynchronous, so this is the only channel a failure
    // has: the DBus reply to SetWallpaper is sent before the render is attempted.
    std::string lastError;
};

// The GPU the daemon resolved, once it has resolved one.
struct GpuStatus {
    bool initialized = false;
    std::string name;
    std::string descriptorTier;
};

// Pending is a real answer: nothing has been tried yet. For shaders and gpu that means no
// render has happened -- which, since restoreSaved() renders at startup, now describes a
// daemon with no saved config rather than every fresh daemon.
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
    // whatever took the port, so there it could only ever read ok. GetStatus carries it --
    // the bus name cannot be taken from a running daemon, so DBus is the channel that can
    // still say "the thing on that port is not mine".
    HealthCheck configServer;

    // Whether the wallpaper choice will survive a restart. Separate from configServer,
    // which is the HTTP surface. A failed save leaves SetWallpaper succeeding -- the
    // wallpaper is on screen and a full disk is not a broken wallpaper -- so this is the
    // only channel that says the choice will be lost.
    HealthCheck persistence;
};

// An exported frame plus the generation counter of the render that produced it.
// The caller owns frame.fd and must close it (or hand it to something that will).
struct FrameHandle {
    ExportedFrame frame;
    uint64_t generation = 0;
};

// Why an export produced nothing. Carried out of the manager rather than re-derived by
// the caller, because the three cases are distinguishable only from state guarded by
// m_stateMutex, and a caller that took a second look would be reading a second, later
// truth -- the exact split this class exists to prevent.
//
// The distinction is not cosmetic. A consumer polls GetFrameFd, and "nothing yet, ask
// again" and "this wallpaper will never render" require opposite responses: retry, or
// stop and show the error. Collapsing them tells a client to retry a dead wallpaper
// forever.
// The single answer to "can this build draw this type", so the render dispatch, the
// GetFrameFd reason and the GetStatus field cannot drift apart. They did: the dispatch
// test and the reason test were two separate `type != "static"` literals, and Step 13
// making video renderable would have had to find both or GetFrameFd would go on telling
// clients a working wallpaper can never produce a frame.
inline bool isRenderableType(const std::string& type) { return type == "static"; }

// The path a client may act on: the bytes as stored, or nothing when they are not valid
// UTF-8 and so cannot be carried by JSON at all. Published beside the escaped `path`,
// which stays display-safe -- one definition because a value raw on one channel and
// escaped on the other is the defect aded2e5 fixed.
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

// Owns the renderer for the daemon: one Vulkan Device, one OffscreenTargetManager
// keyed by monitor, and the per-monitor bookkeeping behind SetWallpaper/GetFrameFd.
//
// Split into a request phase and a render phase, because rendering is slow and the DBus
// event loop is single-threaded. requestWallpaper() only records what was asked for and
// returns; renderPending() does the GPU work, and runs on the render worker's thread.
//
// Thread-safe, under two locks with a strict order: m_gpuMutex may be taken before
// m_stateMutex, never the reverse.
//
//   m_gpuMutex   guards the Device and the offscreen targets, and serialises all GPU
//                submission. Held for the whole of a render, which is the slow part.
//                It is also what makes the export path safe: OffscreenTarget's
//                exportLayout/externalOwner are written through a const reference by
//                *both* render and export, so both must hold this same lock.
//   m_stateMutex guards the per-monitor bookkeeping only. Every operation under it is
//                a map lookup or a string copy, so GetStatus and SetWallpaper never
//                wait on a render -- which is the entire point of the split.
class WallpaperManager {
public:
    // Upper bound on monitor ids. An id that renders successfully retains a full-size
    // exportable image (~9 MB at 1080p) until the daemon exits; an unsupported type or a
    // failed render releases it instead. So an unbounded id space lets any client on the
    // session bus walk ids until the GPU allocator fails -- after which even the user's
    // real monitor 0 can no longer be set. Comfortably above any real display count; the
    // cap exists to make the worst case finite, not to be tight.
    static constexpr int kMaxMonitors = 32;

    struct Config {
        // Placeholder geometry. The daemon has no source of real monitor dimensions
        // until the app supplies them via KScreen (Step 15), so every target is
        // created at this size. Tests override it to keep software-rasterised runs
        // fast.
        uint32_t width = 1920;
        uint32_t height = 1080;

        // Off by default: the validation layers cost real time and are not
        // guaranteed installed on a user's machine. Tests turn them on, since they
        // are the correctness oracle for everything the export path cannot assert.
        bool enableValidation = false;

        // Where the compiled SPIR-V lives. Empty means "resolve at first use" --
        // $WALLPAPERX_SHADER_DIR if set, else the build-time default.
        std::string shaderDir;

        // Where wallpaper choices are persisted. Empty disables persistence entirely.
        //
        // Empty is the default, and production opts in via defaultConfigPath(): a test
        // that says nothing about config must not read the developer's real wallpapers
        // back in, nor overwrite them on its first SetWallpaper.
        std::filesystem::path configFile;
    };

    explicit WallpaperManager(Config config);
    ~WallpaperManager();

    WallpaperManager(const WallpaperManager&) = delete;
    WallpaperManager& operator=(const WallpaperManager&) = delete;

    // Records a request. Fast and GPU-free: it validates, stores the request and marks
    // the monitor as having no current frame. Throws InvalidRequest for a malformed
    // request; everything that can only fail on the GPU fails later, in renderPending().
    //
    // The caller must post the monitor to the render worker afterwards, including for
    // non-static types -- those still need their target released.
    void requestWallpaper(const std::string& type, const std::string& path, int monitorId);

    // Applies the config file's wallpapers, returning the monitor ids to post to the
    // worker. Call once. Neither rejects an unreadable path nor saves -- an entry dropped
    // here is dropped from the file by the next save.
    std::vector<int> restoreSaved();

    // Renders whatever request the monitor currently holds. Called on the render
    // worker's thread; never on the DBus thread.
    //
    // Returns rendered=true only when a frame was actually produced *and* is still the
    // one that was asked for. A request that arrives mid-render supersedes this one, and
    // its result is discarded rather than committed -- otherwise the monitor would end
    // up flagged as holding a frame whose pixels belong to the previous wallpaper.
    // Failures are recorded in the monitor's lastError, not thrown: nobody is waiting.
    RenderOutcome renderPending(int monitorId);

    // Exports the monitor's most recent frame as a fresh fd. An empty handle means
    // there is nothing to export, with `reason` saying which of the three states that
    // is; a thrown exception means the export itself went wrong.
    FrameExport exportCurrentFrame(int monitorId);

    // One consistent snapshot of everything GetStatus reports. Deliberately not two
    // accessors: reading the GPU block and the monitor map under separate lock
    // acquisitions lets a render commit in between, producing a reply that claims no GPU
    // is initialised next to a monitor that already has a frame -- the exact
    // contradiction the GPU block exists to settle.
    struct Snapshot {
        GpuStatus gpu;
        DaemonHealth health;
        std::map<int, MonitorStatus> monitors;
    };
    Snapshot snapshot() const;

    // The config-server health has exactly one writer, and the rule is stated on
    // HttpServer's constructor. Private with a single friend rather than public and
    // documented: a claim any caller can revoke would let a second server stamp its own
    // bind failure over one that is serving, which is the state being prevented.
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
    // The renderer's validation-warning counter, or null when no device has been created
    // yet. Shared so it stays readable after this manager and its Device are destroyed:
    // the layer's teardown complaints are the ones worth having, and they are emitted
    // during destruction.
    //
    // NOT currently surfaced by GetStatus: GpuStatus carries initialized/name/tier only.
    // An earlier version of this comment claimed otherwise, which was both false and the
    // stated justification for these being production surface -- so if the only callers
    // stay in tests, either add the field to GpuStatus or move these behind whatever
    // does consume them. Note the lock: both take m_gpuMutex, which a render holds for
    // its whole duration, so wiring them into GetStatus as-is would make status wait on
    // a full image decode -- the exact thing snapshot() takes only m_stateMutex to avoid.
    std::shared_ptr<const std::atomic<int>> validationCounter() const;
    bool validationLayerActive() const;

private:
    struct MonitorState {
        std::string type;
        std::string path;

        // Counts accepted requests, where generation counts completed renders. The two
        // are what let a render detect that it has been superseded: the worker snapshots
        // requestSeq before rendering and refuses to commit if it has moved.
        uint64_t requestSeq = 0;

        // The requestSeq a render pass has already been run for, successfully or not.
        // renderPending() is idempotent per request because of this: the worker's queue
        // can legitimately hold a monitor twice (a request arriving after the monitor
        // was dequeued but before its seq was read re-queues it), and without this the
        // second pass would render identical pixels again, burn a generation number and
        // emit a second FrameReady for a frame the consumer already imported.
        uint64_t servicedSeq = 0;

        uint64_t generation = 0;
        bool hasFrame = false;
        std::string lastError;

        // Copied from the target at commit time rather than read back from it, so
        // status() can report the frame's size without touching the GPU state and
        // therefore without waiting behind a render.
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // Creates the Device and target manager on first use. Deferred rather than done
    // in the constructor so the daemon still starts, serves GetStatus and reports a
    // clean error on a machine with no working Vulkan, instead of failing at launch.
    // Called with m_gpuMutex held.
    void ensureGpu();

    // Both called with m_gpuMutex held.
    const OffscreenTarget& acquireTarget(int monitorId);
    void releaseTarget(int monitorId);

    // Snapshots the monitors worth persisting. Caller must hold m_stateMutex.
    std::vector<PersistedWallpaper> persistableLocked() const;

    // Writes `entries` and records the persistence health. Must NOT be called holding
    // m_stateMutex: it records health, which takes it, and the write is a syscall.
    void persist(const std::vector<PersistedWallpaper>& entries);

    const Config m_config;
    const ConfigStore m_store;

    // What load() returned, until restoreSaved() consumes it.
    std::vector<PersistedWallpaper> m_restore;

    // Entries restoreSaved() could not apply, carried so a save does not delete them.
    // Guarded by m_stateMutex.
    std::vector<PersistedWallpaper> m_unapplied;

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
