// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaper_manager.h"

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

// Bounds the .bad, .bad.1, ... search. One name meant the second unreadable config had
// nowhere to go, and nowhere to go meant saving was disabled for the run.
constexpr int kMaxPreservedCopies = 10;

// Where a preserved copy is preferably kept: $XDG_STATE_HOME/wallpaperx, else
// ~/.local/state/wallpaperx. Out of the config directory, so a hand edit or a tidy-up there
// cannot take the copy with the file it was made for. Separation from the directory the user
// edits, not protection from the user -- they own this one too. Empty when neither variable is
// usable, which preserveConfigTo() answers by keeping the copy beside the config instead; a
// relative value is ignored, as the basedir spec requires and defaultConfigPath() already does.
std::filesystem::path preservedCopyDir() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        const std::filesystem::path base(xdg);
        if (base.is_absolute()) return base / "wallpaperx";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        const std::filesystem::path base(home);
        if (base.is_absolute()) return base / ".local" / "state" / "wallpaperx";
    }
    return {};
}

// The store's own limit, not a second copy of it: a file it refuses to read is not one to
// compare against a preserved copy.
constexpr std::uintmax_t kMaxComparedBytes = ConfigStore::kMaxBytes;

void appendReason(std::string& why, const std::string& reason) {
    if (!why.empty()) why += "; ";
    why += reason;
}

// Size first: it settles nearly every pair without a read.
bool sameContents(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    const std::uintmax_t sizeA = std::filesystem::file_size(a, ec);
    if (ec) return false;
    const std::uintmax_t sizeB = std::filesystem::file_size(b, ec);
    if (ec || sizeA != sizeB || sizeA > kMaxComparedBytes) return false;

    std::ifstream streamA(a, std::ios::binary);
    std::ifstream streamB(b, std::ios::binary);
    if (!streamA || !streamB) return false;
    return std::equal(std::istreambuf_iterator<char>(streamA), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(streamB));
}

// Keeps `source` in `dir` under the first free <file>.bad[.N]. Returns the name used, or empty
// with `reason` set to the last failure. A copy already holding these bytes counts as preserved:
// a repair that cannot be written recurs every start, and a fresh name each time would exhaust
// kMaxPreservedCopies in as many restarts.
std::string keepCopyIn(const std::filesystem::path& source, const std::filesystem::path& dir,
                       bool move, std::string& reason) {
    const auto unlinkSource = [&source, &reason]() {
        std::error_code removeEc;
        std::filesystem::remove(source, removeEc);
        if (removeEc) reason = removeEc.message();
        return !removeEc;
    };

    for (int n = 0; n < kMaxPreservedCopies; ++n) {
        const std::filesystem::path kept =
            dir / (source.filename().string() + ".bad" + (n == 0 ? "" : "." + std::to_string(n)));
        std::error_code ec;
        const bool taken = std::filesystem::exists(kept, ec);
        if (ec) {
            reason = ec.message();
            continue;
        }
        if (taken) {
            // Never over different bytes: that copy holds an earlier broken config.
            if (!sameContents(source, kept)) continue;
            if (move && !unlinkSource()) return {};
            return kept.string();
        }

        if (move) {
            // Atomic where it can be. The copy below exists only for the cross-filesystem case,
            // and unlike a rename it can leave a partial file behind.
            std::filesystem::rename(source, kept, ec);
            if (!ec) return kept.string();
            if (ec != std::errc::cross_device_link) {
                reason = ec.message();
                continue;
            }
            ec.clear();
        }

        std::filesystem::copy_file(source, kept, ec);
        if (ec) {
            // A truncated remnant would hold a name every later attempt is denied, and offer the
            // user a silently shortened config as a preserved one. Left alone when the copy
            // failed because something else is already there.
            if (ec != std::errc::file_exists) {
                std::error_code cleanupEc;
                std::filesystem::remove(kept, cleanupEc);
            }
            reason = ec.message();
            continue;
        }
        if (move && !unlinkSource()) {
            // Not preservation while the original still sits under the name the next save
            // writes, so the copy goes back out rather than holding a name for nothing.
            std::error_code cleanupEc;
            std::filesystem::remove(kept, cleanupEc);
            return {};
        }
        return kept.string();
    }
    if (reason.empty()) reason = "no free name";
    return {};
}

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
    : m_config(std::move(config)), m_store(m_config.configFile) {
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
        addPersistenceNote(std::to_string(loaded.malformedEntries) + " malformed entries ignored");
    }

    if (!loaded.error.empty()) {
        // Set before any preservation is attempted, and cleared only if the file is
        // actually moved aside: until then, overwriting it would destroy the user's only
        // copy of every other monitor.
        m_configUnread = true;
        recordHealth(&DaemonHealth::persistence, HealthStatus::Fail,
                     persistenceDetail(loaded.error));
        return;
    }

    recordHealth(&DaemonHealth::persistence, HealthStatus::Ok,
                 persistenceDetail(m_store.file().string()));
}

