// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaper_manager.h"

#include "config_preserve.h"
#include "crash_record.h"
#include "kept_rows.h"
#include "log_escape.h"
#include "renderers/static_renderer.h"
#include "video/video_pipeline.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"
#include "vulkan/tier_selection.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <set>
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

std::atomic<bool> g_loadAbandoned{false};

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

void requireValidOutputName(const std::string& outputName) {
    if (const std::string why = outputNameError(outputName); !why.empty()) {
        throw InvalidRequest(why + ": " + sanitizeForLog(outputName));
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

using SteadyTime = std::chrono::steady_clock::time_point;
using PictureTiming = VideoPipeline::PictureTiming;

// Bounds on the time between two pictures. The floor keeps a file claiming an absurd rate from
// occupying the GPU thread; the ceiling reads a longer gap in the timestamps as a discontinuity
// rather than a pause, so a timestamp jump cannot freeze a wallpaper.
constexpr std::chrono::nanoseconds kMinPictureInterval = std::chrono::milliseconds(4);
constexpr std::chrono::nanoseconds kMaxPictureInterval = std::chrono::seconds(5);
// For a picture neither the file nor its stream gives a duration.
constexpr std::chrono::nanoseconds kFallbackPictureInterval{1'000'000'000 / 30};

std::chrono::nanoseconds ownInterval(const PictureTiming& picture) {
    if (!picture.duration || *picture.duration <= std::chrono::nanoseconds::zero()) return kFallbackPictureInterval;
    return std::clamp(*picture.duration, kMinPictureInterval, kMaxPictureInterval);
}

// How long after `before` its successor `after` is due: their timestamps' difference where that is
// usable, else `before`'s own duration -- which is what a loop's first picture, a timestamp going
// backwards or one jumping past the ceiling get.
std::chrono::nanoseconds gapBetween(const PictureTiming& before, const PictureTiming& after) {
    if (!after.startsPass && before.presentation && after.presentation) {
        const std::chrono::nanoseconds gap = *after.presentation - *before.presentation;
        if (gap > std::chrono::nanoseconds::zero() && gap <= kMaxPictureInterval) {
            return std::max(gap, kMinPictureInterval);
        }
    }
    return ownInterval(before);
}

}  // namespace

bool configLoadAbandoned() { return g_loadAbandoned.load(std::memory_order_acquire); }

WallpaperManager::WallpaperManager(Config config)
    : m_config(std::move(config)), m_store(m_config.configFile), m_writer(m_store) {
    if (!m_store.enabled()) {
        // Ok, not Fail: this feeds a worst-wins aggregate, so Fail would make every daemon
        // that never opted in report itself broken.
        recordHealth(&DaemonHealth::persistence, HealthStatus::Ok, "disabled");
        return;
    }

    // Read-only. Everything that writes to the config directory is deferred to
    // preserveUnreadableConfig() and writeRepairedConfig(), which main() calls once this
    // process owns the bus name -- a second instance must not move or rewrite the running
    // daemon's config before it finds out it has no right to run.
    const LoadResult loaded = loadWithDeadline();
    m_restore = loaded.entries;
    m_loadPreservable = loaded.preservable;
    m_loadMalformed = loaded.malformedEntries;
    m_loadError = loaded.error;

    // Raw, not sanitized: recordHealthLocked is the single sanitizer, and escaping twice
    // turns one backslash in a path into two in what the user reads.
    if (loaded.malformedEntries > 0) {
        m_kept.note(std::to_string(loaded.malformedEntries) + " malformed entries ignored");
    }

    if (!loaded.error.empty()) {
        // Set before any preservation is attempted, and cleared only if the file is
        // actually moved aside: until then, overwriting it would destroy the user's only
        // copy of every other monitor.
        m_writer.refuse(m_store.file().string() +
                        ": not saved, this run could not read it and will not overwrite it");
        recordHealth(&DaemonHealth::persistence, HealthStatus::Fail,
                     keptDetail(loaded.error));
        return;
    }

    recordHealth(&DaemonHealth::persistence, HealthStatus::Ok,
                 keptDetail(m_store.file().string()));
}

void WallpaperManager::preserveUnreadableConfig() {
    if (!m_store.enabled()) return;

    // The render worker is already running when main() calls this, so every touch of this
    // object's state below is under m_stateMutex. The copy itself is filesystem work and stays
    // outside it: a stalled mount must not hold up a render.
    if (m_loadError.empty()) {
        // A pending repair means a rewrite from live state is coming, and a row the manager
        // rejected parses cleanly, lives nowhere else, and is what that rewrite removes.
        bool repairPending = false;
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            repairPending = m_repair.has_value();
        }
        if (!m_loadPreservable || (m_loadMalformed == 0 && !repairPending)) return;

        // Copied, not moved: the entries this build understood are restored from this same file.
        const PreservedCopy copy = preserveConfig(m_store.file(), /*move=*/false);

        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_repairCopied = !copy.kept.empty();
        m_kept.note(copy.kept.empty() ? "not copied: " + copy.why : "copied to " + copy.kept);
        // Sticky: the rejected rows exist in this file alone and the rewrite removing them
        // is coming. Saving is not refused over it -- later wallpaper changes are the larger
        // loss -- so a write that succeeds later must not clear the record of that loss.
        if (!m_repairCopied) m_preserveFailed = true;
        recordHealthLocked(&DaemonHealth::persistence,
                           m_repairCopied ? HealthStatus::Ok : HealthStatus::Fail,
                           keptDetail(m_store.file().string()));
        return;
    }

    PreservedCopy copy;
    if (m_loadPreservable) copy = preserveConfig(m_store.file(), /*move=*/true);

    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (m_loadPreservable) {
        if (copy.kept.empty()) {
            m_kept.note("not kept: " + copy.why);
        } else {
            // Safe to save over it now: the bytes are at `kept` whether or not the original
            // went, and `why` says so when it did not.
            m_writer.allow();
            m_kept.note(copy.why.empty() ? "kept as " + copy.kept
                                                : "kept as " + copy.kept + "; " + copy.why);
        }
    }
    // Every path reports. Saying the config could not be read while staying silent about
    // whether a copy survived sends the user looking for a file that is not there.
    if (m_writer.refusing()) m_kept.note("not saving over it");
    recordHealthLocked(&DaemonHealth::persistence, HealthStatus::Fail, keptDetail(m_loadError));
}

void WallpaperManager::writeRepairedConfig() {
    std::optional<std::vector<PersistedWallpaper>> repair;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        repair.swap(m_repair);
    }
    if (!repair) return;

    // Written whether or not a copy was taken: the next ordinary save rebuilds the file from
    // the same live state, so refusing this one write protects nothing while refusing every
    // write costs every later wallpaper choice. m_preserveFailed reports it instead.
    //
    // Queued, not waited on: the bus name is held and no event loop exists yet, so a wait here
    // answers nothing.
    queuePersist(*repair);
}

bool WallpaperManager::liveHolds(int monitorId) const {
    // The same rule LiveWallpapers::holds applies, asked directly of the map. Kept apart
    // because restoreSaved() asks it once per config row -- up to about 28,000 of them, on the
    // startup thread -- and because that loop fills m_monitors as it goes, so a snapshot taken
    // before it would answer for a state that has since moved on.
    const auto live = m_monitors.find(monitorId);
    return live != m_monitors.end() && (!live->second.type.empty() || !live->second.path.empty());
}

LiveWallpapers WallpaperManager::liveWallpapers() const {
    LiveWallpapers live;
    live.rows.reserve(m_monitors.size());
    for (const auto& [monitorId, state] : m_monitors) {
        live.rows.push_back(PersistedWallpaper{monitorId, state.type, state.path, {}});
    }
    for (const auto& [name, monitorId] : m_outputs) {
        const auto size = m_outputSizes.find(name);
        live.outputs.emplace(name, OutputBinding{monitorId, size == m_outputSizes.end()
                                                                ? std::nullopt
                                                                : std::optional<OutputSize>(size->second)});
    }
    return live;
}

std::string WallpaperManager::keptDetail(const std::string& base) const {
    return m_kept.detail(base, liveWallpapers());
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

    // Sessions are freed by member destruction, with no crash record: one left by a stop main's
    // teardown alarm abandoned would refuse a wallpaper that drew. That alarm is all that bounds
    // this teardown, which can enter ffmpeg's unbounded wait.
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

    // Each phase records its outcome before rethrowing, so /health names which dependency
    // failed rather than burying it in one monitor's lastError. The shader directory is
    // resolved first because it is the cheapest of the three, and a missing one would
    // otherwise be reported only after a full Vulkan device creation and teardown.
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
        m_gpuStatus.frameSync = m_device->info().hasExternalSemaphoreFd;
    }
    recordHealth(&DaemonHealth::gpu, HealthStatus::Ok,
                 m_gpuStatus.name + " (" + m_gpuStatus.descriptorTier + ")");

    // Here rather than at startup: the device is created lazily, so a tier printed earlier
    // would be a guess. Format matches wallpaperx-device-probe, to be comparable line for line.
    std::printf("wallpaperx-daemon: GPU: %s | Vendor: %s | Descriptor tier: %s\n",
                m_device->info().gpuName.c_str(), toString(m_device->info().vendor),
                toString(m_device->descriptorTier()));
    std::fflush(stdout);
}

bool WallpaperManager::targetsFit(int monitorId, OutputSize size) const {
    // Keyed on the dimensions, not on mere existence: create() replaces and waits for idle, so
    // reuse is worth it, and a screen that reported a new size is not drawn at the old one.
    return m_targets->has(monitorId) && m_targets->get(monitorId).width == size.width &&
           m_targets->get(monitorId).height == size.height;
}

bool WallpaperManager::ensureTargets(int monitorId, OutputSize size) {
    if (targetsFit(monitorId, size)) return false;

    m_targets->create(monitorId, size.width, size.height, kTargetDepth);
    forgetImages(monitorId);
    return true;
}

void WallpaperManager::releaseTarget(int monitorId) {
    if (m_targets && m_targets->has(monitorId)) {
        m_targets->destroy(monitorId);
        forgetImages(monitorId);
    }
}

// Takes m_stateMutex itself, so its callers hold m_gpuMutex and not m_stateMutex.
void WallpaperManager::forgetImages(int monitorId) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    auto it = m_monitors.find(monitorId);
    if (it == m_monitors.end()) return;
    it->second.holds.clear();
    // With every image replaced there is nothing left to refuse a consumer for, so a refusal
    // after this is a new episode rather than the one these reports covered.
    it->second.refusalsReported.clear();
    it->second.hasFrame = false;
}

