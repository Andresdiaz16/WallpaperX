#include "wallpaper_manager.h"

#include "renderers/static_renderer.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"
#include "vulkan/tier_selection.h"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include <unistd.h>

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
WallpaperManager::~WallpaperManager() {
    // The Device was created on the render thread, but this destructor runs on whichever
    // thread owns the manager -- the daemon's main thread -- and destroying the targets
    // and the device is GPU work. That handoff is legitimate only because the owner joins
    // the render worker before destroying the manager (DbusService::~DbusService stops it
    // first, and main.cpp declares the service after the manager so it dies first).
    // Saying so explicitly is the point: without this the affinity check fires on every
    // shutdown, and silencing it by not checking teardown at all would have given up the
    // check exactly where use-after-free lives.
    if (m_device) {
        m_device->adoptGpuThread();
    }
}

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

    // Published as a plain copy so status() can report the GPU without taking m_gpuMutex
    // -- which a render holds for its whole duration, and GetStatus must not wait on.
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    m_gpuStatus.initialized = true;
    m_gpuStatus.name = m_device->info().gpuName;
    m_gpuStatus.descriptorTier = toString(m_device->descriptorTier());
}

const OffscreenTarget& WallpaperManager::acquireTarget(int monitorId) {
    // Reuse the existing target unless its geometry no longer matches -- create()
    // replaces (and waits for idle), which is wasted work when nothing changed. The
    // size can only differ once real monitor geometry arrives in Step 15, but keying
    // the reuse on the dimensions rather than on mere existence means that change does
    // not silently keep rendering at the old resolution.
    const bool reusable = m_targets->has(monitorId) && m_targets->get(monitorId).width == m_config.width &&
                          m_targets->get(monitorId).height == m_config.height;
    return reusable ? m_targets->get(monitorId) : m_targets->create(monitorId, m_config.width, m_config.height);
}

void WallpaperManager::releaseTarget(int monitorId) {
    if (m_targets && m_targets->has(monitorId)) {
        m_targets->destroy(monitorId);
    }
}

void WallpaperManager::requestWallpaper(const std::string& type, const std::string& path, int monitorId) {
    if (!isKnownType(type)) {
        throw InvalidRequest("unknown wallpaper type '" + type + "' (expected static, video or shader)");
    }
    requireValidMonitorId(monitorId);

    // A path that is not a file at all is a client mistake, and rendering is
    // asynchronous now -- so without this check the single most common user error would
    // come back as a successful reply followed by silence, discoverable only by polling
    // GetStatus. It is an early-out, not a guarantee: a file that exists but is not a
    // decodable image still fails later, on the render thread.
    if (type == "static") {
        // Both halves matter, and the message has to match what is actually tested.
        // Checking only the file type would accept a mode-000 file with a success reply
        // and then fail it on the render thread, so the same user mistake -- "I gave it
        // a path it cannot open" -- would surface through two different channels
        // depending on why it could not be opened.
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ::access(path.c_str(), R_OK) != 0) {
            throw InvalidRequest("'" + path + "' is not a readable file");
        }
    }

    std::lock_guard<std::mutex> stateLock(m_stateMutex);

    MonitorState& state = m_monitors[monitorId];
    state.type = type;
    state.path = path;
    ++state.requestSeq;
    state.lastError.clear();

    // Cleared before the render, not after a failure. Whatever happens next, the frame
    // this monitor currently holds no longer matches the request just recorded, and
    // handing out a frame of the previous wallpaper under the new path is a worse
    // failure than reporting no frame at all. The generation counter is left alone --
    // it counts renders, and no render has happened yet.
    state.hasFrame = false;
}