std::string WallpaperManager::preserveConfigTo(bool move, std::string& why) const {
    // The state directory is preferred but not required: the config's own directory must
    // already be writable for any save to work, so "nowhere preferred" must not become
    // "nowhere" -- which is what a daemon with a config path and no HOME would otherwise hit.
    std::vector<std::filesystem::path> dirs;
    if (std::filesystem::path preferred = preservedCopyDir(); !preferred.empty()) {
        dirs.push_back(std::move(preferred));
    }
    if (std::filesystem::path beside = m_store.file().parent_path();
        !beside.empty() && (dirs.empty() || dirs.front() != beside)) {
        dirs.push_back(std::move(beside));
    }
    if (dirs.empty()) {
        why = "config file has no directory to keep a copy in";
        return {};
    }

    for (const std::filesystem::path& dir : dirs) {
        std::string reason;
        std::error_code dirEc;
        std::filesystem::create_directories(dir, dirEc);
        if (dirEc) {
            appendReason(why, dir.string() + ": " + dirEc.message());
            continue;
        }
        if (std::string kept = keepCopyIn(m_store.file(), dir, move, reason); !kept.empty()) {
            return kept;
        }
        // Every candidate named: a message blaming only the last one sends the user to the
        // wrong directory.
        appendReason(why, dir.string() + ": " + reason);
    }
    return {};
}

void WallpaperManager::preserveUnreadableConfig() {
    if (!m_store.enabled()) return;

    std::string why;
    if (m_loadError.empty()) {
        // A pending repair means a rewrite from live state is coming, and a row the manager
        // rejected parses cleanly, lives nowhere else, and is what that rewrite removes.
        bool repairPending = false;
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            repairPending = m_repair.has_value();
        }
        // Copied, not moved: the entries this build understood are restored from this same file.
        if ((m_loadMalformed > 0 || repairPending) && m_loadPreservable) {
            const std::string copied = preserveConfigTo(/*move=*/false, why);
            m_repairCopied = !copied.empty();
            addPersistenceNote(copied.empty() ? "not copied: " + why : "copied to " + copied);
            // Sticky: the rejected rows exist in this file alone and the rewrite removing them
            // is coming. Saving is not refused over it -- later wallpaper changes are the larger
            // loss -- so a write that succeeds later must not clear the record of that loss.
            if (!m_repairCopied) m_preserveFailed = true;
            recordHealth(&DaemonHealth::persistence,
                         m_repairCopied ? HealthStatus::Ok : HealthStatus::Fail,
                         persistenceDetail(m_store.file().string()));
        }
        return;
    }

    if (m_loadPreservable) {
        const std::string kept = preserveConfigTo(/*move=*/true, why);
        if (kept.empty()) {
            addPersistenceNote("not kept: " + why);
        } else {
            m_configUnread = false;
            addPersistenceNote("kept as " + kept);
        }
    }
    // Every path reports. Saying the config could not be read while staying silent about
    // whether a copy survived sends the user looking for a file that is not there.
    if (m_configUnread) addPersistenceNote("not saving over it");
    recordHealth(&DaemonHealth::persistence, HealthStatus::Fail, persistenceDetail(m_loadError));
}

void WallpaperManager::writeRepairedConfig() {
    std::optional<std::vector<PersistedWallpaper>> repair;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        repair.swap(m_repair);
    }
    if (!repair) return;

    // Written whether or not a copy was taken: refusing this one write protects nothing, since
    // the next ordinary save rebuilds the file from the same live state, and refusing every
    // write costs the user every later wallpaper choice. m_preserveFailed reports it instead.
    //
    // Queued, not waited on: the bus name is claimed and no event loop exists yet, so a wait
    // here answers nothing for the whole deadline. The writer reports a failed write on stderr
    // itself. queuePersist() takes the state lock, so it is called with none held.
    queuePersist(*repair);
}

