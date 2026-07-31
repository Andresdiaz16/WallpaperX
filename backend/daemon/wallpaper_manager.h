#pragma once

#include "vulkan/frame_export.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace wallpaperx {

class Device;
class OffscreenTargetManager;

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

// An exported frame plus the generation counter of the render that produced it.
// The caller owns frame.fd and must close it (or hand it to something that will).
struct FrameHandle {
    ExportedFrame frame;
    uint64_t generation = 0;
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
    // Upper bound on monitor ids. Each accepted id costs a full-size exportable image
    // (~8 MB at 1080p) that lives until the daemon exits, so an unbounded id space lets
    // any client on the session bus walk ids until the GPU allocator fails -- after
    // which even the user's real monitor 0 can no longer be set. Comfortably above any
    // real display count; the cap exists to make the worst case finite, not to be tight.
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

    // Renders whatever request the monitor currently holds. Called on the render
    // worker's thread; never on the DBus thread.
    //
    // Returns rendered=true only when a frame was actually produced *and* is still the
    // one that was asked for. A request that arrives mid-render supersedes this one, and
    // its result is discarded rather than committed -- otherwise the monitor would end
    // up flagged as holding a frame whose pixels belong to the previous wallpaper.
    // Failures are recorded in the monitor's lastError, not thrown: nobody is waiting.
    RenderOutcome renderPending(int monitorId);

    // Exports the monitor's most recent frame as a fresh fd. std::nullopt means
    // there is simply nothing rendered for that monitor (an expected state, not a
    // failure); a thrown exception means the export itself went wrong.
    std::optional<FrameHandle> exportCurrentFrame(int monitorId);

    // One consistent snapshot of everything GetStatus reports. Deliberately not two
    // accessors: reading the GPU block and the monitor map under separate lock
    // acquisitions lets a render commit in between, producing a reply that claims no GPU
    // is initialised next to a monitor that already has a frame -- the exact
    // contradiction the GPU block exists to settle.
    struct Snapshot {
        GpuStatus gpu;
        std::map<int, MonitorStatus> monitors;
    };
    Snapshot snapshot() const;

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
    void renderStatic(int monitorId, const std::string& path);
    void releaseTarget(int monitorId);

    const Config m_config;

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
    GpuStatus m_gpuStatus;
};

}