std::optional<OutputSize> WallpaperManager::reportedSizeLocked(int monitorId) const {
    // A scan, not an index: bindOutputLocked() gives each monitor at most one name, so this
    // holds no more entries than there are monitors.
    for (const auto& [name, boundId] : m_outputs) {
        if (boundId != monitorId) continue;
        const auto size = m_outputSizes.find(name);
        if (size == m_outputSizes.end()) return std::nullopt;
        return size->second;
    }
    return std::nullopt;
}

// Bound names number at most kMaxMonitors, so past that there is always an unbound one to drop.
// Which goes does not matter at any real connector count; the bound is against a client cycling
// through names.
void WallpaperManager::pruneOutputReportsLocked() {
    const auto limit = static_cast<std::size_t>(kMaxMonitors);
    for (auto it = m_outputSizes.begin(); it != m_outputSizes.end() && m_outputSizes.size() > limit;) {
        if (m_outputs.count(it->first) != 0) {
            ++it;
            continue;
        }
        it = m_outputSizes.erase(it);
    }
    for (auto it = m_outputsNotDrawn.begin(); it != m_outputsNotDrawn.end() && m_outputsNotDrawn.size() > limit;) {
        if (m_outputs.count(*it) != 0) {
            ++it;
            continue;
        }
        it = m_outputsNotDrawn.erase(it);
    }
}

OutputSize WallpaperManager::targetSizeLocked(int monitorId) const {
    return reportedSizeLocked(monitorId).value_or(OutputSize{m_config.width, m_config.height});
}

void WallpaperManager::redrawIfResizedLocked(int monitorId, OutputSize before, std::vector<int>& toRender) {
    if (targetSizeLocked(monitorId) == before) return;
    auto it = m_monitors.find(monitorId);
    // A quarantined request is never drawn, so a pass for it would draw nothing.
    if (it == m_monitors.end() || !isRenderableType(it->second.type) || it->second.quarantined) return;
    // Recorded before the bump, so a render owed is never lost to an allocation failure.
    toRender.push_back(monitorId);
    // The frame stays until the pass that redraws it rebuilds the images; renderPending() does not
    // let a pass overtaken after that leave the monitor without one. A pass that fails is reported
    // like any failed render.
    ++it->second.requestSeq;
}

std::vector<uint64_t> WallpaperManager::flattenHolds(const std::vector<BufferHold>& holds) {
    std::vector<uint64_t> held;
    for (const BufferHold& hold : holds) {
        held.insert(held.end(), hold.buffers.begin(), hold.buffers.end());
    }
    return held;
}

std::vector<uint64_t> WallpaperManager::heldBuffersLocked(int monitorId) const {
    auto it = m_monitors.find(monitorId);
    if (it == m_monitors.end()) return {};
    return flattenHolds(it->second.holds);
}

WallpaperManager::Handover WallpaperManager::withHandover(std::vector<BufferHold> holds, const std::string& client,
                                                          uint64_t bufferId) {
    auto holder = std::find_if(holds.begin(), holds.end(),
                               [&client](const BufferHold& hold) { return hold.client == client; });
    if (holder == holds.end()) {
        if (holds.size() >= kMaxHoldersPerMonitor) return {};
        holds.push_back(BufferHold{client, {}});
        holder = holds.end() - 1;
    }

    // The same image again -- the ring cycles, so a consumer is handed the buffer it already holds
    // every kTargetDepth renders -- is the claim it already has, not a second one.
    if (std::find(holder->buffers.begin(), holder->buffers.end(), bufferId) != holder->buffers.end()) {
        return Handover{std::move(holds), Handover::Outcome::AlreadyHeld};
    }

    // Refused rather than trimmed: this consumer's oldest image is one it has given no sign of
    // having stopped reading, and a claim taken back here is a render drawn under a reader.
    if (holder->buffers.size() >= kMaxHeldPerClient) return {};

    holder->buffers.push_back(bufferId);
    return Handover{std::move(holds), Handover::Outcome::Granted};
}

bool WallpaperManager::dropHoldLocked(MonitorState& state, const std::string& client, uint64_t bufferId) {
    auto holder = std::find_if(state.holds.begin(), state.holds.end(),
                               [&client](const BufferHold& hold) { return hold.client == client; });
    if (holder == state.holds.end()) return false;
    if (std::erase(holder->buffers, bufferId) == 0) return false;
    if (holder->buffers.empty()) state.holds.erase(holder);
    return true;
}

// The one place that decides what an image coming free is worth: a render if one is owed,
// otherwise a repeat of the announcement, so a consumer refused for want of a free image hears
// that the ring has moved. Caller must hold m_stateMutex.
void WallpaperManager::reportFreedImageLocked(int monitorId, const MonitorState& state, std::vector<int>& toRender,
                                              std::vector<FrameAvailable>* announce) const {
    // Independent: a redraw at a new size is owed while the frame it replaces is still served, and
    // that frame is what a refused consumer can take now.
    if (state.requestSeq != state.servicedSeq) toRender.push_back(monitorId);
    if (announce != nullptr && frameStillReadableLocked(state)) {
        announce->push_back(FrameAvailable{monitorId, state.generation});
    }
}

std::vector<int> WallpaperManager::releaseBuffer(const std::string& client, int monitorId, uint64_t bufferId,
                                                 std::vector<FrameAvailable>* announce) {
    requireValidMonitorId(monitorId);

    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    auto it = m_monitors.find(monitorId);
    if (it == m_monitors.end()) return {};

    if (!dropHoldLocked(it->second, client, bufferId)) return {};

    std::vector<int> toRender;
    reportFreedImageLocked(monitorId, it->second, toRender, announce);
    return toRender;
}

std::vector<int> WallpaperManager::forgetHeldBuffers(const std::string& client, std::vector<FrameAvailable>* announce) {
    std::vector<int> toRender;

    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    for (auto& [monitorId, state] : m_monitors) {
        // Whether or not it held anything: a consumer that left is one whose refusal episode is
        // over, and the name would otherwise sit in the list until the ring is rebuilt.
        std::erase_if(state.refusalsReported,
                      [&client](const MonitorState::RefusalReport& report) { return report.client == client; });
        const size_t before = state.holds.size();
        std::erase_if(state.holds, [&client](const BufferHold& hold) { return hold.client == client; });
        if (state.holds.size() == before) continue;
        reportFreedImageLocked(monitorId, state, toRender, announce);
    }
    return toRender;
}

std::vector<uint64_t> WallpaperManager::intersectImportModifiersLocked() const {
    std::vector<uint64_t> shared;
    bool first = true;
    for (const auto& [client, reported] : m_importModifiersByClient) {
        if (first) {
            shared = reported;
            first = false;
            continue;
        }
        std::vector<uint64_t> both;
        for (uint64_t candidate : shared) {
            if (std::find(reported.begin(), reported.end(), candidate) != reported.end()) {
                both.push_back(candidate);
            }
        }
        shared = std::move(both);
        if (shared.empty()) break;
    }
    return shared;
}

bool WallpaperManager::frameStillReadableLocked(const MonitorState& state) const {
    if (!state.hasFrame) return false;
    // Empty is "nobody is reporting", which constrains nothing; kDrmModifierInvalid is an
    // opaque-fd frame, which carries no layout for a report to exclude.
    if (m_importModifiers.empty() || state.frameModifier == kDrmModifierInvalid) return true;
    return std::find(m_importModifiers.begin(), m_importModifiers.end(), state.frameModifier) !=
           m_importModifiers.end();
}

ImportModifierUpdate WallpaperManager::adoptImportModifiersLocked(std::vector<uint64_t> modifiers) {
    if (m_importModifiers == modifiers) {
        return {};  // idempotent: several plugin instances share one process and one answer
    }
    m_importModifiers = std::move(modifiers);
    m_importModifiersPending = true;

    // A statement about the set in force, so it is re-derived whenever that set changes rather
    // than accumulated: an entry the new set makes readable again would otherwise still cost the
    // device-idle wait, the rebuild and a visible flicker to arrive back at a frame that was
    // already fine.
    std::erase_if(m_importModifiersInvalidated, [this](int id) {
        auto it = m_monitors.find(id);
        return it != m_monitors.end() && frameStillReadableLocked(it->second);
    });

    // Not every monitor: only those the new set actually costs something. A frame whose layout
    // the set still allows stays readable, so rebuilding it would burn a render and a GPU stall
    // to arrive back where it started -- and a consumer withdrawing one layout it never used
    // would blank every monitor. A monitor with no frame is re-posted regardless: its last
    // attempt produced nothing, and a different set may be exactly what it was missing.
    //
    // Bumping requestSeq is what makes renderPending() act -- it refuses to re-render a request
    // it has already serviced.
    std::vector<int> toRender;
    for (auto& [id, state] : m_monitors) {
        if (frameStillReadableLocked(state)) continue;

        // Condemned whatever its type. A monitor that cannot be drawn right now still owns an
        // image built under the old set, and ensureTargets() reuses an image whose size matches
        // -- so skipping it here leaves a layout the consumer refused to be exported the moment
        // the monitor becomes renderable again, permanently.
        if (std::find(m_importModifiersInvalidated.begin(), m_importModifiersInvalidated.end(), id) ==
            m_importModifiersInvalidated.end()) {
            m_importModifiersInvalidated.push_back(id);
        }

        // Only a renderable one is asked to draw: posting the rest buys a pass that cannot
        // produce a frame.
        if (!isRenderableType(state.type)) continue;
        ++state.requestSeq;
        toRender.push_back(id);
    }
    return ImportModifierUpdate{true, std::move(toRender)};
}

ImportModifierUpdate WallpaperManager::forgetImportModifiers(const std::string& client) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (m_importModifiersByClient.erase(client) == 0) return {};

    // Through the same path as an accepted report, not a hand-rolled copy of it. Widening cannot
    // invalidate an image, but it does un-block a monitor left with no frame at all by the
    // departed consumer's set -- and that monitor recovers only if something re-renders it.
    return adoptImportModifiersLocked(intersectImportModifiersLocked());
}