void WallpaperManager::addPersistenceNote(const std::string& note) {
    if (!m_persistenceNote.empty()) m_persistenceNote += "; ";
    m_persistenceNote += note;
}

std::string WallpaperManager::persistenceDetail(const std::string& base) const {
    // Notes before the path: recordHealthLocked caps the detail and truncation drops the
    // tail, so a long config path must not be what silences them.
    std::string detail;
    if (!m_persistenceNote.empty()) detail = m_persistenceNote + "; ";

    // Two independent facts about one row, not a partition: this text is the user's notice
    // that a picture did not come back, and a row can have lost a name as well as one.
    int keptWallpapers = 0;
    int keptNames = 0;
    for (const PersistedWallpaper& entry : m_unapplied) {
        // Both halves count only what the next save will actually write. A client can take
        // over a kept row's id or claim its name later, and persistableLocked() then drops the
        // kept copy -- so counting it here would go on offering back a wallpaper, or a name,
        // the file no longer holds.
        if ((!entry.type.empty() || !entry.path.empty()) && m_monitors.count(entry.monitorId) == 0) {
            ++keptWallpapers;
        }
        if (!entry.output.empty() && m_outputs.count(entry.output) == 0) ++keptNames;
    }
    if (keptWallpapers > 0) {
        detail += std::to_string(keptWallpapers) + " entries this build cannot apply, kept; ";
    }
    if (keptNames > 0) {
        detail += std::to_string(keptNames) +
                  " screen names this build's monitor limit excludes, kept; ";
    }
    // Reported for the same reason the kept ones are: a wallpaper that did not come back is
    // otherwise invisible, and these are the ones no restart will bring back.
    if (m_droppedEntries > 0) {
        detail += std::to_string(m_droppedEntries) + " entries dropped, no build can apply them; ";
    }
    if (m_droppedNames > 0) {
        detail += std::to_string(m_droppedNames) + " screen names dropped, no build can apply them; ";
    }
    return detail + base;
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
            m_unapplied.begin(), m_unapplied.end(),
            [monitorId](const PersistedWallpaper& row) { return row.monitorId == monitorId; });
        if (kept != m_unapplied.end() && (!kept->type.empty() || !kept->path.empty())) {
            kept->type.clear();
            kept->path.clear();
            addPersistenceNote(
                "1 entry this build cannot apply, replaced by a new wallpaper for that screen");
        }

        toPersist = persistableLocked();
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
        if (bindOutputLocked(monitorId, outputName) == BindResult::Unchanged &&
            persistOutcomeLocked().onDisk) {
            return;
        }
        toPersist = persistableLocked();
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
    auto answer = std::make_shared<std::promise<LoadResult>>();
    std::future<LoadResult> pending = answer->get_future();
    try {
        std::thread([answer, store = m_store] { answer->set_value(store.load()); }).detach();
    } catch (const std::system_error& e) {
        // The store's contract is that config trouble is reported and never stops the
        // daemon booting; this is the one statement here that can throw.
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

std::vector<PersistedWallpaper> WallpaperManager::persistableLocked() const {
    // Keyed by monitor id, not appended: a wallpaper and a name for one monitor belong on
    // one row, and two rows would be two monitors claiming one screen on the next load.
    std::map<int, PersistedWallpaper> rows;
    for (const auto& [monitorId, state] : m_monitors) {
        if (state.type.empty() || state.path.empty()) continue;
        rows.emplace(monitorId, PersistedWallpaper{monitorId, state.type, state.path, {}});
    }

    // Entries this build could not apply are still the user's. Without this the save
    // below, which rebuilds the file from live state alone, would delete a wallpaper set
    // by a newer build the moment anything else is set here. Live state wins per monitor.
    for (const PersistedWallpaper& entry : m_unapplied) {
        if (m_monitors.count(entry.monitorId) == 0) rows.emplace(entry.monitorId, entry);
    }

    // m_outputs is the authority: a carried name is written only for an id this build cannot
    // bind, and only while nothing else claims it. This runs on every save and cannot report,
    // so a name it wrote twice would be one screen lost in silence on the next load.
    std::set<std::string> written;
    for (auto& [monitorId, row] : rows) {
        if (row.output.empty()) continue;
        if (monitorId < kMaxMonitors || m_outputs.count(row.output) != 0 ||
            !written.insert(row.output).second) {
            row.output.clear();
        }
    }
    for (const auto& [name, monitorId] : m_outputs) {
        PersistedWallpaper& row = rows[monitorId];
        // Explicit: operator[] default-constructs monitorId 0 for a named, wallpaper-less
        // screen.
        row.monitorId = monitorId;
        row.output = name;
    }

    std::vector<PersistedWallpaper> entries;
    entries.reserve(rows.size());
    for (auto& [monitorId, row] : rows) {
        // A row the clearing loop emptied has nothing left to say. Written out, the next
        // start reads it back and reports it as a wallpaper that did not come back.
        if (row.type.empty() && row.path.empty() && row.output.empty()) continue;
        entries.push_back(std::move(row));
    }
    return entries;
}

std::shared_ptr<PersistWorker> WallpaperManager::queuePersist(
    const std::vector<PersistedWallpaper>& entries) {
    if (!m_store.enabled()) return nullptr;

    // The save rebuilds the file from live state, so writing now would replace monitors
    // this run never managed to read with the one the user just set.
    if (m_configUnread) {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_notWritten = m_store.file().string() +
                       ": not saved, this run could not read it and will not overwrite it";
        recordHealthLocked(&DaemonHealth::persistence, HealthStatus::Fail,
                           persistenceDetail(m_notWritten));
        return nullptr;
    }

    // Published under m_stateMutex, which is where every reader takes it: an unlocked store
    // beside locked loads is a data race, and the first save can run beside a /health request
    // on the HTTP thread. Released before the worker's lock, keeping the documented order.
    std::shared_ptr<PersistWorker> worker;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        startPersistWorkerLocked();
        worker = m_persist;
        // Cleared only once the snapshot is on its way to a writer: from here the worker's own
        // result is the authority, and a stale refusal would outrank it.
        if (worker) m_notWritten.clear();
    }
    // No writer, and startPersistWorkerLocked() recorded why: the next save retries.
    if (!worker) return nullptr;

    {
        std::lock_guard<std::mutex> workerLock(worker->mutex);
        // Replaced, not appended: an older snapshot waiting behind a stalled write describes a
        // state this one already supersedes, so writing it would only be a slower route to the
        // same file.
        worker->pending = entries;
    }
    worker->wake.notify_one();
    return worker;
}

