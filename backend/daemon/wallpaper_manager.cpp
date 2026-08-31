// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaper_manager.h"

#include "config_preserve.h"
#include "kept_rows.h"
#include "log_escape.h"
#include "renderers/static_renderer.h"
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
    live.outputs = m_outputs;
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

const OffscreenTarget& WallpaperManager::acquireTarget(int monitorId) {
    // Keyed on the dimensions, not on mere existence: create() replaces and waits for idle, so
    // reuse is worth it, and when real monitor geometry arrives this will not go on quietly
    // rendering at the old resolution.
    const bool reusable = m_targets->has(monitorId) && m_targets->get(monitorId).width == m_config.width &&
                          m_targets->get(monitorId).height == m_config.height;
    return reusable ? m_targets->get(monitorId) : m_targets->create(monitorId, m_config.width, m_config.height);
}

void WallpaperManager::releaseTarget(int monitorId) {
    if (m_targets && m_targets->has(monitorId)) {
        m_targets->destroy(monitorId);
    }
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
        // image built under the old set, and acquireTarget() reuses an image whose size matches
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
    // images made after it. Dropping them is what makes it take effect; acquireTarget()
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
        state.lastError.clear();

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

void WallpaperManager::setMonitorOutput(int monitorId, const std::string& outputName) {
    requireValidMonitorId(monitorId);
    if (const std::string why = outputNameError(outputName); !why.empty()) {
        throw InvalidRequest(why + ": " + sanitizeForLog(outputName));
    }

    std::vector<PersistedWallpaper> toPersist;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        // Idempotent is advertised, so a client may re-assert every name on each screen
        // change; persist() parks this thread -- the bus event loop -- on the write. Skipped
        // only when the file is known to hold what the map holds: any other answer, including a
        // write still outstanding, means re-asserting is the only retry a client can reach.
        const BindResult bound = bindOutputLocked(monitorId, outputName);
        if (bound == BindResult::Unchanged && m_writer.outcome().onDisk()) {
            return;
        }

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

WallpaperManager::BindResult WallpaperManager::bindOutputLocked(int monitorId,
                                                                const std::string& outputName) {
    bool displaced = false;
    const auto existing = m_outputs.find(outputName);
    if (existing != m_outputs.end()) {
        if (existing->second == monitorId) return BindResult::Unchanged;
        // Erased, not left: a name on two monitors is two plugin instances answering to one
        // screen, and whichever came back first would decide who shows the wrong picture.
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
    int duplicateIds = 0;

    // Screen name -> the kept row that claimed it last, in file order. The sweep at the end
    // needs "who claimed this name last", which m_kept.carried()'s own order does not carry.
    std::map<std::string, int> keptNameOwner;

    // Monitor id -> its row in m_kept.carried(). Indexed rather than scanned: the file is capped at
    // a mebibyte, roughly 28,000 rows, and a scan per row is that squared in comparisons on the
    // startup path -- before the bus name is claimed, so the desktop has no daemon meanwhile.
    std::map<int, std::size_t> keptIndex;

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

        // Independent halves of a row: this one is restored even when the other cannot be.
        // Last-wins -- file order carries no meaning. What matters is that the loss is
        // reported, not left as two monitors quietly answering to one screen.
        if (nameValid && entry.monitorId < kMaxMonitors &&
            bindOutputLocked(entry.monitorId, entry.output) == BindResult::Displaced) {
            ++displacedNames;
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
                held.output = entry.output;
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
        state.lastError.clear();
        state.hasFrame = false;
        // Once per monitor: a second entry here is a second full render of one request.
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
            ++displacedNames;
        }
    }

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
    repaired = displacedNames > 0 || rejectedNames > 0 || duplicateIds > 0 || m_kept.anyDropped();
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
    bool daemonFault = false;
    std::optional<StaticRenderer> renderer;
    const OffscreenTarget* target = nullptr;
    try {
        ensureGpu();
        // Before the target is acquired, so a set reported since the last render rebuilds the
        // image rather than being applied to one already allocated against the old one.
        applyImportModifiersLocked();
        target = &acquireTarget(monitorId);
        // Not retained past this call. A static wallpaper renders once, so keeping the
        // texture, pipeline and descriptor alive per monitor would hold VRAM for
        // something nothing re-renders; immediateSubmit has already waited on its fence
        // by the time render() returns, so nothing is destroyed out from under the GPU.
        // Step 13's video renderer does need to persist and will live in MonitorState.
        renderer.emplace(*m_device, m_targets->renderPass(), path, m_resolvedShaderDir);
    } catch (const GpuFailure& e) {
        // Caught before std::exception, which it derives from. The GPU going wrong rather
        // than the image, so it re-records gpu health -- otherwise a device that dies after
        // the first successful render reads ok forever.
        error = e.what();
        daemonFault = true;
        recordHealth(&DaemonHealth::gpu, HealthStatus::Fail, error);
    } catch (const WallpaperContentError& e) {
        // The image, not the daemon: a file the user can replace.
        error = e.what();
    } catch (const std::exception& e) {
        // Unclassified, so it counts against the daemon. Failing closed is the point: this
        // is where an allocation failure or a broken invariant arrives, and treating those
        // as the wallpaper's fault published an ok daemon with no picture on any monitor.
        error = e.what();
        daemonFault = true;
    }

    bool committed = false;
    uint64_t generation = 0;
    {
        // Held across the submit deliberately: the target is single-buffered, so a submit
        // writes into memory a consumer is already displaying, and a newer request landing
        // between check and submit would repaint the published frame with nothing on the wire
        // to say so. Atomic check-and-submit makes it "published, or never touched".
        //
        // Bounded by one fullscreen-quad submit -- decode, upload and pipeline creation are all
        // above, unlocked -- so GetStatus waits on a draw, never on a decode.
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
                daemonFault = true;
                recordHealthLocked(&DaemonHealth::gpu, HealthStatus::Fail, error);
            } catch (const WallpaperContentError& e) {
                error = e.what();
            } catch (const std::exception& e) {
                error = e.what();
                daemonFault = true;
            }
        }

        // Marked for every outcome, not just success: a failed render must not be
        // retried by a duplicate queue entry either, or a broken image file would be
        // decoded again for every spurious post.
        it->second.servicedSeq = seq;

        if (!error.empty()) {
            // Sanitized where client text enters daemon state rather than at each exit, so
            // every consumer inherits it: this message embeds the client's path, and a readable
            // file may be named with an ESC sequence. 1024 because the field is polled, and
            // safe only because describeRejection() puts the diagnosis before the path --
            // truncation keeps the head, so reversing that message would cut stb's reason.
            it->second.lastError = sanitizeForLog(error, 1024, LogFraming::Bare);
            it->second.lastErrorIsDaemonFault = daemonFault;
        } else {
            ++it->second.generation;
            it->second.hasFrame = true;
            it->second.width = m_config.width;
            it->second.height = m_config.height;
            it->second.frameModifier = target->drmModifier;
            // Cleared here rather than relying on requestWallpaper having cleared it. A
            // successful commit is what makes "this monitor has a frame" true, and the
            // two facts have to be set together -- otherwise a monitor can report
            // has_frame alongside a stale last_error, and a client branching on
            // last_error calls a working wallpaper broken.
            it->second.lastError.clear();
            it->second.lastErrorIsDaemonFault = false;
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

    FrameHandle handle;
    try {
        const OffscreenTarget& target = m_targets->get(monitorId);
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
        if (state.hasFrame) {
            entry.width = state.width;
            entry.height = state.height;
        }
        out.monitors.emplace(monitorId, std::move(entry));
    }
    out.outputs = m_outputs;
    return out;
}

}
