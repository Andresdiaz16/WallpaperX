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

// What a monitor was last asked to display, and whether that request produced a
// frame. `generation` is monotonic per monitor and never resets -- a consumer uses
// it to tell a fresh frame from a repeat, so reusing a number for different pixels
// would defeat it. `hasFrame` is therefore separate: a request that failed, or one
// for a type this build cannot render yet, leaves the counter alone and clears the
// flag rather than rewinding.
struct MonitorStatus {
    std::string type;
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;
    bool hasFrame = false;
};

// An exported frame plus the generation counter of the render that produced it.
// The caller owns frame.fd and must close it (or hand it to something that will).
struct FrameHandle {
    ExportedFrame frame;
    uint64_t generation = 0;
};

// Owns the renderer for the daemon: one Vulkan Device, one OffscreenTargetManager
// keyed by monitor, and the per-monitor bookkeeping behind SetWallpaper/GetFrameFd.
//
// Thread-safe. Every public method takes m_mutex, and that is load-bearing rather
// than routine: OffscreenTarget's exportLayout/externalOwner are written through a
// const reference by *both* the render path and the export path, so guarding only
// one of them would leave the other's write unsynchronised. Vulkan queue submission
// is not free-threaded either, and both paths submit. One lock covering both is the
// whole synchronisation story; do not add a second.
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

    // Records the request and, for type == "static", renders it synchronously.
    // Throws std::runtime_error if the render fails (unreadable image, no usable
    // GPU, missing shaders); the monitor is then left with no exportable frame
    // rather than holding a frame that no longer matches the recorded request.
    //
    // "video"/"shader" are recorded but not rendered -- they arrive in Step 13 /
    // Phase 3. Any other type throws.
    void setWallpaper(const std::string& type, const std::string& path, int monitorId);

    // Exports the monitor's most recent frame as a fresh fd. std::nullopt means
    // there is simply nothing rendered for that monitor (an expected state, not a
    // failure); a thrown exception means the export itself went wrong.
    std::optional<FrameHandle> exportCurrentFrame(int monitorId);

    std::map<int, MonitorStatus> status() const;

private:
    struct MonitorState {
        std::string type;
        std::string path;
        uint64_t generation = 0;
        bool hasFrame = false;
    };

    // Creates the Device and target manager on first use. Deferred rather than done
    // in the constructor so the daemon still starts, serves GetStatus and reports a
    // clean error on a machine with no working Vulkan, instead of failing at launch.
    void ensureGpu();

    void renderStatic(int monitorId, const std::string& path, MonitorState& state);

    // Frees a monitor's offscreen target if it has one. Every path that leaves a monitor
    // without an exportable frame goes through here, so a target never outlives the
    // frame it was holding.
    void releaseTarget(int monitorId);

    const Config m_config;

    mutable std::mutex m_mutex;

    // Declaration order is destruction order reversed: targets hold a Device&, so
    // the Device must outlive them.
    std::unique_ptr<Device> m_device;
    std::unique_ptr<OffscreenTargetManager> m_targets;

    std::string m_resolvedShaderDir;
    std::map<int, MonitorState> m_monitors;
};

}