bool WallpaperManager::persist(const std::vector<PersistedWallpaper>& entries, std::string* whyNot) {
    const std::shared_ptr<PersistWorker> worker = queuePersist(entries);

    // Waited on, briefly, so the reply to SetWallpaper can say the choice is on disk in the
    // healthy case -- which is the only case it can be said in. Past the deadline the write
    // keeps going on the worker and this thread stops caring, so a stalled filesystem costs
    // one wait rather than the daemon, and the choice is then NOT known to be saved.
    if (worker) {
        std::unique_lock<std::mutex> workerLock(worker->mutex);
        const auto quiesced = [&worker] { return !worker->pending && !worker->writingSince; };
        // Not waited on again until something completes: a second stalled call would add
        // another deadline's delay to a bus thread that has already learned the answer.
        if (!worker->stalled && !worker->settled.wait_for(workerLock, kLoadDeadline, quiesced)) {
            worker->stalled = true;
        }
    }

    // Asked, not re-derived: whether the file now holds these entries is the same question the
    // health field and the re-assert shortcut ask, and answering it separately here is what let
    // three callers disagree about one write.
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    const PersistOutcome outcome = persistOutcomeLocked();
    if (!outcome.onDisk) {
        // Sanitized here, once, because this one leaves the process: the store composes its
        // errors around a path, and a path is arbitrary bytes the daemon did not choose.
        if (whyNot != nullptr) *whyNot = sanitizeForLog(outcome.why, 1024, LogFraming::Bare);
        // Recorded so a status read taken later still shows what this thread learned: the fold-in
        // reports an outstanding write, but only this call knows a caller stopped waiting on one.
        recordHealthLocked(&DaemonHealth::persistence, HealthStatus::Fail,
                           persistenceDetail(outcome.why));
    }
    return outcome.onDisk;
}