ImportModifierUpdate WallpaperManager::setImportModifiers(const std::string& client, uint32_t drmFormat,
                                                          const std::vector<uint64_t>& modifiers) {
    // Only the format this daemon exports is meaningful. A report about another one is a
    // client mistake, and silently keeping it would leave the exporter narrowed by a set that
    // describes a different pixel layout.
    const uint32_t exported = drmFourccForVkFormat(OffscreenTargetManager::kFormat);
    if (drmFormat != exported) {
        throw InvalidRequest("import modifiers were reported for DRM format " + std::to_string(drmFormat) +
                             ", but this daemon exports " + std::to_string(exported));
    }
    // Bounded for the same reason every other client-supplied length is: this is copied into
    // daemon state and walked per image creation. A driver publishing more than this is not a
    // case worth carrying.
    if (modifiers.size() > kMaxImportModifiers) {
        throw InvalidRequest("too many import modifiers reported (" + std::to_string(modifiers.size()) +
                             ", limit " + std::to_string(kMaxImportModifiers) + ")");
    }
    if (modifiers.empty()) {
        // An empty set means "I can import nothing", which no consumer means. Refused rather
        // than stored, because storing it reads identically to "nobody has reported yet" and
        // would silently restore the unnegotiated behaviour.
        throw InvalidRequest("the reported import modifier set is empty");
    }

    // Sorted and deduplicated so that what is stored is the set, not one spelling of it. The
    // "has it changed?" test below compares vectors, and an order-sensitive answer let a client
    // re-report the same layouts in a different order to bump every frameless monitor's request
    // sequence -- which supersedes the render in flight, so no monitor ever committed a frame.
    // The length cap above stays on the raw list, so a huge all-duplicates report is still
    // refused rather than collapsed into a short one.
    std::vector<uint64_t> canonical = modifiers;
    std::sort(canonical.begin(), canonical.end());
    canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());

    std::lock_guard<std::mutex> stateLock(m_stateMutex);

    auto previous = m_importModifiersByClient.find(client);
    const bool hadPrevious = previous != m_importModifiersByClient.end();
    std::vector<uint64_t> restore;
    if (hadPrevious) restore = previous->second;

    m_importModifiersByClient[client] = std::move(canonical);
    std::vector<uint64_t> shared = intersectImportModifiersLocked();
    if (shared.empty()) {
        // Counted before the rollback, which puts the caller back and would make it count itself
        // among the others.
        const size_t others = m_importModifiersByClient.size() - 1;
        // Refused rather than stored: an empty intersection means no image can be created that
        // every reporting consumer could read, so accepting it would fail every later render
        // with the cause an unrelated call away. Undone so the caller's own report is what
        // failed, not the others'.
        if (hadPrevious) {
            m_importModifiersByClient[client] = std::move(restore);
        } else {
            m_importModifiersByClient.erase(client);
        }
        throw InvalidRequest("the reported import modifier set shares no layout with the " +
                             std::to_string(others) + " already reported by other frame consumers");
    }

    return adoptImportModifiersLocked(std::move(shared));
}

// Runs on the render thread with m_gpuMutex held, before any target is created.
void WallpaperManager::applyImportModifiersLocked() {
    std::vector<uint64_t> modifiers;
    std::vector<int> monitorIds;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        if (!m_importModifiersPending) return;
        modifiers = m_importModifiers;
        m_importModifiersPending = false;
        monitorIds = std::move(m_importModifiersInvalidated);
        m_importModifiersInvalidated.clear();
        for (int id : monitorIds) {
            auto it = m_monitors.find(id);
            if (it != m_monitors.end()) it->second.hasFrame = false;
        }
    }
    m_device->memory().setConsumerModifiers(std::move(modifiers));

    // Existing images keep the modifier they were created with, so a new set only affects
    // images made after it. Dropping them is what makes it take effect; ensureTargets()
    // rebuilds each on its next render. Only the monitors the set condemned -- the rest hold
    // a layout it still allows.
    for (int id : monitorIds) releaseTarget(id);
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

    // An early-out, not a guarantee: rendering is asynchronous, so without this the commonest
    // user error returns success and then silence. Both halves matter -- the type check alone
    // would send a mode-000 file down the render path.
    //
    // Known and accepted: this runs on the DBus event-loop thread, so a stalled mount blocks
    // every method. The render worker is no better -- it opens the same file and is FIFO, so a
    // bad path would wait behind every queued render.
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
        // Bumped even when type and path are what this monitor already held: the same path can
        // name different bytes, and a decode session opened on the old ones must not be reused.
        ++state.wallpaperSeq;
        state.lastError.clear();
        state.quarantined = false;

        // Cleared before the render, not after a failure. Whatever happens next, the frame
        // this monitor currently holds no longer matches the request just recorded, and
        // handing out a frame of the previous wallpaper under the new path is a worse
        // failure than reporting no frame at all. The generation counter is left alone --
        // it counts renders, and no render has happened yet.
        state.hasFrame = false;

        // A row kept for a newer build loses its monitor to this request, and the save below
        // drops it. Erased and reported once, here: the same collision is counted when the
        // config is restored, and a client claiming the id later cost the user the same
        // picture with nothing anywhere saying so.
        const auto kept = std::find_if(
            m_kept.carried().begin(), m_kept.carried().end(),
            [monitorId](const PersistedWallpaper& row) { return row.monitorId == monitorId; });
        if (kept != m_kept.carried().end() && KeptRows::dropWallpaper(*kept)) m_kept.countReplacedKept();

        toPersist = m_kept.rowsToWrite(liveWallpapers());
    }

    // Outside the lock, and before the render: what the user asked for is worth keeping
    // whether or not it turns out to draw.
    persist(toPersist);
}

void WallpaperManager::setMonitorOutput(int monitorId, const std::string& outputName, std::vector<int>& redraw) {
    requireValidMonitorId(monitorId);
    requireValidOutputName(outputName);

    std::vector<PersistedWallpaper> toPersist;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        // At most two monitors change size: this one, and the one the name leaves.
        const auto previous = m_outputs.find(outputName);
        const int leaving = previous != m_outputs.end() ? previous->second : monitorId;
        const OutputSize sizeHere = targetSizeLocked(monitorId);
        const OutputSize sizeLeaving = targetSizeLocked(leaving);
        redraw.reserve(redraw.size() + 2);

        // Idempotent is advertised, so a client may re-assert every name on each screen
        // change; persist() parks this thread -- the bus event loop -- on the write. Skipped
        // only when the file is known to hold what the map holds: any other answer, including a
        // write still outstanding, means re-asserting is the only retry a client can reach.
        const BindResult bound = bindOutputLocked(monitorId, outputName);
        if (bound == BindResult::Unchanged && m_writer.outcome().onDisk()) {
            return;
        }

        redrawIfResizedLocked(monitorId, sizeHere, redraw);
        if (leaving != monitorId) redrawIfResizedLocked(leaving, sizeLeaving, redraw);

        // A name carried for a build with more monitors is taken over by this one, and the save
        // below drops it. Erased and reported here for the reason its wallpaper sibling above
        // is: m_kept.rowsToWrite(liveWallpapers()) is const, runs on every save, and cannot say what it dropped.
        // Unchanged means the name was already this monitor's, so nothing new is superseded.
        if (bound != BindResult::Unchanged) {
            const auto kept = std::find_if(
                m_kept.carried().begin(), m_kept.carried().end(),
                [&outputName](const PersistedWallpaper& row) { return row.output == outputName; });
            if (kept != m_kept.carried().end() && !kept->output.empty()) {
                kept->output.clear();
                kept->outputSize.reset();
                m_kept.countTakenOverName();
            }
        }

        toPersist = m_kept.rowsToWrite(liveWallpapers());
    }

    // Outside the lock. Persisted because only the app can name a screen again, and it may not
    // run for weeks. Reported on this call's own write rather than on persistence health, which
    // is an aggregate carrying other monitors' failures: the contract is that an accepted reply
    // means this mapping reached the file.
    std::string whyNot;
    if (!persist(toPersist, &whyNot)) {
        throw std::runtime_error(whyNot.empty() ? "the screen name did not reach the config file"
                                                : whyNot);
    }
}

void WallpaperManager::setOutputSize(const std::string& outputName, uint32_t width, uint32_t height,
                                     std::vector<int>& redraw) {
    requireValidOutputName(outputName);
    const OutputSize size{width, height};
    if (const std::string why = outputSizeError(size); !why.empty()) throw InvalidRequest(why);

    std::vector<PersistedWallpaper> toPersist;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        const auto bound = m_outputs.find(outputName);
        if (bound == m_outputs.end()) {
            throw UnknownOutput("no monitor answers to output " + sanitizeForLog(outputName) +
                                "; it must be named with SetMonitorOutput first");
        }
        // The same rule as re-asserting a name: an unchanged report writes only while the file
        // is not known to hold it.
        const auto known = m_outputSizes.find(outputName);
        if (known != m_outputSizes.end() && known->second == size && m_writer.outcome().onDisk()) return;

        redraw.reserve(redraw.size() + 1);
        const OutputSize before = targetSizeLocked(bound->second);
        m_outputSizes.insert_or_assign(outputName, size);
        pruneOutputReportsLocked();
        redrawIfResizedLocked(bound->second, before, redraw);
        toPersist = m_kept.rowsToWrite(liveWallpapers());
    }

    queuePersist(toPersist);
}

void WallpaperManager::setOutputVisible(const std::string& outputName, bool visible) {
    requireValidOutputName(outputName);

    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    const auto bound = m_outputs.find(outputName);
    if (bound == m_outputs.end()) {
        throw UnknownOutput("no monitor answers to output " + sanitizeForLog(outputName) +
                            "; it must be named with SetMonitorOutput first");
    }
    // Not persisted: what a screen was doing when the daemon last ran says nothing about what it
    // is doing now.
    const bool wasDrawn = m_outputsNotDrawn.count(outputName) == 0;
    if (visible) {
        m_outputsNotDrawn.erase(outputName);
    } else {
        m_outputsNotDrawn.insert(outputName);
    }
    pruneOutputReportsLocked();

    // A screen coming back counts as having collected what it missed, which is what lets a video
    // start again before any consumer has had a chance to fetch: otherwise it would wait for a
    // hand-over that only a picture can prompt. On the change only -- re-asserting it would let a
    // client forge the evidence that something is collecting and keep a video decoding for nobody.
    if (!visible || wasDrawn) return;
    const auto it = m_monitors.find(bound->second);
    if (it != m_monitors.end()) it->second.uncollectedSince.reset();
}

WallpaperManager::BindResult WallpaperManager::bindOutputLocked(int monitorId,
                                                                const std::string& outputName) {
    bool displaced = false;
    const auto existing = m_outputs.find(outputName);
    if (existing != m_outputs.end()) {
        if (existing->second == monitorId) return BindResult::Unchanged;
        // Erased, not left: a name on two monitors is two plugin instances answering to one
        // screen, and whichever came back first would decide who shows the wrong picture.
        //
        // What that name reported about being drawn goes with the binding: no consumer has said
        // anything about the monitor taking it over, and an unreported screen is taken to be
        // drawn. Left behind, a screen that was dark before its name moved would stop the new
        // monitor's video with nothing able to correct it.
        m_outputsNotDrawn.erase(existing->first);
        m_outputs.erase(existing);
        displaced = true;
    }

    // A monitor answers to one name too: without this a screen the app renamed would go on
    // answering to its old name.
    for (auto it = m_outputs.begin(); it != m_outputs.end();) {
        if (it->second != monitorId) {
            ++it;
            continue;
        }
        m_outputsNotDrawn.erase(it->first);
        it = m_outputs.erase(it);
        displaced = true;
    }

    m_outputs.emplace(outputName, monitorId);
    return displaced ? BindResult::Displaced : BindResult::Bound;
}

