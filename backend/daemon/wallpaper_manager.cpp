#include "wallpaper_manager.h"

#include "renderers/static_renderer.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <utility>

#ifndef WALLPAPERX_DEFAULT_SHADER_DIR
#error "WALLPAPERX_DEFAULT_SHADER_DIR must be defined by the build -- see backend/CMakeLists.txt"
#endif

namespace wallpaperx {

namespace {

// The three types the DBus contract enumerates. Anything else is a client bug, and
// silently recording it would surface much later as "my wallpaper never appears".
bool isKnownType(const std::string& type) {
    return type == "static" || type == "video" || type == "shader";
}

// Applied identically by every entry point. Validating on SetWallpaper but not on
// GetFrameFd would report a bad id as "no frame yet" -- a state a polling consumer is
// told to retry, so an off-by-one monitor id becomes an infinite retry loop with a
// blank wallpaper instead of an immediate error.
void requireValidMonitorId(int monitorId) {
    if (monitorId < 0 || monitorId >= WallpaperManager::kMaxMonitors) {
        throw InvalidRequest("monitor id must be a 0-based index below " +
                             std::to_string(WallpaperManager::kMaxMonitors) + ", got " +
                             std::to_string(monitorId));
    }
}

std::string resolveShaderDir(const std::string& configured) {
    std::string dir = configured;
    if (dir.empty()) {
        if (const char* env = std::getenv("WALLPAPERX_SHADER_DIR"); env != nullptr && *env != '\0') {
            dir = env;
        } else {
            dir = WALLPAPERX_DEFAULT_SHADER_DIR;
        }
    }

    // Checked once, here, rather than left to the first shader open. The build-time
    // default points into the build tree, so this is the failure a user hits when the
    // daemon is run from an install prefix that has no shaders -- it deserves a message
    // that names the directory and the override, not a bare "failed to open" on some
    // .spv path.
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        throw std::runtime_error("WallpaperManager: shader directory '" + dir +
                                 "' does not exist (set WALLPAPERX_SHADER_DIR to override)");
    }
    return dir;
}

}  // namespace

WallpaperManager::WallpaperManager(Config config) : m_config(std::move(config)) {}

// Out of line, and non-trivial for a reason: Device and OffscreenTargetManager are
// only forward-declared in the header, so the unique_ptr deleters need their
// definitions, which are visible here.
WallpaperManager::~WallpaperManager() = default;

void WallpaperManager::ensureGpu() {
    if (m_device) return;

    // Resolved before the Device exists: it is the cheapest of the three things that
    // can fail here, and a missing shader directory would otherwise be reported only
    // after paying for a full Vulkan device creation and teardown.
    m_resolvedShaderDir = resolveShaderDir(m_config.shaderDir);

    // Built into locals and only committed on full success. Assigning m_device first
    // would leave a half-initialised manager if the target manager throws: m_device set,
    // m_targets null, and every later call taking the "already initialised" early return
    // straight into a null dereference.
    auto device = std::make_unique<Device>(m_config.enableValidation);
    auto targets = std::make_unique<OffscreenTargetManager>(*device);

    // targets holds a Device& to the object the local unique_ptr owns; moving the
    // pointer does not move the object, so that reference stays valid.
    m_device = std::move(device);
    m_targets = std::move(targets);
}

void WallpaperManager::renderStatic(int monitorId, const std::string& path, MonitorState& state) {
    ensureGpu();

    // Reuse the existing target unless its geometry no longer matches -- create()
    // replaces (and waits for idle), which is wasted work when nothing changed. The
    // size can only differ once real monitor geometry arrives in Step 15, but keying
    // the reuse on the dimensions rather than on mere existence means that change does
    // not silently keep rendering at the old resolution.
    const bool reusable = m_targets->has(monitorId) && m_targets->get(monitorId).width == m_config.width &&
                          m_targets->get(monitorId).height == m_config.height;
    const OffscreenTarget& target =
        reusable ? m_targets->get(monitorId) : m_targets->create(monitorId, m_config.width, m_config.height);

    // Not retained past this call. A static wallpaper renders once, so keeping the
    // texture, pipeline and descriptor alive per monitor would hold VRAM for something
    // nothing re-renders; immediateSubmit has already waited on its fence by the time
    // render() returns, so nothing is destroyed out from under the GPU. Step 13's video
    // renderer does need to persist and will live in MonitorState.
    StaticRenderer renderer(*m_device, m_targets->renderPass(), path, m_resolvedShaderDir);
    m_targets->render(renderer, target);

    ++state.generation;
    state.hasFrame = true;
}

void WallpaperManager::setWallpaper(const std::string& type, const std::string& path, int monitorId) {
    if (!isKnownType(type)) {
        throw InvalidRequest("unknown wallpaper type '" + type + "' (expected static, video or shader)");
    }
    requireValidMonitorId(monitorId);

    std::lock_guard<std::mutex> lock(m_mutex);

    MonitorState& state = m_monitors[monitorId];
    state.type = type;
    state.path = path;

    // Cleared before the attempt, not after a failure. Whatever happens next, the frame
    // this monitor currently holds no longer matches the request just recorded, and
    // handing out a frame of the previous wallpaper under the new path is a worse
    // failure than reporting no frame at all. The generation counter is left alone --
    // it counts renders, and no render has happened yet.
    state.hasFrame = false;

    if (type != "static") {
        // Recorded only; video/shader render in Step 13 / Phase 3. Release the target
        // first -- nothing will read it again, and holding a full-size image per monitor
        // for a wallpaper type this build cannot draw is memory kept for no one.
        releaseTarget(monitorId);
        return;
    }

    try {
        renderStatic(monitorId, path, state);
    } catch (...) {
        // A monitor whose render failed keeps no frame (hasFrame is already false), so
        // its target has no reader. Freeing it also means a retry starts from a clean
        // target rather than reusing one left in whatever state the failure produced.
        releaseTarget(monitorId);
        throw;
    }
}

void WallpaperManager::releaseTarget(int monitorId) {
    if (m_targets && m_targets->has(monitorId)) {
        m_targets->destroy(monitorId);
    }
}

std::optional<FrameHandle> WallpaperManager::exportCurrentFrame(int monitorId) {
    requireValidMonitorId(monitorId);

    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_monitors.find(monitorId);
    if (it == m_monitors.end() || !it->second.hasFrame) {
        return std::nullopt;
    }
    // hasFrame is only ever set by a completed render, which creates the target first,
    // so the two cannot disagree -- but the export path is the one place a stale flag
    // would turn into a std::out_of_range from get() rather than a clean DBus error.
    if (!m_targets || !m_targets->has(monitorId)) {
        throw std::runtime_error("monitor " + std::to_string(monitorId) +
                                 " is marked as rendered but has no offscreen target");
    }

    FrameHandle handle;
    handle.frame = wallpaperx::exportFrame(*m_device, m_targets->get(monitorId));
    handle.generation = it->second.generation;
    return handle;
}

std::map<int, MonitorStatus> WallpaperManager::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::map<int, MonitorStatus> out;
    for (const auto& [monitorId, state] : m_monitors) {
        MonitorStatus entry;
        entry.type = state.type;
        entry.path = state.path;
        entry.generation = state.generation;
        entry.hasFrame = state.hasFrame;
        if (state.hasFrame && m_targets && m_targets->has(monitorId)) {
            entry.width = m_targets->get(monitorId).width;
            entry.height = m_targets->get(monitorId).height;
        }
        out.emplace(monitorId, std::move(entry));
    }
    return out;
}

}
