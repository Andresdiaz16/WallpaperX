// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaper_manager.h"

#include "log_escape.h"
#include "renderers/static_renderer.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"
#include "vulkan/tier_selection.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
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
    for (int n = 0; n < kMaxPreservedCopies; ++n) {
        const std::filesystem::path kept =
            m_store.file().string() + ".bad" + (n == 0 ? "" : "." + std::to_string(n));
        std::error_code ec;
        // Never over an existing copy: it holds an earlier broken config, and this one
        // would replace it with whatever was written since.
        if (std::filesystem::exists(kept, ec)) continue;
        if (ec) {
            why = ec.message();
            continue;
        }
        if (move) {
            std::filesystem::rename(m_store.file(), kept, ec);
        } else {
            std::filesystem::copy_file(m_store.file(), kept, ec);
        }
        if (ec) {
            why = ec.message();
            continue;
        }
        return kept.string();
    }
    if (why.empty()) why = "no free name";
    return {};
}

void WallpaperManager::preserveUnreadableConfig() {
    if (!m_store.enabled()) return;

    std::string why;
    if (m_loadError.empty()) {
        // Copied, not moved: the entries this build did understand are being restored from
        // this same file. Rejected ones never become entries at all, so the keep-for-a-newer-
        // build path cannot see them and the first save would drop them for good.
        if (m_loadMalformed > 0 && m_loadPreservable) {
            const std::string copied = preserveConfigTo(/*move=*/false, why);
            addPersistenceNote(copied.empty() ? "not copied: " + why : "copied to " + copied);
            recordHealth(&DaemonHealth::persistence, HealthStatus::Ok,
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
    // Written once, here, because this run has resolved something the file still states -- a
    // name two rows claimed, a row no build can apply. Leaving it for the next save means the
    // note recurs on every boot, and the next save may never come: a client that only
    // re-asserts names it already has writes nothing. persist() is what refuses a file this
    // run could not read; taken outside the lock, which it requires.
    if (repair) persist(*repair);
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
        if (!entry.type.empty() || !entry.path.empty()) ++keptWallpapers;
        // Only the names the next save will actually write. restoreSaved() settles the
        // collisions the file arrived with, but a client can claim one of these names later,
        // and persistableLocked() drops the kept copy when it does -- so counting it here
        // would go on offering a name the file no longer holds.
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
    // Known and accepted: these run on the DBus event-loop thread, so a stalled mount blocks
    // every method. Moving them alone would not make the daemon killable -- the render thread
    // opens the same file, and a stat served from cache can precede a read that hangs. The
    // render worker is the wrong home either way: FIFO, so a bad path waits behind every
    // queued render.
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

void WallpaperManager::setMonitorOutput(int monitorId, const std::string& outputName) {
    requireValidMonitorId(monitorId);
    if (const std::string why = outputNameError(outputName); !why.empty()) {
        throw InvalidRequest(why + ": " + sanitizeForLog(outputName));
    }

    std::vector<PersistedWallpaper> toPersist;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        // Idempotent is advertised, so a client may re-assert every name on each screen
        // change; persist() parks this thread -- the bus event loop -- on the write. Not
        // skipped when the last write failed: the map agrees with the request but the file
        // does not, and re-asserting is then the only retry a client can reach.
        if (bindOutputLocked(monitorId, outputName) == BindResult::Unchanged &&
            !persistFailedLocked()) {
            return;
        }
        toPersist = persistableLocked();
    }

    // Outside the lock. Persisted because only the app can name a screen again, and it may
    // not run for weeks.
    persist(toPersist);
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

    // m_outputs is the only authority, so a name an unapplied entry carried is dropped --
    // kept only for an id this build cannot bind, and only while nothing else claims it.
    // Nothing here can report a file naming one screen twice, so it must not write one:
    // a build whose limit admits both ids would bind both and one screen would lose.
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

void WallpaperManager::persist(const std::vector<PersistedWallpaper>& entries) {
    if (!m_store.enabled()) return;

    // The save rebuilds the file from live state, so writing now would replace monitors
    // this run never managed to read with the one the user just set.
    if (m_configUnread) {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        recordHealthLocked(
            &DaemonHealth::persistence, HealthStatus::Fail,
            persistenceDetail(m_store.file().string() +
                              ": not saved, this run could not read it and will not overwrite it"));
        return;
    }

    // Published under m_stateMutex, which is where every reader takes it: an unlocked store
    // beside locked loads is a data race, and the first save can run beside a /health request
    // on the HTTP thread. Released before the worker's lock, keeping the documented order.
    std::shared_ptr<PersistWorker> worker;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        startPersistWorker();
        worker = m_persist;
    }

    std::unique_lock<std::mutex> workerLock(worker->mutex);
    // Replaced, not appended: an older snapshot waiting behind a stalled write describes a
    // state this one already supersedes, so writing it would only be a slower route to the
    // same file.
    worker->pending = entries;
    worker->wake.notify_one();

    // Waited on, briefly. A healthy write takes microseconds, and callers -- including the
    // reply to SetWallpaper -- are entitled to assume the choice is on disk when it returns.
    // The deadline is the whole point: past it the write keeps going on the worker and this
    // thread stops caring, so a stalled filesystem costs one wait rather than the daemon.
    if (worker->stalled) return;
    const auto quiesced = [&worker] { return !worker->pending && !worker->writingSince; };
    if (!worker->settled.wait_for(workerLock, kLoadDeadline, quiesced)) {
        // Not waited on again until something completes: a second stalled call would add
        // another deadline's delay to a bus thread that has already learned the answer.
        worker->stalled = true;
    }
}

void WallpaperManager::startPersistWorker() {
    if (m_persist) return;
    m_persist = std::make_shared<PersistWorker>();

    // The store is copied in and nothing else is captured: this thread outlives the manager
    // whenever a write is stuck, and a write on a dead mount cannot be interrupted.
    std::thread([worker = m_persist, store = m_store] {
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
}

bool WallpaperManager::persistFailedLocked() const {
    // Null before the first save, which means nothing has been asked of the file yet: an
    // unchanged binding then came out of the config, so the file already holds it.
    if (!m_persist) return false;
    std::lock_guard<std::mutex> workerLock(m_persist->mutex);
    return m_persist->haveResult && !m_persist->result.error.empty();
}

void WallpaperManager::applyPersistOutcomeLocked(DaemonHealth& health) const {
    if (!m_persist) return;

    SaveResult saved;
    bool haveResult = false;
    std::optional<std::chrono::steady_clock::time_point> writingSince;
    {
        std::lock_guard<std::mutex> workerLock(m_persist->mutex);
        haveResult = m_persist->haveResult;
        saved = m_persist->result;
        writingSince = m_persist->writingSince;
    }

    // Written into the caller's copy rather than recorded: the write finishes on a thread
    // that must never reach this object, so the outcome is folded in when someone asks.
    const auto report = [&health](HealthStatus status, const std::string& detail) {
        health.persistence.status = status;
        health.persistence.detail = sanitizeForLog(detail, 1024, LogFraming::Bare);
    };

    // A write that has not come back is the only symptom a stalled config filesystem has
    // left: the bus thread no longer waits on it, so nothing else would look slow.
    if (writingSince && std::chrono::steady_clock::now() - *writingSince > kLoadDeadline) {
        report(HealthStatus::Fail,
               persistenceDetail(m_store.file().string() +
                                 ": has not answered a write, the filesystem may be stalled"));
        return;
    }
    if (!haveResult) return;

    if (!saved.error.empty()) {
        report(HealthStatus::Fail, persistenceDetail(saved.error));
        return;
    }

    std::string base = m_store.file().string();
    if (saved.skippedNonUtf8 > 0) {
        base += " (" + std::to_string(saved.skippedNonUtf8) +
                " entries skipped: a path, type or screen name is not valid UTF-8)";
    }
    // Fail, not Ok: the write succeeded but those monitors will not come back, and a
    // health field that reads ok is the only thing that would say otherwise.
    report(saved.skippedNonUtf8 > 0 ? HealthStatus::Fail : HealthStatus::Ok, persistenceDetail(base));
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

    bool repaired = false;
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    for (const PersistedWallpaper& entry : pending) {
        // Three outcomes for a name, each of them reported: bound here, carried for a build
        // whose limit admits the id, or dropped. The carried case asks only for valid UTF-8,
        // not the whole client rule -- a newer build may have a larger cap, and only invalid
        // UTF-8 would cost the entire row on the next write.
        const bool nameValid = !entry.output.empty() && outputNameError(entry.output).empty();
        const bool nameTravels =
            !entry.output.empty() && entry.monitorId >= kMaxMonitors && isValidUtf8(entry.output);
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
            // One row per id: persistableLocked() keys the file by monitor id, so a second
            // kept entry would be dropped by the next save with nothing said. Merged half by
            // half, as the in-range arms above treat them -- a later row carrying only a
            // name must not delete the wallpaper this arm exists to preserve. Last-wins, and
            // reported, only where both rows really filled the same half.
            const auto held = std::find_if(
                m_unapplied.begin(), m_unapplied.end(),
                [&](const PersistedWallpaper& kept) { return kept.monitorId == entry.monitorId; });
            if (held == m_unapplied.end()) {
                PersistedWallpaper keep = entry;
                if (!nameTravels) keep.output.clear();
                m_unapplied.push_back(std::move(keep));
                continue;
            }
            if (!entry.type.empty() || !entry.path.empty()) {
                if (!held->type.empty() || !held->path.empty()) ++duplicateIds;
                held->type = entry.type;
                held->path = entry.path;
            }
            if (nameTravels) {
                // The name counter, not the row counter: what was lost here is a screen's
                // name, and duplicateIds prints a note about kept wallpapers.
                if (!held->output.empty() && held->output != entry.output) ++displacedNames;
                held->output = entry.output;
            }
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

    // Settled here, not in the save. persistableLocked() is const and runs on every write, so
    // a name it dropped could only vanish in silence -- and persistenceDetail() would already
    // have promised it back. Reverse order to keep last-wins, as everything else here does.
    std::set<std::string> claimed;
    for (auto it = m_unapplied.rbegin(); it != m_unapplied.rend(); ++it) {
        if (it->output.empty()) continue;
        if (m_outputs.count(it->output) != 0 || !claimed.insert(it->output).second) {
            it->output.clear();
            ++displacedNames;
        }
    }

    if (displacedNames > 0) {
        addPersistenceNote(std::to_string(displacedNames) +
                           " screen names displaced by a later entry claiming the same name or "
                           "the same screen; the last entry won");
    }
    if (rejectedNames > 0) {
        addPersistenceNote(std::to_string(rejectedNames) +
                           " screen names dropped, unusable; those screens must be named again");
    }
    if (duplicateIds > 0) {
        addPersistenceNote(std::to_string(duplicateIds) +
                           " kept entries dropped, a later entry claimed the same monitor; the "
                           "last entry won");
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
            // Sanitized where client text enters daemon state, not at each place it
            // leaves, so every consumer inherits the escaping: the renderer's message
            // embeds the client's path, and a readable file may be named with an ESC
            // sequence.
            //
            // 1024 rather than the default because this field is polled. Safe only because
            // describeRejection() puts the diagnosis before the path -- truncation keeps
            // the head, so reversing that message would cut stb's reason instead.
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
