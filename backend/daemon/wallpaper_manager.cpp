#include "wallpaper_manager.h"

#include "log_escape.h"
#include "renderers/static_renderer.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"
#include "vulkan/tier_selection.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
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

// Long enough that a slow disk is not cut off -- a healthy read measures in microseconds --
// and short enough that a stalled mount does not visibly stall a login.
constexpr std::chrono::seconds kLoadDeadline{2};

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

WallpaperManager::WallpaperManager(Config config)
    : m_config(std::move(config)), m_store(m_config.configFile) {
    if (!m_store.enabled()) {
        // Ok, not Fail: this feeds a worst-wins aggregate, so Fail would make every daemon
        // that never opted in report itself broken.
        recordHealth(&DaemonHealth::persistence, HealthStatus::Ok, "disabled");
        return;
    }

    const LoadResult loaded = loadWithDeadline();
    m_restore = loaded.entries;

    std::string detail = sanitizeForLog(m_store.file().string(), 4096, LogFraming::Bare);
    if (loaded.malformedEntries > 0) {
        detail += " (" + std::to_string(loaded.malformedEntries) + " malformed entries ignored)";
    }

    if (!loaded.error.empty()) {
        // The first save would otherwise overwrite a file this run could not read, and it
        // is the only copy. Moved aside instead, so a hand-edit that broke the syntax is
        // still recoverable afterwards.
        std::string reported = loaded.error;
        if (loaded.preservable) {
            const std::filesystem::path kept = m_store.file().string() + ".bad";
            const std::string keptName = sanitizeForLog(kept.string(), 4096, LogFraming::Bare);
            std::error_code ec;
            const bool present = std::filesystem::exists(kept, ec);
            // Never over an existing copy: the first bad start moves the real config here,
            // and a second one would otherwise replace it with the throwaway written since.
            // A copy that cannot be inspected counts as present for the same reason.
            //
            // Every branch reports. Saying the config could not be read while staying silent
            // about whether the copy was made sends the user looking for a file that is not
            // there.
            if (ec) {
                reported += "; not kept, " + keptName + " could not be checked: " + ec.message();
            } else if (present) {
                reported += "; not kept, " + keptName + " already exists";
            } else {
                std::filesystem::rename(m_store.file(), kept, ec);
                reported += ec ? "; not kept, could not move it to " + keptName + ": " + ec.message()
                               : "; kept as " + keptName;
            }
        }
        recordHealth(&DaemonHealth::persistence, HealthStatus::Fail, reported);
        return;
    }

    recordHealth(&DaemonHealth::persistence, HealthStatus::Ok, detail);
}

// Out of line, and non-trivial for a reason: Device and OffscreenTargetManager are
// only forward-declared in the header, so the unique_ptr deleters need their
// definitions, which are visible here.
WallpaperManager::~WallpaperManager() {
    // The Device was created on the render thread but destroyed on the owner's, which is
    // GPU work on the wrong thread. Legitimate only because the worker is joined first
    // (~DbusService stops it, and main declares the service after the manager). Adopting
    // explicitly, rather than not checking teardown, keeps the affinity check where
    // use-after-free actually lives.
    if (m_device) {
        m_device->adoptGpuThread();
    }
}

const char* toString(HealthStatus status) {
    switch (status) {
        case HealthStatus::Pending: return "pending";
        case HealthStatus::Ok: return "ok";
        case HealthStatus::Fail: return "fail";
    }
    return "unknown";
}

void WallpaperManager::recordHealth(HealthCheck DaemonHealth::*which, HealthStatus status,
                                    const std::string& detail) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    recordHealthLocked(which, status, detail);
}

void WallpaperManager::recordHealthLocked(HealthCheck DaemonHealth::*which, HealthStatus status,
                                          const std::string& detail) {
    (m_health.*which).status = status;
    (m_health.*which).detail = sanitizeForLog(detail, 1024, LogFraming::Bare);
}

void WallpaperManager::recordConfigServer(bool listening, const std::string& error) {
    recordHealth(&DaemonHealth::configServer, listening ? HealthStatus::Ok : HealthStatus::Fail,
                 listening ? "listening" : error);
}

void WallpaperManager::recordConfigServerShutdown(const std::string& detail) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (m_health.configServer.status != HealthStatus::Ok) return;
    recordHealthLocked(&DaemonHealth::configServer, HealthStatus::Fail, detail);
}

bool WallpaperManager::tryAttachConfigServer() {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (m_configServerAttached) return false;
    m_configServerAttached = true;
    return true;
}

void WallpaperManager::detachConfigServer() {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    m_configServerAttached = false;
}