LoadResult WallpaperManager::loadWithDeadline() const {
    // A detached thread and a shared promise, NOT std::async: the future std::async returns
    // blocks in its own destructor until the task finishes, which would wait exactly as long
    // as this exists to avoid. Nothing here ever joins.
    //
    // The store is copied in, so the thread outliving this call cannot reach a destroyed one.
    std::future<LoadResult> pending;
    try {
        auto answer = std::make_shared<std::promise<LoadResult>>();
        pending = answer->get_future();
        std::thread([answer, store = m_store] { answer->set_value(store.load()); }).detach();
    } catch (const std::exception& e) {
        // std::exception, not std::system_error: the promise, the future and the thread's copy
        // of the callable all allocate, so std::bad_alloc reaches here -- and config trouble is
        // reported, never a reason the daemon does not boot.
        LoadResult noThread;
        noThread.error =
            m_store.file().string() + ": could not be read, no thread available: " + e.what();
        return noThread;
    }

    if (pending.wait_for(kLoadDeadline) == std::future_status::ready) return pending.get();

    // Abandoned, not cancelled: the thread stays blocked in the kernel until the process
    // exits. Startup continues without a config rather than becoming unstoppable -- the
    // stuck thread is harmless, a stuck main() is not. Recorded because it is only harmless
    // until static destruction: see configLoadAbandoned().
    g_loadAbandoned.store(true, std::memory_order_release);
    LoadResult timedOut;
    timedOut.error = m_store.file().string() + ": did not respond within " +
                     std::to_string(kLoadDeadline.count()) +
                     "s, starting without it";
    return timedOut;
}


// Neither records health: every reader goes through snapshot(), which asks the writer and
// overwrites persistence with its answer. An abandoned wait marks the worker stalled, so that
// is in the answer too.
void WallpaperManager::queuePersist(const std::vector<PersistedWallpaper>& entries) {
    m_writer.queue(entries);
}

bool WallpaperManager::persist(const std::vector<PersistedWallpaper>& entries, std::string* whyNot) {
    std::string why;
    if (m_writer.write(entries, &why)) return true;

    // Sanitized here, once, because this one leaves the process: the store composes its errors
    // around a path, and a path is arbitrary bytes the daemon did not choose.
    if (whyNot != nullptr) *whyNot = sanitizeForLog(why, 1024, LogFraming::Bare);
    return false;
}

void WallpaperManager::applyPersistOutcomeLocked(DaemonHealth& health) const {
    using State = ConfigWriter::Outcome::State;
    // One call, so every part of the report describes the same instant.
    const ConfigWriter::Outcome outcome = m_writer.outcome();

    // Written into the caller's copy rather than recorded: the write finishes on a thread
    // that must never reach this object, so the outcome is folded in when someone asks.
    const auto report = [&health](HealthStatus status, const std::string& detail) {
        health.persistence.status = status;
        health.persistence.detail = sanitizeForLog(detail, 1024, LogFraming::Bare);
    };

    switch (outcome.state) {
        case State::Stalled:
        case State::Failed:
            report(HealthStatus::Fail, keptDetail(outcome.why));
            return;
        case State::Outstanding:
            // Pending, not Fail: the startup repair is queued and never waited on, so Fail here
            // warned that wallpapers may not survive a restart on every repaired start. A copy
            // this run could not take is the exception -- that loss is already real, and it
            // must not be downgraded by a write happening to be in flight when someone asks.
            report(m_preserveFailed ? HealthStatus::Fail : HealthStatus::Pending,
                   keptDetail(outcome.why));
            return;
        case State::NeverAsked:
            // Whatever the constructor recorded still stands, and the status alone: that detail
            // was sanitized when recorded, and sanitizeForLog is not idempotent -- a second pass
            // turns one backslash in a path into two.
            if (m_preserveFailed) health.persistence.status = HealthStatus::Fail;
            return;
        case State::OnDisk:
            break;
    }

    const SaveResult& saved = outcome.saved;
    std::string base = m_writer.file().string();
    if (saved.skippedNonUtf8 > 0) {
        // The promise about names is made only when it is true: the two halves of a row fail
        // independently, so a dropped wallpaper says nothing about the name beside it.
        base += " (" + std::to_string(saved.skippedNonUtf8) +
                " wallpapers dropped: a path or type is not valid UTF-8" +
                (saved.skippedNames == 0 ? "; any screen name on those rows was kept)" : ")");
    }
    if (saved.skippedNames > 0) {
        base += " (" + std::to_string(saved.skippedNames) +
                " screen names dropped: not valid UTF-8; those screens must be named again)";
    }
    // Fail, not Ok: the write succeeded but those monitors will not come back, and a
    // health field that reads ok is the only thing that would say otherwise. A copy this run
    // could not take is the same kind of loss, and outlives the write that took it.
    report(saved.skippedNonUtf8 > 0 || saved.skippedNames > 0 || m_preserveFailed
               ? HealthStatus::Fail
               : HealthStatus::Ok,
           keptDetail(base));
}
std::vector<int> WallpaperManager::restoreSaved() {
    std::vector<int> monitorIds;
    std::vector<PersistedWallpaper> pending;
    pending.swap(m_restore);

    // A name lost at load is invisible otherwise: it is in no monitor's status, and the
    // entry it came from may restore perfectly well without it.
    int displacedNames = 0;
    int rejectedNames = 0;
    int rejectedSizes = 0;
    int duplicateIds = 0;

    // Screen name -> the kept row that claimed it last, in file order. The sweep at the end
    // needs "who claimed this name last", which m_kept.carried()'s own order does not carry.
    std::map<std::string, int> keptNameOwner;

    // Monitor id -> its row in m_kept.carried(). Indexed rather than scanned: the file is capped at
    // a mebibyte, roughly 28,000 rows, and a scan per row is that squared in comparisons on the
    // startup path -- before the bus name is claimed, so the desktop has no daemon meanwhile.
    std::map<int, std::size_t> keptIndex;

    // What the last run died applying, per monitor. Read here, outside the lock and once per
    // monitor rather than per row, for the same reason the index above exists.
    std::map<int, std::optional<CrashRecord>> crashes;
    for (const PersistedWallpaper& entry : pending) {
        if (entry.monitorId < 0 || entry.monitorId >= kMaxMonitors || crashes.count(entry.monitorId) != 0) continue;
        crashes.emplace(entry.monitorId, readCrashRecord(m_config.stateDir, entry.monitorId));
    }

    bool repaired = false;
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    for (const PersistedWallpaper& entry : pending) {
        // Three outcomes for a name, each of them reported: bound here, carried for a build
        // whose limit admits the id, or dropped. Only the id limit earns carrying -- and the
        // byte cap is this build's own constant too, so symmetry alone would carry an over-long
        // name as well. Settled against (S10.22): monitor counts grow, so an id above the limit
        // is a value a later build may accept, while a legitimate 256-byte connector name is not
        // a thing that exists, and carrying one writes bytes no build accepts into every save.
        const bool nameValid = !entry.output.empty() && outputNameError(entry.output).empty();
        const bool nameTravels = nameValid && entry.monitorId >= kMaxMonitors;
        if (!entry.output.empty() && !nameValid && !nameTravels) ++rejectedNames;
        const bool sizeValid = entry.outputSize && outputSizeError(*entry.outputSize).empty();

        // Dropped, not kept: a kept entry is only ever superseded by a live monitor
        // claiming its id, and a negative id can never be one -- so keeping it means
        // carrying it through every save for the life of the install. An id above this
        // build's limit is different and stays below: a larger kMaxMonitors would take it.
        if (entry.monitorId < 0) {
            // Both halves counted, because a row can lose both: reporting one of two losses
            // sends the user looking for the wrong thing.
            if (!entry.type.empty() || !entry.path.empty() || entry.output.empty()) {
                m_kept.countDroppedEntry();
            }
            if (nameValid) m_kept.countDroppedName();
            continue;
        }

        // Counted only beside a name that survives: a size lost with its name is that name's loss.
        if (nameValid && entry.outputSize && !sizeValid) ++rejectedSizes;

        // Independent halves of a row: this one is restored even when the other cannot be.
        // Last-wins -- file order carries no meaning. What matters is that the loss is
        // reported, not left as two monitors quietly answering to one screen.
        if (nameValid && entry.monitorId < kMaxMonitors) {
            if (bindOutputLocked(entry.monitorId, entry.output) == BindResult::Displaced) ++displacedNames;
            // A row without one does not erase a size an earlier row gave the same screen.
            if (sizeValid) m_outputSizes.insert_or_assign(entry.output, *entry.outputSize);
        }

        // A row with no wallpaper stops here rather than falling into the arm below, whose
        // health text is the user's only notice that a wallpaper did not come back. Only a
        // name bound for a bigger build is still worth carrying; a row with nothing left on
        // it would otherwise ride every save for the life of the install.
        if (entry.type.empty() && entry.path.empty() && !nameTravels) {
            if (entry.output.empty()) m_kept.countDroppedEntry();
            continue;
        }

        // The manager's own rules, not the store's: a type this build has never heard of, an
        // id a larger kMaxMonitors would admit, or a known type with no path -- which is not a
        // row this build wrote, so it is another writer's or a newer build's. None is rendered
        // here, all are kept, so the next save does not delete what a newer build understands.
        if (!isKnownType(entry.type) || entry.path.empty() || entry.monitorId >= kMaxMonitors) {
            // One row per id, merged half by half: last wins, and a loss is counted only
            // where both rows really filled the same half -- a later row carrying just a name
            // must not delete the wallpaper this arm exists to preserve.
            // One namespace with the rows this build *can* apply, not two: an id a live monitor
            // already holds keeps that wallpaper at the next save, so a kept row for it is
            // dropped. Counted here, while the note the user reads is still being built --
            // otherwise the entry left config.json with nothing anywhere reporting it.
            const bool heldLive = liveHolds(entry.monitorId);

            const auto slot = keptIndex.find(entry.monitorId);
            if (slot == keptIndex.end()) {
                PersistedWallpaper keep = entry;
                if (heldLive && KeptRows::dropWallpaper(keep)) ++duplicateIds;
                if (!nameTravels) keep.output.clear();
                if (!nameTravels || !sizeValid) keep.outputSize.reset();
                // Nothing left to carry. Kept, it would ride every save for the life of the
                // install and come back as a wallpaper that did not return.
                if (keep.type.empty() && keep.path.empty() && keep.output.empty()) continue;
                if (nameTravels) keptNameOwner[keep.output] = keep.monitorId;
                keptIndex.emplace(entry.monitorId, m_kept.carried().size());
                m_kept.carried().push_back(std::move(keep));
                continue;
            }
            PersistedWallpaper& held = m_kept.carried()[slot->second];
            if (!entry.type.empty() || !entry.path.empty()) {
                if (heldLive || !held.type.empty() || !held.path.empty()) ++duplicateIds;
                if (!heldLive) {
                    held.type = entry.type;
                    held.path = entry.path;
                }
            }
            if (nameTravels) {
                // The name counter, not the row counter: what was lost here is a screen's
                // name, and duplicateIds counts wallpapers.
                if (!held.output.empty() && held.output != entry.output) ++displacedNames;
                if (held.output != entry.output) held.outputSize.reset();
                held.output = entry.output;
                if (sizeValid) held.outputSize = entry.outputSize;
                keptNameOwner[entry.output] = entry.monitorId;
            }
            continue;
        }

        MonitorState& state = m_monitors[entry.monitorId];
        // m_monitors is empty when this runs, so anything already here is a row this same loop
        // applied: a second row for one monitor, whose wallpaper this one replaces. Reported
        // for the same reason the kept-row collision one arm above is -- the user is otherwise
        // told the restore was clean over a file that cost them a picture.
        const bool alreadyRestored = !state.type.empty() || !state.path.empty();
        if (alreadyRestored) ++duplicateIds;
        // The same one namespace, the other way round: a row kept for a newer build is
        // superseded by this one, and the next save drops it. Erased here so keptDetail()
        // stops offering it back, and counted so the loss is in the note rather than in silence.
        if (const auto slot = keptIndex.find(entry.monitorId); slot != keptIndex.end()) {
            PersistedWallpaper& held = m_kept.carried()[slot->second];
            if (KeptRows::dropWallpaper(held)) ++duplicateIds;
        }
        state.type = entry.type;
        state.path = entry.path;
        ++state.requestSeq;
        ++state.wallpaperSeq;
        state.lastError.clear();
        state.hasFrame = false;

        // Refused, not rendered: the last run was applying exactly this when it died, and the
        // restart that brought this one up would otherwise re-apply it and die the same way.
        // The record stays on disk until a pass for this monitor replaces it, which only an
        // explicit request causes. Matched on type and path, so a wallpaper set since the crash
        // is applied as normal.
        const std::optional<CrashRecord>& crash = crashes[entry.monitorId];
        state.quarantined = crash && crash->type == entry.type && crash->path == entry.path;
        if (state.quarantined) {
            state.lastError = sanitizeForLog(describeCrash(*crash), 1024, LogFraming::Bare);
            state.lastErrorIsDaemonFault = false;
            // Nothing is owed: status must not report a render in progress for one that will
            // not be started.
            state.servicedSeq = state.requestSeq;
            std::fprintf(stderr, "wallpaperx-daemon: monitor %d: %s (%s)\n", entry.monitorId,
                         state.lastError.c_str(), sanitizeForLog(entry.path).c_str());
        }
        // Once per monitor: a second entry here is a second full render of one request. A
        // quarantined one is posted too, and its pass is a no-op.
        if (!alreadyRestored) monitorIds.push_back(entry.monitorId);
    }

    // Settled here, not in the save. m_kept.rowsToWrite(liveWallpapers()) is const and runs on every write, so
    // a name it dropped could only vanish in silence -- and keptDetail() would already
    // have promised it back. Decided by keptNameOwner, which records the LAST row in the file
    // to claim each name: a kept row's position records when its id was first seen, which is a
    // different fact, since a later row for that id is merged into the place it already has.
    for (PersistedWallpaper& kept : m_kept.carried()) {
        if (kept.output.empty()) continue;
        const auto owner = keptNameOwner.find(kept.output);
        if (m_outputs.count(kept.output) != 0 || owner == keptNameOwner.end() ||
            owner->second != kept.monitorId) {
            kept.output.clear();
            kept.outputSize.reset();
            ++displacedNames;
        }
    }

    // Later rows can unbind names an earlier row sized, and the file holds thousands of rows.
    pruneOutputReportsLocked();

    if (displacedNames > 0) {
        // Same correction as the entry counter below: a name this build could bind displaces a
        // later row carrying the same name for an id it cannot, so the earlier row wins there.
        m_kept.note(std::to_string(displacedNames) +
                           " screen names displaced by another entry claiming the same name or "
                           "the same screen; the binding this build could make was kept");
    }
    if (rejectedNames > 0) {
        m_kept.note(std::to_string(rejectedNames) +
                           " screen names dropped, unusable; those screens must be named again");
    }
    if (rejectedSizes > 0) {
        m_kept.note(std::to_string(rejectedSizes) +
                    " screen sizes dropped, unusable; those screens are drawn at the default size "
                    "until they report again");
    }
    if (duplicateIds > 0) {
        // Not "the last entry won": three collisions feed this counter and one is resolved by
        // the EARLIER row, the one this build can show. The copy is named by its own note
        // rather than located here, since where it lands depends on the state directory.
        m_kept.note(std::to_string(duplicateIds) +
                           " entries dropped, another entry claimed the same monitor; the one "
                           "this build can show was kept, and the rest survive only in the "
                           "preserved copy named above");
    }

    // Kept entries are invisible otherwise: they are in no monitor's status and are not
    // malformed, so nothing would tell the user those wallpapers did not come back.
    // Recomposed from the raw path rather than appended to the recorded detail, which is
    // already sanitized and would be escaped a second time.
    repaired = displacedNames > 0 || rejectedNames > 0 || rejectedSizes > 0 || duplicateIds > 0 ||
               m_kept.anyDropped();
    if ((!m_kept.carried().empty() || repaired) && m_health.persistence.status == HealthStatus::Ok) {
        recordHealthLocked(&DaemonHealth::persistence, HealthStatus::Ok,
                           keptDetail(m_store.file().string()));
    }
    // Held, not written: this runs before the bus name is claimed, and the write would land
    // on a running daemon's config and on top of the bytes preserveUnreadableConfig() has
    // yet to copy aside. main() writes it once both are settled.
    if (repaired) m_repair = m_kept.rowsToWrite(liveWallpapers());
    return monitorIds;
}