RenderOutcome WallpaperManager::renderPending(int monitorId) {
    std::lock_guard<std::mutex> gpuLock(m_gpuMutex);

    std::string type;
    std::string path;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        auto it = m_monitors.find(monitorId);
        if (it == m_monitors.end()) {
            return {};  // the request was never recorded; nothing to do
        }
        // Already been through a pass for this exact request. The worker's queue can
        // legitimately name a monitor twice -- a request landing after the monitor was
        // dequeued but before this snapshot re-queues it, since the coalescing set no
        // longer holds it -- and rendering the same request again would duplicate the
        // frame, the generation number and the FrameReady.
        if (it->second.servicedSeq == it->second.requestSeq) {
            return {};
        }
        type = it->second.type;
        path = it->second.path;
        seq = it->second.requestSeq;
    }

    if (type != "static") {
        // Recorded only; video/shader render in Step 13 / Phase 3. Release the target --
        // nothing will read it again, and holding a full-size image per monitor for a
        // wallpaper type this build cannot draw is memory kept for no one.
        releaseTarget(monitorId);

        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        auto it = m_monitors.find(monitorId);
        if (it != m_monitors.end() && it->second.requestSeq == seq) {
            it->second.servicedSeq = seq;
        }
        return {};
    }

    // Everything expensive happens here, outside m_stateMutex: decoding the image,
    // uploading the texture, building the pipeline, creating the target. None of it
    // touches the offscreen image's contents.
    std::string error;
    std::optional<StaticRenderer> renderer;
    const OffscreenTarget* target = nullptr;
    try {
        ensureGpu();
        target = &acquireTarget(monitorId);
        // Not retained past this call. A static wallpaper renders once, so keeping the
        // texture, pipeline and descriptor alive per monitor would hold VRAM for
        // something nothing re-renders; immediateSubmit has already waited on its fence
        // by the time render() returns, so nothing is destroyed out from under the GPU.
        // Step 13's video renderer does need to persist and will live in MonitorState.
        renderer.emplace(*m_device, m_targets->renderPass(), path, m_resolvedShaderDir);
    } catch (const std::exception& e) {
        error = e.what();
    }

    bool committed = false;
    uint64_t generation = 0;
    {
        // m_stateMutex is held across the submit, and that is the whole point rather
        // than an oversight. The target is single-buffered: the submit writes into the
        // very memory an already-imported consumer is displaying. If a newer request
        // could land between the check and the submit, this render would repaint the
        // published frame with a wallpaper the user has just replaced -- and, because a
        // superseded render deliberately does not bump the generation or emit
        // FrameReady, it would do so with nothing on the wire to say it happened. The
        // consumer would go on believing it holds generation N. Making the check and the
        // submit atomic is what reduces that to "either the render is published or the
        // image is never touched".
        //
        // The cost is bounded by one fullscreen-quad submit, not by the render as a
        // whole -- decode, upload and pipeline creation are all above, unlocked -- so
        // GetStatus can wait on a draw, never on an image load or device creation.
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        auto it = m_monitors.find(monitorId);
        if (it == m_monitors.end() || it->second.requestSeq != seq) {
            // Superseded. Nothing has been submitted, so the image still holds exactly
            // what the consumer was last told about. Leave servicedSeq alone so the
            // newer request's own pass is not mistaken for one already done.
            return {};
        }

        if (error.empty()) {
            try {
                m_targets->render(*renderer, *target);
            } catch (const std::exception& e) {
                error = e.what();
            }
        }

        // Marked for every outcome, not just success: a failed render must not be
        // retried by a duplicate queue entry either, or a broken image file would be
        // decoded again for every spurious post.
        it->second.servicedSeq = seq;

        if (!error.empty()) {
            it->second.lastError = error;
        } else {
            ++it->second.generation;
            it->second.hasFrame = true;
            it->second.width = m_config.width;
            it->second.height = m_config.height;
            // Cleared here rather than relying on requestWallpaper having cleared it. A
            // successful commit is what makes "this monitor has a frame" true, and the
            // two facts have to be set together -- otherwise a monitor can report
            // has_frame alongside a stale last_error, and a client branching on
            // last_error calls a working wallpaper broken.
            it->second.lastError.clear();
            committed = true;
            generation = it->second.generation;
        }
    }

    if (!error.empty()) {
        // Outside the state lock: destroying a target waits for device idle, which has
        // no business blocking GetStatus. A monitor whose render failed keeps no frame,
        // so its target has no reader, and freeing it means a retry starts clean.
        renderer.reset();
        releaseTarget(monitorId);
        return {};
    }

    return RenderOutcome{committed, generation};
}

std::optional<FrameHandle> WallpaperManager::exportCurrentFrame(int monitorId) {
    requireValidMonitorId(monitorId);

    std::lock_guard<std::mutex> gpuLock(m_gpuMutex);

    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        auto it = m_monitors.find(monitorId);
        if (it == m_monitors.end() || !it->second.hasFrame) {
            return std::nullopt;
        }
        generation = it->second.generation;
    }

    // A request can land between releasing m_stateMutex and exporting, which clears
    // hasFrame. That is deliberately not re-checked: what goes out is the last frame
    // actually rendered, labelled with its true generation. The consumer learns about
    // the newer one from FrameReady and re-fetches -- which is what the generation
    // counter exists for.

    // hasFrame is only ever set by a completed render, which creates the target first,
    // so the two cannot disagree -- but the export path is the one place a stale flag
    // would turn into a std::out_of_range from get() rather than a clean DBus error.
    if (!m_targets || !m_targets->has(monitorId)) {
        throw std::runtime_error("monitor " + std::to_string(monitorId) +
                                 " is marked as rendered but has no offscreen target");
    }

    FrameHandle handle;
    handle.frame = wallpaperx::exportFrame(*m_device, m_targets->get(monitorId));
    handle.generation = generation;
    return handle;
}

WallpaperManager::Snapshot WallpaperManager::snapshot() const {
    // One acquisition for both halves. Two would let a render commit between them and
    // produce a reply that reports no GPU beside a monitor that already has a frame.
    std::lock_guard<std::mutex> stateLock(m_stateMutex);

    Snapshot out;
    out.gpu = m_gpuStatus;
    for (const auto& [monitorId, state] : m_monitors) {
        MonitorStatus entry;
        entry.type = state.type;
        entry.path = state.path;
        entry.generation = state.generation;
        entry.hasFrame = state.hasFrame;
        entry.lastError = state.lastError;
        if (state.hasFrame) {
            entry.width = state.width;
            entry.height = state.height;
        }
        out.monitors.emplace(monitorId, std::move(entry));
    }
    return out;
}

}