void WallpaperManager::ensureGpu() {
    if (m_device) return;

    // Each phase records its outcome before rethrowing, so /health can name which
    // dependency failed rather than burying it in one monitor's lastError.
    //
    // Resolved before the Device exists: it is the cheapest of the three things that
    // can fail here, and a missing shader directory would otherwise be reported only
    // after paying for a full Vulkan device creation and teardown.
    try {
        m_resolvedShaderDir = resolveShaderDir(m_config.shaderDir);
    } catch (const std::exception& e) {
        recordHealth(&DaemonHealth::shaders, HealthStatus::Fail, e.what());
        throw;
    }
    recordHealth(&DaemonHealth::shaders, HealthStatus::Ok, m_resolvedShaderDir);

    // Built into locals and only committed on full success. Assigning m_device first
    // would leave a half-initialised manager if the target manager throws: m_device set,
    // m_targets null, and every later call taking the "already initialised" early return
    // straight into a null dereference.
    std::unique_ptr<Device> device;
    std::unique_ptr<OffscreenTargetManager> targets;
    try {
        device = std::make_unique<Device>(m_config.enableValidation);
        targets = std::make_unique<OffscreenTargetManager>(*device);
    } catch (const std::exception& e) {
        recordHealth(&DaemonHealth::gpu, HealthStatus::Fail, e.what());
        throw;
    }

    // targets holds a Device& to the object the local unique_ptr owns; moving the
    // pointer does not move the object, so that reference stays valid.
    m_device = std::move(device);
    m_targets = std::move(targets);

    // Published as a plain copy so status() can report the GPU without taking m_gpuMutex
    // -- which a render holds for its whole duration, and GetStatus must not wait on.
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_gpuStatus.initialized = true;
        m_gpuStatus.name = m_device->info().gpuName;
        m_gpuStatus.descriptorTier = toString(m_device->descriptorTier());
    }
    recordHealth(&DaemonHealth::gpu, HealthStatus::Ok,
                 m_gpuStatus.name + " (" + m_gpuStatus.descriptorTier + ")");

    // Definition of Done #1 wants the daemon to report its GPU and descriptor tier. Here
    // rather than at startup, because the device is created lazily and printing a tier
    // before one exists would be a guess -- the same reason GetStatus omits the fields
    // until now. Deliberately the identical format wallpaperx-device-probe prints, so an
    // operator can compare the daemon's answer against the probe's line for line.
    std::printf("wallpaperx-daemon: GPU: %s | Vendor: %s | Descriptor tier: %s\n",
                m_device->info().gpuName.c_str(), toString(m_device->info().vendor),
                toString(m_device->descriptorTier()));
    std::fflush(stdout);
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
        // Sanitized because this message is echoed to the log and returned over the bus,
        // and `type` is whatever the client sent -- a 200 000-character type string was
        // reproduced in full through both channels.
        // No quotes of our own around the sanitized text -- sanitizeForLog supplies them,
        // and a second pair would be a frame the client's payload could imitate.
        throw InvalidRequest("unknown wallpaper type " + sanitizeForLog(type) +
                             " (expected static, video or shader)");
    }
    requireValidMonitorId(monitorId);

    // An early-out, not a guarantee: rendering is asynchronous, so without this the
    // commonest user error returns success and then silence. Both halves matter -- checking
    // only the file type would send a mode-000 file down the render path instead, so one
    // mistake would surface through two channels.
    //
    // Applies to every known type, not just static: while this sat inside a
    // `type == "static"` branch, video and shader accepted an empty string, a missing path
    // or a directory and then sat frameless forever.
    //
    // Known and accepted: these two syscalls run on the DBus event-loop thread, so a hung
    // NFS mount blocks every other method -- and leaves the daemon unkillable with it, since
    // the loop cannot act on leaveEventLoop() until this handler returns. SIGTERM is received
    // and reported, and the process stays up. Moving them to the render worker was considered
    // and rejected -- it is a FIFO, so a bad path would wait behind every queued render and
    // break S6.9's burst bound. The fix, if ever needed, is a separate validation thread.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ::access(path.c_str(), R_OK) != 0) {
        throw InvalidRequest(sanitizeForLog(path) + " is not a readable file");
    }

    std::vector<PersistedWallpaper> toPersist;
    {
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

        toPersist = persistableLocked();
    }

    // Outside the lock, and before the render: what the user asked for is worth keeping
    // whether or not it turns out to draw.
    persist(toPersist);
}