void WallpaperManager::closeVideoSessionLocked(int monitorId) {
    auto it = m_videoSessions.find(monitorId);
    if (it == m_videoSessions.end()) return;

    VideoSession session = std::move(it->second);
    m_videoSessions.erase(it);
    // No crash record, for the reason ~WallpaperManager writes none: this frees a decoder for a
    // file that has already opened, decoded and drawn, and a session is now released whenever a
    // screen goes dark, so a record here would refuse a working wallpaper after any power cut in
    // the five seconds that follow a lock. What the record is for is the file's content running --
    // the open and first decode, and the draw -- and those keep theirs.
    session.pipeline.reset();
}

void WallpaperManager::dropStaleVideoSessionLocked(int monitorId, std::optional<Drawable> kind,
                                                   uint64_t wallpaperSeq) {
    const auto it = m_videoSessions.find(monitorId);
    if (it == m_videoSessions.end()) return;
    if (kind == Drawable::Clip && it->second.wallpaperSeq == wallpaperSeq) return;
    closeVideoSessionLocked(monitorId);
}

static_assert(WallpaperManager::kMaxVideoSessions > 0, "the monitor being drawn needs a session of its own");

bool WallpaperManager::screenDrawnLocked(int monitorId) const {
    // A scan, not an index, for reportedSizeLocked()'s reason: one name per monitor, so it holds
    // no more entries than there are monitors.
    for (const auto& [name, boundId] : m_outputs) {
        if (boundId == monitorId) return m_outputsNotDrawn.count(name) == 0;
    }
    return true;
}

bool WallpaperManager::watchedLocked(int monitorId, const MonitorState& state) const {
    if (!screenDrawnLocked(monitorId)) return false;
    // The clock is read here and not passed in: a round samples its time before it decodes, and a
    // caller asking after a decode would be answering with a reading the decode has already
    // outlived.
    if (!state.uncollectedSince) return true;
    return std::chrono::steady_clock::now() - *state.uncollectedSince < kUnwatchedAfter;
}

std::optional<uint64_t> WallpaperManager::currentSeqLocked(int monitorId, uint64_t wallpaperSeq) const {
    const auto it = m_monitors.find(monitorId);
    if (it == m_monitors.end() || !it->second.hasFrame || it->second.requestSeq != it->second.servicedSeq ||
        it->second.wallpaperSeq != wallpaperSeq) {
        return std::nullopt;
    }
    // A file with one picture is already showing all it has.
    if (it->second.parkedWallpaperSeq == wallpaperSeq) return std::nullopt;
    return it->second.requestSeq;
}

std::optional<uint64_t> WallpaperManager::playingSeqLocked(int monitorId, uint64_t wallpaperSeq) const {
    const std::optional<uint64_t> current = currentSeqLocked(monitorId, wallpaperSeq);
    if (!current) return std::nullopt;
    const auto it = m_monitors.find(monitorId);
    if (!watchedLocked(monitorId, it->second)) return std::nullopt;
    return current;
}

void WallpaperManager::noteHandoverServed(int monitorId) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    const auto it = m_monitors.find(monitorId);
    // The newest picture is what a hand-over carries, so there is nothing outstanding after one.
    if (it != m_monitors.end()) it->second.uncollectedSince.reset();
}

bool WallpaperManager::evictIdleVideoSessionLocked() {
    const VideoSession* victim = nullptr;
    int victimId = 0;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        for (const auto& [monitorId, session] : m_videoSessions) {
            // Not the clock's own test: a screen showing a current picture never loses its decoder
            // for another screen's sake, and nobody having fetched for a couple of seconds is a
            // guess about the consumer, not evidence that the screen is gone. A screen that has
            // said it is not being drawn is evidence, and is taken at once.
            if (currentSeqLocked(monitorId, session.wallpaperSeq) && screenDrawnLocked(monitorId)) continue;
            if (victim != nullptr && victim->lastUsed <= session.lastUsed) continue;
            victim = &session;
            victimId = monitorId;
        }
    }
    if (victim == nullptr) return false;

    // requestSeq is left alone deliberately: bumping it would owe a render for a monitor whose
    // picture is unchanged, which reopens the file this eviction just closed -- and reports a
    // render outstanding until it does. Whatever re-posts the monitor opens a session then.
    closeVideoSessionLocked(victimId);
    return true;
}