void WallpaperManager::startPersistWorkerLocked() {
    if (m_persist) return;

    // The store is copied in and nothing else is captured: this thread outlives the manager
    // whenever a write is stuck, and a write on a dead mount cannot be interrupted.
    std::shared_ptr<PersistWorker> worker;
    try {
        worker = std::make_shared<PersistWorker>();
        std::thread([worker, store = m_store] {
            for (;;) {
                std::vector<PersistedWallpaper> entries;
                {
                    std::unique_lock<std::mutex> lock(worker->mutex);
                    // No stop condition: there is nothing safe to wait for at exit, since a
                    // write on a dead mount cannot be interrupted. The thread ends with the
                    // process, and holds nothing the process needs to release.
                    worker->wake.wait(lock, [&worker] { return worker->pending.has_value(); });
                    entries = std::move(*worker->pending);
                    worker->pending.reset();
                    worker->writingSince = std::chrono::steady_clock::now();
                }

                const SaveResult saved = store.save(entries);

                // Said by the thread that learns it: the startup repair is queued, so any other
                // report of it races the write and can read as a clean start.
                //
                // write(2), not stdio: this thread outlives main() when a write is stuck, and a
                // FILE* torn down by exit is not something it can hold.
                if (!saved.error.empty()) {
                    const std::string line =
                        "wallpaperx-daemon: config not saved: " + sanitizeForLog(saved.error) + "\n";
                    const ssize_t ignored = ::write(STDERR_FILENO, line.data(), line.size());
                    (void)ignored;
                }

                {
                    std::lock_guard<std::mutex> lock(worker->mutex);
                    worker->result = saved;
                    worker->haveResult = true;
                    worker->writingSince.reset();
                    // The filesystem answered, so the next caller is allowed to wait again.
                    worker->stalled = false;
                }
                worker->settled.notify_all();
            }
        }).detach();
    } catch (const std::exception& e) {
        // std::exception, not std::system_error: std::thread also allocates the callable's
        // state, so std::bad_alloc reaches here and would otherwise kill the daemon at startup.
        //
        // Not published and not rethrown: a worker pointer with no thread behind it makes every
        // later save wait out the deadline for an answer that cannot come. Recorded instead,
        // and retried by the next save that is queued. A second failure inside the handler
        // would escape the frame it exists to contain.
        try {
            m_notWritten = m_store.file().string() + ": not saved, no thread available: " + e.what();
            recordHealthLocked(&DaemonHealth::persistence, HealthStatus::Fail,
                               persistenceDetail(m_notWritten));
        } catch (...) {  // NOLINT(bugprone-empty-catch) -- nothing left to report it with
        }
        return;
    }

    // Published only now: m_persist means "a thread is servicing this", and every reader
    // takes it under the lock this call is made with.
    m_persist = std::move(worker);
}

WallpaperManager::PersistOutcome WallpaperManager::persistOutcomeLocked() const {
    // Durability was never promised, so the file cannot disagree with anything.
    if (!m_store.enabled()) return {};

    // The half the worker cannot know: a save refused, or a writer that never started. Recorded
    // rather than inferred from m_persist being null, which also means "nothing asked yet".
    if (!m_notWritten.empty()) return {/*settled=*/true, /*onDisk=*/false, m_notWritten};
    if (!m_persist) return {};

    std::lock_guard<std::mutex> workerLock(m_persist->mutex);
    // Outstanding covers all three ways a write can be unfinished: still queued, in flight, or
    // abandoned by a caller that stopped waiting. Reading only `haveResult` reported a name as
    // durable while the write that would have made it so had never completed.
    const bool outstanding = m_persist->pending.has_value() ||
                             m_persist->writingSince.has_value() || m_persist->stalled;
    if (m_persist->haveResult && !m_persist->result.error.empty()) {
        return {!outstanding, false, m_persist->result.error};
    }
    if (outstanding) {
        return {false, false, m_store.file().string() +
                                  ": a write has not come back, the choice may not be on disk yet"};
    }
    return {true, true, {}};
}