LoadResult WallpaperManager::loadWithDeadline() const {
    // A detached thread and a shared promise, NOT std::async: the future std::async returns
    // blocks in its own destructor until the task finishes, which would wait exactly as long
    // as this exists to avoid. Nothing here ever joins.
    //
    // The store is copied in, so the thread outliving this call cannot reach a destroyed one.
    auto answer = std::make_shared<std::promise<LoadResult>>();
    std::future<LoadResult> pending = answer->get_future();
    std::thread([answer, store = m_store] { answer->set_value(store.load()); }).detach();

    if (pending.wait_for(kLoadDeadline) == std::future_status::ready) return pending.get();

    // Abandoned, not cancelled: the thread stays blocked in the kernel until the process
    // exits. Startup continues without a config rather than becoming unstoppable -- the
    // stuck thread is harmless, a stuck main() is not.
    LoadResult timedOut;
    timedOut.fileExisted = true;
    timedOut.error = sanitizeForLog(m_store.file().string(), 4096, LogFraming::Bare) +
                     ": did not respond within " + std::to_string(kLoadDeadline.count()) +
                     "s, starting without it";
    return timedOut;
}

std::vector<PersistedWallpaper> WallpaperManager::persistableLocked() const {
    std::vector<PersistedWallpaper> entries;
    entries.reserve(m_monitors.size() + m_unapplied.size());
    for (const auto& [monitorId, state] : m_monitors) {
        if (state.type.empty() || state.path.empty()) continue;
        entries.push_back(PersistedWallpaper{monitorId, state.type, state.path});
    }

    // Entries this build could not apply are still the user's. Without this the save
    // below, which rebuilds the file from live state alone, would delete a wallpaper set
    // by a newer build the moment anything else is set here. Live state wins per monitor.
    for (const PersistedWallpaper& entry : m_unapplied) {
        if (m_monitors.count(entry.monitorId) == 0) entries.push_back(entry);
    }
    return entries;
}

void WallpaperManager::persist(const std::vector<PersistedWallpaper>& entries) {
    if (!m_store.enabled()) return;

    const SaveResult saved = m_store.save(entries);
    if (!saved.error.empty()) {
        recordHealth(&DaemonHealth::persistence, HealthStatus::Fail, saved.error);
        return;
    }

    std::string detail = sanitizeForLog(m_store.file().string(), 4096, LogFraming::Bare);
    if (saved.skippedNonUtf8 > 0) {
        detail += " (" + std::to_string(saved.skippedNonUtf8) +
                  " entries skipped: path is not valid UTF-8)";
    }
    // Fail, not Ok: the write succeeded but those monitors will not come back, and a
    // health field that reads ok is the only thing that would say otherwise.
    recordHealth(&DaemonHealth::persistence,
                 saved.skippedNonUtf8 > 0 ? HealthStatus::Fail : HealthStatus::Ok, detail);
}