WallpaperManager::VideoSession& WallpaperManager::videoSessionLocked(int monitorId, const std::string& path,
                                                                    uint64_t wallpaperSeq) {
    if (const auto it = m_videoSessions.find(monitorId); it != m_videoSessions.end()) {
        it->second.lastUsed = ++m_videoSessionUse;
        return it->second;
    }

    while (m_videoSessions.size() >= kMaxVideoSessions) {
        if (evictIdleVideoSessionLocked()) continue;
        // A limit of this daemon's, so it answers for it: the file is fine and would play on a
        // screen whose video was stopped.
        throw ResourceExhausted("this daemon plays at most " + std::to_string(kMaxVideoSessions) +
                                " videos at once and every one of them is showing; stop one to play " + path);
    }

    std::unique_ptr<VideoPipeline> pipeline;
    for (bool retried = false;; retried = true) {
        // On disk across the open and the first decode -- the spans in which the file's content is
        // what runs. Per attempt, and withdrawn before the eviction below, which frees another
        // file's decoder on a handler with one slot.
        std::optional<ApplyingRecord> applying;
        try {
            applying.emplace(m_config.stateDir, monitorId, "video", path);
            pipeline = std::make_unique<VideoPipeline>(*m_device, path, m_resolvedShaderDir,
                                                       VideoPipeline::Playback::Loop);
            // A file with no picture cannot be drawn at a later pass either, so this fails now.
            if (!pipeline->decodeNext()) {
                throw WallpaperContentError("no picture could be decoded from " + path);
            }
            break;
        } catch (const std::exception&) {
            // What the card can hold is settled by trying: a decoder's images are ffmpeg's to
            // size, so nothing here can predict them. A shortage frees a session nothing is
            // showing and asks once more; a second shortage, or none to free, is reported.
            pipeline.reset();  // still under this attempt's record: it is this file being freed
            applying.reset();
            if (retried || !classifyFailure().exhausted || !evictIdleVideoSessionLocked()) throw;
        }
    }

    const auto entry =
        m_videoSessions
            .insert_or_assign(monitorId, VideoSession{wallpaperSeq, ++m_videoSessionUse, path, std::move(pipeline), {}})
            .first;
    return entry->second;
}

void WallpaperManager::markShown(VideoSession& session, SteadyTime now) {
    using Session = VideoSession;
    // Terminal for the schedule: what is on screen is what the session holds.
    if (std::holds_alternative<Session::OnScreen>(session.clock)) return;
    SteadyTime due = now;
    if (const auto* waiting = std::get_if<Session::Waiting>(&session.clock);
        waiting != nullptr && now - waiting->due <= ownInterval(session.pipeline->timing())) {
        due = waiting->due;
    }
    session.clock = Session::OnScreen{due};
}

uint64_t WallpaperManager::commitFrameLocked(MonitorState& state, const OffscreenTarget& target) {
    ++state.generation;
    const bool hadFrame = state.hasFrame;
    state.hasFrame = true;
    // The oldest uncollected picture of this published run. Not this one, because what says a
    // consumer has stopped collecting is how long the first picture it ignored has been waiting;
    // and not an older one either, since a monitor whose frame was thrown away -- a new wallpaper,
    // a failed render, a rebuilt ring -- has had nothing to collect since, so the wait starts here.
    if (!hadFrame || !state.uncollectedSince) state.uncollectedSince = std::chrono::steady_clock::now();
    state.width = target.width;
    state.height = target.height;
    state.frameModifier = target.drmModifier;
    // Cleared here rather than relying on requestWallpaper having cleared it: a monitor must not
    // report has_frame beside a stale last_error, or a client branching on it calls a working
    // wallpaper broken.
    state.lastError.clear();
    state.lastErrorIsDaemonFault = false;
    // Refusals since the last frame, so it says how far behind this monitor is now.
    state.deferredRenders = 0;
    return state.generation;
}

WallpaperManager::RenderFailure WallpaperManager::classifyFailure() {
    try {
        throw;
    } catch (const GpuFailure& e) {
        // The GPU going wrong rather than the image: the caller re-records gpu health, otherwise a
        // device that dies after one successful render reads ok forever.
        return RenderFailure{e.what(), true, true, false};
    } catch (const WallpaperContentError& e) {
        // The image, not the daemon: a file the user can replace.
        return RenderFailure{e.what(), false, false, false};
    } catch (const ResourceExhausted& e) {
        // The daemon's to answer for like any other unclassified failure, but a caller that can
        // free something may get further by asking again.
        return RenderFailure{e.what(), true, false, true};
    } catch (const std::exception& e) {
        // Unclassified, so it counts against the daemon. Failing closed is the point: this is
        // where an allocation failure or a broken invariant arrives, and treating those as the
        // wallpaper's fault published an ok daemon with no picture on any monitor.
        return RenderFailure{e.what(), true, false, false};
    }
}

uint32_t& WallpaperManager::playbackBudgetLocked(MonitorState& state) {
    if (state.playbackRetriesFor != state.wallpaperSeq) {
        state.playbackRetriesFor = state.wallpaperSeq;
        state.playbackRetries = 0;
    }
    return state.playbackRetries;
}

void WallpaperManager::recordFailureLocked(MonitorState& state, const std::string& error, bool daemonFault) {
    // Sanitized where client text enters daemon state, so every consumer inherits it: the message
    // embeds the client's path. 1024 because the field is polled, and safe only because
    // describeRejection() puts the diagnosis before the path -- truncation keeps the head.
    state.lastError = sanitizeForLog(error, 1024, LogFraming::Bare);
    state.lastErrorIsDaemonFault = daemonFault;
    // Before the target is freed, which happens after this lock is dropped: a reader in between
    // would otherwise see a frame beside the error that says there is none.
    state.hasFrame = false;
}

ClockRound WallpaperManager::advanceVideo(SteadyTime now) {
    std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
    ClockRound round;
    // Reserved before anything is shown, so no allocation can fail between a commit and its
    // announcement, or between a failure and the render it leaves owed.
    round.shown.reserve(m_videoSessions.size());
    round.retry.reserve(m_videoSessions.size() + kMaxMonitors);
    const size_t sessionsBefore = m_videoSessions.size();
    for (auto it = m_videoSessions.begin(); it != m_videoSessions.end();) {
        // Advanced first: a failing session erases its own entry, and only its own.
        const int monitorId = (it++)->first;
        std::optional<SteadyTime> due;
        try {
            due = advanceSessionLocked(monitorId, now, round);
        } catch (const std::exception& e) {
            // Only reporting a failure can reach here, and a session whose failure could not be
            // reported still must not be advanced again: its pipeline threw. Dropped without a
            // record, since what went wrong was this daemon and not the file.
            m_videoSessions.erase(monitorId);
            std::fprintf(stderr, "wallpaperx-daemon: monitor %d stopped playing, and why could not be recorded: %s\n",
                         monitorId, sanitizeForLog(e.what(), 1024).c_str());
        }
        if (due && (!round.nextDue || *due < *round.nextDue)) round.nextDue = due;
    }
    reopenVideosLocked(now, m_videoSessions.size() < sessionsBefore, round);
    return round;
}

void WallpaperManager::reopenVideosLocked(SteadyTime now, bool freedSession, ClockRound& round) {
    // A budget, not a test: every render posted below will want a slot of its own, and posting
    // more than there are free would send the losers into videoSessionLocked() with a full map --
    // which cannot take a playing session and so refuses, blanking a monitor that was showing a
    // correct picture. What is not posted this round stays eligible for the next.
    size_t free = kMaxVideoSessions - m_videoSessions.size();

    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    // Nothing is reopened on a device that has just stopped answering: the rule advanceSessionLocked's
    // failure path states, and the recorded health is what carries it across the round boundary. A
    // render somebody asked for still clears it, which is how the daemon finds out the device is back.
    if (m_health.gpu.status == HealthStatus::Fail) return;

    for (auto& [monitorId, state] : m_monitors) {
        // Ahead of the tests below, so a monitor that cannot be posted does not spend the reopen
        // budget one of them takes.
        if (free == 0) break;
        if (drawableType(state.type) != Drawable::Clip || m_videoSessions.count(monitorId) != 0) continue;

        // A video that would be playing and has nothing to play with: its session was freed while
        // nobody was looking, and only a render opens the file again. Self-limiting -- the render
        // either opens a session or clears the frame, and both end this.
        bool owed = playingSeqLocked(monitorId, state.wallpaperSeq).has_value();

        // Or one that never got a session at all because every slot was taken. Its failure is the
        // daemon's own; a broken file records the file's fault instead. Only in a round that freed
        // a slot, and only while the reopen budget lasts: the round that frees a session is also
        // the round a failing video's own session dies in, so without the budget a file that
        // cannot be played would be reopened for as long as it kept failing.
        if (!owed && freedSession && !state.hasFrame && state.requestSeq == state.servicedSeq &&
            !state.lastError.empty() && state.lastErrorIsDaemonFault) {
            if (uint32_t& spent = playbackBudgetLocked(state); spent < kPlaybackRetries) {
                ++spent;
                owed = true;
            }
        }
        if (!owed) continue;

        // Recorded before the bump, as every other re-request is: a render owed must never be lost
        // to an allocation failure.
        round.retry.push_back(monitorId);
        ++state.requestSeq;
        --free;
    }
}