void WallpaperManager::applyPersistOutcomeLocked(DaemonHealth& health) const {
    const PersistOutcome outcome = persistOutcomeLocked();

    // Written into the caller's copy rather than recorded: the write finishes on a thread
    // that must never reach this object, so the outcome is folded in when someone asks.
    const auto report = [&health](HealthStatus status, const std::string& detail) {
        health.persistence.status = status;
        health.persistence.detail = sanitizeForLog(detail, 1024, LogFraming::Bare);
    };

    if (!outcome.onDisk) {
        // A write nobody is waiting for any more is the only symptom a stalled config
        // filesystem leaves, so it is named rather than left to look like an ordinary failure.
        const bool stalled = !outcome.settled && stalledWriteLocked();
        report(HealthStatus::Fail,
               persistenceDetail(stalled ? m_store.file().string() +
                                               ": has not answered a write, the filesystem may "
                                               "be stalled"
                                         : outcome.why));
        return;
    }
    // Nothing has been asked of the file yet, so there is no write to describe and whatever the
    // constructor recorded still stands.
    if (!m_persist) {
        // The status alone: this detail was sanitized when it was recorded, and sanitizeForLog
        // is not idempotent -- a second pass turns one backslash in a path into two, so the
        // alarm would name a path that does not exist.
        if (m_preserveFailed) health.persistence.status = HealthStatus::Fail;
        return;
    }

    int skipped = 0;
    {
        std::lock_guard<std::mutex> workerLock(m_persist->mutex);
        skipped = m_persist->result.skippedNonUtf8;
    }
    std::string base = m_store.file().string();
    if (skipped > 0) {
        base += " (" + std::to_string(skipped) +
                " wallpapers dropped: a path or type is not valid UTF-8; any screen name on "
                "those rows was kept)";
    }
    // Fail, not Ok: the write succeeded but those monitors will not come back, and a
    // health field that reads ok is the only thing that would say otherwise. A copy this run
    // could not take is the same kind of loss, and outlives the write that took it.
    report(skipped > 0 || m_preserveFailed ? HealthStatus::Fail : HealthStatus::Ok,
           persistenceDetail(base));
}