std::vector<int> WallpaperManager::restoreSaved() {
    std::vector<int> monitorIds;
    std::vector<PersistedWallpaper> pending;
    pending.swap(m_restore);

    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    for (const PersistedWallpaper& entry : pending) {
        // The manager's own rules, not the store's: a file naming monitor 99 or a type
        // this build has never heard of is not rendered here -- but it is kept, so the
        // next save does not delete a wallpaper a newer build understands.
        if (!isKnownType(entry.type) || entry.path.empty() || entry.monitorId < 0 ||
            entry.monitorId >= kMaxMonitors) {
            m_unapplied.push_back(entry);
            continue;
        }

        MonitorState& state = m_monitors[entry.monitorId];
        state.type = entry.type;
        state.path = entry.path;
        ++state.requestSeq;
        state.lastError.clear();
        state.hasFrame = false;
        monitorIds.push_back(entry.monitorId);
    }

    // Kept entries are invisible otherwise: they are in no monitor's status and are not
    // malformed, so nothing would tell the user those wallpapers did not come back.
    if (!m_unapplied.empty() && m_health.persistence.status == HealthStatus::Ok) {
        recordHealthLocked(&DaemonHealth::persistence, HealthStatus::Ok,
                           m_health.persistence.detail + " (" + std::to_string(m_unapplied.size()) +
                               " entries this build cannot apply, kept)");
    }
    return monitorIds;
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

    if (!isRenderableType(type)) {
        // Recorded only; video/shader render in Step 13 / Phase 3. Release the target --
        // nothing will read it again, and holding a full-size image per monitor for a
        // wallpaper type this build cannot draw is memory kept for no one.
        releaseTarget(monitorId);

        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        auto it = m_monitors.find(monitorId);
        if (it != m_monitors.end() && it->second.requestSeq == seq) {
            it->second.servicedSeq = seq;
            // Deliberately does NOT write lastError: that field means the request failed,
            // and this one was accepted exactly as asked. Writing it made frontends paint
            // a broken-image state on a wallpaper the daemon had accepted. GetStatus's
            // separate `unsupported` key carries this instead.
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
    } catch (const GpuFailure& e) {
        // Caught before std::exception, which it derives from. This is the GPU going wrong
        // rather than the image, so it re-records gpu health -- otherwise a device that
        // dies after the first successful render reads ok forever.
        error = e.what();
        recordHealth(&DaemonHealth::gpu, HealthStatus::Fail, error);
    } catch (const std::exception& e) {
        error = e.what();
    }

    bool committed = false;
    uint64_t generation = 0;
    {
        // Held across the submit deliberately. The target is single-buffered, so the
        // submit writes into memory a consumer is already displaying; a newer request
        // landing between the check and the submit would repaint the published frame with
        // nothing on the wire to say so, since a superseded render emits no FrameReady.
        // Atomic check-and-submit reduces that to "published, or never touched".
        //
        // Bounded by one fullscreen-quad submit -- decode, upload and pipeline creation are
        // all above, unlocked -- so GetStatus can wait on a draw, never on a decode.
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
            } catch (const GpuFailure& e) {
                error = e.what();
                recordHealthLocked(&DaemonHealth::gpu, HealthStatus::Fail, error);
            } catch (const std::exception& e) {
                error = e.what();
            }
        }

        // Marked for every outcome, not just success: a failed render must not be
        // retried by a duplicate queue entry either, or a broken image file would be
        // decoded again for every spurious post.
        it->second.servicedSeq = seq;

        if (!error.empty()) {
            // Sanitized where client text enters daemon state, not at each place it
            // leaves, so every consumer inherits the escaping: the renderer's message
            // embeds the client's path, and a readable file may be named with an ESC
            // sequence.
            //
            // 1024 rather than the default because this field is polled. Safe only because
            // describeRejection() puts the diagnosis before the path -- truncation keeps
            // the head, so reversing that message would cut stb's reason instead.
            it->second.lastError = sanitizeForLog(error, 1024, LogFraming::Bare);
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
            // A commit is the GPU working, so it clears a failure recorded by an earlier
            // render. Same detail ensureGpu records on success, so a recovered GPU cannot
            // be told from one that never failed.
            recordHealthLocked(&DaemonHealth::gpu, HealthStatus::Ok,
                               m_gpuStatus.name + " (" + m_gpuStatus.descriptorTier + ")");
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

FrameExport WallpaperManager::exportCurrentFrame(int monitorId) {
    requireValidMonitorId(monitorId);

    std::lock_guard<std::mutex> gpuLock(m_gpuMutex);

    FrameExport out;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        auto it = m_monitors.find(monitorId);
        if (it == m_monitors.end()) {
            out.reason = NoFrameReason::NeverRequested;
            return out;
        }
        if (!it->second.hasFrame) {
            // Decided under the lock so the answer cannot be overtaken between reading
            // "no frame" and reading why.
            //
            // Type is consulted first and independently of lastError: the answer is true
            // from the instant the request is recorded, so keying on the worker's progress
            // would say "not yet" for a window and "never" afterwards for one unchanged
            // request.
            if (!isRenderableType(it->second.type)) {
                out.reason = NoFrameReason::Unsupported;
                return out;
            }
            if (!it->second.lastError.empty()) {
                out.reason = NoFrameReason::RenderFailed;
                out.lastError = it->second.lastError;
            } else {
                out.reason = NoFrameReason::NotRendered;
            }
            return out;
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
    try {
        handle.frame = wallpaperx::exportFrame(*m_device, m_targets->get(monitorId));
    } catch (const GpuFailure& e) {
        recordHealth(&DaemonHealth::gpu, HealthStatus::Fail, e.what());
        throw;
    }
    handle.generation = generation;
    out.handle = std::move(handle);
    return out;
}

std::shared_ptr<const std::atomic<int>> WallpaperManager::validationCounter() const {
    std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
    return m_device ? m_device->validationCounter() : nullptr;
}

bool WallpaperManager::validationLayerActive() const {
    std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
    return m_device && m_device->validationLayerActive();
}

WallpaperManager::Snapshot WallpaperManager::snapshot() const {
    // One acquisition for both halves. Two would let a render commit between them and
    // produce a reply that reports no GPU beside a monitor that already has a frame.
    std::lock_guard<std::mutex> stateLock(m_stateMutex);

    Snapshot out;
    out.gpu = m_gpuStatus;
    out.health = m_health;
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