std::optional<SteadyTime> WallpaperManager::advanceSessionLocked(int monitorId, SteadyTime now, ClockRound& round) {
    using Session = VideoSession;
    Session& session = m_videoSessions.at(monitorId);
    const uint64_t wallpaperSeq = session.wallpaperSeq;

    std::optional<uint64_t> playing;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        playing = playingSeqLocked(monitorId, wallpaperSeq);
    }
    if (!playing) {
        // Stopped, for whatever reason: nobody can see it, a render is owed, its file held one
        // picture. The picture on screen stays where it is -- nothing here touches the frame -- and
        // all this decides is how long the decoder and its picture pool are kept against playing
        // starting again. What freeing them costs is not only the reopen: the file is opened from
        // the beginning, so a video that resumes after this starts over rather than carrying on.
        // Outside the state lock, because freeing them re-enters ffmpeg and the driver and every
        // bus handler would wait behind it. A session whose file held one picture waits the same
        // interval as any other: it draws no picture again, but a resize still redraws that picture
        // from it rather than reopening the file.
        if (!session.idleSince) session.idleSince = now;
        const SteadyTime releaseAt = *session.idleSince + kIdleSessionRelease;
        if (now < releaseAt) return releaseAt;
        closeVideoSessionLocked(monitorId);
        return std::nullopt;
    }
    const uint64_t seq = *playing;
    session.idleSince.reset();

    try {
        if (const auto* onScreen = std::get_if<Session::OnScreen>(&session.clock)) {
            // No crash record: this runs for as long as the wallpaper is shown, and the handler's
            // single slot cannot name every playing file at once.
            const SteadyTime shownDue = onScreen->due;
            const PictureTiming before = session.pipeline->timing();
            if (!session.pipeline->decodeNext()) {
                throw WallpaperContentError("no picture could be decoded from " + session.path +
                                            " when it started again");
            }
            const PictureTiming after = session.pipeline->timing();
            if (before.startsPass && after.startsPass) {
                // The pass that just ended held one picture, so the file does: it is already on
                // screen and there is nothing to show after it. Asking again would decode that
                // same picture for ever and announce it as a new frame each time. Recorded on the
                // monitor, where it outlives the session: the session itself is kept like any other
                // that has stopped, because a resize still redraws that picture from it rather than
                // reopening the file, and the idle release frees it on the same terms.
                //
                // `now`, not nothing: a session that stops here has no round scheduled to start its
                // idle countdown, and the worker would sleep with no deadline and hold the decoder
                // for the life of the daemon. The round this asks for takes the stopped branch
                // above, which owns that countdown.
                std::lock_guard<std::mutex> stateLock(m_stateMutex);
                const auto it = m_monitors.find(monitorId);
                if (it != m_monitors.end() && it->second.wallpaperSeq == wallpaperSeq) {
                    it->second.parkedWallpaperSeq = wallpaperSeq;
                }
                return now;
            }
            session.clock = Session::Waiting{shownDue + gapBetween(before, after)};
        }
        if (const auto* waiting = std::get_if<Session::Waiting>(&session.clock); waiting != nullptr && waiting->due > now) {
            return waiting->due;
        }

        const OffscreenTarget* target = nullptr;
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            target = m_targets->nextUnheld(monitorId, heldBuffersLocked(monitorId));
        }
        // The reserve exportCurrentFrame() keeps makes this unreachable; if it is reached, the
        // picture waits one interval rather than being drawn under a reader. Nothing is counted or
        // logged: deferredRenders counts renders owed, and this monitor owes none -- the render
        // path reports the same broken reserve the moment anything posts this monitor.
        if (target == nullptr) return now + ownInterval(session.pipeline->timing());

        uint64_t generation = 0;
        {
            // Checked and drawn under one acquisition, as a render's commit is: a request landing
            // in between must not see this wallpaper's picture published after it.
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            const auto it = m_monitors.find(monitorId);
            // Asked again with a fresh reading, since the decode above may have crossed the moment
            // this monitor stopped being watched. `now`, not nothing, for the park branch's reason:
            // a session that stops here has nothing else scheduled to start its idle countdown.
            if (it == m_monitors.end() || playingSeqLocked(monitorId, wallpaperSeq) != seq) return now;
            session.pipeline->draw(*m_targets, *target);
            generation = commitFrameLocked(it->second, *target);
            // A picture shown is playback working, so the reopens start again from here: what the
            // budget bounds is failures with nothing shown between them.
            playbackBudgetLocked(it->second) = 0;
        }
        markShown(session, now);
        round.shown.push_back(ClockRound::Shown{monitorId, generation});
        // The next picture is decoded at once, so its time is known before it is due.
        return now;
    } catch (const std::exception&) {
        const RenderFailure failure = classifyFailure();

        // The session goes before anything that can throw: a pipeline that threw is not usable, and
        // a failure this cannot finish reporting must not leave one to be drawn with again.
        closeVideoSessionLocked(monitorId);
        if (failure.gpuFault) recordHealth(&DaemonHealth::gpu, HealthStatus::Fail, failure.error);
        std::fprintf(stderr, "wallpaperx-daemon: monitor %d stopped playing: %s\n", monitorId,
                     sanitizeForLog(failure.error, 1024).c_str());

        // Recorded whenever the monitor still holds this wallpaper, whether or not a render is
        // owed, and the monitor is left owing one while the budget lasts: nothing else would ever
        // open the file again, and a frame dropped with no render owed is a screen blank until the
        // user acts. The render starts the file from the beginning -- a pipeline that threw cannot
        // go on -- so a file that always fails at the same picture would reopen for ever without
        // the budget. Spent, the error stands as the monitor's answer.
        //
        // Never after a GPU fault: rendering again on a device that has just stopped answering is
        // worse than stopping, and the recorded gpu health is what says so.
        bool failed = false;
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            const auto it = m_monitors.find(monitorId);
            if (it != m_monitors.end() && it->second.wallpaperSeq == wallpaperSeq) {
                recordFailureLocked(it->second, failure.error, failure.daemonFault);
                failed = true;
                if (uint32_t& spent = playbackBudgetLocked(it->second);
                    !failure.gpuFault && spent < kPlaybackRetries) {
                    ++spent;
                    ++it->second.requestSeq;
                    round.retry.push_back(monitorId);
                }
            }
        }
        if (failed) releaseTarget(monitorId);
        return std::nullopt;
    }
}

RenderOutcome WallpaperManager::renderPending(int monitorId) {
    // Bounds how long a stream of size changes, each landing mid-draw, can hold the worker. Past it
    // the monitor goes without a frame until the newest request's own post runs.
    constexpr int kAttemptsAfterRebuild = 3;

    std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
    bool rebuilt = false;
    for (int attempt = 0;; ++attempt) {
        const RenderPass pass = renderPassLocked(monitorId, rebuilt);
        // An attempt overtaken after the images were replaced has already thrown away the frame.
        // So the newer request is drawn now, still under the GPU lock, before any export can see
        // the gap -- rather than after whatever else is queued ahead of its own post.
        if (!pass.superseded || !rebuilt || attempt + 1 >= kAttemptsAfterRebuild) return pass.outcome;
    }
}

WallpaperManager::RenderPass WallpaperManager::renderPassLocked(int monitorId, bool& rebuilt) {

    std::string type;
    std::string path;
    uint64_t seq = 0;
    // Which wallpaper this pass is for, as against which render: a re-post for a new size shares
    // the wallpaper of the pass before it and so shares its decode session.
    uint64_t wallpaperSeq = 0;
    // Read with the sequence number: every change to it bumps that number, so a commit that finds
    // the sequence unmoved was drawn at the size still in force.
    OutputSize size;
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
        // Re-posted for some other reason -- an import-modifier report, a freed image -- but
        // still the request the last run died applying. Serviced without being decoded.
        if (it->second.quarantined) {
            it->second.servicedSeq = it->second.requestSeq;
            return {};
        }
        type = it->second.type;
        path = it->second.path;
        seq = it->second.requestSeq;
        wallpaperSeq = it->second.wallpaperSeq;
        size = targetSizeLocked(monitorId);
    }

    const std::optional<Drawable> kind = drawableType(type);
    dropStaleVideoSessionLocked(monitorId, kind, wallpaperSeq);
    if (!kind) {
        // Recorded only; nothing here draws a shader yet. Release the target --
        // nothing will read it again, and holding a full-size image per monitor for a
        // wallpaper type this build cannot draw is memory kept for no one.
        releaseTarget(monitorId);
        // A record left by a build that could draw this type would otherwise wait here for the
        // next build that can, and refuse a wallpaper this request has since replaced.
        removeCrashRecord(m_config.stateDir, monitorId);

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
    bool daemonFault = false;
    bool deferred = false;
    // On disk while a still's renderer is built, and from the choice of target through the submit
    // and the teardowns after it -- the spans in which the file's content is what runs -- so a
    // start after a fatal signal in any of them refuses this wallpaper. A video's open and first
    // decode carry videoSessionLocked()'s own record. Not around ensureGpu() or the images: a
    // driver that dies creating those would be blamed on whichever picture happened to be first,
    // so a pass failing there frees its renderer with no record on disk.
    //
    // Declared before the renderer so a return that skips the teardown below still frees it first.
    std::optional<ApplyingRecord> applying;

    // What draws, one per type. The still's renderer is this pass's own; a video's session
    // outlives the pass, and this only points at the one m_videoSessions holds.
    std::optional<StaticRenderer> still;
    VideoSession* clip = nullptr;
    const OffscreenTarget* target = nullptr;
    try {
        ensureGpu();
        // Before the target is acquired, so a set reported since the last render rebuilds the
        // image rather than being applied to one already allocated against the old one.
        applyImportModifiersLocked();

        // A pass that cannot draw anywhere is refused before the decode, so it costs nothing.
        // Only images that are kept can all be held: a rebuild clears every hold.
        if (targetsFit(monitorId, size)) {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            deferred = m_targets->nextUnheld(monitorId, heldBuffersLocked(monitorId)) == nullptr;
        }

        if (!deferred) {
            // Whatever is drawn is ready before the images are rebuilt, so a redraw at a new size
            // goes without a frame for the draw only, not for the decode or the upload. The render
            // pass belongs to the manager, not to this monitor's images, and the draw takes its
            // extent from the target.
            //
            // The still's renderer is not retained past this call: a texture and a pipeline per
            // monitor for something nothing re-renders. render() does not wait, so what keeps it
            // from being destroyed out from under the GPU is its own destructor, which waits for
            // the device to go idle.
            if (*kind == Drawable::Still) {
                applying.emplace(m_config.stateDir, monitorId, type, path);
                still.emplace(*m_device, m_targets->renderPass(), path, m_resolvedShaderDir);
                // Off disk while the images are made, for ensureGpu()'s reason: their size is the
                // screen's, and a driver dying on it is not this picture's doing.
                applying.reset();
            } else {
                clip = &videoSessionLocked(monitorId, path, wallpaperSeq);
            }

            if (ensureTargets(monitorId, size)) rebuilt = true;

            // Nothing can claim an image between here and the submit: a hand-over needs
            // m_gpuMutex, which this pass holds, and a release only ever frees one.
            {
                std::lock_guard<std::mutex> stateLock(m_stateMutex);
                target = m_targets->nextUnheld(monitorId, heldBuffersLocked(monitorId));
            }
            deferred = target == nullptr;
            if (!deferred) applying.emplace(m_config.stateDir, monitorId, type, path);
        }
    } catch (const std::exception&) {
        const RenderFailure failure = classifyFailure();
        error = failure.error;
        daemonFault = failure.daemonFault;
        if (failure.gpuFault) recordHealth(&DaemonHealth::gpu, HealthStatus::Fail, error);
    }

    if (deferred) {
        // Every image on this monitor is in a consumer's hands, which exportCurrentFrame()'s
        // reserve is there to prevent: it admits and records a hand-over in one acquisition, so
        // no recorded claim ever covers the ring. Kept as the backstop because deferring is the
        // only alternative to drawing into an image a consumer is reading, and because
        // deferredRenders is then a report on that invariant rather than a routine count.
        // The request is left outstanding rather than waited on with the GPU lock held --
        // whatever frees an image re-posts this monitor, and until then GetStatus reports the
        // render as still owed.
        uint32_t refusals = 0;
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            auto it = m_monitors.find(monitorId);
            if (it == m_monitors.end() || it->second.requestSeq != seq) return RenderPass{{}, true};
            refusals = ++it->second.deferredRenders;
        }
        // Once per episode, and outside the state lock: a stalled stderr must not hold a lock every
        // bus handler takes. A producer drawing at display rate would otherwise write a line per
        // frame; the running count is in GetStatus for anyone who wants the rest.
        if (refusals == 1) {
            std::fprintf(stderr,
                         "wallpaperx-daemon: monitor %d has no free image -- all %u are being read, so this "
                         "render waits for a consumer to release one\n",
                         monitorId, kTargetDepth);
        }
        return {};
    }

    // Here, not only after the commit block, which an overtaken pass returns from inside: the newer
    // pass must not draw on a device this failure may have lost. Without a record, because a
    // session exists here only once its open and first decode succeeded, so what failed was the
    // GPU or the daemon, not the file.
    if (!error.empty()) m_videoSessions.erase(monitorId);

    bool committed = false;
    uint64_t generation = 0;
    {
        // Held across the submit deliberately: the submit is what makes this image the
        // monitor's published frame, and a newer request landing between the check and the
        // submit would publish the older wallpaper with nothing on the wire to say so.
        // Atomic check-and-submit makes it "published, or never touched".
        //
        // Bounded by one submit -- a fullscreen quad, or the blit that shows a picture decoded
        // above; decode, upload and pipeline creation are all above, unlocked -- so GetStatus
        // waits on a draw, never on a decode.
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        auto it = m_monitors.find(monitorId);
        if (it == m_monitors.end() || it->second.requestSeq != seq) {
            // Superseded. Nothing has been submitted, so kept images still hold exactly what the
            // consumer was last told about; replaced ones hold nothing, and renderPending() draws
            // the newer request at once. Leave servicedSeq alone so the newer request's own pass
            // is not mistaken for one already done.
            return RenderPass{{}, true};
        }

        if (error.empty()) {
            try {
                // One of the two is always set here: every path that leaves both unset sets
                // `error`, which the guard above excludes.
                if (*kind == Drawable::Still) {
                    m_targets->render(*still, *target);
                } else {
                    clip->pipeline->draw(*m_targets, *target);
                    markShown(*clip, std::chrono::steady_clock::now());
                }
            } catch (const std::exception&) {
                // m_stateMutex is held here, so the locked recorder: the same sorting, one lock in.
                const RenderFailure failure = classifyFailure();
                error = failure.error;
                daemonFault = failure.daemonFault;
                if (failure.gpuFault) recordHealthLocked(&DaemonHealth::gpu, HealthStatus::Fail, error);
            }
        }

        // Marked for every outcome, not just success: a failed render must not be
        // retried by a duplicate queue entry either, or a broken image file would be
        // decoded again for every spurious post.
        it->second.servicedSeq = seq;

        if (!error.empty()) {
            recordFailureLocked(it->second, error, daemonFault);
        } else {
            committed = true;
            generation = commitFrameLocked(it->second, *target);
            // A render asked for and delivered is the GPU working, so it clears a failure an
            // earlier one recorded, with the detail ensureGpu() records. Here and not in the
            // commit helper: a video's pictures commit many times a second, unasked, which would
            // wipe a fault recorded for another monitor before anyone could read it.
            recordHealthLocked(&DaemonHealth::gpu, HealthStatus::Ok,
                               m_gpuStatus.name + " (" + m_gpuStatus.descriptorTier + ")");
        }
    }
    // Both outside the state lock: freeing a texture waits for the GPU, and an unlink on a stalled
    // filesystem must not hold up GetStatus. Both teardowns run before the record comes off disk.
    still.reset();
    // A draw that threw leaves its pipeline unusable. Under this pass's own record, which names
    // the file being drawn when it failed.
    if (!error.empty()) closeVideoSessionLocked(monitorId);
    applying.reset();

    if (!error.empty()) {
        // Outside the state lock: destroying a target waits for device idle, which has
        // no business blocking GetStatus. A monitor whose render failed keeps no frame,
        // so its target has no reader, and freeing it means a retry starts clean.
        releaseTarget(monitorId);
        return {};
    }

    return RenderPass{RenderOutcome{committed, generation}, false};
}