bool WallpaperManager::stalledWriteLocked() const {
    if (!m_persist) return false;
    std::lock_guard<std::mutex> workerLock(m_persist->mutex);
    return m_persist->writingSince.has_value() &&
           std::chrono::steady_clock::now() - *m_persist->writingSince > kLoadDeadline;
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
    // needs "who claimed this name last", which m_unapplied's own order does not carry.
    std::map<std::string, int> keptNameOwner;

    // Monitor id -> its row in m_unapplied. Indexed rather than scanned: the file is capped at
    // a mebibyte, roughly 28,000 rows, and a scan per row is that squared in comparisons on the
    // startup path -- before the bus name is claimed, so the desktop has no daemon meanwhile.
    std::map<int, std::size_t> keptIndex;

    bool repaired = false;
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    for (const PersistedWallpaper& entry : pending) {
        // Three outcomes for a name, each of them reported: bound here, carried for a build
        // whose limit admits the id, or dropped. Only the id limit earns carrying. The byte cap
        // is the manager's rule and not the file's (see outputNameError), so by the rule that
        // carries an unknown wallpaper type a name past the cap would travel too -- deliberately
        // not done: it would write a name back into every save that no build has ever accepted.
        // Revisit with the forward-compatibility decision, not as a defect fix.
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
                ++m_droppedEntries;
            }
            if (nameValid) ++m_droppedNames;
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
            if (entry.output.empty()) ++m_droppedEntries;
            continue;
        }

        // The manager's own rules, not the store's: a type this build has never heard of,
        // or an id a larger kMaxMonitors would admit, is not rendered here -- but it is
        // kept, so the next save does not delete a wallpaper a newer build understands.
        if (!isKnownType(entry.type) || entry.path.empty() || entry.monitorId >= kMaxMonitors) {
            // One row per id, merged half by half: last wins, and a loss is counted only
            // where both rows really filled the same half -- a later row carrying just a name
            // must not delete the wallpaper this arm exists to preserve.
            // One namespace with the rows this build *can* apply, not two: an id a live monitor
            // already holds keeps that wallpaper at the next save, so a kept row for it is
            // dropped. Counted here, while the note the user reads is still being built --
            // otherwise the entry left config.json with nothing anywhere reporting it.
            const auto live = m_monitors.find(entry.monitorId);
            const bool liveHolds =
                live != m_monitors.end() && (!live->second.type.empty() || !live->second.path.empty());

            const auto slot = keptIndex.find(entry.monitorId);
            if (slot == keptIndex.end()) {
                PersistedWallpaper keep = entry;
                if (liveHolds && (!keep.type.empty() || !keep.path.empty())) {
                    ++duplicateIds;
                    keep.type.clear();
                    keep.path.clear();
                }
                if (!nameTravels) keep.output.clear();
                // Nothing left to carry. Kept, it would ride every save for the life of the
                // install and come back as a wallpaper that did not return.
                if (keep.type.empty() && keep.path.empty() && keep.output.empty()) continue;
                if (nameTravels) keptNameOwner[keep.output] = keep.monitorId;
                keptIndex.emplace(entry.monitorId, m_unapplied.size());
                m_unapplied.push_back(std::move(keep));
                continue;
            }
            PersistedWallpaper& held = m_unapplied[slot->second];
            if (!entry.type.empty() || !entry.path.empty()) {
                if (liveHolds || !held.type.empty() || !held.path.empty()) ++duplicateIds;
                if (!liveHolds) {
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
        // superseded by this one, and the next save drops it. Erased here so persistenceDetail()
        // stops offering it back, and counted so the loss is in the note rather than in silence.
        if (const auto slot = keptIndex.find(entry.monitorId); slot != keptIndex.end()) {
            PersistedWallpaper& held = m_unapplied[slot->second];
            if (!held.type.empty() || !held.path.empty()) {
                ++duplicateIds;
                held.type.clear();
                held.path.clear();
            }
        }
        state.type = entry.type;
        state.path = entry.path;
        ++state.requestSeq;
        state.lastError.clear();
        state.hasFrame = false;
        // Once per monitor: a second entry here is a second full render of one request.
        if (!alreadyRestored) monitorIds.push_back(entry.monitorId);
    }

    // Settled here, not in the save. persistableLocked() is const and runs on every write, so
    // a name it dropped could only vanish in silence -- and persistenceDetail() would already
    // have promised it back. Decided by keptNameOwner, which records the LAST row in the file
    // to claim each name: a kept row's position records when its id was first seen, which is a
    // different fact, since a later row for that id is merged into the place it already has.
    for (PersistedWallpaper& kept : m_unapplied) {
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
        addPersistenceNote(std::to_string(displacedNames) +
                           " screen names displaced by another entry claiming the same name or "
                           "the same screen; the binding this build could make was kept");
    }
    if (rejectedNames > 0) {
        addPersistenceNote(std::to_string(rejectedNames) +
                           " screen names dropped, unusable; those screens must be named again");
    }
    if (duplicateIds > 0) {
        // Not "the last entry won": three collisions feed this counter and one is resolved by
        // the EARLIER row, the one this build can show. The copy is named by its own note
        // rather than located here, since where it lands depends on the state directory.
        addPersistenceNote(std::to_string(duplicateIds) +
                           " entries dropped, another entry claimed the same monitor; the one "
                           "this build can show was kept, and the rest survive only in the "
                           "preserved copy named above");
    }

    // Kept entries are invisible otherwise: they are in no monitor's status and are not
    // malformed, so nothing would tell the user those wallpapers did not come back.
    // Recomposed from the raw path rather than appended to the recorded detail, which is
    // already sanitized and would be escaped a second time.
    repaired = displacedNames > 0 || rejectedNames > 0 || duplicateIds > 0 || m_droppedEntries > 0 ||
               m_droppedNames > 0;
    if ((!m_unapplied.empty() || repaired) && m_health.persistence.status == HealthStatus::Ok) {
        recordHealthLocked(&DaemonHealth::persistence, HealthStatus::Ok,
                           persistenceDetail(m_store.file().string()));
    }
    // Held, not written: this runs before the bus name is claimed, and the write would land
    // on a running daemon's config and on top of the bytes preserveUnreadableConfig() has
    // yet to copy aside. main() writes it once both are settled.
    if (repaired) m_repair = persistableLocked();
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