FrameHandle WallpaperManager::exportHandle(const OffscreenTarget& target) {
    FrameHandle handle;
    try {
        handle.frame = wallpaperx::exportFrame(*m_device, target);
        try {
            handle.acquireFd = wallpaperx::exportFrameAcquireFd(*m_device, target);
        } catch (...) {
            // The memory descriptor is already open and nothing else will close it. A frame with
            // no acquire beside it is not offered: the consumer would read unowned memory while
            // this daemon reported a mode saying it had been handed the acquire.
            close(handle.frame.fd);
            throw;
        }
    } catch (const GpuFailure& e) {
        recordHealth(&DaemonHealth::gpu, HealthStatus::Fail, e.what());
        throw;
    }
    return handle;
}

FrameExport WallpaperManager::exportCurrentFrame(int monitorId, const std::string& client) {
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
            // Type first, independently of lastError: it is true from the instant the request
            // is recorded, so keying on the worker's progress would answer "not yet" for a
            // window and "never" afterwards for one unchanged request.
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

    const OffscreenTarget& target = m_targets->get(monitorId);

    // One image stays out of consumers' hands so a render always has somewhere to draw. Asked
    // of nextUnheld(), which is the same question the render itself asks, so the two cannot
    // come to different answers about what is free. Only a named caller is charged with a
    // claim, so only a named caller can be refused one.
    //
    // Admitting and recording are one act under one acquisition: the claims that pass the test
    // are the claims written back, so a ReleaseBuffer landing later cannot restore a claim the
    // test had dropped and leave the record covering a ring the test found room in. Testing
    // first and recording after the export was two decisions that a release could make disagree.
    //
    // Refusing costs this consumer a frame it must ask for again; granting past a cap would cost
    // the monitor every render until somebody released, and nothing may be taken back from a
    // consumer that could still be reading it.
    bool claimAdded = false;
    if (!client.empty()) {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        auto it = m_monitors.find(monitorId);
        if (it != m_monitors.end()) {
            Handover next = withHandover(it->second.holds, client, target.bufferId);
            if (next.outcome == Handover::Outcome::Refused ||
                m_targets->nextUnheld(monitorId, flattenHolds(next.holds)) == nullptr) {
                ++it->second.refusedHandovers;
                // Once per consumer per interval. The list is capped so a client inventing names
                // cannot size it, and a full one gives up the slot of whichever consumer has been
                // quiet longest -- quiet for a whole interval, so a consumer being refused now can
                // never be displaced by one that has stopped. Without that, four consumers refused
                // once early would hold every slot for the life of their bus names and the fifth,
                // the one actually stuck, could never be named.
                const SteadyTime now = std::chrono::steady_clock::now();
                std::vector<MonitorState::RefusalReport>& reported = it->second.refusalsReported;
                const auto seen = std::find_if(reported.begin(), reported.end(),
                                               [&client](const MonitorState::RefusalReport& report) {
                                                   return report.client == client;
                                               });
                if (seen != reported.end()) {
                    out.firstRefusal = now - seen->at >= kRefusalLogInterval;
                    if (out.firstRefusal) seen->at = now;
                } else if (reported.size() < kMaxHoldersPerMonitor) {
                    out.firstRefusal = true;
                    reported.push_back(MonitorState::RefusalReport{client, now});
                } else {
                    const auto quietest = std::min_element(
                        reported.begin(), reported.end(),
                        [](const MonitorState::RefusalReport& a, const MonitorState::RefusalReport& b) {
                            return a.at < b.at;
                        });
                    if (now - quietest->at >= kRefusalLogInterval) {
                        out.firstRefusal = true;
                        *quietest = MonitorState::RefusalReport{client, now};
                    }
                }
                // A consumer refused while holding none of this monitor's images is asking for a
                // picture the daemon had none to give -- its own back-pressure -- so it counts as
                // collecting, and the video it would otherwise stop keeps playing for it. A
                // consumer refused while holding images does not: those are the ones it has not
                // given back, which is exactly the state this evidence exists to catch.
                const std::vector<BufferHold>& holders = it->second.holds;
                if (std::none_of(holders.begin(), holders.end(),
                                 [&client](const BufferHold& hold) { return hold.client == client; })) {
                    it->second.uncollectedSince.reset();
                }
                out.reason = NoFrameReason::NoFreeImage;
                return out;
            }
            it->second.holds = std::move(next.holds);
            it->second.refusedHandovers = 0;
            claimAdded = next.outcome == Handover::Outcome::Granted;
        }
    }

    FrameHandle handle;
    try {
        handle = exportHandle(target);
    } catch (...) {
        // The consumer never got the image, so it must not stay charged with holding one it
        // cannot name to release. Undoing this one id restores the whole record, because
        // withHandover() only ever adds -- and a blind restore of the previous record would
        // instead resurrect a claim a ReleaseBuffer ended while the export ran.
        if (claimAdded) {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            auto it = m_monitors.find(monitorId);
            if (it != m_monitors.end()) dropHoldLocked(it->second, client, target.bufferId);
        }
        throw;
    }

    // The picture is out, and to a consumer the bus can name: the only evidence the daemon has
    // that anything is looking, and what a video plays on. Not before the export, which can fail
    // and hand over nothing; not for an unnamed caller either, since a diagnostic poll no claim
    // can be charged to must not keep a screen's video decoding.
    if (!client.empty()) noteHandoverServed(monitorId);

    handle.generation = generation;
    // Under this call's own m_gpuMutex, taken at the top and held throughout, which is what makes
    // touching a session here safe while the clock erases them: only the pictures, never the
    // pipeline, so no GPU work and no thread affinity is involved.
    //
    // Recorded on the session rather than derived from the holds: a consumer that gives its image
    // back before asking for the next one holds none at this moment and is reading all the same,
    // which made every picture of a playing video log a line.
    if (const auto session = m_videoSessions.find(monitorId); !client.empty() && session != m_videoSessions.end()) {
        std::vector<std::string>& served = session->second.served;
        handle.routine = std::find(served.begin(), served.end(), client) != served.end();
        if (!handle.routine) {
            served.push_back(client);
            // Bounded by the consumers a monitor can hold at once; the oldest goes, and would be
            // reported again if it came back.
            if (served.size() > kMaxHoldersPerMonitor) served.erase(served.begin());
        }
    }
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
    applyPersistOutcomeLocked(out.health);
    for (const auto& [monitorId, state] : m_monitors) {
        MonitorStatus entry;
        entry.type = state.type;
        entry.path = state.path;
        entry.generation = state.generation;
        entry.hasFrame = state.hasFrame;
        entry.lastError = state.lastError;
        entry.lastErrorIsDaemonFault = state.lastErrorIsDaemonFault;
        entry.renderOutstanding = state.requestSeq != state.servicedSeq;
        entry.deferredRenders = state.deferredRenders;
        entry.refusedHandovers = state.refusedHandovers;
        entry.holds = state.holds;
        entry.screenVisible = screenDrawnLocked(monitorId);
        entry.watched = watchedLocked(monitorId, state);
        if (state.hasFrame) {
            entry.width = state.width;
            entry.height = state.height;
        }
        entry.targetSize = targetSizeLocked(monitorId);
        entry.targetSizeReported = reportedSizeLocked(monitorId).has_value();
        out.monitors.emplace(monitorId, std::move(entry));
    }
    out.outputs = m_outputs;
    return out;
}

}
