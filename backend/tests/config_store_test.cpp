// SPDX-License-Identifier: GPL-2.0-or-later

// Step 8's persistence layer, below the bus and below the GPU: ConfigStore,
// defaultConfigPath(), and the WallpaperManager half that drives them.
//
// Its own target rather than a phase of dbus_service_test, for the reason log_escape_test
// is its own target: nothing here needs a session bus, a port or a Vulkan device.
// requestWallpaper() is GPU-free by construction (it stats the file, records the request
// and writes the config), so the whole save path is reachable without a device -- and
// folding it into a tier that needs one would make a text-file regression unreachable on a
// host without Vulkan.
//
// What this file cannot establish, and daemon_system_test does instead: that the *daemon*
// starts on any config content (S8.2 is about a process, and a const operator[] on a
// missing key is a SIGABRT that no in-process check survives to report), that a wallpaper
// survives a restart (S8.1), and that a monitor whose file is missing says why it is blank
// (S8.4's second clause needs a render).
//
// Rules followed here:
//   - Nothing this file writes lands outside a directory it created under
//     temp_directory_path() and removes again. A test that touched $HOME/.config/wallpaperx
//     would be restoring its own wallpapers onto a developer's desktop, which has happened.
//   - A phase that cannot be set up on this host prints a reason and is reported to ctest
//     as a skip (exit 77), never as a pass.

#include "daemon/config_store.h"
#include "daemon/log_escape.h"
#include "daemon/wallpaper_manager.h"
#include "tests/client/blocking_config_write.h"
#include "tests/client/scratch_dir.h"
#include "tests/test_report.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

using wallpaperx::ConfigStore;
using wallpaperx::LoadResult;
using wallpaperx::LogFraming;
using wallpaperx::PersistedWallpaper;
using wallpaperx::SaveResult;
using wallpaperx::sanitizeForLog;
using wallpaperx::testing::BlockingWrite;
using wallpaperx::testing::blockingWriteFixtureWorks;
using wallpaperx::testing::isRegularFile;
using wallpaperx::testing::ScratchDir;
using wallpaperx::WallpaperManager;

// Counts the durability syscalls anything in this process makes, the daemon's save
// included: the test links the daemon statically, so a strong definition here is what its
// calls resolve to. Phase 8 is built on this. Defined at namespace scope and outside the
// anonymous namespace because the C symbols have to have external linkage.
std::atomic<unsigned> g_fsyncCalls{0};

extern "C" int fsync(int fd) {
    ++g_fsyncCalls;
    using Fn = int (*)(int);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "fsync"));
    return real != nullptr ? real(fd) : 0;
}

extern "C" int fdatasync(int fd) {
    ++g_fsyncCalls;
    using Fn = int (*)(int);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "fdatasync"));
    return real != nullptr ? real(fd) : 0;
}

// Counts the saves that actually landed, by counting successful rename(2) calls onto one
// path -- the last step of ConfigStore::save(), and the only one a caller can see from
// outside. Interposed for the same reason the two counters above are, and it is the same
// mechanism: the test links the daemon statically, so a strong definition here is what the
// daemon's own call resolves to.
//
// It exists because counting writes is the only way to tell a superseded snapshot from a
// queued one -- both leave the newest state on disk. inotify was tried first and cannot do
// it: the kernel merges an IN_MOVED_TO with the one before it when the mask and name match,
// which is exactly the case here, so two renames onto config.json arrive as one event.
// Measured, with the watch widened to IN_ALL_EVENTS: both renames are there, with distinct
// cookies, and the narrow watch still reported one.
std::mutex g_renameMutex;
std::string g_renameTarget;
unsigned g_renameCount = 0;

// One source path whose renames answer EXDEV, which is how a kernel refuses to move a file to
// another filesystem. The preservation phase that needs it cannot stage the real thing: it would
// have to be handed two directories on two mounts, and a test that skipped unless the host
// offered them would not run anywhere.
std::string g_crossDeviceSource;

extern "C" int rename(const char* oldPath, const char* newPath) {
    using Fn = int (*)(const char*, const char*);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "rename"));
    if (real == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(g_renameMutex);
        if (!g_crossDeviceSource.empty() && oldPath != nullptr && g_crossDeviceSource == oldPath) {
            errno = EXDEV;
            return -1;
        }
    }
    const int result = real(oldPath, newPath);
    if (result == 0 && newPath != nullptr) {
        std::lock_guard<std::mutex> lock(g_renameMutex);
        if (!g_renameTarget.empty() && g_renameTarget == newPath) ++g_renameCount;
    }
    return result;
}

// An allocation that can be made to fail, because one goal here has no other lever.
// std::thread's constructor allocates the callable's state before it ever asks the kernel for a
// thread, so a machine out of memory throws std::bad_alloc where the config writer is started --
// and nothing else in this suite produces that. RLIMIT_NPROC produces a std::system_error, which
// the handler already caught before this fix, so it cannot tell the two builds apart.
//
// Compiled out under a sanitizer, the same trade http_server_test makes for the same measured
// reason: a sanitized runtime owns ::operator new/delete, its versions carry the shadow-memory
// bookkeeping the tool reasons from, and a replacement forwarding to malloc/free silently removes
// it -- 90 spurious "data race in operator delete" reports were measured there. With the
// instrument absent the phase reports itself unexercised and ctest's SKIP_RETURN_CODE turns that
// into a Skip, never into a pass.
#ifndef WALLPAPERX_ALLOCATION_INSTRUMENT
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_LEAK__) || \
    defined(__SANITIZE_MEMORY__) || defined(__SANITIZE_HWADDRESS__)
#define WALLPAPERX_ALLOCATION_INSTRUMENT 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) ||                        \
    __has_feature(leak_sanitizer) || __has_feature(memory_sanitizer) ||                           \
    __has_feature(hwaddress_sanitizer)
#define WALLPAPERX_ALLOCATION_INSTRUMENT 0
#else
#define WALLPAPERX_ALLOCATION_INSTRUMENT 1
#endif
#else
#define WALLPAPERX_ALLOCATION_INSTRUMENT 1
#endif
#endif  // WALLPAPERX_ALLOCATION_INSTRUMENT

namespace {

// One relaxed load is all an unarmed allocation pays, which it has to be: with the instrument
// compiled in, every allocation in this process goes through here.
std::atomic<bool> g_allocProbeArmed{false};

// Per-thread, so only the thread under test is ever faulted -- the config writer's own thread
// allocates too, and faulting it would be a different experiment. -1 means "not armed".
thread_local long long t_failAfter = -1;
thread_local bool t_injected = false;

// Never inlined into operator new, so the unarmed path stays a load and a branch. Must not
// allocate: it runs inside the allocator.
[[gnu::noinline]] void noteAllocation() {
    if (t_failAfter < 0) return;
    if (t_failAfter-- > 0) return;
    // Disarmed before the throw, so exactly one allocation fails per arming: the unwinding it
    // starts allocates as well, and faulting that too would fault code the goal is not about.
    t_failAfter = -1;
    t_injected = true;
    throw std::bad_alloc();
}

}  // namespace

#if WALLPAPERX_ALLOCATION_INSTRUMENT

void* operator new(std::size_t bytes) {
    if (g_allocProbeArmed.load(std::memory_order_relaxed)) noteAllocation();
    const std::size_t request = bytes == 0 ? 1 : bytes;
    void* p = std::malloc(request);
    while (p == nullptr) {
        // The replacement owes the same new_handler protocol the default one implements;
        // returning null instead would be undefined behaviour everywhere in this binary.
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
        p = std::malloc(request);
    }
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#endif  // WALLPAPERX_ALLOCATION_INSTRUMENT

namespace {

// The pass / fail / skip contract lives in tests/test_report.h: exit 77 for a goal this host
// could not exercise (ctest turns it into SKIP through SKIP_RETURN_CODE), exit 1 for a check
// that ran and did not hold, and failures outranking skips so a run that is both broken and
// incomplete cannot hide behind "Skipped".
using wallpaperx::testing::check;
using wallpaperx::testing::g_failures;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

// Sets an environment variable for as long as it is alive and puts back exactly what was
// there before, including "it was not set at all". Every phase that reads
// defaultConfigPath() runs under one of these: leaking an XDG_CONFIG_HOME into a later
// phase would point it at a directory this test then deletes.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::optional<std::string>& value) : m_name(name) {
        if (const char* previous = std::getenv(name); previous != nullptr) m_previous = previous;
        apply(value);
    }
    ~ScopedEnv() { apply(m_previous); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void apply(const std::optional<std::string>& value) {
        if (value.has_value()) {
            ::setenv(m_name, value->c_str(), 1);
        } else {
            ::unsetenv(m_name);
        }
    }

    const char* m_name;
    std::optional<std::string> m_previous;
};

// Points the interposed rename() above at one destination and zeroes its count. Two phases
// read it: the one that counts the saves a stalled write swallowed, and the one that counts
// the saves a redundant SetMonitorOutput must not make.
void countRenamesOnto(const std::filesystem::path& target) {
    std::lock_guard<std::mutex> lock(g_renameMutex);
    g_renameTarget = target.string();
    g_renameCount = 0;
}

unsigned renamesCounted() {
    std::lock_guard<std::mutex> lock(g_renameMutex);
    return g_renameCount;
}

// Makes every rename OUT OF `source` answer EXDEV, and puts it back when it goes out of scope --
// a leak would follow the config writer, whose own rename is how a save lands.
class CrossDeviceRenames {
public:
    explicit CrossDeviceRenames(const std::filesystem::path& source) { set(source.string()); }
    ~CrossDeviceRenames() { set({}); }

    CrossDeviceRenames(const CrossDeviceRenames&) = delete;
    CrossDeviceRenames& operator=(const CrossDeviceRenames&) = delete;

private:
    static void set(const std::string& source) {
        std::lock_guard<std::mutex> lock(g_renameMutex);
        g_crossDeviceSource = source;
    }
};

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeFile(const std::filesystem::path& file, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return static_cast<bool>(out);
}

// A real, readable, regular file -- requestWallpaper rejects anything else on the reply,
// so a manager-level phase needs one per monitor it drives. The contents are irrelevant:
// nothing here renders.
bool writeWallpaperFile(const std::filesystem::path& file) { return writeFile(file, "not really a png"); }

// Where a fixture-driven phase writes, and the wallpaper files it drives.
struct PhasePaths {
    std::filesystem::path config;
    std::vector<std::filesystem::path> wallpapers;
};

// The preconditions every fixture-driven phase shares, in one place rather than copied per
// phase: a guard a phase needs must not be the one its author forgot, and a phase missing one
// grades the daemon on a host state it was never put in.
//
// Takes the ScratchDir by reference and does not own it: ScratchDir deletes its copy
// constructor and declares a destructor, so it has no move constructor and cannot be returned.
//
// CONTROL, for the guards being reached: the binary run under a TMPDIR whose name carries a
// lone 0x80 byte -> all twelve phases routed through here report NOT VERIFIED naming their
// goals, no FAIL line, exit 77, instead of grading the daemon on a path its config writer must
// skip. With the UTF-8 arm below disabled and the same TMPDIR, twelve FAIL lines came back.
// CONTROL, for the migration not having defanged those phases: on a healthy host with
// backend/daemon/config_store.cpp's per-entry UTF-8 test forced true, so save() drops every
// entry -> 40 FAIL lines including all seven these phases contribute. Reverted.
std::optional<PhasePaths> preparePhase(const ScratchDir& scratch, const char* goals, int wallpaperCount) {
    if (!scratch.ok()) {
        notVerified(goals, "a scratch directory could not be created");
        return std::nullopt;
    }
    PhasePaths paths;
    paths.config = scratch.path() / "wallpaperx" / "config.json";
    for (int i = 0; i < wallpaperCount; ++i) {
        const std::filesystem::path wallpaper = scratch.path() / ("wallpaper" + std::to_string(i) + ".png");
        if (!writeWallpaperFile(wallpaper)) {
            notVerified(goals, "the wallpaper files this phase drives could not be written");
            return std::nullopt;
        }
        paths.wallpapers.push_back(wallpaper);
    }
    // nlohmann's dump() throws on a string that is not valid UTF-8, and these phases name a
    // scratch path this host chose inside a JSON fixture. A skip with the reason, rather than
    // an exception the phase would report as a failure of the daemon.
    if (wallpaperCount > 0 && !wallpaperx::isValidUtf8(scratch.path().string())) {
        notVerified(goals, "this host's temporary directory has a path that is not valid UTF-8, so no JSON fixture "
                           "can name it");
        return std::nullopt;
    }
    return paths;
}

// The fixture for the config the daemon is not allowed to open, at namespace scope because
// the case table's plant and intact functions are captureless. Two monitors and valid JSON:
// what makes losing it data loss rather than a reset is that the daemon cannot read either
// path, while a save rebuilds the file from live state that holds neither.
constexpr const char* kUnopenableConfig =
    "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/left.png\"},"
    "{\"monitor_id\":1,\"type\":\"static\",\"path\":\"/photos/right.png\"}]}";

std::string hex(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
        out += ' ';
    }
    return out;
}

// Where the daemon keeps a preserved copy of a config it rejected: $XDG_STATE_HOME/wallpaperx.
// A phase that can produce one sets XDG_STATE_HOME to its own scratch directory, so no run of
// this file writes into the developer's own state directory.
std::filesystem::path stateHomeOf(const ScratchDir& scratch) { return scratch.path() / "state"; }
std::filesystem::path preservedCopyDir(const ScratchDir& scratch) {
    return stateHomeOf(scratch) / "wallpaperx";
}

// Every preserved-copy name in a state directory, sorted. Named by prefix rather than by
// asking whether `config.json.bad` exists: preservation numbers its copies, so a check
// written against the one name would be satisfied by a build that wrote `.bad.1` instead --
// which is still a copy the goal says must not have been made. Both directories are scanned,
// because "no copy was made" has to be false for a copy taken anywhere, the old place beside
// the config file included.
std::vector<std::string> preservedCopyNames(const std::filesystem::path& file,
                                            const std::filesystem::path& stateDir) {
    std::vector<std::string> names;
    const std::string prefix = file.filename().string() + ".bad";
    for (const std::filesystem::path& dir : {stateDir, file.parent_path()}) {
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.rfind(prefix, 0) == 0) names.push_back(it->path().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

// One config row as JSON. `output` is omitted when empty, matching what a build that never
// named a screen writes -- a fixture that always emitted the key would be testing a file
// shape no build produces.
nlohmann::json configRow(int monitorId, const std::string& type, const std::string& path,
                         const std::string& output) {
    nlohmann::json row;
    row["monitor_id"] = monitorId;
    row["type"] = type;
    row["path"] = path;
    if (!output.empty()) row["output"] = output;
    return row;
}

using StallClock = std::chrono::steady_clock;

double millisSince(StallClock::time_point start) {
    return std::chrono::duration<double, std::milli>(StallClock::now() - start).count();
}

// The deadline a save is given, restated rather than shared: kWriteDeadline sits in an
// anonymous namespace in config_writer.cpp. A copy here means a change to it fails the
// phases that use it loudly instead of silently re-pointing them at whatever the new value is.
constexpr double kSaveDeadlineMs = 2000.0;

// How long a phase that parks a config write waits for the call to come back before walking
// away from it. Ten times the deadline above, so no healthy build is ever abandoned, and
// finite, so a build whose wait is unbounded reddens a check instead of hanging the binary.
constexpr std::chrono::seconds kParkedCallPatience{20};

double parkedCallPatienceMs() {
    return std::chrono::duration<double, std::milli>(kParkedCallPatience).count();
}

// One requestWallpaper(), made on a thread the caller can walk away from.
//
// The obvious version -- call it inline -- turns a build whose wait is unbounded into a
// HANG, and a hung test prints nothing at all: no FAIL line, no reason, just a ctest
// timeout thirty seconds later. This one turns that build into a check that goes red.
// The manager is held by shared_ptr and copied into the thread, so a call that never
// returns keeps alive the object it is inside rather than being abandoned on top of a
// destroyed one.
//
// Declared here rather than beside its first caller because two phases park a write and both
// need the bound: the "one answer about the config file" phase below and the stalled-save
// phase further down.
struct AbandonedOrReturned {
    bool returned = false;
    double elapsedMs = 0;
    std::string threw;
};

AbandonedOrReturned requestOnAbandonableThread(const std::shared_ptr<WallpaperManager>& manager,
                                               const std::string& path, int monitorId,
                                               std::chrono::seconds patience) {
    struct Answer {
        double elapsedMs = 0;
        std::string threw;
    };
    auto answered = std::make_shared<std::promise<Answer>>();
    std::future<Answer> future = answered->get_future();
    std::thread([manager, answered, path, monitorId] {
        const auto start = StallClock::now();
        Answer answer;
        try {
            manager->requestWallpaper("static", path, monitorId);
        } catch (const std::exception& e) {
            answer.threw = e.what();
        } catch (...) {
            answer.threw = "an exception that is not a std::exception";
        }
        answer.elapsedMs = millisSince(start);
        answered->set_value(answer);
    }).detach();

    AbandonedOrReturned out;
    if (future.wait_for(patience) != std::future_status::ready) return out;
    const Answer answer = future.get();
    out.returned = true;
    out.elapsedMs = answer.elapsedMs;
    out.threw = answer.threw;
    return out;
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out.empty() ? "(none)" : out;
}

// The paths persisted for each monitor id, read back out of a config file by parsing it
// here rather than through ConfigStore::load(). Deliberate: a check that the writer and
// the reader agree is satisfied by a writer and reader that are wrong in the same way,
// which is the defect a shared field-name constant makes easy.
std::map<int, std::string> pathsInFile(const std::filesystem::path& file, std::string& why) {
    std::map<int, std::string> paths;
    const std::string text = readFile(file);
    if (text.empty()) {
        why = "the config file is empty or absent";
        return paths;
    }
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("monitors") || !root["monitors"].is_array()) {
        why = "the config file is not an object with a monitors array";
        return paths;
    }
    for (const nlohmann::json& entry : root["monitors"]) {
        if (!entry.is_object() || !entry.contains("monitor_id") || !entry.contains("path")) continue;
        if (!entry["monitor_id"].is_number_integer() || !entry["path"].is_string()) continue;
        paths[entry["monitor_id"].get<int>()] = entry["path"].get<std::string>();
    }
    return paths;
}

// One monitor's row as it sits in the file, for the connector-name phases. `hasOutputKey`
// is separate from `output` because "the key is absent" and "the key is an empty string"
// are different files: the first is what a build with no connector names writes, and
// keeping them apart is the only way to check that promise.
struct RowInFile {
    std::string type;
    std::string path;
    bool hasOutputKey = false;
    std::string output;
};

// Read by parsing here rather than through ConfigStore::load(), for the reason
// pathsInFile() above gives: a reader sharing the writer's field-name constants agrees
// with it by construction, including when both are wrong.
//
// `rawRows`, when given, receives how many rows the file really carries, which the map cannot
// say: it keys by monitor id, so two rows for one id and one row for that id are the same map.
// Filled from the SAME parse rather than by a second read, because the daemon's writer can
// replace the file between two reads -- a count from one version against ids from another
// reports a correct save as a monitor written on two rows.
//
// CONTROL, for the four duplicate-row checks really reading this number: `size() + 1` here ->
// all four go red at once, printing "5 rows for 4 monitor ids", "6 rows for 5 monitor ids",
// "6 rows for 5 monitor ids" and "13 rows for 12 monitor ids". Observed 2026-08-15. Reverted.
std::map<int, RowInFile> rowsInFile(const std::filesystem::path& file, std::string& why,
                                    std::size_t* rawRows = nullptr) {
    std::map<int, RowInFile> rows;
    if (rawRows != nullptr) *rawRows = 0;
    const std::string text = readFile(file);
    if (text.empty()) {
        why = "the config file is empty or absent";
        return rows;
    }
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("monitors") || !root["monitors"].is_array()) {
        why = "the config file is not an object with a monitors array";
        return rows;
    }
    if (rawRows != nullptr) *rawRows = root["monitors"].size();
    for (const nlohmann::json& entry : root["monitors"]) {
        if (!entry.is_object() || !entry.contains("monitor_id") || !entry["monitor_id"].is_number_integer()) continue;
        RowInFile row;
        if (entry.contains("type") && entry["type"].is_string()) row.type = entry["type"].get<std::string>();
        if (entry.contains("path") && entry["path"].is_string()) row.path = entry["path"].get<std::string>();
        row.hasOutputKey = entry.contains("output");
        if (row.hasOutputKey && entry["output"].is_string()) row.output = entry["output"].get<std::string>();
        rows[entry["monitor_id"].get<int>()] = std::move(row);
    }
    return rows;
}

std::string idsOf(const std::map<int, RowInFile>& rows) {
    std::string out;
    for (const auto& [id, row] : rows) {
        (void)row;
        if (!out.empty()) out += " ";
        out += std::to_string(id);
    }
    return out.empty() ? "(none)" : out;
}

// Waits for the state a phase is about to grade to reach the config file, up to a bound far
// longer than WallpaperManager::persist()'s own deadline. Needed because persist() waits for
// its detached writer only until that deadline and then returns with the write still queued:
// a read taken at the instant a manager call returned can legitimately see the file as it was,
// and grading it prints a FAIL against the daemon for a slow host.
//
// Sound only where nothing else is writing between the call and the read -- with a second
// writer outstanding, the state the poll observes need not be the one the graded call made.
// An expired wait is left to the caller's own check: ten seconds without the row is a verdict
// on the daemon, not on the host, so no check is weakened by waiting.
//
// CONTROL: a three-second sleep added to this file's interposed rename(), so every write lands
// after persist() has abandoned its wait -- a host slower than the daemon's deadline, with the
// daemon itself correct. With this wait shortened to zero, that run produced 18 FAIL lines
// against the daemon, among them "the repair a start performs is on disk before any client
// call" and "a kept row left holding nothing is not written back". With the wait as it stands,
// the same host left 3, all at reads of the health text and the fsync counter, which no wait on
// the file's rows can settle. Observed 2026-08-15. Both reverted.
bool waitForRows(const std::filesystem::path& file,
                 const std::function<bool(const std::map<int, RowInFile>&)>& landed) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        std::string ignored;
        if (landed(rowsInFile(file, ignored))) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// The number a persistence-report clause is prefixed with. `clause` is the text WITHOUT the
// count, e.g. " screen names dropped, unusable".
//
// Compared as an integer rather than searched for as "3 screen names ...": std::string::find
// has no left boundary, so the expected text is also a substring of "13 screen names ..." and
// a build that over-counted by ten would pass. Returns 0 when the clause is absent, which is
// what production writes for a count of zero, and an EMPTY optional when the digits cannot be
// read. Empty rather than a negative number so the two cannot be compared in the same channel:
// a sentinel graded as a count reports a reworded report as the daemon having lost every row
// the clause counts.
//
// CONTROL: the kept-wallpaper count in WallpaperManager::persistenceDetail()
// (backend/daemon/wallpaper_manager.cpp) written as `keptWallpapers + 10`, which is the
// over-count a find() cannot see -> "FAIL: the persistence detail says how many entries could
// not be applied", "FAIL: only the rows with a wallpaper are reported as entries this build
// cannot apply" and "FAIL: every kept row carrying a wallpaper is reported as one". Observed
// 2026-08-14. Reverted.
std::optional<int> clauseCount(const std::string& detail, const std::string& clause) {
    const std::size_t at = detail.find(clause);
    if (at == std::string::npos) return 0;
    std::size_t start = at;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(detail[start - 1]))) --start;
    if (start == at || at - start > 9) return std::nullopt;
    return std::stoi(detail.substr(start, at - start));
}

// How many times `clause` occurs in `detail`. The counted-losses phase needs it because the
// property there is "ONE clause carrying N", and clauseCount() reads the first occurrence only:
// a report that emitted the same clause twice would answer the same number.
int clauseOccurrences(const std::string& detail, const std::string& clause) {
    int seen = 0;
    for (std::size_t at = detail.find(clause); at != std::string::npos;
         at = detail.find(clause, at + clause.size())) {
        ++seen;
    }
    return seen;
}

// The same, with an unreadable count reported as a skip of `goals` and the report printed.
// A caller that gets an empty result must grade nothing it would have compared: the count it
// wanted is not a number this run knows, and comparing it against a fixture size accuses the
// daemon of a loss the report never claimed.
//
// CONTROL: the leading space dropped from one clause a phase searches for, so the digits no
// longer sit against it -> "NOT VERIFIED -- S10.4/S10.5's reporting half ... a count could not
// be read out of the persistence report's 'screen names dropped, unusable' clause", exit 77.
// With that phase's comparison put back to grading the unreadable result as a number, the same
// build reported "FAIL: every name no build could accept is reported as dropped and unusable"
// -- the daemon blamed for the harness's parse. Observed 2026-08-15. Both reverted.
std::optional<int> clauseCountOrSkip(const std::string& detail, const std::string& clause, const char* goals) {
    const std::optional<int> count = clauseCount(detail, clause);
    if (!count.has_value()) {
        notVerified(goals, "a count could not be read out of the persistence report's '" + clause + "' clause");
        std::fprintf(stderr, "  persistence detail: %s\n", detail.c_str());
    }
    return count;
}

// The texts WallpaperManager::persistenceDetail() emits for the counters that set `repaired` in
// restoreSaved() (backend/daemon/wallpaper_manager.cpp). Returns the ones present, quoted, for
// the failure print; empty means nothing was repaired. One list rather than one per phase: two
// copies can only agree by an editor remembering both, and a copy that misses a clause
// production later added leaves its check green over a daemon that repairs on every boot.
//
// CONTROL, for both readers really reading this list: "config.json" -- a text every persistence
// report carries -- added to it -> "FAIL: this phase's fixture is one the restore has nothing to
// repair in" and "FAIL: a second start with no client acting has nothing left to repair", the
// two checks in different phases that scan for these clauses, from one edit. Observed
// 2026-08-15. Reverted.
std::string repairClausesIn(const std::string& detail) {
    static constexpr const char* kRepairClauses[] = {
        "displaced by another entry", "dropped, unusable", "entries dropped, another entry",
        "entries dropped, no build can apply", "screen names dropped, no build can apply"};
    std::string found;
    for (const char* clause : kRepairClauses) {
        if (detail.find(clause) != std::string::npos) found += std::string(" '") + clause + "'";
    }
    return found;
}

// ---------------------------------------------------------------------------- //
// Phase 1 -- S8.9: where the config file is allowed to be, and where it is not.
// ---------------------------------------------------------------------------- //
void runLocationChecks() {
    const ScratchDir scratch("location");
    if (!scratch.ok()) {
        notVerified("S8.9 (nothing is written outside the XDG location)", "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path xdg = scratch.path() / "xdg";
    const std::filesystem::path home = scratch.path() / "home";

    // GOAL: S8.9 -- $XDG_CONFIG_HOME decides the location when it is absolute, is IGNORED
    //       when it is relative (XDG basedir spec), $HOME/.config is the fallback, and with
    //       neither usable persistence is disabled rather than relocated. False if a
    //       relative value is honoured, and false if an unusable pair yields any path at
    //       all -- a daemon started from / would then leave a config.json in /.
    // WHY-THIS-SHAPE: six cases in one table, each asserting the WHOLE path rather than
    //       "contains wallpaperx". The obvious shorter version -- assert only the absolute
    //       and the empty cases -- passes for an implementation that resolves a relative
    //       XDG_CONFIG_HOME against the working directory, which is the one case the spec
    //       calls out and the one that writes somewhere nobody looks. The empty-string
    //       cases are separate from the unset ones because getenv returns "" for both an
    //       exported empty variable and nothing at all is not what it returns.
    // CONTROL: two, both reverted. The `if (base.is_absolute())` guard removed from the XDG
    //       branch of defaultConfigPath() -> "FAIL: a relative XDG_CONFIG_HOME is ignored,
    //       not resolved against the working directory", printing 'got
    //       "relative/config/wallpaperx/config.json"' and nothing else red. And `return {};`
    //       replaced with `current_path() / kAppDir / kFileName` -> two FAILs, "a relative
    //       HOME is refused rather than resolved" and "with neither XDG_CONFIG_HOME nor HOME
    //       usable, persistence is disabled rather than relocated into the working
    //       directory", both printing the build directory.
    struct LocationCase {
        const char* what;
        std::optional<std::string> xdg;
        std::optional<std::string> home;
        std::filesystem::path expected;
    };
    const std::vector<LocationCase> cases{
        {"an absolute XDG_CONFIG_HOME decides the location", xdg.string(), home.string(),
         xdg / "wallpaperx" / "config.json"},
        {"a relative XDG_CONFIG_HOME is ignored, not resolved against the working directory",
         std::string("relative/config"), home.string(), home / ".config" / "wallpaperx" / "config.json"},
        {"an empty XDG_CONFIG_HOME falls back to $HOME/.config", std::string(), home.string(),
         home / ".config" / "wallpaperx" / "config.json"},
        {"an unset XDG_CONFIG_HOME falls back to $HOME/.config", std::nullopt, home.string(),
         home / ".config" / "wallpaperx" / "config.json"},
        {"a relative HOME is refused rather than resolved", std::nullopt, std::string("home"), {}},
        {"with neither XDG_CONFIG_HOME nor HOME usable, persistence is disabled rather than relocated into the "
         "working directory",
         std::nullopt, std::nullopt, {}},
    };

    for (const LocationCase& c : cases) {
        const ScopedEnv xdgEnv("XDG_CONFIG_HOME", c.xdg);
        const ScopedEnv homeEnv("HOME", c.home);
        const std::filesystem::path got = wallpaperx::defaultConfigPath();
        check(got == c.expected, c.what);
        if (got != c.expected) {
            std::fprintf(stderr, "  expected '%s', got '%s'\n", c.expected.string().c_str(), got.string().c_str());
        }
    }

    // GOAL: S8.9's other half -- a disabled store is SILENT, not merely path-less. A save()
    //       that fell back to a relative name would land in the process's working directory,
    //       which is the "config file left where nobody looks" the decision exists to
    //       prevent.
    // WHY-THIS-SHAPE: the process is moved into a directory this phase created and the whole
    //       directory is listed afterwards. The obvious version -- assert config.json,
    //       wallpaperx and .config do not exist in the working directory as found -- is red
    //       for anything already sitting in the build tree (an install-staging `wallpaperx`,
    //       a leftover from an aborted run) and blind to a write under any fourth name. This
    //       one owns the directory, so anything in it afterwards was put there by the save.
    //       chdir is process-wide and this phase runs before any WallpaperManager exists, so
    //       no other thread can be resolving a relative path while it moves.
    // CONTROL: ConfigStore's constructor given the cwd fallback the decision forbids
    //       (`if (m_file.empty()) m_file = std::filesystem::path(kAppDir) / kFileName;`) ->
    //       "FAIL: a disabled store writes nothing into the working directory", printing
    //       "  left behind: wallpaperx", plus the three checks that say a store with no path
    //       is disabled and silent. Reverted.
    const std::filesystem::path silent = scratch.path() / "silent";
    std::error_code silentEc;
    std::filesystem::create_directories(silent, silentEc);
    const std::filesystem::path cwd = std::filesystem::current_path();
    if (silentEc || ::chdir(silent.c_str()) != 0) {
        notVerified("S8.9's silence clause", "a working directory this phase owns could not be entered");
        return;
    }

    const ConfigStore disabled{std::filesystem::path{}};
    check(!disabled.enabled(), "a ConfigStore built from an empty path reports itself disabled");
    const SaveResult saved = disabled.save({PersistedWallpaper{0, "static", "/tmp/x.png"}});
    check(saved.error.empty(), "saving through a disabled store is not an error");
    const LoadResult loaded = disabled.load();
    check(loaded.entries.empty() && loaded.error.empty() && !loaded.fileExisted,
          "loading through a disabled store reports nothing, not an error");

    std::vector<std::string> left;
    std::error_code walkEc;
    for (auto it = std::filesystem::directory_iterator(silent, walkEc);
         !walkEc && it != std::filesystem::directory_iterator(); it.increment(walkEc)) {
        left.push_back(it->path().filename().string());
    }
    // Restored before anything is graded: a check that fails here must not also leave every
    // later phase resolving relative paths inside a directory this one is about to delete.
    if (::chdir(cwd.c_str()) != 0) {
        std::fprintf(stderr, "FAIL: the working directory could not be restored after S8.9's silence check\n");
        ++g_failures;
    }
    if (walkEc) {
        notVerified("S8.9's silence clause",
                    "the working directory this phase owns could not be listed: " + walkEc.message());
        return;
    }
    check(left.empty(), "a disabled store writes nothing into the working directory");
    for (const std::string& name : left) std::fprintf(stderr, "  left behind: %s\n", name.c_str());
}

// ---------------------------------------------------------------------------- //
// Phase 2 -- S8.2: load() survives any file content at all.
// ---------------------------------------------------------------------------- //
void runLoadCorpusChecks() {
    const ScratchDir scratch("corpus");
    if (!scratch.ok()) {
        notVerified("S8.2 (no config content stops the daemon)", "a scratch directory could not be created");
        return;
    }

    std::string huge(100u * 1024u * 1024u, 'x');
    std::string deep = "{\"version\":1,\"monitors\":";
    deep.append(100000, '[');
    deep.append(100000, ']');
    deep += "}";
    // A complete, valid document with a NUL and rubbish after it. The length is the
    // compiler's, not a hand count: the previous count was one byte short of its own
    // literal, which quietly turned the case into a second copy of the truncation row.
    using namespace std::string_literals;
    const std::string nulAfterValid =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a.png\"}]}\0 junk"s;

    // GOAL: S8.2, in-process half -- no file content makes load() throw, abort, or hand
    //       back entries it did not read. The daemon half (the process still starts) is
    //       daemon_system_test's; what is established here is the shape of the result for
    //       each input, which that tier cannot see.
    // WHY-THIS-SHAPE: each case states the number of entries AND the number of malformed
    //       ones it expects, and one case is a well-formed file expecting two entries.
    //       Asserting only "it did not throw" would be satisfied by a load() that returned
    //       nothing for everything -- including for a valid file -- which is a daemon that
    //       starts and has forgotten every wallpaper. The well-formed row is what makes the
    //       rest of the table mean "rejected", not "always empty".
    // CONTROL: two, both reverted. (a) `if (!field.is_string()) return false;` deleted from
    //       readString() in config_store.cpp -> "FAIL: a type field of the wrong type" and
    //       "FAIL: a null path field", each reporting "0 malformed (expected 1), error
    //       '...could not be read: [json.exception.type_error.302] type must be string, but
    //       is number/null'" -- the throw being caught by load() rather than escaping.
    //       (b) the version read replaced by a const `root[kKeyVersion]` -> this process
    //       died on SIGABRT in the "valid JSON of the wrong shape" case, printing json.hpp's
    //       `it != m_data.m_value.object->end()` assertion and NO check output at all. That
    //       is why S8.2's process-level half lives in daemon_system_test: an in-process
    //       binary cannot report the failure mode it dies from.
    // COVERAGE-ASSERT: every case is accounted for -- run, or reported as one this host
    //       could not stage -- against the table's size, so a `continue` or an early return
    //       that silently thins the corpus fails instead of going quiet.
    //       CONTROL: `if (c.content.size() > 1000) continue;` added at the top of the loop,
    //       which is what a well-meaning "skip the slow ones" edit looks like -> "FAIL: every
    //       load case was either exercised or reported as one this host could not stage",
    //       and nothing else. Reverted.
    //
    // S8.15's three rows are not a variation on the fractional one: is_number_integer() is
    // TRUE for any width, so 2^32+1 passes that guard and only readInt's range check stops
    // get<int>() narrowing it to 1. Both the unsigned and the signed branch are covered,
    // because nlohmann stores a positive literal as unsigned and a negative one as signed --
    // one row would leave the other branch as uncovered as the whole guard was.
    // CONTROL: readInt's body in config_store.cpp replaced with `out = field.get<int>();
    //       return true;` -> exactly three FAILs, "a monitor_id past int range" and "a
    //       negative monitor_id past int range" each reporting "1 entries (expected 0), 0
    //       malformed (expected 1)" -- the entry restored onto monitor 1 and -1 -- and "a
    //       version past int range" reporting a file accepted where an error was expected.
    //       Reverted.
    struct LoadCase {
        const char* what;
        std::string content;
        int expectedEntries;
        int expectedMalformed;
        bool expectError;
    };
    std::vector<LoadCase> cases{
        {"an empty file", "", 0, 0, true},
        {"a file that is not JSON at all", "wallpapers: all of them\n", 0, 0, true},
        {"a file truncated mid-object", "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\"", 0, 0,
         true},
        {"a JSON array where an object belongs", "[]", 0, 0, true},
        {"valid JSON of the wrong shape", "{}", 0, 0, true},
        {"a version field of the wrong type", "{\"version\":\"1\",\"monitors\":[]}", 0, 0, true},
        {"an unrecognised schema version", "{\"version\":2,\"monitors\":[]}", 0, 0, true},
        {"a monitors field that is an object", "{\"version\":1,\"monitors\":{\"0\":{}}}", 0, 0, true},
        {"monitor entries that are not objects", "{\"version\":1,\"monitors\":[null,3,\"x\",[],true]}", 0, 5, false},
        {"entry fields of the wrong type",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":\"0\",\"type\":5,\"path\":null}]}", 0, 1, false},
        // Separately, and not for symmetry: the three field reads are short-circuited, so
        // with only the row above a `type`/`path` that is the wrong type is never even
        // reached -- readMonitorId rejects the entry first. Measured: deleting the
        // is_string() guard from readString() left the row above green.
        {"a type field of the wrong type",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":5,\"path\":\"/a\"}]}", 0, 1, false},
        {"a null path field",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":null}]}", 0, 1, false},
        {"a fractional monitor_id", "{\"version\":1,\"monitors\":[{\"monitor_id\":1.5,\"type\":\"static\","
                                    "\"path\":\"/a\"}]}",
         0, 1, false},
        // S8.15. 2^32+1 narrows to exactly 1 under get<int>() -- inside the 0..31 range the
        // manager applies -- so a lost width check does not report anything: it puts a
        // wallpaper on somebody else's monitor.
        {"a monitor_id past int range",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":4294967297,\"type\":\"static\",\"path\":\"/a\"}]}", 0, 1,
         false},
        {"a negative monitor_id past int range",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":-4294967297,\"type\":\"static\",\"path\":\"/a\"}]}", 0, 1,
         false},
        // The same reader parses `version`, where narrowing is worse still: 2^32+1 becomes
        // 1, equals kSchemaVersion, and a file written by a schema this build predates is
        // read as if it were current.
        {"a version past int range", "{\"version\":4294967297,\"monitors\":[]}", 0, 0, true},
        // nlohmann's lexer treats a NUL as end of input, so `{...}\0junk` parsed clean and
        // the tail was dropped -- the file the daemon believed it read was not the file on
        // disk. Refused outright as of 2026-08-11, the decision this row was written to
        // force.
        // CONTROL: the `text.find('\0')` refusal in config_store.cpp's load() disabled ->
        //       "FAIL: a NUL byte is refused rather than silently ending the document: 1
        //       entries (expected 0), 0 malformed (expected 0), error ''", i.e. the row goes
        //       red naming the truncation, and it is the only failure in the run. Reverted.
        {"a NUL byte is refused rather than silently ending the document", nulAfterValid, 0, 0, true},
        {"a well-formed file",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a.png\"},"
         "{\"monitor_id\":3,\"type\":\"static\",\"path\":\"/b.png\"}]}",
         2, 0, false},
    };
    // Appended rather than braced in: an initializer_list's backing array is const, so the
    // vector copies every element and the 100 MB row would exist twice at once.
    cases.reserve(cases.size() + 2);
    cases.push_back({"100 MB of no JSON whatsoever", std::move(huge), 0, 0, true});
    // Parses fine and holds one element -- an array 100 000 deep -- which is one malformed
    // entry, not a broken file. The case is here for the parser's stack, not for its
    // verdict: a recursive-descent parser (or a recursive destructor) would take the process
    // down rather than return anything at all.
    cases.push_back({"100 000 levels of nesting", std::move(deep), 0, 1, false});

    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const ConfigStore store(file);
    int exercised = 0;
    int couldNotStage = 0;
    for (const LoadCase& c : cases) {
        if (!writeFile(file, c.content)) {
            // A host whose scratch filesystem cannot take the 100 MB case is not a defect in
            // the store. Counted, so the coverage assertion below can tell this apart from a
            // case that silently stopped running -- without the counter a small /tmp turns
            // its own skip into a FAIL against the daemon.
            ++couldNotStage;
            notVerified("one S8.2 load case", std::string("the case file could not be written: ") + c.what);
            continue;
        }
        const LoadResult result = store.load();
        const bool shaped = static_cast<int>(result.entries.size()) == c.expectedEntries &&
                            result.malformedEntries == c.expectedMalformed &&
                            result.error.empty() == !c.expectError && result.fileExisted;
        check(shaped, c.what);
        if (!shaped) {
            std::fprintf(stderr, "  %s: %zu entries (expected %d), %d malformed (expected %d), error '%s'\n", c.what,
                         result.entries.size(), c.expectedEntries, result.malformedEntries, c.expectedMalformed,
                         result.error.c_str());
        }
        ++exercised;
    }
    check(exercised + couldNotStage == static_cast<int>(cases.size()),
          "every load case was either exercised or reported as one this host could not stage");

    // The ordinary first-run case, and the only one where an absent file is not an error.
    std::error_code ec;
    std::filesystem::remove(file, ec);
    const LoadResult missing = store.load();
    check(missing.entries.empty() && missing.error.empty() && !missing.fileExisted,
          "no config file at all is the first-run case, not an error");
}

// ---------------------------------------------------------------------------- //
// Phase 3 -- S8.3: a persisted path comes back byte for byte.
// ---------------------------------------------------------------------------- //
void runRoundTripChecks() {
    const ScratchDir scratch("roundtrip");
    if (!scratch.ok()) {
        notVerified("S8.3 (a restored path is byte-identical)", "a scratch directory could not be created");
        return;
    }
    const ConfigStore store(scratch.path() / "wallpaperx" / "config.json");

    // GOAL: S8.3 -- what comes back out of the store is the bytes that went in, not the
    //       display-safe rendering of them. False if a path holding U+202E, a newline, DEL
    //       or an escape sequence returns sanitized: /config and GetStatus both publish
    //       sanitizeForLog(path), and persisting what those publish would restore a
    //       DIFFERENT file (PLAN.md Step 8 trap 4).
    // WHY-THIS-SHAPE: every case is asserted to be one that sanitizeForLog would ALTER,
    //       and that assertion is part of the check. A round-trip test over ordinary ASCII
    //       paths passes just as well against a store that sanitizes, so without this the
    //       phase would be green on the exact defect it was written for.
    // CONTROL: `{kKeyPath, entry.path}` in ConfigStore::save() replaced with
    //       `{kKeyPath, sanitizeForLog(entry.path, 4096, LogFraming::Bare)}` -> four FAIL
    //       lines, one per case, e.g. "a path containing U+202E (right-to-left override)"
    //       with "in:  2f 74 6d 70 2f e2 80 ae ..." against "out: 2f 74 6d 70 2f 5c 78 65
    //       32 5c 78 38 30 5c 78 61 65 ...", the three raw bytes having become nine escape
    //       characters. Reverted.
    // COVERAGE-ASSERT: the discriminating-case count is asserted equal to the case count,
    //       so a future edit that swaps in a tame path fails rather than going quiet.
    //       CONTROL: "/tmp/plain.png" added as a fifth case -> "FAIL: every round-trip case
    //       is one sanitizeForLog would alter, so a sanitizing store would fail this phase",
    //       while the four round-trip checks stayed green. Removed again.
    const std::vector<std::pair<const char*, std::string>> cases{
        {"a path containing U+202E (right-to-left override)", "/tmp/\xE2\x80\xAE" "gnp.png"},
        {"a path containing a newline", "/tmp/two\nlines.png"},
        {"a path containing DEL", "/tmp/del\x7F" "end.png"},
        {"a path containing an ANSI escape sequence", "/tmp/\x1B[31mred.png"},
    };

    int discriminating = 0;
    std::vector<PersistedWallpaper> entries;
    for (size_t i = 0; i < cases.size(); ++i) {
        // A case sanitizeForLog leaves alone could not tell a sanitizing store from a
        // faithful one, so the case itself is graded before it is used.
        if (sanitizeForLog(cases[i].second, 4096, LogFraming::Bare) != cases[i].second) ++discriminating;
        entries.push_back(PersistedWallpaper{static_cast<int>(i), "static", cases[i].second});
    }
    check(discriminating == static_cast<int>(cases.size()),
          "every round-trip case is one sanitizeForLog would alter, so a sanitizing store would fail this phase");

    const SaveResult saved = store.save(entries);
    check(saved.error.empty() && saved.skippedNonUtf8 == 0, "the adversarial paths are all saved (they are UTF-8)");
    const LoadResult loaded = store.load();
    check(loaded.entries.size() == cases.size(), "every saved entry comes back");

    // Matched by monitor id, not by position: the store promises which monitor a path
    // belongs to, not what order the array comes back in. Indexing would report a store that
    // reordered or de-duplicated entries as one that mangled bytes, under a heading naming a
    // byte sequence that in fact survived -- and a short result would silently run fewer
    // comparisons instead of failing.
    std::map<int, std::string> byId;
    for (const PersistedWallpaper& entry : loaded.entries) byId[entry.monitorId] = entry.path;
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto it = byId.find(static_cast<int>(i));
        const bool identical = it != byId.end() && it->second == cases[i].second;
        check(identical, cases[i].first);
        if (!identical) {
            std::fprintf(stderr, "  in:  %s\n  out: %s\n", hex(cases[i].second).c_str(),
                         it != byId.end() ? hex(it->second).c_str() : "(no entry for this monitor)");
        }
    }
}

// ---------------------------------------------------------------------------- //
// Phase 4 -- S8.7: an invalid-UTF-8 path costs only its own monitor.
// ---------------------------------------------------------------------------- //
void runNonUtf8Checks() {
    const ScratchDir scratch("utf8");
    if (!scratch.ok()) {
        notVerified("S8.7 (a non-UTF-8 path costs only its own entry)",
                    "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const ConfigStore store(file);

    // GOAL: S8.7 -- a path that is not valid UTF-8 costs that monitor's entry and nothing
    //       else: the write succeeds, the other three monitors are in the file, and the
    //       result says how many were skipped. False if one such path throws out of
    //       dump() and takes the whole save with it -- Linux paths are arbitrary bytes, so
    //       this is reachable from an ordinary unusual filename, no hand-editing needed.
    // WHY-THIS-SHAPE: four entries with the bad one in the MIDDLE, and the surviving ids
    //       are asserted individually. Two entries with the bad one last would pass for an
    //       implementation that stops at the first bad path, and a bare "the file exists"
    //       assertion would pass for one that wrote an empty monitors array.
    // CONTROL: the `if (!isValidUtf8(...)) { ++skipped; continue; }` guard deleted from
    //       ConfigStore::save() -> all four checks of this phase red, led by "a path that is
    //       not valid UTF-8 does not fail the write" with "save error: ...could not be
    //       saved: [json.exception.type_error.316] invalid UTF-8 byte at index 8: 0x80", and
    //       "the monitors with usable paths are all in the file..." reporting no file at
    //       all -- the whole write lost to one filename. Reverted.
    const std::string lone80 = std::string("/tmp/bad\x80name.png");
    const std::vector<PersistedWallpaper> entries{
        {0, "static", "/tmp/first.png"},
        {1, "static", lone80},
        {2, "static", "/tmp/third.png"},
        {3, "static", "/tmp/fourth.png"},
    };
    check(!wallpaperx::isValidUtf8(lone80), "the case path really is invalid UTF-8, so this phase can discriminate");

    const SaveResult saved = store.save(entries);
    check(saved.error.empty(), "a path that is not valid UTF-8 does not fail the write");
    if (!saved.error.empty()) std::fprintf(stderr, "  save error: %s\n", saved.error.c_str());
    check(saved.skippedNonUtf8 == 1, "the save reports exactly one entry skipped for its path");

    std::string why;
    const std::map<int, std::string> onDisk = pathsInFile(file, why);
    const bool kept = onDisk.size() == 3 && onDisk.count(0) == 1 && onDisk.count(2) == 1 && onDisk.count(3) == 1 &&
                      onDisk.count(1) == 0;
    check(kept, "the monitors with usable paths are all in the file, and only the bad one is missing");
    if (!kept) {
        std::fprintf(stderr, "  %s; ids on disk:", why.empty() ? "read back" : why.c_str());
        for (const auto& entry : onDisk) std::fprintf(stderr, " %d", entry.first);
        std::fprintf(stderr, "\n");
    }

    // The same rule for `type`. It cannot come from a client -- requestWallpaper rejects
    // anything outside three literals -- but the store is a public class whose save() must
    // not throw for any input, and dump() throws on either field.
    const SaveResult badType = store.save({{0, std::string("sta\xFFtic"), "/tmp/first.png"}});
    check(badType.error.empty() && badType.skippedNonUtf8 == 1,
          "a type that is not valid UTF-8 is skipped the same way rather than failing the write");

    // GOAL: a screen name does not ride on the validity of the picture's filename. A row whose
    //       path cannot be written keeps its `output`, so the screen is still named after a
    //       restart. False if the two halves share one gate: the user renames the image to fix
    //       the wallpaper and the screen is still anonymous, because what was lost was the name
    //       and nothing said so -- and renaming the image cannot put it back.
    // WHY-THIS-SHAPE: the row is asserted PRESENT with an empty path and its name intact, not
    //       merely "the file still parses". Both keys are asserted present-but-empty as well,
    //       because ConfigStore::load() reads a row missing `type` or `path` as malformed, so a
    //       writer that omitted them would turn this degraded write into a row the next start
    //       throws away -- the same loss one restart later, and invisible from the save alone.
    // CONTROL: `wallpaperOk ? entry.type : ""` and its `path` twin in ConfigStore::save()
    //       (backend/daemon/config_store.cpp) put back to the single combined gate
    //       `if (!isValidUtf8(path) || !isValidUtf8(type) || !isValidUtf8(output)) continue;`,
    //       which is the code before this fix -> "FAIL: a row whose path cannot be written keeps
    //       its screen name", printing "monitor 7 is not in the file at all", and "FAIL: the row
    //       that kept only its screen name is read back cleanly at the next start", printing "0
    //       entries". Two more go red in the connector-name phase from the same edit.
    const std::filesystem::path namedFile = scratch.path() / "named.json";
    const ConfigStore namedStore(namedFile);
    const SaveResult namedSave = namedStore.save({{7, "static", lone80, "DP-KEEP"}});
    check(namedSave.error.empty(), "a row with an unwritable path and a usable name does not fail the write");
    std::string namedWhy;
    const std::map<int, RowInFile> namedRows = rowsInFile(namedFile, namedWhy);
    const bool nameSurvived = namedRows.count(7) == 1 && namedRows.at(7).hasOutputKey &&
                              namedRows.at(7).output == "DP-KEEP" && namedRows.at(7).path.empty() &&
                              namedRows.at(7).type.empty();
    check(nameSurvived, "a row whose path cannot be written keeps its screen name");
    if (!nameSurvived) {
        std::fprintf(stderr, "  %s; %s\n", namedWhy.empty() ? "read back" : namedWhy.c_str(),
                     namedRows.count(7) == 0
                         ? "monitor 7 is not in the file at all"
                         : ("monitor 7 holds type '" + namedRows.at(7).type + "' path '" + namedRows.at(7).path +
                            "' output '" + namedRows.at(7).output + "'" +
                            (namedRows.at(7).hasOutputKey ? "" : " (no output key)"))
                               .c_str());
    }
    check(namedSave.skippedNonUtf8 == 1,
          "the wallpaper that could not be written is still counted when its row survives for the name");

    // COVERAGE-ASSERT: the degraded row is one a later start can still read. Without it the two
    // checks above are equally true of a writer that emitted a row load() throws away, which
    // loses the name one restart later instead of now.
    const LoadResult namedBack = namedStore.load();
    const bool readsBack = namedBack.error.empty() && namedBack.malformedEntries == 0 &&
                           namedBack.entries.size() == 1 && namedBack.entries.front().output == "DP-KEEP";
    check(readsBack, "the row that kept only its screen name is read back cleanly at the next start");
    if (!readsBack) {
        std::fprintf(stderr, "  load error '%s', %d malformed, %zu entries\n", namedBack.error.c_str(),
                     namedBack.malformedEntries, namedBack.entries.size());
    }

    // GOAL: the daemon's notice says what was lost and what survived -- N WALLPAPERS dropped,
    //       because a path or type is not valid UTF-8, and any screen name on those rows was
    //       kept. False if it counts "entries": the row is still in the file and the screen is
    //       still named, so telling the user an entry was skipped sends them to re-name a screen
    //       that has a name, and to look for a monitor that is not missing. Equally false if it
    //       blames the path alone -- the same counter is incremented for a bad type, asserted
    //       directly above.
    // WHY-THIS-SHAPE: driven through a real manager save rather than read off SaveResult,
    //       because the text lives in the health detail
    //       (WallpaperManager::applyPersistOutcomeLocked), not in the store. It is graded as
    //       wording, deliberately: the only cause a client can actually reach is the path, since
    //       requestWallpaper admits only three literal types. What it asserts is that the one
    //       reachable cause is not reported as the only possible one, and that the notice does
    //       not overstate the loss. The name clause is asserted here rather than only where the
    //       behaviour is: this is the text the user reads, and the two can drift apart.
    // CONTROL: the detail text in WallpaperManager::applyPersistOutcomeLocked
    //       (backend/daemon/wallpaper_manager.cpp) put back to " entries skipped: a path, type
    //       or screen name is not valid UTF-8)", which is the wording before this fix -> "FAIL:
    //       the daemon reports how many wallpapers its config writer had to drop", printing the
    //       detail, and "FAIL: the dropped-wallpaper notice names the type as a cause and says
    //       the screen name survived".
    // CONTROL, for searching only from the notice rather than across the whole detail: the same
    //       text with just the type dropped, run with TMPDIR=/tmp/prototype-build -> the same
    //       FAIL. Searching the whole detail instead -- the simpler form this rejects -- reported
    //       zero failures on that run: "prototype" in the config path satisfied the search for
    //       "type" on its own. Observed 2026-08-14 and still the reason for the substr below.
    const std::filesystem::path oddlyNamed = scratch.path() / std::string("bad\x80name.png");
    if (!writeWallpaperFile(oddlyNamed) || wallpaperx::isValidUtf8(oddlyNamed.string())) {
        notVerified("the wording of the daemon's skipped-entry notice",
                    "this host would not create a file whose name is not valid UTF-8, so no monitor can hold a path "
                    "the config writer has to skip");
        return;
    }
    WallpaperManager::Config managerConfig;
    managerConfig.configFile = scratch.path() / "manager" / "config.json";
    WallpaperManager manager(managerConfig);
    manager.requestWallpaper("static", oddlyNamed.string(), 0);
    const std::string detail = manager.snapshot().health.persistence.detail;
    const std::optional<int> skipped =
        clauseCountOrSkip(detail, " wallpapers dropped", "the wording of the daemon's dropped-wallpaper notice");
    if (!skipped.has_value()) return;
    const bool skipReported = *skipped == 1;
    check(skipReported, "the daemon reports how many wallpapers its config writer had to drop");
    // Searched from the notice rather than across the whole detail. The detail begins with the
    // config path, which is derived from this host's temporary directory, so a machine whose
    // $TMPDIR contains "type" satisfies the first term with the path alone -- and a notice that
    // had regressed to naming the path only would still read green.
    const std::size_t causesAt = detail.find("wallpapers dropped");
    const std::string causes = causesAt == std::string::npos ? std::string() : detail.substr(causesAt);
    const bool everyCauseNamed = causes.find("type") != std::string::npos &&
                                 causes.find("screen name") != std::string::npos &&
                                 causes.find("kept") != std::string::npos;
    check(everyCauseNamed,
          "the dropped-wallpaper notice names the type as a cause and says the screen name survived");
    if (!skipReported || !everyCauseNamed) std::fprintf(stderr, "  persistence detail: %s\n", detail.c_str());

    // GOAL: the promise the notice makes -- "any screen name on those rows was kept" -- is only
    //       made when the file really kept it. False if the name went out with the wallpaper: the
    //       user reads that their screen is still named, restarts, and finds it anonymous, which
    //       is worse than being told nothing because it stops them re-naming it.
    // WHY-THIS-SHAPE: the promise is graded against the FILE, not against the wording. Asserting
    //       the sentence is present -- the check directly above, and the simpler shape this one
    //       rejects -- is satisfied by any build that prints it, including one that dropped the
    //       row; only reading the row back can tell the two apart. The name is bound after the
    //       unwritable wallpaper is set, so the row's two halves fail and succeed in one save.
    // CONTROL: the row-dropping test in ConfigStore::save() (backend/daemon/config_store.cpp)
    //       collapsed to the one gate over both halves, `if (!wallpaperOk || !nameOk) continue;`,
    //       which is the code before this fix -> "FAIL: the screen name the notice promises was
    //       kept is in the file", printing "monitor 0 is not in the file at all" beside a detail
    //       still reading "(1 wallpapers dropped: ...; any screen name on those rows was kept)".
    //       Observed 2026-08-18, reverted. Two checks above it go red from the same edit.
    manager.setMonitorOutput(0, "DP-PROMISED");
    const std::string namedDetail = manager.snapshot().health.persistence.detail;
    std::string promisedWhy;
    const std::map<int, RowInFile> promisedRows = rowsInFile(managerConfig.configFile, promisedWhy);
    const bool promiseKept = promisedRows.count(0) == 1 && promisedRows.at(0).hasOutputKey &&
                             promisedRows.at(0).output == "DP-PROMISED" &&
                             namedDetail.find("any screen name on those rows was kept") != std::string::npos;
    check(promiseKept, "the screen name the notice promises was kept is in the file");
    if (!promiseKept) {
        const std::string what = promisedRows.count(0) == 0
                                     ? std::string("monitor 0 is not in the file at all")
                                     : "monitor 0 holds output '" + promisedRows.at(0).output + "'" +
                                           (promisedRows.at(0).hasOutputKey ? "" : " (no output key)");
        std::fprintf(stderr, "  %s; %s; persistence detail: %s\n",
                     promisedWhy.empty() ? "read back" : promisedWhy.c_str(), what.c_str(), namedDetail.c_str());
    }
    // COVERAGE-ASSERT: the save this promise is about really did drop a wallpaper. Without it the
    // check is equally true of a run where nothing was skipped and the promise was never made --
    // the clause would be absent, the name present, and the check would pass having graded
    // nothing.
    const std::optional<int> stillSkipped =
        clauseCountOrSkip(namedDetail, " wallpapers dropped", "the wording of the daemon's dropped-wallpaper notice");
    if (!stillSkipped.has_value()) return;
    check(*stillSkipped == 1, "the save that carried the kept name is still one that dropped a wallpaper");
    if (*stillSkipped != 1) std::fprintf(stderr, "  persistence detail: %s\n", namedDetail.c_str());
}

// ---------------------------------------------------------------------------- //
// Phase 5 -- S8.8: a malformed entry does not cost the well-formed ones.
// ---------------------------------------------------------------------------- //
void runMalformedEntryChecks() {
    const ScratchDir scratch("malformed");
    if (!scratch.ok()) {
        notVerified("S8.8 (a malformed entry does not cost the well-formed ones)",
                    "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const ConfigStore store(file);

    // GOAL: S8.8 -- a file whose second of four entries is broken restores the other
    //       three, and says how many it dropped. False if one bad entry empties the
    //       result: the user loses three working monitors to one line somebody hand-edited.
    // WHY-THIS-SHAPE: two shapes of broken entry (not an object; an object missing `path`)
    //       and the SURVIVING ids are asserted, not just the count. A count-only assertion
    //       passes for a loader that dropped entry 2 and kept a duplicate of entry 1, and
    //       the id is what decides which monitor a wallpaper lands on.
    // CONTROL: the `continue` in load()'s element loop replaced by `result.entries.clear();
    //       return result;` -- the "one bad line costs everything" implementation -> "FAIL:
    //       a monitor entry that is not an object costs only itself" and "FAIL: a monitor
    //       entry with no path costs only itself", both printing "0 entries (1 malformed),
    //       ids:", plus the corpus phase's "monitor entries that are not objects" row, which
    //       counted 1 malformed where it expects 5. Reverted.
    const std::vector<std::pair<const char*, std::string>> cases{
        {"a monitor entry that is not an object costs only itself",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a\"},42,"
         "{\"monitor_id\":2,\"type\":\"static\",\"path\":\"/c\"},"
         "{\"monitor_id\":3,\"type\":\"static\",\"path\":\"/d\"}]}"},
        {"a monitor entry with no path costs only itself",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a\"},"
         "{\"monitor_id\":1,\"type\":\"static\"},"
         "{\"monitor_id\":2,\"type\":\"static\",\"path\":\"/c\"},"
         "{\"monitor_id\":3,\"type\":\"static\",\"path\":\"/d\"}]}"},
    };

    for (const auto& [what, content] : cases) {
        if (!writeFile(file, content)) {
            notVerified("one S8.8 case", std::string("the case file could not be written: ") + what);
            continue;
        }
        const LoadResult loaded = store.load();
        std::vector<int> ids;
        for (const PersistedWallpaper& entry : loaded.entries) ids.push_back(entry.monitorId);
        const bool kept = ids == std::vector<int>{0, 2, 3} && loaded.malformedEntries == 1 && loaded.error.empty();
        check(kept, what);
        if (!kept) {
            std::fprintf(stderr, "  %zu entries (%d malformed), ids:", loaded.entries.size(),
                         loaded.malformedEntries);
            for (const int id : ids) std::fprintf(stderr, " %d", id);
            std::fprintf(stderr, "; error '%s'\n", loaded.error.c_str());
        }
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6 -- S8.5 and S8.4: a restore with nothing to repair reads and does not write.
// ---------------------------------------------------------------------------- //
void runRestoreChecks() {
    const ScratchDir scratch("restore");
    if (!scratch.ok()) {
        notVerified("S8.5 (a restore with nothing to repair does not write) and S8.4 (an unreadable entry is not "
                    "forgotten)",
                    "a scratch directory could not be created");
        return;
    }
    // Its own state directory, like every phase whose start takes a copy aside: the copy names
    // are numbered and run out after ten, so phases sharing one directory would silently stop
    // being able to write their repairs partway through a run.
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path present = scratch.path() / "present.png";
    const std::filesystem::path other = scratch.path() / "other.png";
    const std::string missing = "/nonexistent/wallpaperx-was-here.png";
    if (!writeWallpaperFile(present) || !writeWallpaperFile(other)) {
        notVerified("S8.5 and S8.4", "the wallpaper files this phase drives could not be written");
        return;
    }

    // Written by hand, in a formatting ConfigStore::save() would never produce (compact,
    // and with the keys in a different order). That is what makes the byte comparison
    // below able to see a rewrite that happens to preserve the same entries. The two paths
    // are serialised rather than spliced in: $TMPDIR may hold a quote or a backslash, and a
    // fixture that is not valid JSON would report a correct restore as a broken one.
    if (!wallpaperx::isValidUtf8(present.string())) {
        notVerified("S8.5 and S8.4", "this host's temporary directory has a path that is not valid UTF-8, so no "
                                     "JSON fixture can name it");
        return;
    }
    const std::string original = "{\"monitors\":[{\"path\":" + nlohmann::json(missing).dump() +
                                 ",\"type\":\"static\",\"monitor_id\":0},{\"path\":" +
                                 nlohmann::json(present.string()).dump() +
                                 ",\"type\":\"static\",\"monitor_id\":1}],\"version\":1}";
    if (!writeFile(file, original)) {
        notVerified("S8.5 and S8.4", "the config file this phase reads could not be written");
        return;
    }
    std::error_code beforeEc;
    const auto mtimeBefore = std::filesystem::last_write_time(file, beforeEc);

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);

    // GOAL: S8.5 -- a start that restores entries needing no repair does not write the config
    //       file. False if its contents or its mtime move. Not hypothetical: restore records
    //       the same per-monitor state requestWallpaper() records, and requestWallpaper() is
    //       where the save hook is attached, so one line in the wrong place saves on startup --
    //       and S8.4 (the unreadable entry surviving) depends on this not happening. The other
    //       half of the rule is the opposite property and is graded in the startup-repair
    //       phase: a restore that DOES repair something writes the repair down at that start.
    // WHY-THIS-SHAPE: bytes AND mtime, against a file written in a formatting save() does
    //       not produce. Comparing only the mtime would miss a rewrite inside the
    //       filesystem's timestamp granularity; comparing only the parsed entries would
    //       miss a rewrite that preserved them, which is precisely what an accidental save
    //       on restore would do -- it would look identical in every way except that the
    //       file had been replaced.
    // CONTROL: the `if (repaired)` guard on `m_repair = persistableLocked();` at the end of
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp) deleted, so
    //       every start hands writeRepairedConfig() a snapshot -> "FAIL: restoring entries that
    //       need no repair does not rewrite the config file", and that alone in the whole
    //       binary. The mtime half stayed green under
    //       it, because this host's scratch filesystem stamps both writes with the same coarse
    //       time -- which is the reason both comparisons are here rather than either alone. The
    //       mtime half is what carries the equivalent check in daemon_system_test, where the
    //       file was written by the daemon's own save() and a second save reproduces it byte
    //       for byte. Observed 2026-08-15. Reverted.
    // COVERAGE-ASSERT: the byte comparison is only evidence of this rule while the fixture
    //       really needs no repair, and a repair-free fixture is not self-evident from two
    //       rows of JSON. The repair clauses are asserted absent below, so a later fixture edge
    //       -- a negative id, a duplicate name, a name this build refuses -- fails naming the
    //       fixture rather than accusing the daemon of overwriting the user's config. Observed
    //       by adding a row with monitor id -3 to the fixture: "FAIL: this phase's fixture is
    //       one the restore has nothing to repair in", printing "repaired: 'entries dropped, no
    //       build can apply'". Reverted.
    check(readFile(file) == original, "constructing a manager reads the config file without rewriting it");

    const std::vector<int> restored = manager.restoreSaved();
    check(restored == std::vector<int>({0, 1}), "restoreSaved returns the monitors to post, in file order");
    // The rest of the startup sequence backend/daemon/main.cpp performs, in its order.
    // restoreSaved() alone cannot write at all since the repair moved to writeRepairedConfig(),
    // so grading the file after it by itself would assert a rule nothing could break.
    manager.preserveUnreadableConfig();
    manager.writeRepairedConfig();

    const std::string restoreDetail = manager.snapshot().health.persistence.detail;
    const std::string repairsFound = repairClausesIn(restoreDetail);
    const bool fixtureNeedsNoRepair = repairsFound.empty();
    check(fixtureNeedsNoRepair, "this phase's fixture is one the restore has nothing to repair in");
    if (!fixtureNeedsNoRepair) std::fprintf(stderr, "  repaired:%s; detail: %s\n", repairsFound.c_str(),
                                            restoreDetail.c_str());
    check(readFile(file) == original, "restoring entries that need no repair does not rewrite the config file");

    // An error code per sample, and a failed read is a skip rather than a verdict. Sharing
    // one code let the second call's success erase the first call's failure, so a stat this
    // harness could not perform was reported as the daemon having rewritten the user's
    // config -- an accusation of data loss with nothing behind it. The byte comparison above
    // is unaffected and still runs either way.
    std::error_code afterEc;
    const auto mtimeAfter = std::filesystem::last_write_time(file, afterEc);
    if (beforeEc || afterEc) {
        notVerified("S8.5's mtime clause", "the config file's modification time could not be read on this host");
    } else {
        check(mtimeAfter == mtimeBefore,
              "restoring entries that need no repair does not touch the config file's mtime");
    }

    // GOAL: S8.4 -- the monitor whose file is unreadable keeps its entry, across the
    //       restore itself and across a later save made for a different monitor. False if
    //       remounting the drive and restarting no longer brings that wallpaper back
    //       (PLAN.md decision 3: the case is a disk that mounts after the daemon starts).
    // WHY-THIS-SHAPE: the surviving entry is checked after a save triggered by ANOTHER
    //       monitor, because that is the write that would drop it. Checking only that
    //       restoreSaved() returned its id would pass for a manager that then forgot it at
    //       the first SetWallpaper -- which is the state the user actually reaches, since
    //       something always sets a wallpaper eventually.
    // CONTROL: a `std::filesystem::is_regular_file(entry.path)` guard added to
    //       restoreSaved()'s loop, i.e. decision 3 reversed -> "FAIL: a monitor whose file
    //       was unreadable at restore is still in the file after another monitor is set"
    //       (ids on disk: 1 2), plus "restoreSaved returns the monitors to post, in file
    //       order" and "the restored monitor is in the manager's state with its path
    //       unchanged". Reverted.
    manager.requestWallpaper("static", other.string(), 2);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(2) == 1; });
    std::string why;
    const std::map<int, std::string> onDisk = pathsInFile(file, why);
    const bool kept = onDisk.count(0) == 1 && onDisk.at(0) == missing && onDisk.count(1) == 1 &&
                      onDisk.count(2) == 1;
    check(kept, "a monitor whose file was unreadable at restore is still in the file after another monitor is set");
    if (!kept) {
        std::fprintf(stderr, "  %s; ids on disk:", why.empty() ? "read back" : why.c_str());
        for (const auto& entry : onDisk) std::fprintf(stderr, " %d", entry.first);
        std::fprintf(stderr, "\n");
    }

    // The restored entry is the manager's state, not just a line in a file: a client asking
    // GetStatus sees the monitor rather than nothing at all.
    const WallpaperManager::Snapshot snap = manager.snapshot();
    check(snap.monitors.count(0) == 1 && snap.monitors.at(0).path == missing,
          "the restored monitor is in the manager's state with its path unchanged");
}

// ---------------------------------------------------------------------------- //
// Phase 6b -- S8.13: a config this build could not read is kept, not overwritten.
// ---------------------------------------------------------------------------- //
void runUnreadableConfigChecks() {
    constexpr const char* kGoal = "S8.13 (a config that could not be read is preserved byte-identical)";
    const ScratchDir scratch("keptbad");
    // Through preparePhase because the last check here reads back a path the daemon persisted:
    // on a host whose temporary directory is not valid UTF-8 the config writer is required to
    // skip that entry (S8.7), so the row would be legitimately absent and the check would
    // report the daemon as having lost a wallpaper.
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoal, 1);
    if (!paths.has_value()) return;
    const std::filesystem::path file = paths->config;
    // The copy is kept in $XDG_STATE_HOME/wallpaperx, out of the directory the user hand-edits,
    // so the env var is what decides where this phase looks for it.
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path kept = preservedCopyDir(scratch) / "config.json.bad";
    const std::filesystem::path wallpaper = paths->wallpapers.front();
    // A hand edit that broke the syntax: one trailing comma in an otherwise complete file
    // holding two wallpapers the user picked. What makes this the case worth pinning is that
    // the bytes are still valuable -- the paths are readable in it -- so losing them is data
    // loss and not merely a reset.
    const std::string handEdited =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a.png\"},]}";
    if (!writeFile(file, handEdited)) {
        notVerified(kGoal, "the damaged config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    // main.cpp's order: construct, restore, and only then preserve. The constructor is
    // read-only and restoreSaved() only computes a repair, so preserveUnreadableConfig() is the
    // one call here that can touch the config directory -- and this fixture's config could not
    // be read at all, which is what makes writeRepairedConfig() (not called, main.cpp's next
    // step) unable to write over it. Nothing in this file can establish that this ordering is
    // what the daemon performs, because there is no bus here; daemon_system_test's
    // preservation-ownership phase grades it for an unreadable config.
    manager.restoreSaved();
    manager.preserveUnreadableConfig();

    // GOAL: S8.13 -- a config file this build could not parse survives the run that could
    //       not parse it. False if the next SetWallpaper overwrites it: the file is the only
    //       copy, so a user who broke the syntax by hand would have their wallpaper choices
    //       destroyed by the daemon at the moment they set the next one.
    // WHY-THIS-SHAPE: the preserved copy is compared BYTE for byte against what was written,
    //       and the comparison is made after a later save, not straight after startup. A
    //       check that only asserted config.json.bad exists would pass for a build that
    //       wrote a summary, a truncation, or the sanitized form into it -- none of which
    //       gets the user's paths back -- and a check made before the save would pass for a
    //       build that preserved the file and then clobbered the copy on the first write.
    //       The health status is asserted alongside because a preserved file nobody is told
    //       about reads to the user as a config that silently emptied itself.
    // CONTROL: the `std::filesystem::rename(source, kept, ec)` call deleted from putBytes()
    //       (backend/daemon/wallpaper_manager.cpp), so the placement reports success without
    //       writing anything -> "FAIL: a config that could not be read is kept as
    //       config.json.bad" and "FAIL: the preserved copy is byte-identical to the file the
    //       daemon could not read", printing "no .../config.json.bad exists; copies anywhere:
    //       (none)". The rename and not the copy_file below it: this path moves the file, and
    //       here the two directories are on one filesystem, so the rename succeeds and deleting
    //       copy_file instead leaves both checks GREEN -- measured. Observed 2026-08-18. Reverted.
    // CONTROL, for the copy being kept OUT of the config directory: the preferred copy directory
    //       in preserveConfig() (backend/daemon/config_preserve.cpp) put back to the config
    //       file's own parent -> "FAIL: the config the daemon could not read is
    //       kept out of the directory the user edits", printing that copy's path in the config
    //       directory, and this phase's other two copy checks red with it -- they look for the
    //       copy in the state directory, where nothing now lands. Observed 2026-08-18. Reverted.
    const WallpaperManager::Snapshot afterLoad = manager.snapshot();
    check(afterLoad.health.persistence.status == wallpaperx::HealthStatus::Fail,
          "a config that could not be read leaves the persistence check reporting failure");
    check(afterLoad.health.persistence.detail.find(".bad") != std::string::npos,
          "the persistence failure says where the unreadable config was kept");
    if (afterLoad.health.persistence.detail.find(".bad") == std::string::npos) {
        std::fprintf(stderr, "  persistence detail: %s\n", afterLoad.health.persistence.detail.c_str());
    }

    manager.requestWallpaper("static", wallpaper.string(), 1);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(1) == 1; });

    const bool keptExists = std::filesystem::exists(kept);
    check(keptExists, "a config that could not be read is kept as config.json.bad");
    check(keptExists && readFile(kept) == handEdited,
          "the preserved copy is byte-identical to the file the daemon could not read");
    if (!keptExists) {
        std::fprintf(stderr, "  no %s exists; copies anywhere: %s\n", kept.string().c_str(),
                     joined(preservedCopyNames(file, preservedCopyDir(scratch))).c_str());
    } else if (readFile(kept) != handEdited) {
        std::fprintf(stderr, "  kept: %s\n  written: %s\n", readFile(kept).c_str(), handEdited.c_str());
    }

    // GOAL: the copy is kept OUT of the directory the config file lives in, so a user tidying
    //       their config directory by hand cannot take the copy with the file it was made for.
    //       False if it sits beside config.json, which is where it used to go. This is
    //       separation from the directory the user edits, not protection from the user: they
    //       own the state directory too.
    // WHY-THIS-SHAPE: asserted as an absence beside config.json AND a presence in the state
    //       directory (the check above). Either alone is satisfied by the wrong build -- an
    //       absence alone by a daemon that preserves nothing at all, a presence alone by one
    //       that writes both.
    // CONTROL: the preferred copy directory in preserveConfig()
    //       (backend/daemon/config_preserve.cpp) put back to the config file's own parent ->
    //       "FAIL: the config the daemon could not read is kept out of the directory the user
    //       edits", printing "copies in the config directory: .../wallpaperx/config.json.bad".
    //       Observed 2026-08-18. Reverted.
    const std::vector<std::string> besideConfig = preservedCopyNames(file, {});
    check(besideConfig.empty(), "the config the daemon could not read is kept out of the directory the user edits");
    if (!besideConfig.empty()) {
        std::fprintf(stderr, "  copies in the config directory: %s\n", joined(besideConfig).c_str());
    }

    // The daemon has to keep working afterwards: preserving the old file must not mean
    // refusing to persist anything new.
    std::string why;
    const std::map<int, std::string> onDisk = pathsInFile(file, why);
    const bool wroteNew = onDisk.size() == 1 && onDisk.count(1) == 1 && onDisk.at(1) == wallpaper.string();
    check(wroteNew, "a wallpaper set after the damaged config is persisted into a fresh config.json");
    if (!wroteNew) std::fprintf(stderr, "  %s\n", why.empty() ? "the file holds something else" : why.c_str());
}

// ---------------------------------------------------------------------------- //
// Phase 6bb -- S8.18 and S8.22: when the copy must NOT be made, and what must survive.
// ---------------------------------------------------------------------------- //
void runPreservedCopyChecks() {
    constexpr const char* kGoal = "S8.18 (the preserved copy is made only for an ordinary file that was read and "
                                  "rejected, and never over an existing copy) and S8.22 (a config that could not be "
                                  "opened is neither moved aside nor overwritten)";

    // A config.json the daemon could not read at all -- as opposed to one it read and
    // rejected. `rename()` would succeed on every one of these, which is what makes the
    // checks below able to fail.
    struct UnreadCase {
        const char* what;
        bool (*plant)(const std::filesystem::path& file);
        // What the file must still be after the daemon has run and saved. Two of these are
        // not files whose bytes exist, so this is a predicate rather than a comparison.
        bool (*intact)(const std::filesystem::path& file);
        const char* detailFragment;
        // Why this host might refuse to stage it, so a skip names a cause instead of
        // leaving a reader to guess which of the three ways it could fail happened.
        const char* stageHint;
    };
    const UnreadCase cases[] = {
        // load() opens the path and inspects the descriptor, so a symlink loop is refused by
        // open() with ELOOP rather than by a stat beforehand. It is here because it is the
        // one "cannot be read at all" answer an unprivileged test can produce on demand.
        {"a self-referential symlink, which open() answers with ELOOP",
         [](const std::filesystem::path& file) {
             std::error_code ec;
             std::filesystem::create_symlink(file, file, ec);
             return !ec;
         },
         [](const std::filesystem::path& file) {
             std::error_code ec;
             return std::filesystem::is_symlink(std::filesystem::symlink_status(file, ec)) &&
                    std::filesystem::read_symlink(file, ec) == file;
         },
         "could not be opened", "this filesystem may not support symbolic links"},
        {"a named pipe", [](const std::filesystem::path& file) { return ::mkfifo(file.c_str(), 0644) == 0; },
         [](const std::filesystem::path& file) {
             std::error_code ec;
             return std::filesystem::is_fifo(std::filesystem::symlink_status(file, ec));
         },
         "is not a regular file", "this filesystem may not support FIFOs"},
        // S8.22. An ordinary file holding the user's real wallpapers that this process is
        // not allowed to open -- the one case of the three where bytes with value in them
        // are what stands to be lost.
        {"an ordinary file that cannot be opened",
         [](const std::filesystem::path& file) {
             if (!writeFile(file, kUnopenableConfig)) return false;
             if (::chmod(file.c_str(), 0) != 0) return false;
             // Root ignores the mode bits, and so do some filesystems. Staging has not
             // happened unless the file really is unopenable, or this case would grade a
             // daemon that read the config perfectly well.
             const int fd = ::open(file.c_str(), O_RDONLY);
             if (fd >= 0) {
                 ::close(fd);
                 return false;
             }
             return true;
         },
         [](const std::filesystem::path& file) {
             // Readable again first: this process cannot compare bytes it is not allowed to
             // read either, and the mode is no longer load-bearing once the daemon is done.
             if (::chmod(file.c_str(), 0600) != 0) return false;
             return readFile(file) == kUnopenableConfig;
         },
         "could not be opened",
         "this process can open a file it made mode 000 (running as root, or a filesystem that ignores mode bits)"},
    };

    // GOAL: S8.18's first clause with S8.22 -- a config the daemon could not READ is left
    //       exactly where it is: no copy under any .bad name, and the file itself neither
    //       moved nor rewritten by the SetWallpaper that follows. False in two directions,
    //       and both lose the user's wallpapers: a rename decided by an error nobody
    //       diagnosed moves their only config out from under them, and a save that goes
    //       ahead anyway replaces every monitor in it with the one just set.
    // WHY-THIS-SHAPE: the fixtures are things `rename()` and `open(O_CREAT|O_TRUNC)` both
    //       SUCCEED on -- a symlink is renamed as a symlink, a FIFO as a FIFO, and the
    //       mode-000 file sits in a directory this process owns and may write to. The
    //       obvious cheaper fixture, a config inside a directory chmod'd to 000, fails to
    //       open for a reason that ALSO makes both of those fail, so a check on it would be
    //       green against a build that tried to destroy the file and merely could not. Each
    //       case drives preserveUnreadableConfig() -- the daemon's only writer of that
    //       directory -- and both observations are made AFTER a save, because preservation is
    //       not the only writer and the save is the write that had the defect.
    // CONTROL: three, all reverted. (a) `result.preservable = true;` hoisted in
    //       config_store.cpp's load() to just above the open-failure return -> the symlink
    //       and mode-000 cases each red on all three of their checks, led by "FAIL: a
    //       config.json that is <case> is left where it is, under its own name" printing
    //       "  preserved copies: <state dir>/config.json.bad". (b) the same, hoisted above the
    //       S_ISREG return -> the same three for the named-pipe case. (c) the `if (m_refusing)`
    //       early return deleted from ConfigWriter::queue() (backend/daemon/config_writer.cpp),
    //       re-observed 2026-08-18 -> all three cases red on "still holds the bytes it held
    //       before the daemon ran" and on "the daemon says it refused to save over it", the
    //       latter reading "ok / not saving over it", with the mode-000 file rewritten to hold
    //       monitor 1 alone -- the two paths the goal names, measured separately. Twelve red in
    //       all; the other six grade the same refusal from the copy and retry phases.
    // COVERAGE-ASSERT: every case is accounted for -- exercised, or reported as one this
    //       host could not stage -- against the table's size, so a case that quietly stops
    //       being planted shrinks the phase loudly rather than silently.
    //       CONTROL: a `continue` for the named-pipe case added at the top of the loop, which
    //       is what a "skip the awkward one" edit looks like -> "FAIL: every unreadable-config
    //       case was either exercised or reported as one this host could not stage", and
    //       nothing else. Reverted.
    int exercised = 0;
    int unstageable = 0;
    for (const UnreadCase& c : cases) {
        const ScratchDir scratch("nopreserve");
        if (!scratch.ok()) {
            ++unstageable;
            notVerified(kGoal, std::string("a scratch directory for the '") + c.what + "' case could not be created");
            continue;
        }
        // Preserved copies are kept under $XDG_STATE_HOME, so "no copy was made" is only an
        // observation about this daemon if that variable names a directory this phase owns.
        const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
        const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
        const std::filesystem::path wallpaper = scratch.path() / "wallpaper.png";
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec || !writeWallpaperFile(wallpaper) || !c.plant(file)) {
            ++unstageable;
            notVerified(kGoal, std::string("the '") + c.what + "' case could not be staged: " + c.stageHint);
            continue;
        }
        ++exercised;

        WallpaperManager::Config config;
        config.configFile = file;
        WallpaperManager manager(config);
        // Called, not skipped, and that is the whole phase: the constructor writes nothing,
        // so a version of this loop that left it out would assert "no copy was made" about a
        // manager that was never asked to make one -- green against any build at all.
        manager.restoreSaved();
        manager.preserveUnreadableConfig();
        const WallpaperManager::Snapshot snap = manager.snapshot();

        // The refusal itself is graded, so "no copy was made" cannot be satisfied by a
        // build that read the file happily and had nothing to preserve.
        const bool refused = snap.health.persistence.status == wallpaperx::HealthStatus::Fail &&
                             snap.health.persistence.detail.find(c.detailFragment) != std::string::npos;
        check(refused, (std::string("a config.json that is ") + c.what + " is refused, and the detail says why").c_str());
        if (!refused) {
            std::fprintf(stderr, "  persistence: %s / %s\n",
                         snap.health.persistence.status == wallpaperx::HealthStatus::Fail ? "fail" : "not fail",
                         snap.health.persistence.detail.c_str());
        }

        manager.requestWallpaper("static", wallpaper.string(), 1);

        // Asked after the save, and asked of the health field the save writes: the
        // constructor's own note survives into it, so a build that reported the refusal at
        // startup and then quietly overwrote the file would still have to say so here.
        const WallpaperManager::Snapshot afterSave = manager.snapshot();
        const bool saidNotSaved =
            afterSave.health.persistence.status == wallpaperx::HealthStatus::Fail &&
            afterSave.health.persistence.detail.find("will not overwrite it") != std::string::npos;
        check(saidNotSaved, (std::string("with ") + c.what +
                             " at config.json, the daemon says it refused to save over it")
                                .c_str());
        if (!saidNotSaved) {
            std::fprintf(stderr, "  persistence after the save: %s / %s\n",
                         wallpaperx::toString(afterSave.health.persistence.status),
                         afterSave.health.persistence.detail.c_str());
        }

        const std::vector<std::string> copies = preservedCopyNames(file, preservedCopyDir(scratch));
        check(copies.empty(),
              (std::string("a config.json that is ") + c.what + " is left where it is, under its own name").c_str());
        if (!copies.empty()) {
            std::fprintf(stderr, "  preserved copies: %s\n", joined(copies).c_str());
        }
        const bool unchanged = c.intact(file);
        check(unchanged, (std::string("a config.json that is ") + c.what +
                          " still holds the bytes it held before the daemon ran")
                             .c_str());
        if (!unchanged) std::fprintf(stderr, "  the file the daemon could not read is not what was planted\n");
    }
    check(exercised + unstageable == static_cast<int>(std::size(cases)),
          "every unreadable-config case was either exercised or reported as one this host could not stage");

    // GOAL: S8.18's second clause -- a second bad start never writes over the copy the first
    //       one made, and the second broken config is preserved too, under a name of its
    //       own. False in two ways, both of which lose data: the first copy overwritten
    //       leaves the user's real config gone from both names, and a second config with
    //       nowhere to go is left in place with saving disabled for the run, so the daemon
    //       stops persisting anything until somebody deletes a file by hand.
    // WHY-THIS-SHAPE: the fixture is the SECOND start -- config.json.bad already holds the
    //       user's real wallpapers and config.json holds a later, different broken file.
    //       Asserting that a copy appears on a first start is what Phase 6b already does,
    //       and it passes against a build that overwrites; only the second pass through the
    //       same code can lose anything. Both files are compared byte for byte rather than
    //       merely counted, because an overwrite and a correct second copy leave the same
    //       NUMBER of files behind and differ only in which bytes are in which name.
    // CONTROL: two, both reverted, both observed 2026-08-18. (a) the
    //       `(slot == 0 ? "" : "." + std::to_string(slot))` suffix dropped from preservedName()
    //       (backend/daemon/config_preserve.cpp), so every copy competes for one name ->
    //       "FAIL: the second unreadable config is preserved under a name of its own", listing
    //       config.json.bad in the state directory AND one in the config directory, and "FAIL:
    //       the daemon says where the second unreadable config was kept", reading "kept as
    //       .../wallpaperx/config.json.bad". Saving survives it: the second file falls back to
    //       the config directory rather than having nowhere to go, so the overwrite check and
    //       the save-afterwards check both stay green -- which is why the name a copy is
    //       reported under has to be graded, and not just whether the run went on saving and
    //       not just whether the first copy survived. (b) inspectName() made to answer
    //       NameState::Free for a name that is already taken, so the placement is unconditional
    //       -> "FAIL: a second unreadable config does not overwrite the copy the first one
    //       preserved", printing the throwaway's bytes where the user's config had been, beside
    //       the name-of-its-own check.
    const ScratchDir scratch("twicebad");
    // preparePhase, not a bare scratch.ok(): the last check here reads back a path the daemon
    // persisted, and a temporary directory whose name is not valid UTF-8 makes the writer skip
    // that entry by design (S8.7).
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoal, 1);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path file = paths->config;
    const std::filesystem::path kept = preservedCopyDir(scratch) / "config.json.bad";
    const std::filesystem::path keptSecond = preservedCopyDir(scratch) / "config.json.bad.1";
    const std::filesystem::path wallpaper = paths->wallpapers.front();
    const std::string usersOwn =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/kept.png\"},]}";
    const std::string throwaway = "{\"version\":1,\"monitors\":[{,";
    if (!writeFile(kept, usersOwn) || !writeFile(file, throwaway)) {
        notVerified(kGoal, "the two config files the second-bad-start case needs could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();
    manager.preserveUnreadableConfig();
    const WallpaperManager::Snapshot snap = manager.snapshot();

    const std::string keptNow = readFile(kept);
    check(keptNow == usersOwn, "a second unreadable config does not overwrite the copy the first one preserved");
    if (keptNow != usersOwn) {
        std::fprintf(stderr, "  config.json.bad now holds: %s\n  it held: %s\n", keptNow.c_str(), usersOwn.c_str());
    }
    const bool secondKept = readFile(keptSecond) == throwaway;
    check(secondKept, "the second unreadable config is preserved under a name of its own");
    if (!secondKept) {
        std::fprintf(stderr, "  copies: %s\n", joined(preservedCopyNames(file, preservedCopyDir(scratch))).c_str());
    }
    // Silence here is its own defect: the file the user is looking at is not the one on
    // disk any more, so the name it went to is the only way back to it.
    const bool said = snap.health.persistence.detail.find(keptSecond.filename().string()) != std::string::npos;
    check(said, "the daemon says where the second unreadable config was kept");
    if (!said) std::fprintf(stderr, "  persistence detail: %s\n", snap.health.persistence.detail.c_str());

    // Preserving it is what clears the path, so persistence has to work again afterwards --
    // a build that moved the file aside and then still refused to save would leave the user
    // with no config at all and no way to make one.
    manager.requestWallpaper("static", wallpaper.string(), 1);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(1) == 1; });
    std::string why;
    const std::map<int, std::string> onDisk = pathsInFile(file, why);
    const bool wroteNew = onDisk.size() == 1 && onDisk.count(1) == 1 && onDisk.at(1) == wallpaper.string();
    check(wroteNew, "a wallpaper set after the second unreadable config is persisted into a fresh config.json");
    if (!wroteNew) std::fprintf(stderr, "  %s\n", why.empty() ? "the file holds something else" : why.c_str());
}

// ---------------------------------------------------------------------------- //
// Phase 6bc -- S8.22: with every copy name taken, the config is kept, not destroyed.
// ---------------------------------------------------------------------------- //
void runPreservationExhaustedChecks() {
    constexpr const char* kGoal = "S8.22 (a config that cannot be preserved is left in place, and is not saved over)";
    const ScratchDir scratch("nofreename");
    if (!scratch.ok()) {
        notVerified(kGoal, "a scratch directory could not be created");
        return;
    }
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path wallpaper = scratch.path() / "wallpaper.png";
    // One more than the daemon's bound would leave a free name, one fewer would not test
    // it: this is exactly the count at which the search runs out.
    constexpr int kCopies = 10;
    // Both directories preservation will try, not only the state one. The copy falls back to
    // the config file's own directory when the state directory yields nothing, so ten occupied
    // names in the state directory alone leaves an eleventh free name beside the config and the
    // "nowhere to put it" case is never staged at all.
    const std::filesystem::path dirs[] = {preservedCopyDir(scratch), file.parent_path()};
    constexpr int kOccupied = kCopies * static_cast<int>(std::size(dirs));
    const std::string usersOwn =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/kept.png\"},"
        "{\"monitor_id\":1,\"type\":\"static\",\"path\":\"/photos/right.png\"},]}";
    bool staged = writeWallpaperFile(wallpaper) && writeFile(file, usersOwn);
    for (const std::filesystem::path& dir : dirs) {
        for (int n = 0; n < kCopies && staged; ++n) {
            // Bytes of its own per name: an occupant holding the same bytes as config.json is
            // recognised as a copy already taken and reused, which is a free name by another
            // route and would unstage this case silently.
            staged = writeFile(dir / ("config.json.bad" + (n == 0 ? std::string() : "." + std::to_string(n))),
                               "an earlier broken config in " + dir.string() + " " + std::to_string(n));
        }
    }
    if (!staged) {
        notVerified(kGoal, "the twenty occupied copy names this case needs could not be written");
        return;
    }

    // GOAL: S8.22 -- when there is nowhere left to preserve it to, an unreadable config is
    //       LEFT ALONE and the run stops saving, rather than being destroyed to make room.
    //       False if the next SetWallpaper rewrites the file: the user has ten preserved
    //       copies and the eleventh start silently discards the live one, which is the only
    //       file the daemon will read on the next start.
    // WHY-THIS-SHAPE: the number of occupied names is the daemon's own bound, taken in BOTH
    //       directories preservation tries, and the check is made after a SetWallpaper rather
    //       than after the constructor. Occupying only the state directory does not stage the
    //       case at all -- the copy falls back beside the config and succeeds. Making the config
    //       directory unwritable instead would stage it, but then no save could land there
    //       either, so "the config was not saved over" would be true of any build whatever; with
    //       every name taken the directory stays writable, so a build that went on to rewrite the
    //       file is free to, and is caught. A check that only asserted "no config.json.bad.10 was
    //       created" would pass for a build that gave up on preserving and then went on to
    //       overwrite the file, which is the damaging half; and one made before the save would
    //       pass for any build at all, since the constructor writes nothing on this path.
    // CONTROL: three, all reverted. (a) `kMaxPreservedCopies` in
    //       backend/daemon/config_preserve.cpp raised to 11, so a free name exists after all ->
    //       all three checks red, "  21 copies (expected 20)" naming config.json.bad.10.
    //       (b) the `m_writer.refuse(...)` call in the constructor's error branch deleted -- the
    //       successor to the old `m_configUnread = true;` -- re-observed 2026-08-18 against the
    //       twenty-name fixture -> "FAIL: with every copy name taken, the daemon says the config
    //       could not be kept", reading "ok / not kept: ... no free name" in BOTH directories, and
    //       "FAIL: with every copy name taken, the config the daemon could not read is not saved
    //       over", printing the file rewritten to hold monitor 1 alone -- with the copy-count
    //       check still green: that pair is what separates "gave up on preserving" from "went on
    //       to destroy it", and only the second is data loss.
    //       (Six more go red in the phase above, which grades the same refusal on a config that
    //       could not be opened at all.) (c) the config directory dropped from
    //       preserveConfig()'s candidate list, which is the shape this fixture used to be
    //       written against -> nothing red here, which is the point: it is the fallback that
    //       makes the twenty names necessary.
    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();
    manager.preserveUnreadableConfig();
    manager.requestWallpaper("static", wallpaper.string(), 1);

    const WallpaperManager::Snapshot snap = manager.snapshot();
    const bool reported = snap.health.persistence.status == wallpaperx::HealthStatus::Fail &&
                          snap.health.persistence.detail.find("not kept") != std::string::npos;
    check(reported, "with every copy name taken, the daemon says the config could not be kept");
    if (!reported) {
        std::fprintf(stderr, "  persistence: %s / %s\n", wallpaperx::toString(snap.health.persistence.status),
                     snap.health.persistence.detail.c_str());
    }

    const std::vector<std::string> copies = preservedCopyNames(file, preservedCopyDir(scratch));
    check(static_cast<int>(copies.size()) == kOccupied,
          "with every copy name taken, the config the daemon could not read is still there");
    if (static_cast<int>(copies.size()) != kOccupied) {
        std::fprintf(stderr, "  %zu copies (expected %d): %s\n", copies.size(), kOccupied, joined(copies).c_str());
    }
    const std::string onDisk = readFile(file);
    check(onDisk == usersOwn, "with every copy name taken, the config the daemon could not read is not saved over");
    if (onDisk != usersOwn) std::fprintf(stderr, "  config.json now holds: %s\n", onDisk.c_str());
}

// ---------------------------------------------------------------------------- //
// Phase 6bd -- S8.23: the copy made for malformed entries is a copy, and is made once.
// ---------------------------------------------------------------------------- //
void runMalformedEntryCopyChecks() {
    constexpr const char* kGoal = "S8.23 (the copy made when entries are malformed is a copy, and is not made twice)";
    const ScratchDir scratch("malformedcopy");
    // preparePhase, not a bare scratch.ok(): one check here reads back a path the daemon
    // persisted, and a temporary directory whose name is not valid UTF-8 makes the writer skip
    // that entry by design (S8.7).
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoal, 1);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path file = paths->config;
    const std::filesystem::path kept = preservedCopyDir(scratch) / "config.json.bad";
    const std::filesystem::path wallpaper = paths->wallpapers.front();
    // Parses, so nothing here is an unreadable config: one entry the daemon restores, one it
    // cannot make sense of, and one it repairs. A rejected entry never becomes an entry at
    // all, so the keep-for-a-newer-build path cannot carry it and the first save drops it --
    // which is what makes the copy the user's only record of it. The negative id is the
    // repairable row: it makes this start a repairing start, so the copy is taken while a
    // rewrite of the same file is owed.
    const std::string firstBroken =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/left.png\"},"
        "{\"monitor_id\":\"two\",\"type\":\"static\",\"path\":\"/photos/right.png\"},"
        "{\"monitor_id\":-4,\"type\":\"static\",\"path\":\"/photos/gone.png\"}]}";
    const std::string secondBroken =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/later.png\"},"
        "{\"monitor_id\":1.5,\"type\":\"static\",\"path\":\"/photos/other.png\"}]}";
    if (!writeFile(file, firstBroken)) {
        notVerified(kGoal, "the config file this phase reads could not be written");
        return;
    }

    // GOAL: S8.23 -- a file whose entries were rejected is COPIED aside, not moved: the
    //       daemon goes on using it, because the entries it did understand are being
    //       restored from it. False if config.json is gone after the load, which is the
    //       whole config lost to one bad line; and false if a later start with a different
    //       broken file overwrites the copy, which loses the first file's rejected entry
    //       for good -- nothing else on disk holds it.
    // WHY-THIS-SHAPE: two starts with DIFFERENT broken files, compared byte for byte. One
    //       start can only establish that a copy exists, and a second start with the same
    //       bytes cannot tell "not copied again" from "copied again over identical
    //       content" -- the two leave the same file. The restored entry is asserted in the
    //       same breath, so this cannot pass for a build that refused the file outright and
    //       had nothing to restore, which is a different code path with a different verdict.
    // CONTROL: the `move` argument of preserveConfig() forced true on the malformed-entry
    //       path, so the original is unlinked where a copy is owed -> "FAIL: a config with
    //       malformed entries is still there after the copy is made" -- the only check red in
    //       this phase, though two later phases that drive the same copy path go red as well.
    //       Re-observed 2026-08-18, with move now taking the rename fast path inside putBytes()
    //       rather than copy-then-unlink; the file goes either way. Note what
    //       stayed GREEN under it: "a wallpaper set after a config with malformed entries is
    //       persisted", because the save simply recreates the file the unlink took away --
    //       which is why the loss has to be caught by the file's absence rather than by the
    //       daemon misbehaving afterwards. The two second-start checks stayed green too: the
    //       numbered search still sends the second file to a name of its own whether it is
    //       moved or copied, so what the second start grades is the naming, not the mode.
    //       Reverted.
    //
    // GOAL: S8.23 with S10.14 -- the copy holds what the USER's file held, on a start that
    //       also owes the file a repair. False if the repair is written first: the aside copy
    //       is then a copy of the daemon's own rewrite, and the row this build could not parse
    //       -- the one a newer build wrote, which nothing else on disk holds -- is in neither
    //       file, while the daemon still reports that the config was copied aside.
    // WHY-THIS-SHAPE: the fixture carries a repairable row (monitor id -4) BESIDE the
    //       unparseable one, and the phase deliberately stops after preserveUnreadableConfig()
    //       instead of going on to writeRepairedConfig(). Both matter. Without the repairable
    //       row the start writes nothing at all and the byte comparisons cannot see the
    //       ordering. And calling writeRepairedConfig() here would rewrite config.json
    //       legitimately, which destroys this phase's OTHER control: a build that MOVED the
    //       file aside instead of copying it would have config.json recreated by that write,
    //       so "still there after the copy is made" would go green on a rename. The instant
    //       between the two calls is a real point in backend/daemon/main.cpp's startup, and it
    //       is the only one at which both properties are visible.
    // CONTROL: the repair written from inside restoreSaved() instead of being handed to
    //       writeRepairedConfig() -- `if (repaired) persist(persistableLocked());` at the end of
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp), in place of
    //       `m_repair = persistableLocked();` -> "FAIL: a config with malformed entries is
    //       still there after the copy is made", "FAIL: the copy holds the bytes of the file
    //       the entries were rejected from", and "FAIL: a second start with a different
    //       malformed config does not overwrite the first start's copy", the last printing a
    //       copy holding the daemon's pretty-printed rewrite with monitor 0 alone in it -- the
    //       "monitor_id":"two" row gone from both files. Observed 2026-08-15. Reverted.
    {
        WallpaperManager::Config config;
        config.configFile = file;
        WallpaperManager manager(config);
        const std::vector<int> restored = manager.restoreSaved();
        check(restored == std::vector<int>{0}, "the entry the daemon did understand is restored from the same file");
        manager.preserveUnreadableConfig();

        check(readFile(file) == firstBroken, "a config with malformed entries is still there after the copy is made");
        check(readFile(kept) == firstBroken, "the copy holds the bytes of the file the entries were rejected from");
        const WallpaperManager::Snapshot snap = manager.snapshot();
        const bool said = snap.health.persistence.detail.find("malformed") != std::string::npos &&
                          snap.health.persistence.detail.find(kept.filename().string()) != std::string::npos;
        check(said, "the daemon says how many entries were dropped and where the file was copied");
        if (!said) std::fprintf(stderr, "  persistence detail: %s\n", snap.health.persistence.detail.c_str());

        // The copy is not a substitute for the daemon working: a rejected entry must not
        // stop the file being written from here on.
        manager.requestWallpaper("static", wallpaper.string(), 1);
        waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(1) == 1; });
        std::string why;
        const std::map<int, std::string> onDisk = pathsInFile(file, why);
        const bool wrote = onDisk.count(1) == 1 && onDisk.at(1) == wallpaper.string();
        check(wrote, "a wallpaper set after a config with malformed entries is persisted");
        if (!wrote) std::fprintf(stderr, "  %s\n", why.empty() ? "the file holds something else" : why.c_str());
    }

    if (!writeFile(file, secondBroken)) {
        notVerified(kGoal, "the second malformed config this phase needs could not be written");
        return;
    }
    WallpaperManager::Config second;
    second.configFile = file;
    WallpaperManager secondManager(second);
    secondManager.restoreSaved();
    secondManager.preserveUnreadableConfig();
    const std::string keptNow = readFile(kept);
    check(keptNow == firstBroken, "a second start with a different malformed config does not overwrite the first "
                                  "start's copy");
    if (keptNow != firstBroken) {
        std::fprintf(stderr, "  the copy now holds: %s\n  it held: %s\n", keptNow.c_str(), firstBroken.c_str());
    }
    // GOAL: S8.24 -- the second start's rejected entries get a copy under a name of their
    //       own, and the daemon says which name. False if the second file is silently not
    //       copied: its rejected entries exist nowhere else, so nothing on disk would hold
    //       them and nothing would say so. Equally false if the name it reports is not the
    //       file that is there.
    // WHY-THIS-SHAPE: the reported name is compared against the .bad.1 that must exist on
    //       disk holding secondBroken's bytes, not merely searched for the word "copied".
    //       Asserting only that the detail contains a name would pass for a daemon that
    //       announced a copy it did not make -- which is the worse failure, since the user
    //       then goes looking for a file that is not there.
    // CONTROL: the `(slot == 0 ? "" : "." + std::to_string(slot))` suffix dropped from
    //       preservedName() (backend/daemon/config_preserve.cpp), so both starts compete for
    //       config.json.bad -> "FAIL: the second start's malformed config is copied under a name
    //       of its own", listing config.json.bad in the state directory and one in the config
    //       directory, and "FAIL: the daemon says where the second malformed config was copied",
    //       reading "1 malformed entries ignored; copied to .../wallpaperx/config.json.bad" --
    //       a copy that was made, under the wrong name, which is why the reported name is
    //       compared against the file rather than searched for the word "copied". The first
    //       start's copy stayed intact, so the pair separates "the second file was lost" from
    //       "the first was destroyed". Observed 2026-08-18. Reverted.
    const std::filesystem::path keptSecond = preservedCopyDir(scratch) / "config.json.bad.1";
    const bool secondCopied = readFile(keptSecond) == secondBroken;
    check(secondCopied, "the second start's malformed config is copied under a name of its own");
    if (!secondCopied) {
        std::fprintf(stderr, "  copies: %s\n", joined(preservedCopyNames(file, preservedCopyDir(scratch))).c_str());
    }
    const std::string reported = secondManager.snapshot().health.persistence.detail;
    const bool said = reported.find(keptSecond.filename().string()) != std::string::npos;
    check(said, "the daemon says where the second malformed config was copied");
    if (!said) std::fprintf(stderr, "  persistence detail: %s\n", reported.c_str());
}

// ---------------------------------------------------------------------------- //
// Phase 6be -- S10.18: the startup rewrite is gated on a copy of what it replaces.
// ---------------------------------------------------------------------------- //
void runRepairCopyChecks() {
    constexpr const char* kGoals = "S10.18 (a start that rewrites the config copies it aside first, and does not "
                                   "rewrite it at all when no copy could be made)";

    // Two rows claiming one screen name: every row parses, so the store finds nothing
    // malformed, and the rejection is the manager's own. This is what a monitor replug
    // produces, and it is the case in which the file used to be rewritten with no copy taken.
    const auto fixture = [](const std::filesystem::path& first, const std::filesystem::path& second) {
        nlohmann::json monitors = nlohmann::json::array();
        monitors.push_back(configRow(1, "static", first.string(), "DP-3"));
        monitors.push_back(configRow(2, "static", second.string(), "DP-3"));
        nlohmann::json root;
        root["version"] = 1;
        root["monitors"] = monitors;
        return root.dump();
    };

    {
        const ScratchDir scratch("repaircopy");
        const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 2);
        if (!paths.has_value()) return;
        const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
        const std::filesystem::path& file = paths->config;
        const std::filesystem::path kept = preservedCopyDir(scratch) / "config.json.bad";
        const std::string usersOwn = fixture(paths->wallpapers[0], paths->wallpapers[1]);
        if (!writeFile(file, usersOwn)) {
            notVerified(kGoals, "the config file this phase reads could not be written");
            return;
        }

        // GOAL: S10.18's first half -- a start that will rewrite config.json copies the file
        //       aside first, even though nothing in it was malformed. False if no copy is made:
        //       the rewrite rebuilds the file from what this run kept, so the row it rejected
        //       exists nowhere afterwards, while the health note goes on saying the config was
        //       preserved.
        // WHY-THIS-SHAPE: the fixture repairs for a MANAGER-level reason (two rows claiming one
        //       screen) and for no store-level one, which is the case the copy used to miss --
        //       a fixture with a malformed row copies the file for the other reason entirely and
        //       is green either way. The copy is compared byte for byte against what was
        //       written, and the rewrite is asserted to have happened in the same breath: "a
        //       copy exists" is trivially satisfiable by a build that never rewrites anything.
        // CONTROL: the `|| repairPending` disjunct removed from preserveUnreadableConfig()'s copy
        //       gate (backend/daemon/wallpaper_manager.cpp), so only a malformed row takes the
        //       copy -> twelve red across five phases, led by "FAIL: a start that rewrites the
        //       config copies it aside first" printing "copies: (none)" and "FAIL: the copy holds
        //       the bytes the user's file held", with the rewrite check GREEN: the rewrite landing
        //       over a file nothing had copied is the loss itself. Re-observed 2026-08-18.
        // WAS: this named a second mutation beside it -- the `if (repairUncopied())` retry in
        //       queuePersist() -- and recorded that removing the disjunct alone left these two
        //       checks GREEN, because that retry took the copy the startup path declined. True
        //       when written; 6fa5be3 deleted the retry the next day, so this gate is now the
        //       only thing that takes the copy and one mutation reddens the pair directly.
        countRenamesOnto(file);
        WallpaperManager::Config config;
        config.configFile = file;
        WallpaperManager manager(config);
        manager.restoreSaved();
        manager.preserveUnreadableConfig();
        manager.writeRepairedConfig();

        const bool copied = std::filesystem::exists(kept);
        check(copied, "a start that rewrites the config copies it aside first");
        if (!copied) {
            std::fprintf(stderr, "  copies: %s\n",
                         joined(preservedCopyNames(file, preservedCopyDir(scratch))).c_str());
        }
        check(copied && readFile(kept) == usersOwn, "the copy holds the bytes the user's file held");
        if (copied && readFile(kept) != usersOwn) {
            std::fprintf(stderr, "  the copy holds: %s\n  the user had:   %s\n", readFile(kept).c_str(),
                         usersOwn.c_str());
        }
        const std::string presentPath = paths->wallpapers[1].string();
        const bool rewritten =
            waitForRows(file, [&presentPath](const std::map<int, RowInFile>& rows) {
                return rows.count(2) == 1 && rows.at(2).output == "DP-3" && rows.count(1) == 1 &&
                       rows.at(1).output.empty();
            });
        check(rewritten, "the repair this start computed reaches the file once the copy is taken");
        if (!rewritten) {
            std::string why;
            const std::map<int, RowInFile> onDisk = rowsInFile(file, why);
            std::fprintf(stderr, "  %s; ids on disk: %s\n", why.empty() ? "read back" : why.c_str(),
                         idsOf(onDisk).c_str());
        }
        const std::string detail = manager.snapshot().health.persistence.detail;
        const bool said = detail.find(kept.filename().string()) != std::string::npos;
        check(said, "the daemon says where it copied the config it was about to rewrite");
        if (!said) std::fprintf(stderr, "  persistence detail: %s\n", detail.c_str());
    }

    // The same start with nowhere to put the copy. That takes blocking BOTH candidates, because
    // preservation falls back to the config file's own directory: XDG_STATE_HOME names a REGULAR
    // FILE, so creating a directory under it fails for a reason no privilege changes -- unlike a
    // read-only directory, which root ignores -- and every one of the ten names the fallback
    // would use is already occupied beside the config.
    //
    // Occupied names rather than an unwritable config directory, deliberately: an unwritable
    // directory makes every write impossible, so "saving still works" below would be false for
    // reasons that have nothing to do with the copy. Here the directory is writable throughout,
    // so the daemon is free to save and is asserted to.
    const ScratchDir scratch("repairnocopy");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 2);
    if (!paths.has_value()) return;
    const std::filesystem::path blocked = scratch.path() / "not-a-directory";
    if (!writeFile(blocked, "this is a file, not a state directory")) {
        notVerified(kGoals, "the file that stands in for an unusable state directory could not be written");
        return;
    }
    const ScopedEnv stateHome("XDG_STATE_HOME", blocked.string());
    const std::filesystem::path& file = paths->config;
    const std::string usersOwn = fixture(paths->wallpapers[0], paths->wallpapers[1]);
    if (!writeFile(file, usersOwn)) {
        notVerified(kGoals, "the config file the no-copy case reads could not be written");
        return;
    }
    // The name freed later, to ask whether saving resumes; the rest stay taken.
    const std::filesystem::path firstBesideName = file.parent_path() / "config.json.bad";
    {
        bool occupied = true;
        for (int n = 0; n < 10 && occupied; ++n) {
            // Distinct bytes per name: an occupant holding exactly config.json's bytes is
            // recognised as a copy already taken, which would leave preservation succeeding.
            occupied = writeFile(file.parent_path() /
                                     ("config.json.bad" + (n == 0 ? std::string() : "." + std::to_string(n))),
                                 "an earlier broken config " + std::to_string(n));
        }
        if (!occupied) {
            notVerified(kGoals, "the ten occupied copy names beside the config could not be written, so the fallback "
                                "directory cannot be blocked");
            return;
        }
    }

    countRenamesOnto(file);
    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();

    // GOAL: a copy that was attempted and failed is reported as FAILED persistence, not as a
    //       healthy daemon. False if it reads ok: main() prints its "wallpapers may not survive a
    //       restart" line off exactly this status, so ok here is a start that lost rows in
    //       silence.
    // WHY-THIS-SHAPE: sampled between preserveUnreadableConfig() and writeRepairedConfig(),
    //       because the refusal that follows records Fail of its own and would mask this one. The
    //       status is asked separately from the text for the same reason: a build reporting the
    //       right words at the wrong severity is the defect.
    // WHY-THIS-SHAPE, second half: sampled between preserveUnreadableConfig() and
    //       writeRepairedConfig() so it is that call's own record being graded, not one the
    //       rewrite left behind.
    // CONTROL: the `m_repairCopied ? HealthStatus::Ok : HealthStatus::Fail` in
    //       WallpaperManager::preserveUnreadableConfig() (backend/daemon/wallpaper_manager.cpp)
    //       forced back to a bare `HealthStatus::Ok`, which is the code before this fix -> "FAIL:
    //       a copy that could not be taken is reported as failed persistence, not as ok".
    manager.preserveUnreadableConfig();
    const WallpaperManager::Snapshot afterCopy = manager.snapshot();
    const bool copyFailedLoudly = afterCopy.health.persistence.status == wallpaperx::HealthStatus::Fail &&
                                  afterCopy.health.persistence.detail.find("not copied") != std::string::npos;
    check(copyFailedLoudly, "a copy that could not be taken is reported as failed persistence, not as ok");
    if (!copyFailedLoudly) {
        std::fprintf(stderr, "  persistence: %s / %s\n", wallpaperx::toString(afterCopy.health.persistence.status),
                     afterCopy.health.persistence.detail.c_str());
    }

    // GOAL: with no copy obtainable, the repair is STILL WRITTEN and saving goes on working.
    //       False if the daemon declines: refusing the rewrite protects nothing, because the next
    //       ordinary save rebuilds the file from the same live state -- and refusing every write
    //       instead, which this daemon did for one commit, trades a bounded loss the user has
    //       already been told about for every wallpaper choice they make for the rest of the run.
    //       That is the rule GOALS.md S10.18 states: declining to rewrite must not disable saving
    //       for the session.
    // WHY-THIS-SHAPE: graded on the rename(2) counter over a window AND on the file's rows. The
    //       counter alone cannot distinguish the repair from any other write; the rows alone
    //       cannot see a write that has not landed yet, since writeRepairedConfig() queues and
    //       returns. The config directory is writable here and every copy NAME is taken instead,
    //       so a build that refuses is refusing by choice rather than by inability -- an
    //       unwritable directory would make this check green for a daemon that never got to
    //       decide.
    // CONTROL: `writeRepairedConfig()` (backend/daemon/wallpaper_manager.cpp) given back an early
    //       `if (!m_repairCopied) return;`, which is the shape two commits ago -> "FAIL: with no
    //       copy taken, the repair is written anyway rather than lost", printing "0 save(s)
    //       landed on it, and config.json holds ids 1 2".
    manager.writeRepairedConfig();

    const auto window = StallClock::now() + std::chrono::seconds(1);
    while (StallClock::now() < window && renamesCounted() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const bool repairLanded = waitForRows(file, [&paths](const std::map<int, RowInFile>& rows) {
        return rows.count(2) == 1 && rows.at(2).output == "DP-3" && rows.count(1) == 1 &&
               rows.at(1).output.empty();
    });
    const bool repairWritten = renamesCounted() > 0 && repairLanded;
    check(repairWritten, "with no copy taken, the repair is written anyway rather than lost");
    if (!repairWritten) {
        std::string why;
        const std::map<int, RowInFile> onDisk = rowsInFile(file, why);
        std::fprintf(stderr, "  %u save(s) landed on it, and config.json holds ids %s\n", renamesCounted(),
                     idsOf(onDisk).c_str());
    }

    // GOAL: an ordinary client save still reaches the file after a copy that could not be taken.
    //       False if saving is switched off for the session -- the user sets a wallpaper, watches
    //       it appear, and finds it gone at every restart, for a reason they were told about once
    //       at startup and cannot act on.
    // WHY-THIS-SHAPE: a real client save on the SAME manager that failed to preserve, graded on
    //       the row reaching the file. A fresh manager would prove nothing: it never attempted a
    //       copy, so it is not in the state this check is about.
    // CONTROL: an early `if (m_preserveFailed) return false;` added to WallpaperManager::persist()
    //       (backend/daemon/wallpaper_manager.cpp) -- refusing every save over a copy that could
    //       not be taken, which is what this daemon did for one commit -> "FAIL: an ordinary save
    //       still reaches the file after a copy that could not be taken", printing "ids on disk:
    //       1 2", and nothing else red. Re-observed 2026-08-18. (The pre-fix code put that refusal
    //       in queuePersist() and read it back through repairUncopied(); both are long gone, and
    //       the writer now knows nothing of preservation, so the manager is where it can go.)
    manager.requestWallpaper("static", paths->wallpapers[0].string(), 3);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(3) == 1; });
    std::string why;
    const std::map<int, RowInFile> afterSave = rowsInFile(file, why);
    const bool stillSaves = afterSave.count(3) == 1 && afterSave.at(3).path == paths->wallpapers[0].string();
    check(stillSaves, "an ordinary save still reaches the file after a copy that could not be taken");
    if (!stillSaves) {
        std::fprintf(stderr, "  %s; ids on disk: %s\n", why.empty() ? "read back" : why.c_str(),
                     idsOf(afterSave).c_str());
    }

    // GOAL: the alarm is STICKY -- a write that succeeds afterwards does not clear the record of
    //       what the uncopied rewrite destroyed. False if health flips back to ok: the rows this
    //       run rejected are gone from the only file that held them, and the one channel saying
    //       so is silenced by the very next ordinary save, which happens within seconds of
    //       startup on any desktop that sets a wallpaper.
    // WHY-THIS-SHAPE: read AFTER a save that is separately asserted to have landed, which is what
    //       makes it a test of stickiness rather than of the startup record. Reading health
    //       before any successful write would be green for a build with no stickiness at all.
    //       The status and the text are asked separately: a daemon that keeps the words and drops
    //       the severity reads healthy to /health, which aggregates on status.
    // CONTROL: the `|| m_preserveFailed` term dropped from applyPersistOutcomeLocked()'s report
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a copy that could not be taken is
    //       still reported after a later write succeeds", printing "persistence reads ok".
    const WallpaperManager::Snapshot afterSaveSnap = manager.snapshot();
    const bool stillFailing = afterSaveSnap.health.persistence.status == wallpaperx::HealthStatus::Fail;
    check(stillFailing, "a copy that could not be taken is still reported after a later write succeeds");
    if (!stillFailing) {
        std::fprintf(stderr, "  persistence reads %s (%s)\n",
                     wallpaperx::toString(afterSaveSnap.health.persistence.status),
                     afterSaveSnap.health.persistence.detail.c_str());
    }
    const bool stillSaysWhy = afterSaveSnap.health.persistence.detail.find("not copied") != std::string::npos;
    check(stillSaysWhy, "the surviving alarm still says which copy could not be taken");
    if (!stillSaysWhy) {
        std::fprintf(stderr, "  persistence detail: %s\n", afterSaveSnap.health.persistence.detail.c_str());
    }

    // COVERAGE-ASSERT: the stickiness above is not simply "this daemon can never report ok". A
    // second manager over the same directory, with a copy name freed so preservation succeeds,
    // must reach ok -- otherwise every check above holds for a build whose persistence health is
    // stuck at Fail from the first line of the constructor.
    // CONTROL: `m_preserveFailed` initialised to `true` in wallpaper_manager.h -> "FAIL: a run
    // whose copy WAS taken reports persistence ok, so the alarm above is not permanent", printing
    // "persistence reads fail", and four more across three other phases -- "an entry this build
    // cannot apply is reported without failing the persistence check", "the persistence health
    // recovers once a retried write lands" and "persistence reports itself working again once a
    // save has landed". A daemon whose persistence can never read ok is caught in several
    // independent places, which is the point of asking here as well.
    std::error_code freeEc;
    std::filesystem::remove(firstBesideName, freeEc);
    if (freeEc) {
        notVerified(kGoals, "a copy name could not be freed again, so whether a healthy run still reports ok was not "
                            "asked");
        return;
    }
    WallpaperManager::Config healthyConfig;
    healthyConfig.configFile = file;
    WallpaperManager healthy(healthyConfig);
    healthy.restoreSaved();
    healthy.preserveUnreadableConfig();
    healthy.requestWallpaper("static", paths->wallpapers[1].string(), 4);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(4) == 1; });
    const WallpaperManager::Snapshot healthySnap = healthy.snapshot();
    const bool healthyIsOk = healthySnap.health.persistence.status == wallpaperx::HealthStatus::Ok;
    check(healthyIsOk, "a run whose copy WAS taken reports persistence ok, so the alarm above is not permanent");
    if (!healthyIsOk) {
        std::fprintf(stderr, "  persistence reads %s (%s)\n",
                     wallpaperx::toString(healthySnap.health.persistence.status),
                     healthySnap.health.persistence.detail.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6be -- S10.18's location clause: a daemon with no state directory at all.
// ---------------------------------------------------------------------------- //
void runPreservationWithoutHomeChecks() {
    constexpr const char* kGoals = "S10.18's location clause (a daemon with a valid config path and no usable state "
                                   "directory still preserves the config it rejects, and goes on saving)";
    const ScratchDir scratch("nostatedir");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;
    // Neither variable set: a systemd user unit that exports XDG_CONFIG_HOME and nothing else is
    // the ordinary case, not a contrived one.
    const ScopedEnv stateHome("XDG_STATE_HOME", std::nullopt);
    const ScopedEnv home("HOME", std::nullopt);
    const std::filesystem::path& file = paths->config;
    const std::filesystem::path& wallpaper = paths->wallpapers.front();
    const std::filesystem::path beside = file.parent_path() / "config.json.bad";
    const std::string usersOwn =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/kept.png\"},]}";
    if (!writeFile(file, usersOwn)) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    // GOAL: with no usable state directory, the copy is kept beside the config file instead of
    //       not being kept at all -- and saving therefore works for the rest of the run. False if
    //       "nowhere preferred" is read as "nowhere": the daemon can then never move an
    //       unreadable config aside, so it refuses every save for the life of the process, and
    //       the user watches a wallpaper appear on screen and vanish at every restart.
    // WHY-THIS-SHAPE: both variables are unset rather than pointed somewhere unusable, because
    //       the failure being graded is the resolution returning nothing at all. The save
    //       afterwards is what makes this about the user rather than about a file name: a build
    //       that kept a copy and still refused to save would satisfy a check on the copy alone,
    //       and that is the whole cost of the defect.
    // CONTROL: the config file's own directory removed from preserveConfig()'s candidate list
    //       in backend/daemon/wallpaper_manager.cpp, which is the code before this fix -> "FAIL:
    //       with no state directory the config is kept beside the config file", printing "copies:
    //       (none)", "FAIL: the config that could not be read is not left where the daemon reads
    //       it", and "FAIL: a wallpaper set after the copy was kept is persisted", printing "ids
    //       on disk: (none)".
    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();
    manager.preserveUnreadableConfig();

    const bool keptBeside = readFile(beside) == usersOwn;
    check(keptBeside, "with no state directory the config is kept beside the config file");
    if (!keptBeside) {
        std::fprintf(stderr, "  copies: %s\n", joined(preservedCopyNames(file, file.parent_path())).c_str());
    }
    std::error_code ec;
    const bool moved = !std::filesystem::exists(file, ec) && !ec;
    check(moved, "the config that could not be read is not left where the daemon reads it");

    manager.requestWallpaper("static", wallpaper.string(), 1);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(1) == 1; });
    std::string why;
    const std::map<int, RowInFile> onDisk = rowsInFile(file, why);
    const bool saved = onDisk.count(1) == 1 && onDisk.at(1).path == wallpaper.string();
    check(saved, "a wallpaper set after the copy was kept is persisted");
    if (!saved) {
        std::fprintf(stderr, "  %s; ids on disk: %s\n", why.empty() ? "read back" : why.c_str(),
                     idsOf(onDisk).c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6bf -- S10.18's copy mechanics: what a move is, and what a failed copy leaves.
// ---------------------------------------------------------------------------- //
void runCopyMechanicsChecks() {
    constexpr const char* kMoveGoal = "S10.18's move clause (a config moved aside within one filesystem is renamed, so "
                                      "no partly-written copy can exist)";
    {
        const ScratchDir scratch("copyisrename");
        if (!scratch.ok()) {
            notVerified(kMoveGoal, "a scratch directory could not be created");
            return;
        }
        const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
        const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
        const std::filesystem::path kept = preservedCopyDir(scratch) / "config.json.bad";
        const std::string usersOwn =
            "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/kept.png\"},]}";
        if (!writeFile(file, usersOwn)) {
            notVerified(kMoveGoal, "the config file this phase reads could not be written");
            return;
        }
        struct stat before {};
        if (::stat(file.c_str(), &before) != 0) {
            notVerified(kMoveGoal, "the config file could not be stat'd, so its identity cannot be followed");
            return;
        }

        // GOAL: moving a rejected config aside within one filesystem is a rename, not a copy
        //       followed by a delete. False if it copies: a copy can fail part-written, and the
        //       truncated remnant then holds one of the ten preservation names for good while
        //       offering the user a silently shortened config as their preserved one. A rename
        //       either happened or did not.
        // WHY-THIS-SHAPE: graded on the inode, because a rename and a copy-then-unlink leave
        //       byte-identical results and differ in nothing else observable. The two directories
        //       are checked to be on one filesystem first: across filesystems a rename cannot
        //       work and the copy is correct, so grading there would report a fault for
        //       behaviour the goal asks for.
        struct stat configDir {};
        struct stat keptDir {};
        std::error_code dirEc;
        std::filesystem::create_directories(preservedCopyDir(scratch), dirEc);
        if (dirEc || ::stat(file.parent_path().c_str(), &configDir) != 0 ||
            ::stat(preservedCopyDir(scratch).c_str(), &keptDir) != 0) {
            notVerified(kMoveGoal, "the two directories could not be compared, so whether a rename is even possible "
                                   "here is unknown");
            return;
        }
        if (configDir.st_dev != keptDir.st_dev) {
            notVerified(kMoveGoal, "this host puts the temporary directory's subdirectories on different filesystems, "
                                   "where a rename cannot be used and the copy is correct");
            return;
        }

        // CONTROL: the `if (move)` rename arm in putBytes() (backend/daemon/config_preserve.cpp)
        //       disabled so the copy-then-unlink path runs, which is the code before this fix ->
        //       "FAIL: a config moved aside on one filesystem keeps its identity, so no partial
        //       copy is possible", printing "config.json was inode 32031, the copy is inode
        //       32034", and nothing else in the file red. Observed 2026-08-18. Reverted.
        WallpaperManager::Config config;
        config.configFile = file;
        WallpaperManager manager(config);
        manager.restoreSaved();
        manager.preserveUnreadableConfig();

        struct stat after {};
        if (::stat(kept.c_str(), &after) != 0) {
            check(false, "the config this phase rejected was preserved at all");
            std::fprintf(stderr, "  copies: %s\n",
                         joined(preservedCopyNames(file, preservedCopyDir(scratch))).c_str());
            return;
        }
        const bool renamed = after.st_ino == before.st_ino && after.st_dev == before.st_dev;
        check(renamed, "a config moved aside on one filesystem keeps its identity, so no partial copy is possible");
        if (!renamed) {
            std::fprintf(stderr, "  config.json was inode %ju, the copy is inode %ju\n",
                         static_cast<std::uintmax_t>(before.st_ino), static_cast<std::uintmax_t>(after.st_ino));
        }
    }

    // The copy path -- the one taken before a repair, which cannot be a rename because the file
    // has to stay where it is -- made to fail part-written.
    constexpr const char* kPartialGoal = "S10.18's copy clause (a copy that fails leaves no truncated file holding a "
                                         "preservation name)";
    const ScratchDir scratch("partialcopy");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kPartialGoal, 2);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;
    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(1, "static", paths->wallpapers[0].string(), "DP-3"));
    monitors.push_back(configRow(2, "static", paths->wallpapers[1].string(), "DP-3"));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    const std::string usersOwn = root.dump();
    if (!writeFile(file, usersOwn)) {
        notVerified(kPartialGoal, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();

    // A file-size limit is what makes the copy fail after it has already created its output and
    // written part of it -- the state a rename cannot produce and a copy can. Everything that
    // could write is done before the limit drops and after it is restored.
    rlimit originalSize{};
    if (::getrlimit(RLIMIT_FSIZE, &originalSize) != 0) {
        notVerified(kPartialGoal, "this host would not report RLIMIT_FSIZE, so no copy can be made to fail part-way");
        return;
    }
    // SIGXFSZ's default disposition kills the process; ignored, the write reports EFBIG instead.
    struct sigaction ignoreXfsz {};
    struct sigaction previousXfsz {};
    ignoreXfsz.sa_handler = SIG_IGN;
    ::sigemptyset(&ignoreXfsz.sa_mask);
    if (::sigaction(SIGXFSZ, &ignoreXfsz, &previousXfsz) != 0) {
        notVerified(kPartialGoal, "SIGXFSZ could not be ignored, so an over-long write would kill this process "
                                  "instead of reporting");
        return;
    }
    // A guard, not a call at the end: the limit and the disposition are process-wide, and a
    // return added later would take every following phase's writes down with them.
    struct RestoreSizeLimit {
        rlimit original;
        struct sigaction previous;
        bool restored = false;
        ~RestoreSizeLimit() { restore(); }
        void restore() {
            if (restored) return;
            restored = ::setrlimit(RLIMIT_FSIZE, &original) == 0;
            ::sigaction(SIGXFSZ, &previous, nullptr);
        }
    } restoreSizeLimit{originalSize, previousXfsz};

    rlimit tiny = originalSize;
    tiny.rlim_cur = 16;
    bool limitBites = ::setrlimit(RLIMIT_FSIZE, &tiny) == 0;
    if (limitBites) {
        // X.3: a filesystem or a privilege that ignores the limit would leave the copy
        // succeeding, and every check below would grade a daemon that was never put in the state
        // this phase is about.
        const std::filesystem::path probe = scratch.path() / "sizeprobe";
        { std::ofstream out(probe, std::ios::binary); out << std::string(4096, 'x'); }
        std::error_code probeEc;
        limitBites = std::filesystem::file_size(probe, probeEc) < 4096 || probeEc;
        std::filesystem::remove(probe, probeEc);
    }
    if (!limitBites) {
        restoreSizeLimit.restore();
        notVerified(kPartialGoal, "this process can still write a file past RLIMIT_FSIZE (running privileged, or on a "
                                  "filesystem that ignores the limit), so no copy can be made to fail part-way");
        return;
    }

    // GOAL: a copy that fails removes its own partial output. False if the truncated file stays:
    //       it occupies one of the ten preservation names for the life of the install, every
    //       later attempt is denied that name, and the user who goes looking for their preserved
    //       config finds a silently shortened one.
    // WHY-THIS-SHAPE: the failure is a real EFBIG from a real copy_file, produced by a file-size
    //       limit rather than by an unwritable directory. An unwritable directory is the cheaper
    //       fixture and grades nothing: the copy fails before it creates anything, so "no file
    //       was left behind" is true of every build. The limit is restored before any check runs,
    //       so a red line is never itself truncated.
    // CONTROL: the `std::filesystem::remove(kept, cleanupEc)` cleanup disabled in putBytes()'s
    //       copy-failure arm (backend/daemon/config_preserve.cpp), which is the code before
    //       this fix -> "FAIL: a copy that failed part-way leaves no file behind holding its
    //       name", printing all TWENTY names -- ten in each candidate directory -- occupied by
    //       16-byte remnants, spent by this single start. Preservation is dead from here on.
    //       Observed 2026-08-18. Reverted.
    manager.preserveUnreadableConfig();
    restoreSizeLimit.restore();
    if (!restoreSizeLimit.restored) {
        harnessFailure("RLIMIT_FSIZE could not be put back, so every later phase would be graded against a process "
                       "that cannot write");
        return;
    }

    const std::vector<std::string> copies = preservedCopyNames(file, preservedCopyDir(scratch));
    check(copies.empty(), "a copy that failed part-way leaves no file behind holding its name");
    if (!copies.empty()) {
        std::string sizes;
        for (const std::string& copy : copies) {
            std::error_code sizeEc;
            sizes += copy + " (" + std::to_string(std::filesystem::file_size(copy, sizeEc)) + " bytes) ";
        }
        std::fprintf(stderr, "  %s\n", sizes.c_str());
    }
    // COVERAGE-ASSERT: the copy really was attempted and really did fail. Without this, "no file
    // was left behind" is equally true of a run where the limit never bit and nothing was copied
    // at all -- and of one where the daemon never tried.
    const std::string detail = manager.snapshot().health.persistence.detail;
    const bool tried = detail.find("not copied") != std::string::npos &&
                       detail.find("File too large") != std::string::npos;
    check(tried, "the copy this phase is about was attempted, and the daemon says how it failed");
    if (!tried) std::fprintf(stderr, "  persistence detail: %s\n", detail.c_str());
}

// ---------------------------------------------------------------------------- //
// Phase 6bh -- S10.18's removal clause: the bytes are safe, the original would not go.
// ---------------------------------------------------------------------------- //
void runPreservedCopySurvivesFailedRemovalChecks() {
    constexpr const char* kGoals = "S10.18's removal clause (a copy that holds the bytes counts as preserved even "
                                   "when the original cannot be removed, the report says the original is still in "
                                   "place, and saving is not refused for the rest of the run)";

    // The two ways a copy can already hold the bytes when the removal fails. They are separate
    // arms of keepCopyIn() and were separately wrong, so one fixture cannot stand for both.
    struct Arm {
        const char* what;
        bool stageExistingCopy;  // an earlier start already preserved exactly these bytes
        bool crossDevice;        // the state directory is on another filesystem, so a copy is made
    };
    const Arm arms[] = {
        {"a copy an earlier start already made, holding these same bytes", true, false},
        {"a copy this start wrote, because the two directories are on different filesystems", false, true},
    };

    int exercised = 0;
    int unstageable = 0;
    for (const Arm& arm : arms) {
        const ScratchDir scratch("keptnoremove");
        // preparePhase: the save at the end of each arm persists a real wallpaper path, which a
        // temporary directory whose name is not valid UTF-8 would make the writer drop by design.
        const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
        if (!paths.has_value()) {
            ++unstageable;
            continue;
        }
        const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
        const std::filesystem::path& file = paths->config;
        const std::filesystem::path kept = preservedCopyDir(scratch) / "config.json.bad";
        // Read and rejected, not unopenable: preservation only moves bytes the daemon actually
        // holds, and the trailing comma is what makes this file valid enough to read and invalid
        // as JSON. Two monitors, so losing it is losing wallpapers rather than losing nothing.
        const std::string usersOwn =
            "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/left.png\"},"
            "{\"monitor_id\":1,\"type\":\"static\",\"path\":\"/photos/right.png\"},]}";
        if (!writeFile(file, usersOwn) || (arm.stageExistingCopy && !writeFile(kept, usersOwn))) {
            ++unstageable;
            notVerified(kGoals, std::string("the '") + arm.what + "' case could not be staged: the config file, or "
                                                                  "the copy standing for an earlier start, could not "
                                                                  "be written");
            continue;
        }

        // The config directory read-only is what makes the REMOVAL fail while the bytes are
        // reachable: the file is still readable and still copyable out, and only unlinking it
        // from this directory is refused.
        std::error_code ec;
        std::filesystem::permissions(file.parent_path(),
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace, ec);
        if (ec) {
            ++unstageable;
            notVerified(kGoals, std::string("the config directory could not be made read-only for the '") + arm.what +
                                    "' case: " + ec.message());
            continue;
        }
        // Restored explicitly before the checks -- the last of them needs a save to land in this
        // directory -- with the destructor as the backstop for any return added later.
        struct RestorePermissions {
            std::filesystem::path dir;
            bool restored = false;
            ~RestorePermissions() { restore(); }
            void restore() {
                if (restored) return;
                std::error_code restoreEc;
                std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::add, restoreEc);
                restored = !restoreEc;
            }
        } restorePermissions{file.parent_path()};
        { std::ofstream probe(file.parent_path() / "probe"); }
        if (std::filesystem::exists(file.parent_path() / "probe")) {
            std::filesystem::remove(file.parent_path() / "probe", ec);
            ++unstageable;
            notVerified(kGoals, "this process can write into a directory it made read-only (running as root, or a "
                                "filesystem that ignores mode bits), so the removal of the original cannot be made "
                                "to fail");
            continue;
        }
        ++exercised;

        // GOAL: bytes safely at <kept> are PRESERVED even though the original would not go away,
        //       the note says so, and the run goes on saving. False before this fix in both arms:
        //       keepCopyIn() reported nothing preserved when only the removal failed -- and the
        //       fresh-copy arm deleted the good copy it had just written on the way out. Reporting
        //       nothing preserved leaves the writer's refusal standing (ConfigWriter::refuse(),
        //       lifted only by the allow() preserveUnreadableConfig() makes when a copy lands),
        //       which refuses EVERY save for the rest of the process, so a user with a read-only
        //       config directory loses every wallpaper change until they restart, silently.
        // WHY-THIS-SHAPE: the last check is a real save landing in the file, not a reading of the
        //       health status. The status is Fail either way here -- the config was unreadable, and
        //       that is what it reports -- so grading the status would be green against the build
        //       that refuses every save. Only a row on disk separates them. The directory is put
        //       back to writable BEFORE that save, so what it measures is the daemon's refusal and
        //       not the mode bits. The failure is a real EACCES from a real unlink rather than an
        //       interposed one; what is simulated is only the arm's precondition (see below).
        // CONTROL: two, one per arm, both in keepCopyIn() (backend/daemon/config_preserve.cpp),
        //       both reverted -- see the arm table above for which fixture reaches which.
        //       (a) the SameBytes arm made to return `{{}, trouble}` when the removal failed,
        //       which is the code before this fix -> the reuse arm alone red, on "the copy holding
        //       the config's bytes is reported as preserved" with a detail reading "not kept:
        //       <state dir>: Permission denied; <config dir>: Permission denied; not saving over
        //       it; ...", and on "a wallpaper set after the copy was made reaches the config file"
        //       with the file still holding the user's own bytes and the daemon saying "not saved,
        //       this run could not read it and will not overwrite it".
        //       (b) the fresh-copy arm made to remove <kept> and return `{{}, trouble}` the same
        //       way -> the cross-filesystem arm alone red, on those same two plus "the preserved
        //       copy holds exactly the bytes the config held", printing "no copy exists at all" --
        //       the good copy deleted on the way out. Each arm stays green under the other's
        //       mutation, which is why they are separate fixtures.
        // COVERAGE-ASSERT: `exercised + unstageable` is asserted against the table's size below,
        //       so an arm that stops being reached is neither counted nor reported and the sum
        //       falls short. CONTROL: a `continue` for the cross-filesystem arm at the top of the
        //       loop -> "FAIL: every failed-removal case was either exercised or reported as one
        //       this host could not stage", printing "1 exercised and 0 unstageable, of 2".
        //       Reverted.
        const std::string armName = arm.what;
        {
            // Scoped so the injection is gone before the save below: the config writer's own
            // rename is how a save lands, and a leaked EXDEV would fail it for the wrong reason.
            std::optional<CrossDeviceRenames> crossDevice;
            if (arm.crossDevice) crossDevice.emplace(file);

            WallpaperManager::Config config;
            config.configFile = file;
            WallpaperManager manager(config);
            manager.restoreSaved();
            manager.preserveUnreadableConfig();
            const std::string detail = manager.snapshot().health.persistence.detail;
            crossDevice.reset();

            const std::vector<std::string> copies = preservedCopyNames(file, preservedCopyDir(scratch));
            const bool copyHoldsIt = copies.size() == 1 && readFile(copies.front()) == usersOwn;
            check(copyHoldsIt, (armName + ": the preserved copy holds exactly the bytes the config held").c_str());
            if (!copyHoldsIt) {
                std::fprintf(stderr, "  %s\n",
                             copies.empty() ? "no copy exists at all"
                                            : ("copies: " + joined(copies)).c_str());
            }
            // The original really did stay: without this the arm could be staged by a removal that
            // quietly succeeded, and every check below would grade the ordinary path.
            const bool originalStayed = readFile(file) == usersOwn;
            check(originalStayed, (armName + ": the original this run could not remove is still where it was").c_str());
            if (!originalStayed) {
                std::error_code existsEc;
                std::fprintf(stderr, "  %s\n",
                             std::filesystem::exists(file, existsEc)
                                 ? ("config.json now holds: " + readFile(file)).c_str()
                                 : "config.json is gone, so the removal this arm needs to fail succeeded");
            }

            const bool reportedKept = detail.find("kept as " + kept.string()) != std::string::npos &&
                                      detail.find("original still in place") != std::string::npos &&
                                      detail.find("not kept") == std::string::npos &&
                                      detail.find("not saving over it") == std::string::npos;
            check(reportedKept, (armName + ": the copy holding the config's bytes is reported as preserved").c_str());
            if (!reportedKept) std::fprintf(stderr, "  persistence detail: %s\n", detail.c_str());

            restorePermissions.restore();
            if (!restorePermissions.restored) {
                harnessFailure("the config directory could not be made writable again, so whether the daemon still "
                               "saves cannot be asked");
                continue;
            }
            manager.requestWallpaper("static", paths->wallpapers[0].string(), 1);
            const bool landed = waitForRows(file, [&paths](const std::map<int, RowInFile>& rows) {
                return rows.count(1) == 1 && rows.at(1).path == paths->wallpapers[0].string();
            });
            check(landed, (armName + ": a wallpaper set after the copy was made reaches the config file").c_str());
            if (!landed) {
                std::fprintf(stderr, "  config.json holds: %s\n  persistence detail: %s\n", readFile(file).c_str(),
                             manager.snapshot().health.persistence.detail.c_str());
            }
        }
    }

    const bool allAccounted = exercised + unstageable == static_cast<int>(std::size(arms));
    check(allAccounted, "every failed-removal case was either exercised or reported as one this host could not stage");
    if (!allAccounted) {
        std::fprintf(stderr, "  %d exercised and %d unstageable, of %zu\n", exercised, unstageable, std::size(arms));
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6bg -- S10.18's exhaustion clause: a repair that recurs every start.
// ---------------------------------------------------------------------------- //
void runPreservationReuseChecks() {
    constexpr const char* kGoals = "S10.18's exhaustion clause (a repair that cannot be written is recomputed on every "
                                   "start, and must not spend a fresh preserved-copy name each time)";
    const ScratchDir scratch("repairrestarts");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 2);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;
    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(1, "static", paths->wallpapers[0].string(), "DP-3"));
    monitors.push_back(configRow(2, "static", paths->wallpapers[1].string(), "DP-3"));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    const std::string usersOwn = root.dump();
    if (!writeFile(file, usersOwn)) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }
    // The config directory read-only, so the repair can never be written and the same repair is
    // recomputed at every start. The state directory is elsewhere and stays writable, which is
    // what makes the copy the only thing this phase varies.
    std::error_code ec;
    std::filesystem::permissions(file.parent_path(),
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
        notVerified(kGoals, "the config directory could not be made read-only: " + ec.message());
        return;
    }
    struct RestorePermissions {
        std::filesystem::path dir;
        ~RestorePermissions() {
            std::error_code restoreEc;
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::add, restoreEc);
        }
    } restorePermissions{file.parent_path()};
    { std::ofstream probe(file.parent_path() / "probe"); }
    if (std::filesystem::exists(file.parent_path() / "probe")) {
        std::filesystem::remove(file.parent_path() / "probe", ec);
        notVerified(kGoals, "this process can write into a directory it made read-only (running as root, or a "
                            "filesystem that ignores mode bits), so the repair cannot be made to fail every start");
        return;
    }

    // GOAL: a start whose repair cannot be written must not consume a preservation name, so the
    //       eleventh such start still has somewhere to keep the config. False if each start takes
    //       a fresh name: after ten restarts every name is taken, preservation is dead for the
    //       life of the install, and from then on a genuinely unreadable config has nowhere to
    //       go -- with the daemon refusing every save because no copy could be made.
    // WHY-THIS-SHAPE: the restart count is deliberately above the daemon's own bound of ten. Nine
    //       starts would pass for the pre-fix build too, which is why "it makes a copy" is not
    //       the property; exceeding the budget is. Each start is a fresh WallpaperManager over
    //       the same unchanged file, which is what a restart is at this tier. The copy's bytes
    //       are compared as well as counted: one name held by a file that is not the user's
    //       config is the same loss by another route.
    // CONTROL: the reuse arm in inspectName() (backend/daemon/config_preserve.cpp) made to
    //       answer OtherBytes always, so every start takes a fresh name, which is the code
    //       before this fix -> all three red: "FAIL: twelve starts that could not write
    //       their repair leave one preserved copy, not one per start", printing "10 copies" and
    //       naming config.json.bad through .bad.9; "FAIL: the one preserved copy holds the bytes
    //       the user's config held"; and "FAIL: the last of twelve identical starts still reports
    //       where it kept the config", whose detail reads "not copied: .../state/wallpaperx: no
    //       free name; .../wallpaperx: Permission denied" -- preservation dead from the eleventh
    //       start on, and the run left with nowhere to keep the next unreadable config. The same
    //       mutation also reddens the removal-clause phase's already-preserved arm, which is
    //       built on the same answer. Observed 2026-08-18. Reverted.
    constexpr int kStarts = 12;
    std::printf("config_store_test: the 'config not saved' lines naming %s are this phase's -- up to %d of them, one "
                "for each simulated restart against a read-only config directory\n",
                file.string().c_str(), kStarts);
    std::string lastDetail;
    for (int start = 0; start < kStarts; ++start) {
        WallpaperManager::Config config;
        config.configFile = file;
        WallpaperManager manager(config);
        manager.restoreSaved();
        manager.preserveUnreadableConfig();
        manager.writeRepairedConfig();
        lastDetail = manager.snapshot().health.persistence.detail;
    }

    const std::vector<std::string> copies = preservedCopyNames(file, preservedCopyDir(scratch));
    check(copies.size() == 1, "twelve starts that could not write their repair leave one preserved copy, not one per "
                              "start");
    if (copies.size() != 1) {
        std::fprintf(stderr, "  %zu copies: %s\n", copies.size(), joined(copies).c_str());
    }
    const bool holdsTheOriginal = copies.size() == 1 && readFile(copies.front()) == usersOwn;
    check(holdsTheOriginal, "the one preserved copy holds the bytes the user's config held");
    const bool stillReports = lastDetail.find("copied to") != std::string::npos;
    check(stillReports, "the last of twelve identical starts still reports where it kept the config");
    if (!stillReports) std::fprintf(stderr, "  persistence detail: %s\n", lastDetail.c_str());
}

// ---------------------------------------------------------------------------- //
// Phase 6c -- S8.14: entries this build cannot apply survive, and are reported.
// ---------------------------------------------------------------------------- //
void runUnappliedEntryChecks() {
    constexpr const char* kGoal = "S8.14 (entries this build cannot apply survive a save and are reported)";
    const ScratchDir scratch("unapplied");
    if (!scratch.ok()) {
        notVerified(kGoal, "a scratch directory could not be created");
        return;
    }
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path present = scratch.path() / "present.png";
    const std::filesystem::path other = scratch.path() / "other.png";
    if (!writeWallpaperFile(present) || !writeWallpaperFile(other)) {
        notVerified(kGoal, "the wallpaper files this phase drives could not be written");
        return;
    }
    // dump() throws on a string that is not valid UTF-8, and the fixture below names a
    // scratch path this host chose. A skip with the reason, rather than an exception the
    // phase would report as a failure of the daemon.
    if (!wallpaperx::isValidUtf8(present.string())) {
        notVerified(kGoal, "this host's temporary directory has a path that is not valid UTF-8, so no JSON fixture "
                           "can name it");
        return;
    }

    // GOAL: S8.14, both clauses -- an entry this build cannot apply is neither deleted nor
    //       kept in silence. False if the next save drops it (the wallpaper a newer build
    //       set is gone for good the moment this one writes) and equally false if nothing
    //       anywhere says an entry did not come back.
    // WHY-THIS-SHAPE: three kinds of unusable entry at once -- an id past the daemon's
    //       32-monitor range, a type this build has never heard of, an empty path -- and the
    //       on-disk ids are compared as an exact set TWICE: once straight after the restore
    //       and once after a save made for a DIFFERENT monitor. Two reads, not one, because
    //       two different writes can drop these rows and a single read cannot say which did.
    //       This fixture's negative ids are a repair, so restoreSaved() persists at startup;
    //       the SetWallpaper save is a second, independent chance to forget them, and it is
    //       the one a user actually reaches. The count in the health detail is derived from
    //       the fixture, not spelled out, so a build that reports a fixed number cannot pass.
    // CONTROL: `m_unapplied.push_back(std::move(keep));` deleted from restoreSaved()'s kept
    //       branch (backend/daemon/wallpaper_manager.cpp) -> "FAIL: the entries this build
    //       cannot apply survive the write the restore itself makes" with "  ids on disk: 3",
    //       "FAIL: the entries this build cannot apply are all still in the file, and the ones
    //       no build can apply are gone, after another monitor is set" with "  ids on disk: 1
    //       3", and "FAIL: the persistence detail says how many entries could not be applied".
    //       Twenty more went red with them across the kept-row phases, keeping the row being
    //       what they all rest on. Observed 2026-08-14. Reverted.
    const std::vector<int> unusableIds{40, 5, 6};
    // Negative ids, the one kind of entry that is dropped rather than kept. A monitor id is
    // an index into this build's array, so no larger kMaxMonitors could ever admit one --
    // keeping them would carry them through every save for the life of the install.
    const std::vector<int> droppedIds{-1, -7};
    const std::string config = "{\"version\":1,\"monitors\":["
                               "{\"monitor_id\":3,\"type\":\"static\",\"path\":" +
                               nlohmann::json(present.string()).dump() +
                               "},"
                               "{\"monitor_id\":40,\"type\":\"static\",\"path\":\"/a.png\"},"
                               "{\"monitor_id\":-1,\"type\":\"static\",\"path\":\"/neg.png\"},"
                               "{\"monitor_id\":5,\"type\":\"slideshow\",\"path\":\"/b.png\"},"
                               "{\"monitor_id\":-7,\"type\":\"static\",\"path\":\"/neg2.png\"},"
                               "{\"monitor_id\":6,\"type\":\"static\",\"path\":\"\"}]}";
    if (!writeFile(file, config)) {
        notVerified(kGoal, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config managerConfig;
    managerConfig.configFile = file;
    WallpaperManager manager(managerConfig);
    const std::vector<int> restored = manager.restoreSaved();
    check(restored == std::vector<int>{3}, "only the entry this build can apply is posted for rendering");
    // The two calls backend/daemon/main.cpp makes once it owns the bus name, in that order.
    // restoreSaved() computes the repair but writes nothing, so a phase that graded the file
    // after it alone would be grading a startup the daemon does not perform.
    manager.preserveUnreadableConfig();
    manager.writeRepairedConfig();

    // The first of the two reads: the negative ids make this a repair, so the start has
    // already written the file. What it wrote must hold the rows no build here can apply and
    // not the ones no build anywhere can. Waited for on the row the fixture cannot supply --
    // the write drops -1 -- because persist() may return with the write still queued.
    waitForRows(file, [](const std::map<int, RowInFile>& rows) {
        return rows.count(-1) == 0 && rows.count(3) == 1;
    });
    std::string repairWhy;
    const std::map<int, std::string> afterRestore = pathsInFile(file, repairWhy);
    std::vector<int> idsAfterRestore;
    for (const auto& entry : afterRestore) idsAfterRestore.push_back(entry.first);
    const std::vector<int> expectedAfterRestore{3, 5, 6, 40};
    const bool survivedRepair = idsAfterRestore == expectedAfterRestore;
    check(survivedRepair, "the entries this build cannot apply survive the write the restore itself makes");
    if (!survivedRepair) {
        std::fprintf(stderr, "  %s; ids on disk:", repairWhy.empty() ? "read back" : repairWhy.c_str());
        for (const int id : idsAfterRestore) std::fprintf(stderr, " %d", id);
        std::fprintf(stderr, "\n");
    }

    const WallpaperManager::Snapshot snap = manager.snapshot();
    const std::optional<int> unusableCount =
        clauseCountOrSkip(snap.health.persistence.detail, " entries this build cannot apply", kGoal);
    if (!unusableCount.has_value()) return;
    const bool reported = *unusableCount == static_cast<int>(unusableIds.size());
    check(reported, "the persistence detail says how many entries could not be applied");
    if (!reported) std::fprintf(stderr, "  persistence detail: %s\n", snap.health.persistence.detail.c_str());
    check(snap.health.persistence.status == wallpaperx::HealthStatus::Ok,
          "an entry this build cannot apply is reported without failing the persistence check");

    // GOAL: an entry no build could ever apply is DROPPED, and the loss is counted and
    //       named. False in either direction: a negative monitor id kept in the file is
    //       carried through every save for the life of the install, and one dropped in
    //       silence is a wallpaper the user set that never comes back with nothing anywhere
    //       saying why. The two halves are reported differently on purpose -- "kept" and
    //       "dropped" are different outcomes and a reader has to be able to tell them apart.
    // WHY-THIS-SHAPE: TWO negative ids, and the expected count is derived from that list
    //       rather than written as "2". With one, a detail that reported the number of
    //       *kinds* of loss, or a hardcoded 1, would pass. The wording is matched in full,
    //       including "no build can apply them", because the kept-entry clause of the same
    //       detail also ends in a count and a semicolon -- matching only a number would let
    //       either clause satisfy this.
    // CONTROL: `++m_droppedEntries;` deleted from restoreSaved()'s negative-id branch ->
    //       "FAIL: the persistence detail says how many entries no build can apply", printing
    //       the detail with only the kept-entry clause in it. The id-set check below stayed
    //       green under it, which is what separates "not counted" from "not dropped".
    //       Reverted.
    const std::string droppedClause = " entries dropped, no build can apply them";
    const std::optional<int> droppedCount =
        clauseCountOrSkip(snap.health.persistence.detail, droppedClause, kGoal);
    if (!droppedCount.has_value()) return;
    const bool droppedReported = *droppedCount == static_cast<int>(droppedIds.size());
    check(droppedReported, "the persistence detail says how many entries no build can apply");
    if (!droppedReported) {
        std::fprintf(stderr, "  persistence detail: %s\n  expected %zu of '%s'\n",
                     snap.health.persistence.detail.c_str(), droppedIds.size(), droppedClause.c_str());
    }

    manager.requestWallpaper("static", other.string(), 1);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(1) == 1; });
    std::string why;
    const std::map<int, std::string> onDisk = pathsInFile(file, why);
    std::vector<int> ids;
    for (const auto& entry : onDisk) ids.push_back(entry.first);
    // An exact set, so it is simultaneously the kept-entry assertion and the dropped-entry
    // one: -1 and -7 are absent, and their absence cannot be satisfied by a build that lost
    // the others too.
    const std::vector<int> expectedIds{1, 3, 5, 6, 40};
    const bool survived = ids == expectedIds;
    check(survived, "the entries this build cannot apply are all still in the file, and the ones no build can "
                    "apply are gone, after another monitor is set");
    if (!survived) {
        std::fprintf(stderr, "  %s; ids on disk:", why.empty() ? "read back" : why.c_str());
        for (const int id : ids) std::fprintf(stderr, " %d", id);
        std::fprintf(stderr, "\n");
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6e -- Step 10: the connector name as a field of the stored row.
// ---------------------------------------------------------------------------- //
void runOutputFieldChecks() {
    constexpr const char* kGoals = "S10.5's storage half (a connector name is a persisted field of a monitor's row)";
    const ScratchDir scratch("outputfield");
    if (!scratch.ok()) {
        notVerified(kGoals, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const ConfigStore store(file);

    // GOAL: a connector name written into the store comes back byte for byte, including on
    //       a row that carries no wallpaper at all. False if the name is lost, altered, or
    //       reachable only when a wallpaper rides with it -- a screen the app has named but
    //       for which nothing has been chosen is the ordinary first-run state, and it must
    //       survive a restart or every plugin instance is anonymous again.
    // WHY-THIS-SHAPE: one case is a name sanitizeForLog would ALTER, and that property is
    //       asserted as part of the check. A round trip over "DP-1" alone passes just as
    //       well against a store that persists the display-safe rendering of the name --
    //       which is the defect that already had to be closed for `path` (PLAN.md Step 8
    //       trap 4), and the store has no reason to have learned it twice.
    // CONTROL: `row[kKeyOutput] = entry.output;` in ConfigStore::save()
    //       (backend/daemon/config_store.cpp) replaced with
    //       `sanitizeForLog(entry.output, kMaxOutputNameBytes, LogFraming::Bare)` -> "FAIL: a
    //       connector name holding U+202E and DEL survives the round trip byte for byte",
    //       printing the four raw bytes (e2 80 ae 7f) against the sixteen ASCII characters
    //       that escaping turns them into. Reverted.
    // COVERAGE-ASSERT: the altering case is asserted to be one sanitizeForLog alters, so an
    //       edit that tamed it fails instead of leaving the phase unable to discriminate.
    const std::string hostileName = std::string("DP-\xE2\x80\xAE\x7F-1");
    check(sanitizeForLog(hostileName, wallpaperx::kMaxOutputNameBytes, LogFraming::Bare) != hostileName,
          "the round-trip connector name is one sanitizeForLog alters, so a sanitizing store would fail here");
    const std::vector<PersistedWallpaper> entries{
        {0, "static", "/tmp/zero.png", "DP-1"},
        {1, "static", "/tmp/one.png", hostileName},
        // A screen that has been named and has nothing set on it.
        {2, "", "", "HDMI-A-2"},
        // A wallpaper with no name, which is every row a build without this field writes.
        {3, "static", "/tmp/three.png", ""},
    };
    const SaveResult saved = store.save(entries);
    check(saved.error.empty() && saved.skippedNonUtf8 == 0, "a row carrying a connector name is saved");
    if (!saved.error.empty()) std::fprintf(stderr, "  save error: %s\n", saved.error.c_str());

    const LoadResult loaded = store.load();
    std::map<int, PersistedWallpaper> byId;
    for (const PersistedWallpaper& entry : loaded.entries) byId[entry.monitorId] = entry;
    check(loaded.entries.size() == entries.size() && loaded.malformedEntries == 0,
          "every saved row comes back, and none of them reads as malformed");
    check(byId.count(0) == 1 && byId[0].output == "DP-1", "an ordinary connector name survives the round trip");
    const bool hostileKept = byId.count(1) == 1 && byId[1].output == hostileName;
    check(hostileKept, "a connector name holding U+202E and DEL survives the round trip byte for byte");
    if (!hostileKept) {
        std::fprintf(stderr, "  in:  %s\n  out: %s\n", hex(hostileName).c_str(),
                     byId.count(1) == 1 ? hex(byId[1].output).c_str() : "(no entry for this monitor)");
    }
    const bool nameOnlyKept =
        byId.count(2) == 1 && byId[2].output == "HDMI-A-2" && byId[2].type.empty() && byId[2].path.empty();
    check(nameOnlyKept, "a row carrying a connector name and no wallpaper survives with the name intact");

    // GOAL: the `output` key is written only when there is one, so a config for an install
    //       that has never named a screen is byte-identical to what a build without this
    //       field writes. False if every row grows an `"output": ""` -- that is a config an
    //       older build would then read, and a change to a file format is not free just
    //       because this build tolerates it.
    // WHY-THIS-SHAPE: the KEY's presence is asserted, not the value. `output == ""` is true
    //       of a row with no key and of a row with an empty one alike, so the value-shaped
    //       version of this check cannot fail for the thing it exists to forbid.
    // CONTROL: the `if (!entry.output.empty())` guard removed from ConfigStore::save() in
    //       backend/daemon/config_store.cpp, so the key is always written -> "FAIL: a row
    //       with no connector name carries no output key at all", and, in the restore phase
    //       below, "a rebinding is written to the file, not just held in memory" -- the
    //       released monitor now carries an empty output key instead of none. Reverted.
    std::string why;
    const std::map<int, RowInFile> onDisk = rowsInFile(file, why);
    check(onDisk.count(3) == 1 && !onDisk.at(3).hasOutputKey,
          "a row with no connector name carries no output key at all");
    check(onDisk.count(0) == 1 && onDisk.at(0).hasOutputKey && onDisk.at(0).output == "DP-1",
          "a row with a connector name carries it under the output key");
    if (onDisk.count(3) == 0) std::fprintf(stderr, "  %s; ids on disk: %s\n", why.c_str(), idsOf(onDisk).c_str());

    // GOAL: a config written by a build that had never heard of connector names loads
    //       cleanly. False if the absent key counts as a malformed entry, which would put a
    //       "N malformed entries ignored" notice in front of every user who upgrades.
    // WHY-THIS-SHAPE: the malformed COUNT is asserted, not just that the entries came back.
    //       A loader that counted the missing key and kept the entry anyway would satisfy an
    //       entries-only check while telling the user their config is damaged.
    // CONTROL: the `element.contains(kKeyOutput) &&` guard removed from ConfigStore::load()
    //       in backend/daemon/config_store.cpp, making the key mandatory -> "FAIL: a config
    //       written before connector names existed loads with no malformed entries", 0
    //       entries and 2 malformed. Reverted.
    const std::string oldConfig =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a.png\"},"
        "{\"monitor_id\":1,\"type\":\"static\",\"path\":\"/b.png\"}]}";
    if (!writeFile(file, oldConfig)) {
        notVerified(kGoals, "the old-format config file could not be written");
    } else {
        const LoadResult old = store.load();
        const bool clean = old.entries.size() == 2 && old.malformedEntries == 0 && old.error.empty();
        check(clean, "a config written before connector names existed loads with no malformed entries");
        if (!clean) {
            std::fprintf(stderr, "  %zu entries, %d malformed, error '%s'\n", old.entries.size(),
                         old.malformedEntries, old.error.c_str());
        }
        bool anyOutput = false;
        for (const PersistedWallpaper& entry : old.entries) anyOutput = anyOutput || !entry.output.empty();
        check(!anyOutput, "a config with no output keys yields rows with no connector name");
    }

    // GOAL: an `output` that is present and is not a string is a malformed entry, costing
    //       that row and nothing else. False in either direction: accepting it would put a
    //       number where a name belongs, and failing the whole load would cost a user every
    //       working monitor because one line was hand-edited wrong.
    // WHY-THIS-SHAPE: the SURVIVING ids are asserted as an exact set, not just the count.
    //       "one malformed" is equally true of a loader that dropped entry 1 and of one that
    //       dropped entry 2 and kept a duplicate -- and which id a wallpaper lands on is the
    //       whole subject of this step.
    // CONTROL: the `!readString(element, kKeyOutput, entry.output)` arm of that guard in
    //       ConfigStore::load() (backend/daemon/config_store.cpp) replaced with `false`, so a
    //       non-string output is accepted -> "FAIL: an output that is not a string costs its
    //       own entry and no other", reporting 3 entries and 0 malformed. Reverted.
    const std::string badTypeConfig =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a.png\"},"
        "{\"monitor_id\":1,\"type\":\"static\",\"path\":\"/b.png\",\"output\":42},"
        "{\"monitor_id\":2,\"type\":\"static\",\"path\":\"/c.png\",\"output\":\"DP-2\"}]}";
    if (!writeFile(file, badTypeConfig)) {
        notVerified(kGoals, "the non-string-output config file could not be written");
    } else {
        const LoadResult mixed = store.load();
        std::vector<int> ids;
        for (const PersistedWallpaper& entry : mixed.entries) ids.push_back(entry.monitorId);
        const bool asExpected = ids == std::vector<int>{0, 2} && mixed.malformedEntries == 1 && mixed.error.empty();
        check(asExpected, "an output that is not a string costs its own entry and no other");
        if (!asExpected) {
            std::fprintf(stderr, "  %zu entries (%d malformed), ids:", mixed.entries.size(), mixed.malformedEntries);
            for (const int id : ids) std::fprintf(stderr, " %d", id);
            std::fprintf(stderr, "; error '%s'\n", mixed.error.c_str());
        }
    }

    // GOAL: a connector name that is not valid UTF-8 costs the NAME and nothing else on the way
    //       out -- not the wallpaper on that row, and not the write. False in two directions:
    //       one such name throwing out of dump() takes every other monitor with it, and one such
    //       name costing its row loses a picture over a field the picture does not depend on.
    //       The mirror of the rule the path case above establishes.
    // WHY-THIS-SHAPE: asserted at the ConfigStore level with a hand-built entry, and NOT by
    //       driving the daemon: nothing a client can do produces this state. The manager
    //       validates every name it binds (outputNameError rejects invalid UTF-8), and the read
    //       path cannot deliver one either -- nlohmann's parser rejects an ill-formed UTF-8 byte
    //       as a whole-file syntax error (measured: "parse error at line 1 ... ill-formed UTF-8
    //       byte"), so a hand-edited config carrying one never reaches the per-entry code. What
    //       is left is ConfigStore's own contract -- save() must not throw for any input -- and
    //       that is what this grades. The bad row is in the MIDDLE, so a writer that stops at the
    //       first bad name cannot pass. The row is asserted to have LOST its output key rather
    //       than merely to be present: a writer that emitted the bad bytes anyway would keep the
    //       row and produce a file no later start can parse.
    // CONTROL: the two halves of ConfigStore::save()'s gate
    //       (backend/daemon/config_store.cpp) collapsed back into the single combined test
    //       `if (!isValidUtf8(path) || !isValidUtf8(type) || !isValidUtf8(output)) { ++skipped;
    //       continue; }`, which is the code before this fix -> "FAIL: a connector name that is
    //       not valid UTF-8 costs only the name, not the wallpaper on its row", printing
    //       "monitor 1 is not in the file at all", and "FAIL: a name that could not be written
    //       is not counted as a dropped wallpaper", printing "1 wallpapers reported dropped".
    const std::string badUtf8Name = std::string("DP\xFF-1");
    check(!wallpaperx::isValidUtf8(badUtf8Name), "the case name really is invalid UTF-8, so this phase can discriminate");
    const SaveResult badSave = store.save({{0, "static", "/tmp/zero.png", "DP-0"},
                                           {1, "static", "/tmp/one.png", badUtf8Name},
                                           {2, "static", "/tmp/two.png", "DP-2"}});
    check(badSave.error.empty(), "a connector name that is not valid UTF-8 does not fail the write");
    if (!badSave.error.empty()) std::fprintf(stderr, "  save error: %s\n", badSave.error.c_str());
    std::string badWhy;
    const std::map<int, RowInFile> afterBad = rowsInFile(file, badWhy);
    const bool wallpaperKept = afterBad.size() == 3 && afterBad.count(1) == 1 &&
                               afterBad.at(1).path == "/tmp/one.png" && !afterBad.at(1).hasOutputKey;
    check(wallpaperKept, "a connector name that is not valid UTF-8 costs only the name, not the wallpaper on its row");
    if (!wallpaperKept) {
        std::fprintf(stderr, "  %s; ids on disk: %s; %s\n", badWhy.empty() ? "read back" : badWhy.c_str(),
                     idsOf(afterBad).c_str(),
                     afterBad.count(1) == 0 ? "monitor 1 is not in the file at all"
                                            : ("monitor 1 holds path '" + afterBad.at(1).path + "'" +
                                               (afterBad.at(1).hasOutputKey
                                                    ? " and still carries an output key '" + afterBad.at(1).output + "'"
                                                    : " with no output key"))
                                                  .c_str());
    }
    // The counter means dropped WALLPAPERS now, and no wallpaper was dropped here. Counting this
    // would tell the user a monitor will not come back when every monitor did.
    const bool notCountedAsWallpaper = badSave.skippedNonUtf8 == 0;
    check(notCountedAsWallpaper, "a name that could not be written is not counted as a dropped wallpaper");
    if (!notCountedAsWallpaper) {
        std::fprintf(stderr, "  %d wallpapers reported dropped\n", badSave.skippedNonUtf8);
    }
    // The other rows are untouched, which is the "and nothing else" half.
    const bool othersKept = afterBad.count(0) == 1 && afterBad.at(0).output == "DP-0" && afterBad.count(2) == 1 &&
                            afterBad.at(2).output == "DP-2";
    check(othersKept, "a connector name that is not valid UTF-8 costs no other row its name");
    if (!othersKept) {
        std::fprintf(stderr, "  ids on disk: %s\n", idsOf(afterBad).c_str());
    }

    // GOAL: the lost name is COUNTED. A screen this file cannot carry the name of is a screen
    //       that comes back anonymous, and only the caller can say so -- so a save that dropped
    //       it and reported success is a loss with nothing anywhere pointing at it. False if the
    //       name half of the gate skips the field and increments nothing, which is what it did:
    //       the two checks above were satisfied by exactly that build.
    // WHY-THIS-SHAPE: the count is graded on its own, beside the assertion above that the same
    //       save reports NO dropped wallpaper. One counter for both losses is the simpler shape
    //       and is wrong in the direction that matters: the health text turns skippedNonUtf8 into
    //       "N wallpapers dropped", so a name counted there tells the user a picture will not come
    //       back and sends them looking for a monitor that is not missing.
    // CONTROL: `++result.skippedNames;` deleted from ConfigStore::save()
    //       (backend/daemon/config_store.cpp), which is the code before this fix -> "FAIL: a
    //       screen name the config file cannot carry is counted as one lost name", printing "0
    //       screen names reported dropped, 0 wallpapers". Observed 2026-08-18, reverted.
    //       Graded here and not through a manager because nothing a client can do makes this
    //       counter move -- every path that reaches save() with a name has already run it past
    //       outputNameError(), which rejects invalid UTF-8, and nlohmann refuses such a byte as a
    //       whole-file syntax error on the way in. The persistence health going to Fail on this
    //       counter is therefore defence in depth, and no check in this file can reach it.
    const bool nameLossCounted = badSave.skippedNames == 1;
    check(nameLossCounted, "a screen name the config file cannot carry is counted as one lost name");
    if (!nameLossCounted) {
        std::fprintf(stderr, "  %d screen names reported dropped, %d wallpapers\n", badSave.skippedNames,
                     badSave.skippedNonUtf8);
    }

    // GOAL: the name is counted wherever it is lost, including on a row that keeps nothing at all.
    //       False if the counter sits after the "nothing left worth a row" continue: the row with
    //       BOTH halves unwritable is the one where the screen loses its name most completely,
    //       and it would be the one case reported as a wallpaper loss alone -- the user re-picks a
    //       picture and the screen is still anonymous.
    // WHY-THIS-SHAPE: its own store and file, written last, because it replaces the file the
    //       checks above read back. Both counters are asserted from one save: the row is gone
    //       entirely, so "one wallpaper and one name were lost" is the only report that describes
    //       what happened, and asserting the name count alone would pass for a build that had
    //       stopped counting the wallpaper.
    // CONTROL: the `if (!nameOk && !entry.output.empty()) ++result.skippedNames;` line in
    //       ConfigStore::save() moved below the `if (!wallpaperOk && (!nameOk ||
    //       entry.output.empty())) continue;` that discards the row -> "FAIL: a row that loses
    //       both halves is reported as one lost wallpaper and one lost name", printing "1
    //       wallpapers and 0 names reported dropped, 0 rows on disk".
    const std::filesystem::path bothBadFile = scratch.path() / "bothbad.json";
    const ConfigStore bothBadStore(bothBadFile);
    const std::string badUtf8Path = std::string("/tmp/bad\x80name.png");
    const SaveResult bothBad = bothBadStore.save({{4, "static", badUtf8Path, badUtf8Name}});
    std::string bothWhy;
    const std::map<int, RowInFile> afterBoth = rowsInFile(bothBadFile, bothWhy);
    const bool bothCounted =
        bothBad.error.empty() && bothBad.skippedNonUtf8 == 1 && bothBad.skippedNames == 1 && afterBoth.empty();
    check(bothCounted, "a row that loses both halves is reported as one lost wallpaper and one lost name");
    if (!bothCounted) {
        std::fprintf(stderr, "  %d wallpapers and %d names reported dropped, %zu rows on disk; save error '%s'\n",
                     bothBad.skippedNonUtf8, bothBad.skippedNames, afterBoth.size(), bothBad.error.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6f -- Step 10: what the manager does with the names it reads back.
// ---------------------------------------------------------------------------- //
void runOutputRestoreChecks() {
    constexpr const char* kGoals = "S10.4 and S10.5 at the manager level (names are restored, merged into one row "
                                   "per monitor, and never claimed by two monitors)";
    const ScratchDir scratch("outputrestore");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 2);
    if (!paths) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;
    const std::filesystem::path& present = paths->wallpapers[0];
    const std::filesystem::path& other = paths->wallpapers[1];

    // Monitor 1: a wallpaper and a name. Monitor 2: a name and nothing else. Monitors 3 and
    // 4: both claiming "DP-DUP". Monitor 5: a name past the cap, with a wallpaper that must
    // still restore.
    const std::string oversized(wallpaperx::kMaxOutputNameBytes + 1, 'D');
    const std::string config = "{\"version\":1,\"monitors\":["
                               "{\"monitor_id\":1,\"type\":\"static\",\"path\":" +
                               nlohmann::json(present.string()).dump() +
                               ",\"output\":\"DP-1\"},"
                               "{\"monitor_id\":2,\"type\":\"\",\"path\":\"\",\"output\":\"DP-2\"},"
                               "{\"monitor_id\":3,\"type\":\"\",\"path\":\"\",\"output\":\"DP-DUP\"},"
                               "{\"monitor_id\":4,\"type\":\"\",\"path\":\"\",\"output\":\"DP-DUP\"},"
                               "{\"monitor_id\":5,\"type\":\"static\",\"path\":" +
                               nlohmann::json(other.string()).dump() + ",\"output\":" +
                               nlohmann::json(oversized).dump() + "}]}";
    if (!writeFile(file, config)) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config managerConfig;
    managerConfig.configFile = file;
    WallpaperManager manager(managerConfig);
    const std::vector<int> restored = manager.restoreSaved();
    // The two calls backend/daemon/main.cpp makes once it owns the bus name, in that order.
    // restoreSaved() computes the repair this fixture's contested name owes and writes nothing,
    // so without writeRepairedConfig() the file read below would still be the test's own
    // fixture and the two checks on the start's write would grade nothing the daemon did.
    manager.preserveUnreadableConfig();
    manager.writeRepairedConfig();

    // GOAL: S10.5's manager half -- a connector name read out of the config is bound, and
    //       bound independently of whether the wallpaper half of its row restores. A row
    //       carrying only a name must NOT be posted for rendering. False if a named screen
    //       comes back anonymous, or if naming a screen queues a render of nothing.
    // WHY-THIS-SHAPE: the ids posted for rendering are asserted as an exact vector, not just
    //       checked for containing 1. A manager that posted monitor 2 -- a row with an empty
    //       type and path -- would satisfy a contains-style check and would then drive the
    //       render worker for a screen with nothing on it.
    // CONTROL, for the posted-ids check: a row with no wallpaper is stopped three times over,
    //       and it takes all three -- in backend/daemon/wallpaper_manager.cpp, the
    //       `if (entry.type.empty() && entry.path.empty() && !nameTravels)` early-continue in
    //       restoreSaved() disabled, isKnownType() returning true for the empty type, and the
    //       `entry.path.empty()` term dropped from the kept-row arm, which together are what
    //       let a name-only row reach the render arm -> "FAIL: only the rows with a wallpaper
    //       are posted for rendering", printing "posted: 1 2 3 4 5". The first two without the
    //       third left it GREEN, the kept-row arm still catching the row on its empty path.
    //       Observed 2026-08-14. All reverted.
    // CONTROL, for the two binding checks below: the `if (nameValid && entry.monitorId <
    //       kMaxMonitors && bindOutputLocked(...) == BindResult::Displaced)` statement deleted
    //       from WallpaperManager::restoreSaved(), taking the bind with it (`nameValid` is the
    //       local declared a few lines above it) -> "FAIL: a connector name in the config is
    //       bound at startup" and six more in this phase, the snapshot's outputs map coming
    //       back empty, while the posted-ids check above stays green. Observed 2026-08-14.
    //       Reverted.
    check(restored == std::vector<int>({1, 5}), "only the rows with a wallpaper are posted for rendering");
    if (restored != std::vector<int>({1, 5})) {
        std::string ids;
        for (const int id : restored) ids += " " + std::to_string(id);
        std::fprintf(stderr, "  posted:%s\n", ids.empty() ? " (none)" : ids.c_str());
    }

    WallpaperManager::Snapshot snap = manager.snapshot();
    check(snap.outputs.count("DP-1") == 1 && snap.outputs.at("DP-1") == 1,
          "a connector name in the config is bound at startup");
    check(snap.outputs.count("DP-2") == 1 && snap.outputs.at("DP-2") == 2,
          "a connector name on a row with no wallpaper is bound at startup too");

    // GOAL: S10.4 across a restore -- two rows claiming one connector name leave exactly one
    //       monitor answering to it, and the loss is reported. False if both ids resolve for
    //       that name, which is two plugin instances claiming one screen.
    // WHY-THIS-SHAPE: the binding COUNT is asserted, not merely that the name resolves.
    //       "DP-DUP resolves to 4" is true of an implementation that also left 3 bound,
    //       because the snapshot is keyed by name -- the only way to see the second binding
    //       is to count how many names point at monitor 3. The health note is asserted
    //       alongside, because a silent last-wins is a monitor the user set that comes back
    //       nameless with nothing saying so.
    // CONTROL: `++displacedNames;` deleted from restoreSaved()'s bindOutputLocked call in
    //       backend/daemon/wallpaper_manager.cpp -> "FAIL: two config rows claiming one
    //       connector name are reported as a loss", printing the persistence detail without
    //       the clause, while the resolution checks stayed green -- which is what separates
    //       "not reported" from "not resolved". Reverted.
    check(snap.outputs.count("DP-DUP") == 1 && snap.outputs.at("DP-DUP") == 4,
          "two config rows claiming one connector name leave the last one holding it");
    int namesOnThree = 0;
    for (const auto& [name, id] : snap.outputs) {
        (void)name;
        if (id == 3) ++namesOnThree;
    }
    check(namesOnThree == 0, "the monitor that lost a contested connector name answers to no name");
    const std::optional<int> displacedCount =
        clauseCountOrSkip(snap.health.persistence.detail, " screen names displaced by another entry", kGoals);
    if (!displacedCount.has_value()) return;
    const bool displacedReported = *displacedCount == 1;
    check(displacedReported, "two config rows claiming one connector name are reported as a loss");
    if (!displacedReported) {
        std::fprintf(stderr, "  persistence detail: %s\n", snap.health.persistence.detail.c_str());
    }

    // GOAL: a connector name the daemon could not accept is dropped, its row's wallpaper is
    //       restored anyway, and the dropped name is reported. False if an unusable name
    //       costs the wallpaper on the same row, and equally false if it is dropped in
    //       silence -- the user's only clue that a screen must be named again.
    // WHY-THIS-SHAPE: the case is an OVERSIZED name, not a non-UTF-8 one, and that is not
    //       arbitrary. nlohmann's parser rejects an ill-formed UTF-8 byte as a whole-file
    //       syntax error, so no config file can deliver a non-UTF-8 name to this code --
    //       measured. An over-length name is the one shape of unusable name that actually
    //       reaches restoreSaved()'s rejection arm from a file, so it is the only one that
    //       can control it.
    // CONTROL: the `outputNameError(entry.output).empty()` term of `const bool nameValid` in
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp) replaced by
    //       `true`, so nameValid degrades to `!entry.output.empty()` and the oversized name is
    //       accepted and bound -> "FAIL: an unusable connector name is not bound" and "FAIL: an
    //       unusable connector name in the config is reported as dropped", the persistence
    //       detail carrying no dropped-name clause at all. The narrower mutation of only
    //       suppressing `++rejectedNames` reddens the reporting check alone -- it never binds
    //       the name -- so it does not control the first of the two. Re-observed 2026-08-16,
    //       now with six further checks in other phases red beside them, since carrying a name
    //       for a bigger build is gated on the same predicate. Reverted.
    check(snap.outputs.count(oversized) == 0, "an unusable connector name is not bound");
    check(snap.monitors.count(5) == 1 && snap.monitors.at(5).path == other.string(),
          "the wallpaper on a row whose connector name was refused is restored anyway");
    const std::optional<int> rejectedCount =
        clauseCountOrSkip(snap.health.persistence.detail, " screen names dropped, unusable", kGoals);
    if (!rejectedCount.has_value()) return;
    const bool rejectedReported = *rejectedCount == 1;
    check(rejectedReported, "an unusable connector name in the config is reported as dropped");
    if (!rejectedReported) {
        std::fprintf(stderr, "  persistence detail: %s\n", snap.health.persistence.detail.c_str());
    }

    // GOAL: a row carrying only a connector name is NOT counted among "entries this build
    //       cannot apply". False if naming a screen before choosing a wallpaper for it puts
    //       a loss notice in the daemon's persistence health -- which tells the user a
    //       wallpaper did not come back when none was ever set.
    // WHY-THIS-SHAPE: BOTH kept-row clauses are asserted absent, not just the wallpaper one.
    //       Asserting only the wallpaper clause cannot fail: persistenceDetail() counts a row
    //       carrying only a name under its own wording, so a restoreSaved() without that guard
    //       carries these rows and reports them as "screen names ... kept", leaving a check
    //       that looks only for "entries this build cannot apply" green. The property is that
    //       the row is not carried AT ALL, so neither clause may name it.
    // CONTROL: the `if (entry.type.empty() && entry.path.empty() && !nameTravels)` guard in
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp) disabled ->
    //       "FAIL: a screen named with no wallpaper is reported as neither a kept entry nor a
    //       kept name", printing a persistence detail containing "3 screen names this build's
    //       monitor limit excludes, kept". Observed 2026-08-14. Reverted. Measured 2026-08-13:
    //       the single-clause form of this check stayed GREEN under that same mutation.
    const bool noFalseLoss =
        snap.health.persistence.detail.find("entries this build cannot apply") == std::string::npos &&
        snap.health.persistence.detail.find("screen names this build's monitor limit excludes") ==
            std::string::npos;
    check(noFalseLoss, "a screen named with no wallpaper is reported as neither a kept entry nor a kept name");
    if (!noFalseLoss) {
        std::fprintf(stderr, "  persistence detail: %s\n", snap.health.persistence.detail.c_str());
    }

    // GOAL: a name and a wallpaper for one monitor are written back as ONE row. False if the
    //       next save emits two rows with the same monitor_id, which the next load reads as
    //       two monitors claiming one screen -- the ambiguity this whole step exists to
    //       remove. And a screen that has only been named still gets a row of its own.
    // WHY-THIS-SHAPE: read off the disk TWICE -- once straight after the restore, once after a
    //       save triggered by a DIFFERENT monitor -- and the ids are compared as an exact set
    //       each time. Two reads, not one: this fixture's contested name is a repair, so the
    //       start writes the file itself, and the first SetWallpaper is a second, independent
    //       chance to split the row. A single read after the save cannot say which write did
    //       it, and a single read after the restore would pass for a build that splits at the
    //       first SetWallpaper -- the state a user actually reaches.
    //       rowsInFile() keys by monitor id, so a duplicated row is invisible to it; the row
    //       COUNT in the raw file is therefore asserted separately, at both reads.
    // CONTROL: the m_outputs loop in WallpaperManager::persistableLocked()
    //       (backend/daemon/wallpaper_manager.cpp) changed from merging each name into the
    //       row keyed by its monitor id to appending a name-only row of its own -> "FAIL: the
    //       restore's own write puts a monitor's wallpaper and its connector name on one row"
    //       with "5 rows for 4 monitor ids", "FAIL: a monitor's wallpaper and its connector
    //       name are written as one row" with "6 rows for 5 monitor ids", and the two
    //       merged-row checks, neither read finding the name on the row that holds the
    //       wallpaper. Observed 2026-08-14. Reverted.
    // CONTROL, for the first read being of the START's write and not of the fixture this phase
    //       wrote: rowsInFile()'s raw-row count returned one too high -> "FAIL: the restore's
    //       own write puts a monitor's wallpaper and its connector name on one row", printing
    //       "5 rows for 4 monitor ids (read back); ids: 1 2 4 5" -- four ids, where the fixture
    //       above has five. Observed 2026-08-15. Reverted.
    const std::string presentPath = present.string();
    waitForRows(file, [&presentPath](const std::map<int, RowInFile>& rows) {
        return rows.count(1) == 1 && rows.at(1).output == "DP-1" && rows.at(1).path == presentPath;
    });
    std::string repairWhy;
    std::size_t repairRowCount = 0;
    const std::map<int, RowInFile> afterRestore = rowsInFile(file, repairWhy, &repairRowCount);
    const bool oneRowEachAfterRestore = repairRowCount == afterRestore.size() && repairRowCount > 0;
    check(oneRowEachAfterRestore, "the restore's own write puts a monitor's wallpaper and its connector name on one "
                                  "row");
    if (!oneRowEachAfterRestore) {
        std::fprintf(stderr, "  %zu rows for %zu monitor ids (%s); ids: %s\n", repairRowCount, afterRestore.size(),
                     repairWhy.empty() ? "read back" : repairWhy.c_str(), idsOf(afterRestore).c_str());
    }
    const bool mergedAfterRestore = afterRestore.count(1) == 1 && afterRestore.at(1).output == "DP-1" &&
                                    afterRestore.at(1).path == present.string();
    check(mergedAfterRestore, "the row the restore wrote carries both the wallpaper and the connector name");

    manager.requestWallpaper("static", other.string(), 6);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(6) == 1; });
    std::string why;
    std::size_t rowCount = 0;
    const std::map<int, RowInFile> onDisk = rowsInFile(file, why, &rowCount);
    const bool oneRowEach = rowCount == onDisk.size() && rowCount > 0;
    check(oneRowEach, "a monitor's wallpaper and its connector name are written as one row");
    if (!oneRowEach) {
        std::fprintf(stderr, "  %zu rows for %zu monitor ids (%s); ids: %s\n", rowCount, onDisk.size(),
                     why.empty() ? "read back" : why.c_str(), idsOf(onDisk).c_str());
    }
    const bool mergedRow = onDisk.count(1) == 1 && onDisk.at(1).output == "DP-1" &&
                           onDisk.at(1).path == present.string();
    check(mergedRow, "the merged row carries both the wallpaper and the connector name");
    const bool nameOnlyRow = onDisk.count(2) == 1 && onDisk.at(2).output == "DP-2" && onDisk.at(2).path.empty();
    check(nameOnlyRow, "a screen that has only been named keeps a row of its own across a save");
    if (!mergedRow || !nameOnlyRow) {
        std::fprintf(stderr, "  ids on disk: %s\n", idsOf(onDisk).c_str());
    }

    // GOAL: S10.4 through the manager's own entry point -- setMonitorOutput moves a name off
    //       whatever monitor held it, and releases whatever name the target monitor had.
    //       Asserted here as well as over the bus because this is where a save happens, and
    //       the file must reflect the move rather than keeping the stale binding.
    // WHY-THIS-SHAPE: graded on the FILE, not on the snapshot. The bus tier already grades
    //       the in-memory map; what only this tier can see is that the write that follows a
    //       rebinding carries the new mapping and not the old one -- a manager that rebound
    //       in memory and persisted from a stale copy would pass every check in the other
    //       tier and lose the mapping at the next restart.
    // CONTROL: `row.output = name;` in WallpaperManager::persistableLocked()
    //       (backend/daemon/wallpaper_manager.cpp) deleted -> three FAILs: "a rebinding is
    //       written to the file, not just held in memory", "the merged row carries both the
    //       wallpaper and the connector name", and "a screen that has only been named keeps
    //       a row of its own across a save" -- no output key on any row. Reverted.
    manager.setMonitorOutput(7, "DP-1");
    waitForRows(file, [](const std::map<int, RowInFile>& rows) {
        return rows.count(7) == 1 && rows.at(7).output == "DP-1";
    });
    std::string moveWhy;
    const std::map<int, RowInFile> afterMove = rowsInFile(file, moveWhy);
    const bool movedOnDisk = afterMove.count(7) == 1 && afterMove.at(7).output == "DP-1" &&
                             afterMove.count(1) == 1 && !afterMove.at(1).hasOutputKey;
    check(movedOnDisk, "a rebinding is written to the file, not just held in memory");
    if (!movedOnDisk) {
        std::fprintf(stderr, "  %s; monitor 1 output '%s', monitor 7 output '%s'\n",
                     moveWhy.empty() ? "read back" : moveWhy.c_str(),
                     afterMove.count(1) ? afterMove.at(1).output.c_str() : "(no row)",
                     afterMove.count(7) ? afterMove.at(7).output.c_str() : "(no row)");
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6g -- Step 10: what the restore report says about the names it could not keep.
// ---------------------------------------------------------------------------- //
void runOutputLossReportChecks() {
    constexpr const char* kGoals = "S10.4/S10.5's reporting half (every screen name a restore could not keep is "
                                   "reported, and none of them is reported as a wallpaper)";
    const ScratchDir scratch("outputloss");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths) return;
    const std::filesystem::path& file = paths->config;
    const std::filesystem::path& present = paths->wallpapers[0];

    // The kinds of unusable row, at DIFFERENT multiplicities -- 2, 1, 4, 3, 2, 2, 1. The
    // counts are what the checks below match, so a build that reported the right numbers
    // under swapped clauses could not pass.
    const std::vector<int> keptWallpaperIds{40, 41};
    const std::vector<int> keptNameIds{42};
    const std::vector<int> droppedWallpaperIds{-1, -2, -4, -6};
    const std::vector<int> droppedNameIds{-3, -5, -7};
    // The three shapes a row whose id no build can use takes when it is not simply one
    // wallpaper or one name: both halves at once, a name no build could ever accept, and a
    // row with nothing on it at all. Each is counted differently from the two above.
    const std::vector<int> droppedBothIds{-8, -10};
    const std::vector<int> droppedUnusableNameIds{-9, -11};
    const std::vector<int> droppedEmptyIds{-12};
    // The rows with an id this build CAN use whose name it still refuses. Counted under the
    // same clause as droppedUnusableNameIds: both are names no build accepts.
    const std::vector<int> refusedInRangeIds{2};
    const std::string oversized(wallpaperx::kMaxOutputNameBytes + 1, 'D');

    nlohmann::json monitors = nlohmann::json::array();
    // Monitor 1 twice, under two different names: the second row claims a screen the first
    // already named, which is the monitor-side collision a bind resolves.
    monitors.push_back(configRow(1, "", "", "DP-1"));
    monitors.push_back(configRow(1, "", "", "HDMI-A-1"));
    // Monitor 2: no wallpaper, and a name too long for this build to hold.
    for (const int id : refusedInRangeIds) monitors.push_back(configRow(id, "", "", oversized));
    for (const int id : keptWallpaperIds) monitors.push_back(configRow(id, "static", "/kept.png", ""));
    for (const int id : keptNameIds) monitors.push_back(configRow(id, "", "", "DP-K" + std::to_string(id)));
    for (const int id : droppedWallpaperIds) monitors.push_back(configRow(id, "static", "/gone.png", ""));
    for (const int id : droppedNameIds) monitors.push_back(configRow(id, "", "", "DP-N" + std::to_string(-id)));
    for (const int id : droppedBothIds) {
        monitors.push_back(configRow(id, "static", "/gone.png", "DP-B" + std::to_string(-id)));
    }
    for (const int id : droppedUnusableNameIds) monitors.push_back(configRow(id, "", "", oversized));
    for (const int id : droppedEmptyIds) monitors.push_back(configRow(id, "", "", ""));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config managerConfig;
    managerConfig.configFile = file;
    WallpaperManager manager(managerConfig);
    const std::vector<int> restored = manager.restoreSaved();
    check(restored.empty(), "a config whose only usable rows carry no wallpaper posts nothing for rendering");
    const WallpaperManager::Snapshot snap = manager.snapshot();
    const std::string& detail = snap.health.persistence.detail;
    // The count is compared as a number, not searched for as text: "3 screen names dropped"
    // is a substring of "13 screen names dropped", so a find() would pass for a build that
    // over-counted by ten. A count the harness could not read is not compared at all -- it is
    // no number, and grading it against a fixture size accuses the daemon of losing every row
    // the clause counts. `countsReadable` gates each group of verdicts below.
    bool countsReadable = true;
    const auto reports = [&detail, &countsReadable, kGoals](const std::string& clause, std::size_t expected) {
        const std::optional<int> count = clauseCountOrSkip(detail, clause, kGoals);
        if (!count.has_value()) {
            countsReadable = false;
            return false;
        }
        return *count == static_cast<int>(expected);
    };

    // GOAL: S10.4's monitor-side clause across a restore -- two rows naming the SAME monitor
    //       differently leave it answering to one name, and the name it lost is reported.
    //       False if the loss is silent: the user's screen comes back under a name the app
    //       never gave it, and the persistence health -- the only place a lost name is ever
    //       mentioned -- says nothing happened.
    // WHY-THIS-SHAPE: the fixture collides on the MONITOR, never on the name, which is the
    //       only way to grade this half. The neighbouring phase's two-rows-one-name fixture
    //       exercises the name-side release, and that path reported a loss even before this
    //       one did -- so a check built on it stays green with the monitor-side release
    //       reporting nothing. The binding count is asserted beside the report, because a
    //       silent last-wins and a correct one are indistinguishable from the map alone.
    // CONTROL: `displaced = true;` deleted from the monitor-side release loop in
    //       WallpaperManager::bindOutputLocked (backend/daemon/wallpaper_manager.cpp), so it
    //       returns Bound -> "FAIL: the name a second row took off a monitor is reported as a
    //       loss", printing a detail with no displaced clause, while both binding checks
    //       stayed green -- which is what separates "not reported" from "not released".
    //       Reverted.
    int namesOnMonitorOne = 0;
    for (const auto& [name, id] : snap.outputs) {
        (void)name;
        if (id == 1) ++namesOnMonitorOne;
    }
    check(namesOnMonitorOne == 1, "a monitor named twice in the config answers to exactly one name");
    check(snap.outputs.count("HDMI-A-1") == 1 && snap.outputs.at("HDMI-A-1") == 1,
          "the name the later row gave that monitor is the one it answers to");
    const bool displacedReported = reports(" screen names displaced by another entry", 1);
    if (!countsReadable) return;
    check(displacedReported, "the name a second row took off a monitor is reported as a loss");
    if (!displacedReported || namesOnMonitorOne != 1) {
        std::fprintf(stderr, "  %d names on monitor 1; persistence detail: %s\n", namesOnMonitorOne, detail.c_str());
    }

    // GOAL: every name no build could ever accept is reported as dropped and unusable,
    //       whether the id carrying it is one this build can bind or one no build can. False
    //       if either kind is left out: the user is told to name that screen again only for
    //       the names the report happens to notice, and the rest disappear in silence.
    // WHY-THIS-SHAPE: one clause, one exact count, summed over TWO id lists that are counted
    //       under different rules elsewhere -- an in-range row (whose name is refused because
    //       this build would bind it and cannot) and negative-id rows (whose name is refused
    //       because nothing can travel on an id no build will read back). Asserting the clause
    //       is merely present passes for a build that reports one of the two kinds, which is
    //       the state this fixture found.
    // CONTROL: `if (!entry.output.empty() && !nameValid && !nameTravels) ++rejectedNames;` in
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp) moved
    //       below the negative-id branch, so a name on an id no build can use is never
    //       examined -> "FAIL: every name no build could accept is reported as dropped and
    //       unusable", printing a detail whose clause read "1 screen names dropped, unusable"
    //       against 3 expected. Observed 2026-08-14. Reverted.
    const std::size_t unusableNames = refusedInRangeIds.size() + droppedUnusableNameIds.size();
    const bool unusableReported = reports(" screen names dropped, unusable", unusableNames);
    if (!countsReadable) return;
    check(unusableReported, "every name no build could accept is reported as dropped and unusable");
    if (!unusableReported) {
        std::fprintf(stderr, "  persistence detail: %s\n  expected %zu of ' screen names dropped, unusable'\n",
                     detail.c_str(), unusableNames);
    }

    // GOAL: Step 10's split of the persistence report -- a row carrying only a screen's name
    //       is counted as a name, never as a wallpaper, on both the kept and the dropped
    //       side. False if naming a screen puts "entries this build cannot apply" or "entries
    //       dropped" in front of a user whose wallpapers all came back: that text is what
    //       someone reads when a picture is missing, and a name was never a picture.
    // WHY-THIS-SHAPE: four clauses at four DIFFERENT multiplicities, each derived from the
    //       fixture rather than written as a literal. The obvious version -- one kept name and
    //       one dropped name -- passes for a build that merely renamed the clauses while still
    //       counting name rows and wallpaper rows together, because every count would read 1
    //       either way. With 2/1/4/3 a flat count reads 3 and 7 and no arrangement of the old
    //       wording matches.
    // CONTROL: the whole flat-counting regime put back in backend/daemon/wallpaper_manager.cpp
    //       -- persistenceDetail() reporting `m_unapplied.size()` and `m_droppedEntries` with
    //       neither name clause, AND restoreSaved()'s negative-id branch counting every drop
    //       into m_droppedEntries -> all four FAILs, the detail reading "3 entries this build
    //       cannot apply, kept; 12 entries dropped, no build can apply them". Both sites are
    //       needed: mutating persistenceDetail() alone leaves the dropped-name rows counted in
    //       m_droppedNames, so the dropped-wallpaper clause still reads 7 and its check stays
    //       green. Re-run 2026-08-14. Reverted.
    // CONTROL, for comparing each count as a number rather than searching for it as text:
    //       `keptNames` printed as `keptNames + 10` in WallpaperManager::persistenceDetail()
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: only the out-of-range row's name
    //       is reported as a name this build's limit excludes". With that one clause put back
    //       to a std::string::find of "1 screen names this build's monitor limit excludes,
    //       kept", the same mutated build read GREEN: the expected text is a substring of
    //       "11 screen names ...", so a search has no left boundary and an over-count of ten
    //       satisfies it. Observed 2026-08-14. Both reverted.
    // COVERAGE-ASSERT: the expected numbers are computed from the id lists, so deleting rows
    //       from the fixture changes what is demanded rather than quietly weakening it.
    //
    // GOAL, on the dropped pair specifically: a row an id no build can use takes off the disk
    //       is counted under EVERY heading it really lost. False under a partition of the two
    //       counters: a row carrying a wallpaper and a name is then reported as a lost
    //       wallpaper only, so the user is never told to name that screen again; a row
    //       carrying neither is reported as a lost name, which it never had.
    // WHY-THIS-SHAPE: the two expected numbers are sums over id lists chosen so that the
    //       partition moves BOTH of them and in opposite directions -- two rows with both
    //       halves push the wallpaper count up, one row with nothing on it pushes it up again
    //       while the partition files it under names. Balanced lists were the trap here: with
    //       only the both-halves rows and the unusable-name rows in play the partition scores
    //       7 and 5 as well, because the two shifts cancel exactly.
    // CONTROL: the two counted conditions in restoreSaved()'s negative-id branch
    //       (backend/daemon/wallpaper_manager.cpp) replaced by the partition
    //       `if (!entry.type.empty() || !entry.path.empty()) ++m_droppedEntries; else
    //       ++m_droppedNames;` -> "FAIL: only the rows with a wallpaper are reported as
    //       entries no build can apply" and "FAIL: the name-only rows no build can apply are
    //       reported as names, not as entries", the detail reading "6 entries dropped" against
    //       7 and "6 screen names dropped" against 5. Observed 2026-08-14. Reverted.
    //       Separately, `if (nameValid)` in that branch widened to `if (!entry.output.empty())`
    //       -- the shape that reported an unusable name as a name a build could have used ->
    //       "FAIL: the name-only rows no build can apply are reported as names, not as
    //       entries", the detail reading "7 screen names dropped, no build can apply them".
    //       Observed 2026-08-14. Reverted.
    const std::size_t keptWallpapers = keptWallpaperIds.size();
    const std::size_t keptNames = keptNameIds.size();
    const std::size_t droppedWallpapers = droppedWallpaperIds.size() + droppedBothIds.size() + droppedEmptyIds.size();
    const std::size_t droppedNames = droppedNameIds.size() + droppedBothIds.size();
    const bool keptWallpapersReported = reports(" entries this build cannot apply, kept", keptWallpapers);
    const bool keptNamesReported = reports(" screen names this build's monitor limit excludes, kept", keptNames);
    const bool droppedWallpapersReported = reports(" entries dropped, no build can apply them", droppedWallpapers);
    const bool droppedNamesReported = reports(" screen names dropped, no build can apply them", droppedNames);
    if (!countsReadable) return;
    check(keptWallpapersReported, "only the rows with a wallpaper are reported as entries this build cannot apply");
    check(keptNamesReported, "only the out-of-range row's name is reported as a name this build's limit excludes");
    check(droppedWallpapersReported, "only the rows with a wallpaper are reported as entries no build can apply");
    check(droppedNamesReported, "the name-only rows no build can apply are reported as names, not as entries");
    if (!keptWallpapersReported || !keptNamesReported || !droppedWallpapersReported || !droppedNamesReported) {
        std::fprintf(stderr, "  persistence detail: %s\n  expected %zu kept wallpapers, %zu kept names, %zu dropped "
                             "wallpapers, %zu dropped names\n",
                     detail.c_str(), keptWallpapers, keptNames, droppedWallpapers, droppedNames);
    }

    // The save that lets the on-disk half of the refused-name goal above be read. Made for a
    // monitor none of the fixture rows mention, so nothing it writes can be confused for a
    // row the restore kept.
    manager.requestWallpaper("static", present.string(), 8);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(8) == 1; });
    std::string why;
    std::size_t rowsWritten = 0;
    const std::map<int, RowInFile> onDisk = rowsInFile(file, why, &rowsWritten);
    // COVERAGE-ASSERT: the absence of the refused rows below is only evidence if the file was
    // written and read at all -- an empty or unparseable file has no such row either.
    const bool saveLanded = onDisk.count(8) == 1;
    check(saveLanded, "the save this phase reads back really happened");
    if (!saveLanded) {
        std::fprintf(stderr, "  %s; ids on disk: %s\n", why.empty() ? "read back" : why.c_str(),
                     idsOf(onDisk).c_str());
    } else {
        // GOAL: the two rows this fixture gives monitor 1 come back as one row. False if both
        //       are written: the next start reads two monitors claiming one screen again, so
        //       the collision this run reported as resolved is back, on every boot.
        // WHY-THIS-SHAPE: the RAW row count, because every other check in this phase reads the
        //       file through rowsInFile(), which keys by monitor id -- two rows for id 1 and
        //       one row for id 1 are the same map, so nothing else here can see the duplicate.
        //       Both numbers come from the ONE read above rather than from a second read of the
        //       same path: the daemon's writer can replace the file between two reads, and a
        //       count taken from a later version than the ids reports a correct save as a
        //       duplicated row.
        // CONTROL: every m_unapplied entry appended to the returned vector as well as being
        //       merged into the id-keyed rows, in WallpaperManager::persistableLocked()
        //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: no monitor is written on two
        //       rows of the config", printing "8 rows for 5 monitor ids". Observed 2026-08-14.
        //       Reverted.
        const bool oneRowPerMonitor = rowsWritten == onDisk.size() && rowsWritten > 0;
        check(oneRowPerMonitor, "no monitor is written on two rows of the config");
        if (!oneRowPerMonitor) {
            std::fprintf(stderr, "  %zu rows for %zu monitor ids; ids: %s\n", rowsWritten, onDisk.size(),
                         idsOf(onDisk).c_str());
        }

        // GOAL: a row with no wallpaper and a name this build refused leaves nothing in the
        //       config. False if an empty row is written: the next start reads it back and
        //       reports it as a wallpaper that did not come back, on every boot forever.
        // WHY-THIS-SHAPE: graded on the file, not on whether the row is held in memory. Being
        //       held is no longer observable from outside -- such a row is stripped of its
        //       name as it is kept, so it moves no counter -- and the file is where the harm
        //       would be.
        // CONTROL: two mutations together, because this property is defended twice and
        //       neither defence alone reaches the file: the `if (entry.type.empty() &&
        //       entry.path.empty())` guard in WallpaperManager::restoreSaved() disabled, so
        //       the row is held, AND the empty-row skip in
        //       WallpaperManager::persistableLocked() (both in
        //       backend/daemon/wallpaper_manager.cpp) put back to an unconditional
        //       `entries.push_back(std::move(row));` -> "FAIL: a wallpaper-less row whose name
        //       was refused leaves no row in the config at all", printing "ids on disk: 1 2 8
        //       40 41 42" with monitor 2 empty in every field. Each mutation alone leaves this
        //       green; the save-side one alone is controlled on its own in the phase below,
        //       where a held row really is emptied. Observed 2026-08-14. Both reverted.
        std::vector<int> refusedRowsOnDisk;
        for (const int id : refusedInRangeIds) {
            if (onDisk.count(id) != 0) refusedRowsOnDisk.push_back(id);
        }
        const bool refusedRowGone = refusedRowsOnDisk.empty();
        check(refusedRowGone, "a wallpaper-less row whose name was refused leaves no row in the config at all");
        if (!refusedRowGone) {
            std::fprintf(stderr, "  ids on disk: %s; monitor %d type '%s' path '%s' output '%s'\n",
                         idsOf(onDisk).c_str(), refusedRowsOnDisk.front(),
                         onDisk.at(refusedRowsOnDisk.front()).type.c_str(),
                         onDisk.at(refusedRowsOnDisk.front()).path.c_str(),
                         onDisk.at(refusedRowsOnDisk.front()).output.c_str());
        }
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6g1 -- S10.21: a kept wallpaper a live monitor has taken the id of.
// ---------------------------------------------------------------------------- //
void runKeptWallpaperSupersededChecks() {
    constexpr const char* kGoals = "S10.21 (the report promises back only the kept wallpapers the next save really "
                                   "writes, so a kept row a live monitor has superseded stops being counted)";
    const ScratchDir scratch("keptsuperseded");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;
    const std::filesystem::path& file = paths->config;
    const std::filesystem::path& wallpaper = paths->wallpapers[0];

    // A type only a newer build understands, on a monitor id this build can use: kept at load,
    // and dropped by the first save that has live state for that id.
    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(0, "hologram", "/x.png", ""));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();

    // COVERAGE-ASSERT: the report has to promise the row back BEFORE the client acts, or the
    // check below is green over a build that never counted it at all -- and over a fixture the
    // daemon rejected outright.
    const std::string kept = " entries this build cannot apply, kept";
    const std::optional<int> before = clauseCountOrSkip(manager.snapshot().health.persistence.detail, kept, kGoals);
    if (!before.has_value()) return;
    check(*before == 1, "a wallpaper this build cannot apply is reported as kept before anything else is set");
    if (*before != 1) {
        std::fprintf(stderr, "  persistence detail: %s\n", manager.snapshot().health.persistence.detail.c_str());
    }

    // GOAL: S10.21 -- once a live monitor takes the id a kept row was holding, the report stops
    //       saying that row is kept. False if it keeps counting: the save has already stopped
    //       writing the row, so the user is told for the life of the install that a wallpaper a
    //       newer build set is preserved, and finds it gone when they reinstall that build.
    // WHY-THIS-SHAPE: the row's absence from the FILE is asserted beside the count, in the same
    //       phase. Grading the count alone cannot tell "the report is honest" from "the report
    //       and the save both keep it", which is a different build and not a defect; grading the
    //       file alone is what the save-side phases already do and says nothing about the text
    //       the user reads.
    // CONTROL: TWO mutations together, because this property is now defended twice and neither
    //       defence alone changes the count (both in backend/daemon/wallpaper_manager.cpp, both
    //       reverted): the `&& !liveHoldsLocked(entry.monitorId)` guard removed from the
    //       kept-wallpaper count in WallpaperManager::persistenceDetail(), AND the
    //       `dropKeptWallpaper(*kept)` call in WallpaperManager::requestWallpaper() disabled, so
    //       the kept row keeps its type and path -> "FAIL: a kept wallpaper a live monitor has
    //       replaced is no longer reported as kept", printing "1 of ' entries this build cannot
    //       apply, kept' after the row was dropped", with the file check green -- the save and
    //       the report disagreeing is the defect itself. Each mutation alone leaves this GREEN
    //       (measured): the row is emptied on the request, so the count is zero whatever the
    //       guard says, and the guard is false whatever the row holds. The request-side one is
    //       controlled on its own in the displaced-wallpaper phase below, where the note it
    //       writes is what is graded. Observed 2026-08-18.
    manager.requestWallpaper("static", wallpaper.string(), 0);
    waitForRows(file, [&wallpaper](const std::map<int, RowInFile>& rows) {
        return rows.count(0) == 1 && rows.at(0).path == wallpaper.string();
    });
    std::string why;
    const std::map<int, RowInFile> onDisk = rowsInFile(file, why);
    const bool droppedFromFile = onDisk.count(0) == 1 && onDisk.at(0).type == "static";
    check(droppedFromFile, "the save really stopped writing the kept row this phase is about");
    if (!droppedFromFile) {
        std::fprintf(stderr, "  %s; monitor 0 holds type '%s' path '%s'\n", why.empty() ? "read back" : why.c_str(),
                     onDisk.count(0) ? onDisk.at(0).type.c_str() : "(no row)",
                     onDisk.count(0) ? onDisk.at(0).path.c_str() : "(no row)");
        return;
    }
    const std::string detail = manager.snapshot().health.persistence.detail;
    const std::optional<int> after = clauseCountOrSkip(detail, kept, kGoals);
    if (!after.has_value()) return;
    check(*after == 0, "a kept wallpaper a live monitor has replaced is no longer reported as kept");
    if (*after != 0) {
        std::fprintf(stderr, "  %d of '%s' after the row was dropped; persistence detail: %s\n", *after, kept.c_str(),
                     detail.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6g1b -- S10.19 at runtime: a client claiming the monitor a kept row holds.
// ---------------------------------------------------------------------------- //

// How many times `needle` occurs in `haystack`, without overlaps. The note this phase grades is
// added by appending to one string, so a build that adds it twice produces a detail that still
// CONTAINS it -- find() alone cannot tell "reported" from "reported on every save".
int occurrencesOf(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    int found = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++found;
    }
    return found;
}

void runKeptWallpaperDisplacedAtRuntimeChecks() {
    constexpr const char* kGoals = "S10.19 at runtime (a wallpaper kept for a newer build that a client displaces by "
                                   "setting that monitor is reported once, at the moment it is displaced, and is gone "
                                   "from the kept set rather than promised back)";
    const ScratchDir scratch("keptdisplaced");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 2);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;

    // "hologram" is a type no build here knows, so the row is KEPT for a newer build rather than
    // applied -- which is what makes displacing it a loss the user cannot recover from anywhere
    // else. A `video` row would not do: this build knows that type and applies it, so no kept
    // row would exist to displace.
    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(0, "hologram", "/newer-build.holo", ""));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();

    // The note is recomposed into the health detail only when a write has come back, so a
    // snapshot taken while the save is still in flight legitimately shows the detail as it was.
    // Polled with a bound, and an expired bound is returned as a verdict to the caller rather
    // than being read as "the note is not there" -- the callers below grade the count they get,
    // and a wait that ran out prints its own reason.
    // The count is read out of one aggregated clause, not counted as repeated sentences: the
    // daemon renders this loss the way it renders every other repeatable one, as "N ..." emitted
    // once. clauseCount() answers 0 for a clause that is absent, which is what the
    // nothing-was-displaced check below needs, and a build that reported the same loss twice
    // reads back as 2 -- so all three controls named below still redden it.
    const std::string kNote = " entries this build cannot apply, replaced by new wallpapers";
    const auto noteCountWhenSettled = [&manager, &kNote](int wanted) {
        const auto deadline = StallClock::now() + std::chrono::seconds(10);
        int count = 0;
        for (;;) {
            count = clauseCount(manager.snapshot().health.persistence.detail, kNote).value_or(-1);
            if (count >= wanted || StallClock::now() >= deadline) return count;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };

    // GOAL: a wallpaper this build cannot apply, displaced by a client setting that monitor, is
    //       reported ONCE at the moment it is displaced. False before this fix: persistableLocked()
    //       dropped the kept row in favour of live state and persistenceDetail() stopped counting
    //       it, so a picture a newer build had set left config.json with nothing anywhere saying
    //       so -- the identical collision at RESTORE time was reported all along, so the user was
    //       told only when the file, and not a client, was what took the id.
    // WHY-THIS-SHAPE: three saves in one order that no cheaper phase can replace. A save on a
    //       monitor NO kept row holds comes first, so the note is established to be attributable
    //       to the displacement rather than to any save at all; the displacing save comes second;
    //       and a SECOND save on the same monitor comes last, because the note is appended to one
    //       string that lives for the process -- a build that re-adds it on every SetWallpaper
    //       would satisfy a find() and leave a user reading "1 entry ... replaced" nine times for
    //       one lost picture. Occurrences are counted, not searched for, for that reason. Each
    //       count is taken after a bounded poll rather than at the instant the call returned,
    //       because the note reaches the health detail only when the write comes back and
    //       persist() abandons its own wait after two seconds: sampling at the return would read
    //       a slow host as a daemon that said nothing.
    // CONTROL: three, all in WallpaperManager::requestWallpaper()
    //       (backend/daemon/wallpaper_manager.cpp), all reverted. (a) the whole erase-and-note
    //       block disabled, which is the code before this fix -> "FAIL: a kept wallpaper a client
    //       displaced is reported at the moment it is displaced", printing "the note appears 0
    //       times", and "FAIL: setting the same monitor again does not report the same loss a
    //       second time", printing "the note appears 0 times after a second save". (b) the row
    //       counted without being cleared -- `dropKeptWallpaper(*kept)` at that call site replaced
    //       by a bare emptiness test, which is the plausible half-fix -> "FAIL: setting the same
    //       monitor again does not report the same loss a second time", printing "the note appears
    //       2 times after a second save; persistence detail: 2 entries this build cannot apply,
    //       replaced by new wallpapers for those screens; ...", with the first two checks GREEN.
    //       (c) the `row.type.empty() && row.path.empty()` guard deleted from
    //       WallpaperManager::dropKeptWallpaper(), so a row already emptied is counted again ->
    //       the same single check red with the same count of 2, and nothing else in this file red:
    //       this phase is the only defender of that guard.
    //       All three re-observed 2026-08-18 against the aggregated clause, after the daemon
    //       stopped appending one sentence per displacement, and all three reverted. Reading the
    //       count out of the clause is what keeps (b) and (c) red: a search for the sentence would
    //       find one occurrence either way.
    // COVERAGE-ASSERT: the kept row is asserted to have been promised back BEFORE any client
    //       acts. Without it every count below is equally true of a fixture the daemon rejected
    //       outright, and the phase would be green over a manager that never kept anything.
    const std::string keptClause = " entries this build cannot apply, kept";
    const std::optional<int> promisedBefore =
        clauseCountOrSkip(manager.snapshot().health.persistence.detail, keptClause, kGoals);
    if (!promisedBefore.has_value()) return;
    check(*promisedBefore == 1, "the wallpaper this phase displaces was promised back before any client acted");
    if (*promisedBefore != 1) {
        std::fprintf(stderr, "  %d promised back; persistence detail: %s\n", *promisedBefore,
                     manager.snapshot().health.persistence.detail.c_str());
        return;
    }

    // A monitor no kept row holds. Nothing is lost here, so nothing may be reported.
    manager.requestWallpaper("static", paths->wallpapers[1].string(), 1);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(1) == 1; });
    const int noteAfterUnrelated =
        clauseCount(manager.snapshot().health.persistence.detail, kNote).value_or(-1);
    check(noteAfterUnrelated == 0, "a save that displaces no kept row reports no displacement");
    if (noteAfterUnrelated != 0) {
        std::fprintf(stderr, "  the note appears %d times after a save on a monitor no kept row holds\n",
                     noteAfterUnrelated);
    }

    manager.requestWallpaper("static", paths->wallpapers[0].string(), 0);
    const int noteAfterDisplacing = noteCountWhenSettled(1);
    check(noteAfterDisplacing == 1, "a kept wallpaper a client displaced is reported at the moment it is displaced");
    if (noteAfterDisplacing != 1) {
        std::fprintf(stderr, "  the note appears %d times; persistence detail: %s\n", noteAfterDisplacing,
                     manager.snapshot().health.persistence.detail.c_str());
    }

    // NOT checked here, deliberately: that the report stops promising the row back. It is true
    // either way, so no mutation of this fix can redden it -- persistenceDetail() already filters
    // kept rows by whether a live monitor holds the id, which the phase above owns and controls.
    // What the erase is observable through is the second save below: an uncleared row makes the
    // note fire again.

    // The second save on the same monitor. Waited for on the FILE, so the count below is taken
    // from a daemon whose second write has settled -- a count read at the return could miss a
    // note that had been added and not yet folded into the detail, and would pass for exactly the
    // build this check exists to catch.
    manager.requestWallpaper("static", paths->wallpapers[1].string(), 0);
    waitForRows(file, [&paths](const std::map<int, RowInFile>& rows) {
        return rows.count(0) == 1 && rows.at(0).path == paths->wallpapers[1].string();
    });
    const int noteAfterSecond =
        clauseCount(manager.snapshot().health.persistence.detail, kNote).value_or(-1);
    check(noteAfterSecond == 1, "setting the same monitor again does not report the same loss a second time");
    if (noteAfterSecond != 1) {
        std::fprintf(stderr, "  the note appears %d times after a second save; persistence detail: %s\n",
                     noteAfterSecond, manager.snapshot().health.persistence.detail.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6g2 -- S10.19: two rows claiming one monitor this build can apply.
// ---------------------------------------------------------------------------- //
void runDuplicateLiveRowChecks() {
    constexpr const char* kGoals = "S10.19 (a second config row for a monitor this build CAN apply loses the first "
                                   "row's wallpaper, and that loss is counted and named)";
    const ScratchDir scratch("duplicatelive");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 2);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;
    const std::filesystem::path& first = paths->wallpapers[0];
    const std::filesystem::path& second = paths->wallpapers[1];

    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(0, "static", first.string(), ""));
    monitors.push_back(configRow(0, "static", second.string(), ""));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    const std::vector<int> restored = manager.restoreSaved();
    const std::string detail = manager.snapshot().health.persistence.detail;

    // GOAL: S10.19 -- a wallpaper the config lost to a second row for the same monitor is
    //       reported, and the monitor is posted for rendering once. False on both counts before
    //       this fix: the daemon reported a clean restore over a file that had just cost the
    //       user a picture, and queued the same screen for two renders. The identical collision
    //       between two rows this build CANNOT apply was reported all along, so the user was
    //       told only when the loss happened to a wallpaper this build cannot draw.
    // WHY-THIS-SHAPE: the count is read out of the report as a number and the render queue is
    //       compared as a whole vector. Asserting only that monitor 0 holds the later wallpaper
    //       passes for the build that lost the earlier one in silence -- last-wins is the
    //       policy, and it is not what is missing. The queue is graded because a monitor listed
    //       twice is a second full render of the same request.
    // CONTROL: the `alreadyRestored` test deleted from the applicable-row arm of
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp), which is
    //       the code before this fix -> "FAIL: a wallpaper a later row for the same monitor
    //       displaced is reported as a loss", printing a detail with no such clause, and "FAIL:
    //       a monitor named by two rows is posted for rendering once", printing "0 0".
    //       Observed 2026-08-15.
    const std::string duplicateClause = " entries dropped, another entry claimed the same monitor";
    const std::optional<int> duplicates = clauseCountOrSkip(detail, duplicateClause, kGoals);
    if (!duplicates.has_value()) return;
    check(*duplicates == 1, "a wallpaper a later row for the same monitor displaced is reported as a loss");
    if (*duplicates != 1) {
        std::fprintf(stderr, "  persistence detail: %s\n  expected 1 of '%s'\n", detail.c_str(),
                     duplicateClause.c_str());
    }
    const bool postedOnce = restored == std::vector<int>{0};
    check(postedOnce, "a monitor named by two rows is posted for rendering once");
    if (!postedOnce) {
        std::string ids;
        for (const int id : restored) ids += std::to_string(id) + " ";
        std::fprintf(stderr, "  posted: %s\n", ids.empty() ? "(none)" : ids.c_str());
    }
    // Last-wins is the policy, and it is asserted so this phase cannot be satisfied by a build
    // that reports the loss and then keeps the wrong row.
    const WallpaperManager::Snapshot snap = manager.snapshot();
    const bool laterWon = snap.monitors.count(0) == 1 && snap.monitors.at(0).path == second.string();
    check(laterWon, "the later of two rows for one monitor is the one restored");
    if (!laterWon) {
        std::fprintf(stderr, "  monitor 0 holds '%s'\n",
                     snap.monitors.count(0) ? snap.monitors.at(0).path.c_str() : "(no row)");
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6g2b -- S10.19 across the two containers: one row this build can apply and
// one it cannot, both claiming the same monitor.
// ---------------------------------------------------------------------------- //
void runCrossContainerDuplicateChecks() {
    constexpr const char* kGoals = "S10.19 across both containers (a monitor id claimed by one row this build can "
                                   "apply and one it cannot loses a wallpaper, and that loss is counted and named "
                                   "rather than dropped at the next save in silence)";

    // Both file orders, because the two are different code: one row reaches the kept container
    // first and is superseded by the live one, and the other finds the live monitor already
    // there. Before the fix each order lost the same wallpaper in the same silence, and a phase
    // written against one of them would leave the other's arm untested.
    struct Ordering {
        const char* what;
        bool keptRowFirst;
    };
    const Ordering orderings[] = {
        {"the row this build cannot apply comes first", true},
        {"the row this build can apply comes first", false},
    };

    int graded = 0;
    for (const Ordering& ordering : orderings) {
        const ScratchDir scratch("crossdup");
        const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
        if (!paths.has_value()) return;
        const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
        const std::filesystem::path& file = paths->config;
        const std::filesystem::path& wallpaper = paths->wallpapers.front();

        // "hologram" is a type no build here knows, so its row is KEPT for a newer build rather
        // than applied -- which is what puts the two rows in different containers. A `video` row
        // would not: this build knows that type and merely cannot draw it, so both rows would
        // take the same arm and the collision the goal is about never arises.
        nlohmann::json monitors = nlohmann::json::array();
        const nlohmann::json keptRow = configRow(3, "hologram", "/newer-build.holo", "");
        const nlohmann::json liveRow = configRow(3, "static", wallpaper.string(), "");
        monitors.push_back(ordering.keptRowFirst ? keptRow : liveRow);
        monitors.push_back(ordering.keptRowFirst ? liveRow : keptRow);
        nlohmann::json root;
        root["version"] = 1;
        root["monitors"] = monitors;
        if (!writeFile(file, root.dump())) {
            notVerified(kGoals, std::string("the config file the '") + ordering.what + "' case reads could not be "
                                                                                       "written");
            return;
        }

        // GOAL: S10.19 across the two containers -- a config holding, for one monitor id, a row
        //       this build can apply and a row it cannot loses one wallpaper, and the user is
        //       told. False before this fix: the two rows took different arms, so nothing counted
        //       a collision, the report said the restore was clean, and the first ordinary save
        //       deleted the row a newer build had written -- from the one file that held it, with
        //       no copy anywhere and no note ever.
        // WHY-THIS-SHAPE: the report and the FILE are graded together, and the file is graded
        //       after the repair has been written rather than after a client save. Grading the
        //       count alone passes for a build that counts and still writes the dead row; grading
        //       the file alone passes for the pre-fix build, which dropped the row correctly and
        //       said nothing -- the loss and the silence are separate properties and the silence
        //       is the defect. The raw row count is asserted, not just the ids: a map keyed by
        //       monitor id cannot tell one row for monitor 3 from two.
        // CONTROL: two, one per arm of the fix, both in
        //       backend/daemon/wallpaper_manager.cpp's restoreSaved(). (a) the `liveHolds`
        //       clearing removed from the kept-row arm -> the "row this build can apply comes
        //       first" case red on "a monitor claimed across both containers reports the
        //       wallpaper it lost", printing 0 losses, and on "the repair leaves one row for that
        //       monitor", printing "2 rows for 1 monitor id". (b) the `keptIndex.find` erase
        //       removed from the applicable-row arm -> the other case red the same way.
        WallpaperManager::Config config;
        config.configFile = file;
        WallpaperManager manager(config);
        const std::vector<int> restored = manager.restoreSaved();
        const std::string detail = manager.snapshot().health.persistence.detail;

        const std::optional<int> duplicates =
            clauseCountOrSkip(detail, " entries dropped, another entry claimed the same monitor", kGoals);
        if (!duplicates.has_value()) return;
        ++graded;
        const bool counted = *duplicates == 1;
        check(counted, (std::string("a monitor claimed across both containers reports the wallpaper it lost, when ") +
                        ordering.what)
                           .c_str());
        if (!counted) std::fprintf(stderr, "  %d losses reported; persistence detail: %s\n", *duplicates, detail.c_str());

        // The other half of the same silence: a row the next save will not write must not be
        // promised back to the user as preserved either.
        const std::optional<int> kept =
            clauseCountOrSkip(detail, " entries this build cannot apply, kept", kGoals);
        if (!kept.has_value()) return;
        const bool notPromised = *kept == 0;
        check(notPromised, (std::string("the dead row is not promised back as kept, when ") + ordering.what).c_str());
        if (!notPromised) std::fprintf(stderr, "  %d promised kept; persistence detail: %s\n", *kept, detail.c_str());

        const bool postedOnce = restored == std::vector<int>{3};
        check(postedOnce, (std::string("a monitor named by rows in both containers is posted for rendering once, "
                                       "when ") +
                           ordering.what)
                              .c_str());
        if (!postedOnce) {
            std::string ids;
            for (const int id : restored) ids += std::to_string(id) + " ";
            std::fprintf(stderr, "  posted: %s\n", ids.empty() ? "(none)" : ids.c_str());
        }

        // The repair, not a client save: the point of counting the collision at restore is that
        // the rewrite happens at startup, so a user who never sets another wallpaper still ends
        // up with a file that does not lose a row silently later.
        manager.preserveUnreadableConfig();
        manager.writeRepairedConfig();
        // Waited on the RAW row count, not on the map. writeRepairedConfig() queues its write and
        // returns, so this is a real wait -- and the map cannot serve as its predicate: it is
        // keyed by monitor id, so for the ordering whose LAST row is the applicable one, "monitor
        // 3 holds the wallpaper" is already true of the two-row file as the fixture wrote it. That
        // predicate returned at once and graded whichever sample the read landed on; under the
        // load of a neighbouring phase's ten-second poll it read the unwritten file and failed a
        // correct daemon, three runs out of three. One row is a state the fixture cannot be in.
        const auto repairLanded = [&file, &wallpaper] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (;;) {
                std::string ignored;
                std::size_t raw = 0;
                const std::map<int, RowInFile> rows = rowsInFile(file, ignored, &raw);
                if (raw == 1 && rows.count(3) == 1 && rows.at(3).path == wallpaper.string()) return true;
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        };
        // An expired wait is left to the check below rather than swallowed: ten seconds without
        // the repair is a verdict on the daemon, and the check prints the rows it found.
        repairLanded();
        std::string why;
        std::size_t rawRows = 0;
        const std::map<int, RowInFile> onDisk = rowsInFile(file, why, &rawRows);
        const bool oneRow = rawRows == 1 && onDisk.count(3) == 1 && onDisk.at(3).type == "static" &&
                            onDisk.at(3).path == wallpaper.string();
        check(oneRow, (std::string("the repair leaves one row for that monitor, when ") + ordering.what).c_str());
        if (!oneRow) {
            std::fprintf(stderr, "  %s; %zu rows for %zu monitor ids: %s\n", why.empty() ? "read back" : why.c_str(),
                         rawRows, onDisk.size(), idsOf(onDisk).c_str());
        }

        // GOAL: the note's promise is kept -- it now tells the user the rows it dropped "are only
        //       in the copy beside the config file", and that copy really holds them. False if
        //       the note names a place the rows are not: the user is sent to a file that does not
        //       have their picture in it, which is worse than not being told where to look.
        // WHY-THIS-SHAPE: the copy's BYTES are compared against the fixture, not merely its
        //       existence. A copy taken after the rewrite exists too and holds the repaired file,
        //       which is exactly the row the user is looking for having been removed -- and that
        //       ordering defect has shipped here before (S10.14's ordering half).
        // CONTROL: writeRepairedConfig() moved above preserveUnreadableConfig() in this phase,
        //       which is the caller ordering main.cpp is required not to use -> "FAIL: the rows
        //       the note says are only in the copy really are in the copy, when the row this
        //       build cannot apply comes first", printing a copy holding one row where the
        //       fixture had two.
        const bool copyHasTheDroppedRow = readFile(preservedCopyDir(scratch) / "config.json.bad") == root.dump();
        check(copyHasTheDroppedRow,
              (std::string("the rows the note says are only in the copy really are in the copy, when ") +
               ordering.what)
                  .c_str());
        if (!copyHasTheDroppedRow) {
            std::fprintf(stderr, "  the copy holds: %s\n  the fixture was: %s\n",
                         readFile(preservedCopyDir(scratch) / "config.json.bad").c_str(), root.dump().c_str());
        }
    }

    // COVERAGE-ASSERT: both file orders were really graded. Each exercises a different arm of the
    // fix, so a phase that quietly stopped running one of them would leave that arm untested
    // while still reporting a full green.
    check(graded == static_cast<int>(std::size(orderings)),
          "both file orders of the cross-container collision were graded");
    if (graded != static_cast<int>(std::size(orderings))) {
        std::fprintf(stderr, "  %d of %zu orderings graded\n", graded, std::size(orderings));
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6g3b -- S10.19: what "a live monitor owns this id" is allowed to mean.
// ---------------------------------------------------------------------------- //
void runStrictOwnershipChecks() {
    constexpr const char* kGoals = "S10.19's ownership test in its strict form (a kept row is surrendered only to a "
                                   "live monitor that actually HOLDS a wallpaper, so a monitor this daemon records "
                                   "without one cannot silently drop a user's kept row)";
    const ScratchDir scratch("strictownership");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;

    // A row for monitor 0 -- an id this build CAN bind -- kept only because "hologram" is a type
    // no build here knows. That is the row a loosened ownership test would give away.
    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(0, "hologram", "/newer-build.holo", ""));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();

    // COVERAGE-ASSERT: the row this phase protects is really being kept before anything is
    // driven. Without it every observation below holds trivially for a manager that kept nothing.
    const std::optional<int> keptBefore =
        clauseCountOrSkip(manager.snapshot().health.persistence.detail, " entries this build cannot apply, kept",
                          kGoals);
    if (!keptBefore.has_value()) return;
    check(*keptBefore == 1, "the kept row this phase protects is there before anything is driven");
    if (*keptBefore != 1) return;

    // GOAL: no call a client can make records monitor 0 as live-but-wallpaper-less, and the kept
    //       row for it survives every one of them. The strict ownership test -- liveHoldsLocked()
    //       asks whether a live monitor HOLDS a wallpaper, not whether its id is present -- is
    //       what makes the second half true if the first ever stops being.
    // WHY-THIS-SHAPE: the strict form and the loose one are equivalent today, so the difference
    //       between them cannot be observed through any interface: nothing records a monitor
    //       without a wallpaper. Asserting the difference directly is therefore impossible, and a
    //       check that pretended to would be vacuous. What this asserts instead is the
    //       PRECONDITION under which they agree, driven across every public call that could
    //       plausibly break it, together with the row whose survival depends on it. When a future
    //       change does record a placeholder monitor -- a screen named before anything is chosen
    //       for it is the obvious one -- this goes red at that change rather than at the config
    //       file, which is where the loss would otherwise first be visible, one restart later.
    // CONTROL: two mutations, applied together and separately, both in
    //       backend/daemon/wallpaper_manager.cpp, both reverted. (a) `m_monitors[monitorId];`
    //       added to WallpaperManager::setMonitorOutput(), which is the placeholder monitor this
    //       check exists to catch -> "FAIL: no call this phase drives records a monitor without a
    //       wallpaper", printing that line for all three calls, with the kept row still safe --
    //       the strict test doing its job. (b) the same with liveHoldsLocked() ALSO loosened to
    //       `m_monitors.count(monitorId) != 0` -> both red, the second printing "the kept row for
    //       monitor 0 is of type ''", the row surviving as an empty shell with the user's
    //       wallpaper gone from it. That is the loss (a) exists to fail before.
    // COVERAGE-ASSERT: three calls are asserted to have been driven, against a literal three.
    //       CONTROL: the render entry deleted from the table, which is what dropping an awkward
    //       call looks like -> "FAIL: all three calls this phase means to drive were driven",
    //       printing "2 calls driven, expected 3", and nothing else. Reverted.
    struct Touch {
        const char* what;
        void (*drive)(WallpaperManager&);
    };
    static constexpr Touch kTouches[] = {
        {"naming the screen", [](WallpaperManager& m) { m.setMonitorOutput(0, "DP-KEPT"); }},
        {"asking it to render what has not been set", [](WallpaperManager& m) { m.renderPending(0); }},
        {"asking it for a frame", [](WallpaperManager& m) { m.exportCurrentFrame(0); }},
    };
    int driven = 0;
    bool placeholderSeen = false;
    for (const Touch& touch : kTouches) {
        touch.drive(manager);
        ++driven;
        for (const auto& [monitorId, status] : manager.snapshot().monitors) {
            if (!status.type.empty() || !status.path.empty()) continue;
            placeholderSeen = true;
            std::fprintf(stderr, "  monitor %d is recorded with no wallpaper after: %s\n", monitorId, touch.what);
        }
    }
    check(!placeholderSeen, "no call this phase drives records a monitor without a wallpaper");
    // Against a literal, not against std::size(kTouches): the table's own size is true by
    // construction and stays green after an entry is deleted, which is the edit this guards.
    check(driven == 3, "all three calls this phase means to drive were driven");
    if (driven != 3) std::fprintf(stderr, "  %d calls driven, expected 3\n", driven);

    // The row itself, read from the file the last save left: the ownership test decides what
    // persistableLocked() writes, so this is where a loosened one is paid for.
    const bool survived = waitForRows(file, [](const std::map<int, RowInFile>& rows) {
        return rows.count(0) == 1 && rows.at(0).type == "hologram";
    });
    check(survived, "the kept row for a monitor nothing has been set on survives the save");
    if (!survived) {
        std::string why;
        const std::map<int, RowInFile> rows = rowsInFile(file, why);
        std::fprintf(stderr, "  %s; the kept row for monitor 0 is %s; ids on disk: %s\n",
                     why.empty() ? "read back" : why.c_str(),
                     rows.count(0) == 0 ? "gone from the file" : ("of type '" + rows.at(0).type + "'").c_str(),
                     idsOf(rows).c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6g4 -- S10.19 for names: a carried screen name a client takes over.
// ---------------------------------------------------------------------------- //
void runKeptNameTakenOverAtRuntimeChecks() {
    constexpr const char* kGoals = "S10.19 for names at runtime (a screen name carried for a build with more monitors, "
                                   "taken over by a client naming a monitor this build CAN bind, is reported rather "
                                   "than dropped in silence)";
    const ScratchDir scratch("nametakenover");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;

    // Monitor 100 is above this build's limit of 32, which is the ONLY reason its name is
    // carried rather than dropped: a larger kMaxMonitors would bind it. "hologram" keeps the
    // row itself unapplied too, so the row survives the save that follows.
    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(100, "hologram", "/newer-build.holo", "DP-CARRIED"));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();

    const std::string carriedClause = " screen names this build's monitor limit excludes, kept";
    const std::string takenClause = " screen names this build cannot bind, taken over by new names";

    // COVERAGE-ASSERT: the name really is being carried before any client acts. Without it every
    // check below is equally true of a fixture whose name the restore had already dropped, and
    // the phase would grade a daemon that never carried anything.
    const std::optional<int> carriedBefore =
        clauseCountOrSkip(manager.snapshot().health.persistence.detail, carriedClause, kGoals);
    if (!carriedBefore.has_value()) return;
    check(*carriedBefore == 1, "the screen name this phase takes over was promised back before any client acted");
    if (*carriedBefore != 1) {
        std::fprintf(stderr, "  %d carried; persistence detail: %s\n", *carriedBefore,
                     manager.snapshot().health.persistence.detail.c_str());
        return;
    }

    // GOAL: a client naming a monitor this build CAN bind takes a name away from a row carried
    //       for a build with more monitors, and the daemon says so. False before this fix: the
    //       name was dropped inside persistableLocked(), which is const, runs on every save and
    //       cannot report -- so the screen the user had named on their other machine came back
    //       anonymous with nothing anywhere saying why. Each mutator is expected to erase and
    //       report first, and setMonitorOutput() had no such patch while requestWallpaper() did.
    // WHY-THIS-SHAPE: three observations, not one. That the taken-over clause appears is not
    //       enough on its own -- the simpler check, and it would pass for a build that reported
    //       the loss and went on promising the name back as well, which is the more confusing
    //       report of the two. So the carried clause is asserted to have gone, and the FILE is
    //       read to confirm the name really did move rather than being reported as lost while
    //       still on the carried row. The startup twin below is what pins the asymmetry the fix
    //       was about: the same collision arriving from the file was reported all along.
    // CONTROL: the erase-and-count block in WallpaperManager::setMonitorOutput()
    //       (backend/daemon/wallpaper_manager.cpp) disabled, which is the code before this fix ->
    //       "FAIL: a carried screen name a client takes over is reported at the moment it is
    //       taken", printing "the report counts 0" beside a detail that still promises the row's
    //       wallpaper back. Reverted. The other two checks here are each defended TWICE in
    //       production, so that one mutation cannot redden them, and each needed the second
    //       defence removed as well -- both observed, both reverted:
    //       the promise check with `m_outputs.count(entry.output) == 0` dropped from
    //       persistenceDetail()'s keptNames test -> "FAIL: the report stops promising back a
    //       screen name a client has taken", printing "1 still promised";
    //       the file check with `m_outputs.count(row.output) != 0` dropped from
    //       persistableLocked()'s clearing loop -> "FAIL: the name the report says was taken over
    //       really did leave the carried row", printing "ids on disk: 0 100" with both rows
    //       carrying the name -- two monitors answering to one screen at the next start.
    manager.setMonitorOutput(0, "DP-CARRIED");
    const std::string detail = manager.snapshot().health.persistence.detail;

    const std::optional<int> takenOver = clauseCountOrSkip(detail, takenClause, kGoals);
    const std::optional<int> carriedAfter = clauseCountOrSkip(detail, carriedClause, kGoals);
    if (!takenOver.has_value() || !carriedAfter.has_value()) return;
    check(*takenOver == 1, "a carried screen name a client takes over is reported at the moment it is taken");
    if (*takenOver != 1) std::fprintf(stderr, "  the report counts %d; detail: %s\n", *takenOver, detail.c_str());
    check(*carriedAfter == 0, "the report stops promising back a screen name a client has taken");
    if (*carriedAfter != 0) std::fprintf(stderr, "  %d still promised; detail: %s\n", *carriedAfter, detail.c_str());

    std::string why;
    const std::map<int, RowInFile> rows = rowsInFile(file, why);
    const bool moved = rows.count(0) == 1 && rows.at(0).output == "DP-CARRIED" &&
                       (rows.count(100) == 0 || !rows.at(100).hasOutputKey);
    check(moved, "the name the report says was taken over really did leave the carried row");
    if (!moved) {
        std::string carrying;
        for (const auto& [id, row] : rows) {
            if (row.output == "DP-CARRIED") carrying += " " + std::to_string(id);
        }
        std::fprintf(stderr, "  %s; ids on disk: %s; rows carrying 'DP-CARRIED':%s\n",
                     why.empty() ? "read back" : why.c_str(), idsOf(rows).c_str(),
                     carrying.empty() ? " (none)" : carrying.c_str());
    }

    // The startup twin of the same collision, in its own manager over its own file: a row this
    // build CAN bind claims the name a carried row also claims. This was reported before the fix
    // and must still be, which is what makes the check above a statement about the runtime path
    // rather than about reporting in general.
    const ScratchDir twinScratch("nametakenovertwin");
    const std::optional<PhasePaths> twinPaths = preparePhase(twinScratch, kGoals, 1);
    if (!twinPaths.has_value()) return;
    const ScopedEnv twinStateHome("XDG_STATE_HOME", stateHomeOf(twinScratch).string());
    nlohmann::json twinMonitors = nlohmann::json::array();
    twinMonitors.push_back(configRow(0, "static", twinPaths->wallpapers[0].string(), "DP-CARRIED"));
    twinMonitors.push_back(configRow(100, "hologram", "/newer-build.holo", "DP-CARRIED"));
    nlohmann::json twinRoot;
    twinRoot["version"] = 1;
    twinRoot["monitors"] = twinMonitors;
    if (!writeFile(twinPaths->config, twinRoot.dump())) {
        notVerified(kGoals, "the config file the startup twin reads could not be written");
        return;
    }
    WallpaperManager::Config twinConfig;
    twinConfig.configFile = twinPaths->config;
    WallpaperManager twin(twinConfig);
    twin.restoreSaved();
    const std::optional<int> displaced = clauseCountOrSkip(twin.snapshot().health.persistence.detail,
                                                          " screen names displaced by another entry", kGoals);
    if (!displaced.has_value()) return;
    check(*displaced == 1, "the same collision arriving from the config file is reported too");
    if (*displaced != 1) {
        std::fprintf(stderr, "  %d displaced; persistence detail: %s\n", *displaced,
                     twin.snapshot().health.persistence.detail.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6g5 -- S10.19: the two losses a client can cause over and over.
// ---------------------------------------------------------------------------- //
void runRepeatedRuntimeLossesAreBoundedChecks() {
    constexpr const char* kGoals = "S10.19's reporting bound (the two losses a client can cause repeatedly are "
                                   "rendered as one clause carrying a count, so N of them cannot crowd the counts "
                                   "and the config path out of a report truncated at 1024 bytes)";
    const ScratchDir scratch("repeatedlosses");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;

    // Twelve of each loss. The number is chosen against the daemon's own 1024-byte cap on the
    // health detail: one sentence per occurrence runs past it at this count, and the cap drops
    // the TAIL, which is where the config path and every count live. Fewer would leave the
    // pre-fix build inside its budget and the phase would pass against it.
    constexpr int kEach = 12;
    nlohmann::json monitors = nlohmann::json::array();
    for (int i = 0; i < kEach; ++i) {
        // Ids this build can bind, kept only because the type is one no build here knows: a
        // client setting that monitor is what displaces them.
        monitors.push_back(configRow(i, "hologram", "/newer-build-" + std::to_string(i) + ".holo", ""));
        // Ids above this build's limit, carrying names a client can take over.
        monitors.push_back(configRow(100 + i, "hologram", "/future-" + std::to_string(i) + ".holo",
                                     "DP-CARRIED-" + std::to_string(i)));
    }
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();

    const std::string replacedClause = " entries this build cannot apply, replaced by new wallpapers for those screens";
    const std::string takenClause = " screen names this build cannot bind, taken over by new names";

    // COVERAGE-ASSERT: all twenty-four losses this phase counts were really available to be
    // caused. Without it a fixture the restore had thrown away would leave every count below
    // trivially satisfied by a daemon that reported nothing at all.
    const std::string before = manager.snapshot().health.persistence.detail;
    const std::optional<int> keptBefore = clauseCountOrSkip(before, " entries this build cannot apply, kept", kGoals);
    const std::optional<int> namesBefore =
        clauseCountOrSkip(before, " screen names this build's monitor limit excludes, kept", kGoals);
    if (!keptBefore.has_value() || !namesBefore.has_value()) return;
    const bool fixtureStands = *keptBefore == 2 * kEach && *namesBefore == kEach;
    check(fixtureStands, "the fixture offers twelve displaceable wallpapers and twelve takeable names");
    if (!fixtureStands) {
        std::fprintf(stderr, "  %d kept entries and %d carried names; persistence detail: %s\n", *keptBefore,
                     *namesBefore, before.c_str());
        return;
    }

    // GOAL: twelve occurrences of a loss produce ONE clause carrying twelve, not twelve
    //       sentences. False before this fix: each occurrence appended a fixed sentence to a
    //       note string that lives for the whole process and is never trimmed, while the health
    //       detail is capped at 1024 bytes and the cap drops the tail -- so a dozen of them
    //       pushed the counts and the config path, the things the message exists to carry, out
    //       of the report entirely. A client can cause both of these as often as it likes.
    // WHY-THIS-SHAPE: three assertions per loss, because each alone is passable by the build the
    //       goal is about. The count alone is satisfied by a report that also repeats itself; the
    //       occurrence count alone is satisfied by one clause carrying the wrong number; and
    //       neither is what the user actually loses -- the config path is, so it is asserted
    //       present in the same detail. clauseCount() reads the FIRST occurrence, which is why
    //       clauseOccurrences() exists beside it.
    // CONTROL: the two counter increments replaced by the per-occurrence sentence they replaced
    //       -- `++m_replacedKept` in WallpaperManager::requestWallpaper() and `++m_takenOverNames`
    //       in WallpaperManager::setMonitorOutput() (backend/daemon/wallpaper_manager.cpp) each
    //       made an addPersistenceNote() of a fixed sentence -> all three checks here red: both
    //       count checks printing "the clause appears 0 times and reads 0", and "FAIL: the report
    //       still names the config file after twelve losses of each kind" printing "1059 bytes of
    //       detail, not naming <path>", the detail being twelve identical sentences and a
    //       thirteenth cut mid-word. Three more went red in the two phases above, which grade the
    //       same two counters one occurrence at a time. Observed 2026-08-18, reverted.
    for (int i = 0; i < kEach; ++i) {
        manager.requestWallpaper("static", paths->wallpapers[0].string(), i);
    }
    for (int i = 0; i < kEach; ++i) {
        manager.setMonitorOutput(kEach + i, "DP-CARRIED-" + std::to_string(i));
    }

    // Read once, after both bursts: every assertion below is about the same report, which is
    // what the user is shown.
    const std::string detail = manager.snapshot().health.persistence.detail;
    const std::optional<int> replaced = clauseCountOrSkip(detail, replacedClause, kGoals);
    const std::optional<int> taken = clauseCountOrSkip(detail, takenClause, kGoals);
    if (!replaced.has_value() || !taken.has_value()) return;

    const int replacedTimes = clauseOccurrences(detail, replacedClause);
    const bool replacedBounded = *replaced == kEach && replacedTimes == 1;
    check(replacedBounded, "twelve displaced wallpapers are reported as one clause counting twelve");
    if (!replacedBounded) {
        std::fprintf(stderr, "  the clause appears %d times and reads %d\n", replacedTimes, *replaced);
    }
    const int takenTimes = clauseOccurrences(detail, takenClause);
    const bool takenBounded = *taken == kEach && takenTimes == 1;
    check(takenBounded, "twelve taken-over screen names are reported as one clause counting twelve");
    if (!takenBounded) {
        std::fprintf(stderr, "  the clause appears %d times and reads %d\n", takenTimes, *taken);
    }
    // What the truncation actually costs, asserted directly rather than by measuring the length:
    // a length bound would need a number nobody can justify, while the config path is the thing
    // the message exists to name and it sits at the very end, so it is the first casualty.
    const bool pathSurvives = detail.find(file.string()) != std::string::npos;
    check(pathSurvives, "the report still names the config file after twelve losses of each kind");
    if (!pathSurvives) {
        std::fprintf(stderr, "  %zu bytes of detail, not naming %s: %s\n", detail.size(), file.string().c_str(),
                     detail.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6g3 -- S10.20: which kept row keeps a name several of them claim.
// ---------------------------------------------------------------------------- //
void runKeptNameLastWinsChecks() {
    constexpr const char* kGoals = "S10.20 (of the kept rows claiming one screen name, the one the FILE names last "
                                   "keeps it, which is what the report says happened)";
    const ScratchDir scratch("keptnamelastwins");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;

    // Three rows, all above this build's monitor limit, so all are kept for a bigger build. The
    // third names the screen last and names monitor 5000, whose row was created by the FIRST
    // row -- so the position of a kept row and the last claim on a name are different facts.
    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(5000, "static", "/a.png", "DP-9"));
    monitors.push_back(configRow(6000, "static", "/b.png", "DP-9"));
    monitors.push_back(configRow(5000, "", "", "DP-9"));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();
    manager.preserveUnreadableConfig();
    manager.writeRepairedConfig();

    // GOAL: S10.20 -- the row the file names LAST for a screen keeps that name. False if the
    //       resolution goes by the order the rows sit in memory: a later row for a monitor id
    //       already seen is merged into the place that id first appeared, so the two orders
    //       differ, and the wrong row wins while the health note says "the last entry won". The
    //       binding is then written to disk, so a build with a larger monitor limit brings the
    //       screen back attached to the monitor the config did not last ask for.
    // WHY-THIS-SHAPE: the fixture needs three rows and a repeated id -- with one row per id, the
    //       memory order and the file order agree and every build passes. It is graded on the
    //       FILE rather than on the health text, because the text says "the last entry won"
    //       whichever row won. The count is asserted too: exactly one name is lost here, and a
    //       build that cleared both rows' names would otherwise satisfy "6000 does not hold it".
    // CONTROL: the reverse-order sweep put back in place of the last-claim pass at the end of
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp), which is
    //       the code before this fix -> "FAIL: the kept row the file names last for a screen is
    //       the one that keeps the name", printing "monitor 5000 output '', monitor 6000 output
    //       'DP-9'", and nothing else in the file red. Re-observed 2026-08-15 with the
    //       tightened wait below.
    // CONTROL, for the monitor-id -> kept-row index that replaced the per-entry scan of
    //       m_unapplied in restoreSaved(): `keptIndex.emplace(entry.monitorId, 0)` -- an index
    //       whose entries exist but point at the wrong row -> twelve red across this phase and
    //       "the rows kept for a build with more screens", among them "the kept row keeps its
    //       wallpaper when the name it carried is taken from it" and "two kept rows for one
    //       monitor leave the later row's wallpaper in the file". Dropping the emplace entirely,
    //       so no id is ever found again, reddens eleven of the same. Nothing distinguishes the
    //       index from the scan it replaced, and nothing should: the change is an algorithmic one
    //       for a file capped at ~28,000 rows, and its whole claim is that the result is
    //       identical. What is established is that the merge these checks grade is the code the
    //       index feeds.
    // Waited for the REPAIRED content, not merely for both ids: the fixture already has a row
    // for each, so a wait on their presence is satisfied by the file as it was written here and
    // grades whichever sample the read lands on -- which made this check fail about one run in
    // four before it was tightened. An expired wait is left to the checks below: ten seconds
    // without the repair is a verdict on the daemon, not on the host.
    waitForRows(file, [](const std::map<int, RowInFile>& rows) {
        return rows.count(5000) == 1 && rows.at(5000).output == "DP-9" && rows.count(6000) == 1 &&
               rows.at(6000).output.empty();
    });
    std::string why;
    const std::map<int, RowInFile> onDisk = rowsInFile(file, why);
    if (onDisk.count(5000) != 1 || onDisk.count(6000) != 1) {
        check(false, "the repair this phase reads back was really written");
        std::fprintf(stderr, "  %s; ids on disk: %s\n", why.empty() ? "read back" : why.c_str(),
                     idsOf(onDisk).c_str());
        return;
    }
    const bool lastClaimWon = onDisk.at(5000).output == "DP-9" && onDisk.at(6000).output.empty();
    check(lastClaimWon, "the kept row the file names last for a screen is the one that keeps the name");
    if (!lastClaimWon) {
        std::fprintf(stderr, "  monitor 5000 output '%s', monitor 6000 output '%s'\n",
                     onDisk.at(5000).output.c_str(), onDisk.at(6000).output.c_str());
    }

    const std::string detail = manager.snapshot().health.persistence.detail;
    const std::optional<int> displaced =
        clauseCountOrSkip(detail, " screen names displaced by another entry", kGoals);
    if (!displaced.has_value()) return;
    check(*displaced == 1, "exactly one name is reported lost when three rows claim one screen");
    if (*displaced != 1) std::fprintf(stderr, "  persistence detail: %s\n", detail.c_str());
}

// ---------------------------------------------------------------------------- //
// Phase 6h -- Step 10: the rows kept for a newer build, once names share the file.
// ---------------------------------------------------------------------------- //
void runPreservedRowChecks() {
    constexpr const char* kGoals = "S8.14's Step-10 half (a row kept for a newer build keeps both halves of what it "
                                   "carried, never keeps a connector name another row already carries, and carries a "
                                   "name this build cannot bind rather than deleting it)";
    const ScratchDir scratch("preservedrows");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 2);
    if (!paths) return;
    const std::filesystem::path& file = paths->config;
    const std::filesystem::path& present = paths->wallpapers[0];
    const std::filesystem::path& other = paths->wallpapers[1];

    // A row kept for a newer build has two independent halves -- a wallpaper (type and path)
    // and a screen name -- and every combination below is a case the merge has to get right.
    // Two names this build refuses, distinct so the file can be asked which one it holds: one
    // on an id this build binds, one on an id only a bigger build could.
    const std::string oversized(wallpaperx::kMaxOutputNameBytes + 1, 'D');
    const std::string oversizedBindable(wallpaperx::kMaxOutputNameBytes + 9, 'E');
    nlohmann::json monitors = nlohmann::json::array();
    // A live screen and a row this build cannot apply, both claiming "DP-3".
    monitors.push_back(configRow(1, "static", present.string(), "DP-3"));
    // A live screen carrying a name this build could not use: the third outcome a name can
    // have, beside bound (monitor 1) and carried (monitor 47).
    monitors.push_back(configRow(3, "static", "/kept3.png", oversizedBindable));
    monitors.push_back(configRow(40, "static", "/kept.png", "DP-3"));
    // Two rows this build cannot apply for one monitor id: a real collision, both halves.
    monitors.push_back(configRow(41, "static", "/first.png", ""));
    monitors.push_back(configRow(41, "static", "/second.png", ""));
    // Complementary halves for one id, in both file orders. Neither pair collides: each row
    // fills a half the other left empty.
    monitors.push_back(configRow(64, "video", "/clip.mp4", ""));
    monitors.push_back(configRow(64, "", "", "DP-5"));
    monitors.push_back(configRow(65, "", "", "DP-6"));
    monitors.push_back(configRow(65, "video", "/other.mp4", ""));
    // Two names for one id, which IS a collision -- of the name half.
    monitors.push_back(configRow(66, "", "", "DP-7"));
    monitors.push_back(configRow(66, "", "", "DP-8"));
    // One name on two kept rows, with no live screen holding it.
    monitors.push_back(configRow(42, "static", "/dup-a.png", "DP-9"));
    monitors.push_back(configRow(43, "static", "/dup-b.png", "DP-9"));
    // One row carrying both halves, with nothing else in play: its name is uncontested, no
    // live screen holds it, and it merges with no other row. The both-headings check below
    // rests on this row.
    monitors.push_back(configRow(44, "static", "/kept44.png", "DP-10"));
    // A kept row carrying a name this build could not use but a bigger build might.
    monitors.push_back(configRow(47, "static", "/kept47.png", oversized));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config managerConfig;
    managerConfig.configFile = file;
    WallpaperManager manager(managerConfig);
    const std::vector<int> restored = manager.restoreSaved();
    check(restored == std::vector<int>{1, 3}, "only the in-range rows are posted for rendering");
    const WallpaperManager::Snapshot snap = manager.snapshot();

    // GOAL: a kept row for a monitor id this build cannot apply is replaced, not silently
    //       discarded, when a second row claims the same id -- and the wallpaper that survives
    //       is the later one. False if the first row wins with nothing said: the next save
    //       then deletes a wallpaper a newer build set, which is the exact loss keeping the
    //       row exists to prevent, and the report the user consults says only that some rows
    //       were kept.
    // WHY-THIS-SHAPE: graded on WHICH path is in the file, not on how many rows there are.
    //       The file is keyed by monitor id on the way out, so both the losing build and the
    //       correct one write exactly one row for this monitor -- a row-count check cannot
    //       tell them apart. The report is asserted beside it because a build could pick the
    //       right row and still say nothing about the one it dropped.
    // CONTROL: the find-and-overwrite in WallpaperManager::restoreSaved()
    //       (backend/daemon/wallpaper_manager.cpp) short-circuited so every kept row is a bare
    //       `m_unapplied.push_back(...)` -> "FAIL: two kept rows for one monitor leave the
    //       later row's wallpaper in the file", printing "monitor 41 holds '/first.png'", and
    //       "FAIL: every kept row a later row really overwrote is reported as a loss, and no
    //       other" with no such clause in the detail at all, nothing having been overwritten.
    //       Six more went red with them, the merge being what every other check here rests on.
    //       Re-observed 2026-08-14. Reverted.
    // COVERAGE-ASSERT: the expected number of losses is the length of the collision list
    //       below, so adding a colliding pair to the fixture without it being reported fails
    //       rather than quietly leaving the count alone.
    const std::string& detail = snap.health.persistence.detail;
    // The count is compared as a number, not searched for as text: "1 entries dropped"
    // is a substring of "11 entries dropped", so a find() would pass for a build that
    // over-counted by ten.
    // A count the harness could not read is not compared at all -- it is no number, and grading
    // it against a fixture size accuses the daemon of losing every row the clause counts.
    // `countsReadable` gates each group of verdicts below.
    bool clauseCountsReadable = true;
    const auto reports = [&detail, &clauseCountsReadable, kGoals](const std::string& clause, std::size_t expected) {
        const std::optional<int> count = clauseCountOrSkip(detail, clause, kGoals);
        if (!count.has_value()) {
            clauseCountsReadable = false;
            return false;
        }
        return *count == static_cast<int>(expected);
    };
    // Only monitor 41's two rows collide on the WALLPAPER half. Monitor 66's two collide on
    // the name half, which is a lost name and counted below, not here; the other repeated ids
    // (64, 65) fill complementary halves and are not losses at all.
    const std::vector<int> collidingIds{41};
    const std::string duplicateClause = " entries dropped, another entry claimed the same monitor";
    const bool duplicateReported = reports(duplicateClause, collidingIds.size());
    if (!clauseCountsReadable) return;
    check(duplicateReported, "every kept row a later row really overwrote is reported as a loss, and no other");
    if (!duplicateReported) {
        std::fprintf(stderr, "  persistence detail: %s\n  expected %zu of '%s'\n", detail.c_str(),
                     collidingIds.size(), duplicateClause.c_str());
    }

    // GOAL: every screen name a kept row lost is reported as a lost NAME, and the count is
    //       exactly the names really lost. Two ways to be false, and this fixture has both:
    //       reporting a name a later row took as a dropped WALLPAPER tells a user whose
    //       pictures all came back to go looking for a missing picture; and settling a name
    //       collision in the save instead of at the load reports nothing at all, because the
    //       save is const and cannot count.
    // WHY-THIS-SHAPE: an exact count summed over a list of the three DIFFERENT ways a name is
    //       lost here -- one monitor named twice among kept rows (66), one name on two kept
    //       rows (42 loses it to 43), one name a live screen already answers to (40). Checking
    //       that the clause is merely present cannot separate them: each of the three alone
    //       prints the same clause. The duplicate-wallpaper count above is asserted in the
    //       same breath, since the failure being ruled out is a loss counted under the other
    //       heading, which leaves the total right and both clauses wrong.
    // CONTROL: `++displacedNames` in the two-kept-rows-one-name arm of
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp) put back
    //       to `++duplicateIds` -> "FAIL: every screen name a kept row lost
    //       is reported as a lost name, and no other" with the detail reading "2 screen names
    //       displaced" against 3, and "FAIL: every kept row a later row really overwrote is
    //       reported as a loss, and no other" reading "2 entries dropped" against 1.
    //       Separately, the whole last-claim reconciliation loop at the end of restoreSaved()
    //       deleted -> the same first FAIL reading "1 screen names displaced" against 3, with
    //       eight other checks across three phases. Re-observed 2026-08-15 against the reworked
    //       loop. Both reverted.
    // COVERAGE-ASSERT: the expected number is the length of the list, so a fixture row that
    //       stops losing its name fails here rather than quietly lowering the bar.
    const std::vector<int> namesLostIds{40, 42, 66};
    const std::string displacedClause = " screen names displaced by another entry";
    const bool displacedReported = reports(displacedClause, namesLostIds.size());
    if (!clauseCountsReadable) return;
    check(displacedReported, "every screen name a kept row lost is reported as a lost name, and no other");
    if (!displacedReported) {
        std::fprintf(stderr, "  persistence detail: %s\n  expected %zu of '%s'\n", detail.c_str(),
                     namesLostIds.size(), displacedClause.c_str());
    }

    // GOAL: Fix 5's property -- a kept row that carries BOTH an unrenderable wallpaper and a
    //       screen name is reported under both headings. False if the two counts partition the
    //       rows: this build can bind that name no more than it can draw that wallpaper, so a
    //       report that mentions only the wallpaper tells the user their screen will come back
    //       named when it will not.
    // WHY-THIS-SHAPE: exact counts derived from two ID LISTS that deliberately overlap -- six
    //       rows keep a name, eight keep a wallpaper, and five rows are in both. The obvious
    //       alternative, asserting that each clause is merely present, passes for the
    //       partition too: this fixture has one name-only row (66) and rows with wallpapers,
    //       so a partitioning build still prints both clauses, with the wrong numbers.
    //       Monitor 44 is in the fixture for this check alone: it is the one row in both lists
    //       with nothing else riding on it. Every other overlapping row is also something
    //       else -- 43 took a contested name from 42, 64 and 65 are two config rows merged,
    //       47 carries a name this build cannot bind -- so a later rule about any of those
    //       could empty the overlap and leave this check green while covering nothing.
    // CONTROL: the counting loop in WallpaperManager::persistenceDetail()
    //       (backend/daemon/wallpaper_manager.cpp) replaced by the if/else partition
    //       `if (type.empty() && path.empty()) ++keptNames; else ++keptWallpapers;` -> "FAIL:
    //       every kept row carrying a screen name is reported as one", the detail reading
    //       "1 screen names this build's monitor limit excludes, kept" -- only monitor 66,
    //       whose row has no wallpaper to be partitioned under -- against six expected. The
    //       kept-wallpaper check stayed green at eight, which is what separates "counted once"
    //       from "not counted". Re-observed 2026-08-14. Reverted.
    // Every kept row holding a name the next save will really leave behind, and every kept row
    // holding a wallpaper. Monitors 40 and 42 are in the second list only: a live screen holds
    // "DP-3" and a later kept row took "DP-9", so the load clears both copies and the report
    // does not promise them back.
    const std::vector<int> keptNameIds{43, 44, 64, 65, 66};
    const std::vector<int> keptWallpaperIds{40, 41, 42, 43, 44, 47, 64, 65};
    const std::string keptNameClause = " screen names this build's monitor limit excludes, kept";
    const std::string keptWallpaperClause = " entries this build cannot apply, kept";
    const bool keptNamesReported = reports(keptNameClause, keptNameIds.size());
    const bool keptWallpapersReported = reports(keptWallpaperClause, keptWallpaperIds.size());
    if (!clauseCountsReadable) return;
    check(keptNamesReported, "every kept row carrying a screen name is reported as one");
    check(keptWallpapersReported, "every kept row carrying a wallpaper is reported as one");
    if (!keptNamesReported || !keptWallpapersReported) {
        std::fprintf(stderr, "  persistence detail: %s\n  expected %zu of '%s' and %zu of '%s'\n", detail.c_str(),
                     keptNameIds.size(), keptNameClause.c_str(), keptWallpaperIds.size(), keptWallpaperClause.c_str());
    }

    // GOAL: the kept-name count promises back only names the next save will really leave in the
    //       file. False if a kept row's name that a live screen also answers to is counted: that
    //       copy is taken off the row, so the user is told a name returns on a larger build when
    //       the very next save writes the file without it.
    // WHY-THIS-SHAPE: a DIFFERENCE between two loads of the same rows -- once with the live row
    //       that claims "DP-3", once without it -- rather than a second literal count. A
    //       literal ("must not say seven") is reddened by any mutation that adds one kept name
    //       for any reason at all, and would print this check's message about a rule it never
    //       touched. The difference cancels every such shift, so only the live-screen rule
    //       itself can move it. The exact count above cannot replace this either -- it goes red
    //       for the regression but says only that some name total is wrong.
    // CONTROL: this rule is defended in two independent places and NEITHER mutation alone
    //       reddens this check, so it takes both -- `m_outputs.count(it->output) != 0 ||`
    //       deleted from the reconciliation loop at the end of
    //       WallpaperManager::restoreSaved(), which is what strips the name from the kept row
    //       at load, AND `&& m_outputs.count(entry.output) == 0` deleted from the kept-name
    //       loop in WallpaperManager::persistenceDetail(), which refuses to count such a name
    //       even when the row still carries it (both in backend/daemon/wallpaper_manager.cpp)
    //       -> "FAIL: a name a live screen already holds is left out of the count of names that
    //       will come back", printing "7 kept names with the live row, 7 without it; expected 1
    //       fewer with it". Deleting the load-side clause alone leaves this GREEN, the counting
    //       guard still refusing the name; what it reddens instead is the displaced-names check
    //       above, reading "2 screen names displaced" against 3. The counting guard has a
    //       control of its own further down this phase, where a client claims a kept row's name
    //       after startup -- the one state the load-side rule cannot reach. Observed 2026-08-14.
    //       Both reverted.
    // COVERAGE-ASSERT: both loads' claim on the name is asserted -- one live screen holding
    //       "DP-3" and none in the other run. If the second load kept a live claimant too the
    //       difference would be 0 for a correct build and the mutated one alike, and the check
    //       would establish nothing.
    const std::vector<int> liveHeldNameIds{40};
    const auto keptNameCount = [&keptNameClause](const std::string& text) {
        return clauseCount(text, keptNameClause);
    };
    // Read once, here, and its readability graded once: keptNameCount comes back empty for a
    // report whose count it cannot parse, and an unread count compared against a row count
    // reads as the daemon having lost every kept name -- an accusation with nothing behind it
    // but a reworded report. Every consumer below is gated on this.
    //
    // CONTROL: the kept-name count wrapped in parentheses in
    // WallpaperManager::persistenceDetail() (backend/daemon/wallpaper_manager.cpp), so the
    // digits no longer sit against the clause -> "FAIL: the kept-name count can be read out of
    // the persistence report" and the two sibling readability checks, plus a NOT VERIFIED line
    // for this phase's goal; the file-comparison check below no longer reports the daemon as
    // having lost kept names. Observed 2026-08-14. Reverted.
    const std::optional<int> promisedKeptNames = keptNameCount(detail);
    const bool keptNameCountReadable = promisedKeptNames.has_value();
    check(keptNameCountReadable, "the kept-name count can be read out of the persistence report");
    if (!keptNameCountReadable) std::fprintf(stderr, "  persistence detail: %s\n", detail.c_str());
    // The same rows the run above loaded, minus the one that claims "DP-3" -- so the only
    // difference between the two reports is whether a live screen answers to that name.
    nlohmann::json noLiveRows = nlohmann::json::array();
    for (const nlohmann::json& row : monitors) {
        if (row.at("monitor_id").get<int>() != 1) noLiveRows.push_back(row);
    }
    nlohmann::json noLiveRoot;
    noLiveRoot["version"] = 1;
    noLiveRoot["monitors"] = noLiveRows;
    const std::filesystem::path noLiveFile = scratch.path() / "wallpaperx" / "nolive.json";
    if (!writeFile(noLiveFile, noLiveRoot.dump())) {
        notVerified("S8.14's live-name half (a kept name a live screen already holds is not counted as one that "
                    "comes back)",
                    "the second config file this comparison loads could not be written");
    } else {
        WallpaperManager::Config noLiveConfig;
        noLiveConfig.configFile = noLiveFile;
        WallpaperManager noLiveManager(noLiveConfig);
        const std::vector<int> noLiveRestored = noLiveManager.restoreSaved();
        const WallpaperManager::Snapshot noLiveSnap = noLiveManager.snapshot();
        const std::string& noLiveDetail = noLiveSnap.health.persistence.detail;
        const bool claimantsAsExpected = snap.outputs.count("DP-3") == 1 && noLiveSnap.outputs.count("DP-3") == 0 &&
                                         noLiveRestored == std::vector<int>{3};
        check(claimantsAsExpected,
              "one load has a live screen answering to the kept row's name and the other has none");
        const std::optional<int> withoutLive = keptNameCount(noLiveDetail);
        const bool bothCountsReadable = keptNameCountReadable && withoutLive.has_value();
        check(withoutLive.has_value(), "the kept-name count can be read out of the second load's persistence report");
        if (!claimantsAsExpected || !bothCountsReadable) {
            std::fprintf(stderr, "  live claimants %zu/%zu, restored %zu rows; details: %s | %s\n",
                         snap.outputs.count("DP-3"), noLiveSnap.outputs.count("DP-3"), noLiveRestored.size(),
                         detail.c_str(), noLiveDetail.c_str());
        } else {
            const bool liveNameLeftOut =
                *withoutLive - *promisedKeptNames == static_cast<int>(liveHeldNameIds.size());
            check(liveNameLeftOut,
                  "a name a live screen already holds is left out of the count of names that will come back");
            if (!liveNameLeftOut) {
                std::fprintf(stderr, "  %d kept names with the live row, %d without it; expected %zu fewer with it\n",
                             *promisedKeptNames, *withoutLive, liveHeldNameIds.size());
            }
        }
    }

    // GOAL: a name past the stored format's byte cap is dropped and reported whatever monitor
    //       id carries it -- the id decides whether the ROW travels to a bigger build, not
    //       whether the name is usable. False if such a name is carried on an out-of-range id:
    //       the cap belongs to the wire format, so no future build accepts it either, and the
    //       row then rides every save for the life of the install while the report files the
    //       loss under "this build's monitor limit", which is not the obstacle.
    // WHY-THIS-SHAPE: two rows carrying names this build refuses for the same reason, one on an
    //       id it binds (3) and one on an id it cannot (47), graded by an exact count of two.
    //       Asserting only that some name was dropped passes for a build that drops one of
    //       them, which is precisely the state this fixture found.
    // CONTROL: the carried case in WallpaperManager::restoreSaved()
    //       (backend/daemon/wallpaper_manager.cpp) gated on valid UTF-8 rather than on the whole
    //       rule, as `const bool nameTravels = !entry.output.empty() && entry.monitorId >=
    //       kMaxMonitors && isValidUtf8(entry.output);` -- the code before this fix -> "FAIL: a
    //       name the stored format cannot hold is dropped whatever id carries it", the detail
    //       reading "1 screen names dropped, unusable" against 2, with "FAIL: every kept row
    //       carrying a screen name is reported as one" at six instead of five and "FAIL: a name
    //       this build refuses is written back on no row at all". Observed 2026-08-15.
    // COVERAGE-ASSERT: both refused names are asserted present in the fixture rows the load
    //       read, so a fixture edit that removes either one fails here rather than reducing
    //       this to a count of nothing.
    const std::vector<int> refusedNameDroppedIds{3, 47};
    const std::string rejectedClause = " screen names dropped, unusable";
    const bool rejectedReported = reports(rejectedClause, refusedNameDroppedIds.size());
    if (!clauseCountsReadable) return;
    check(rejectedReported, "a name the stored format cannot hold is dropped whatever id carries it");
    if (!rejectedReported) {
        std::fprintf(stderr, "  persistence detail: %s\n  expected %zu of '%s'\n", detail.c_str(),
                     refusedNameDroppedIds.size(), rejectedClause.c_str());
    }
    // Read off the fixture rows, not off the two id lists: asking whether `{3}` and `{47}` are
    // non-empty restates literals written four lines above, so it holds for every fixture and
    // cannot notice a fixture edit.
    //
    // CONTROL: monitor 47's fixture row given the ordinary name "DP-47" instead of the
    // oversized one -> "FAIL: the fixture really carries a refused name on both an id this
    // build binds and one it cannot", printing "fixture rows carrying a refused name: 3".
    // Graded instead as `!refusedNameDroppedIds.empty() && !carriedNameIds.empty()` plus the
    // two name strings, this stays GREEN under the same edit and only the two downstream
    // checks fail, naming the daemon for a fixture that changed. Observed 2026-08-14. Reverted.
    std::vector<int> fixtureIdsWithRefusedName;
    for (const nlohmann::json& row : monitors) {
        const std::string name = row.value("output", std::string());
        if (!name.empty() && !wallpaperx::outputNameError(name).empty()) {
            fixtureIdsWithRefusedName.push_back(row.at("monitor_id").get<int>());
        }
    }
    std::vector<int> expectedRefusedIds = refusedNameDroppedIds;
    std::sort(fixtureIdsWithRefusedName.begin(), fixtureIdsWithRefusedName.end());
    std::sort(expectedRefusedIds.begin(), expectedRefusedIds.end());
    const bool refusedNamesInFixture = fixtureIdsWithRefusedName == expectedRefusedIds;
    check(refusedNamesInFixture, "the fixture really carries a refused name on both an id this build binds and one it "
                                 "cannot");
    if (!refusedNamesInFixture) {
        std::string ids;
        for (const int id : fixtureIdsWithRefusedName) ids += " " + std::to_string(id);
        std::fprintf(stderr, "  fixture rows carrying a refused name:%s\n", ids.empty() ? " (none)" : ids.c_str());
    }

    // The save that rewrites the file from live state plus the kept rows. Made for a monitor
    // the fixture never mentions, so nothing it writes can be mistaken for a kept row.
    manager.requestWallpaper("static", other.string(), 2);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(2) == 1; });
    std::string why;
    std::size_t rowsWritten = 0;
    const std::map<int, RowInFile> onDisk = rowsInFile(file, why, &rowsWritten);
    const bool saveLanded = onDisk.count(2) == 1;
    check(saveLanded, "the save this phase reads back really happened");
    if (!saveLanded) {
        std::fprintf(stderr, "  %s; ids on disk: %s\n", why.empty() ? "read back" : why.c_str(),
                     idsOf(onDisk).c_str());
        return;
    }

    // GOAL: no monitor id is written on two rows, kept rows included. False if one is: the
    //       next start reads that as two monitors claiming one screen, resolves it last-wins
    //       and deletes the half it did not keep -- a wallpaper lost to the mechanism whose
    //       whole job is to preserve it.
    // WHY-THIS-SHAPE: the RAW row count against the number of distinct ids. Every other check
    //       below reads the file through rowsInFile(), which keys by monitor id, so a second
    //       row for an id it already holds is invisible to all of them -- and this fixture's
    //       repeated ids (41, 64, 65, 66) are exactly where such a row would come from. Both
    //       numbers come from the ONE read above: the daemon's writer can replace the file
    //       between two reads, and a count from a later version than the ids reports a correct
    //       save as a duplicated row.
    // CONTROL: every m_unapplied entry appended to the returned vector as well as being merged
    //       into the id-keyed rows, in WallpaperManager::persistableLocked()
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: no monitor is written on two rows
    //       of the config, kept rows included", printing "21 rows for 12 monitor ids". Observed
    //       2026-08-14. Reverted.
    const bool oneRowPerMonitor = rowsWritten == onDisk.size() && rowsWritten > 0;
    check(oneRowPerMonitor, "no monitor is written on two rows of the config, kept rows included");
    if (!oneRowPerMonitor) {
        std::fprintf(stderr, "  %zu rows for %zu monitor ids; ids: %s\n", rowsWritten, onDisk.size(),
                     idsOf(onDisk).c_str());
    }

    const bool lastRowWon = onDisk.count(41) == 1 && onDisk.at(41).path == "/second.png";
    check(lastRowWon, "two kept rows for one monitor leave the later row's wallpaper in the file");
    if (!lastRowWon) {
        std::fprintf(stderr, "  monitor 41 holds '%s'\n",
                     onDisk.count(41) ? onDisk.at(41).path.c_str() : "(no row)");
    }

    // GOAL: Fix 1's property -- when two config rows share a monitor id this build cannot
    //       apply and each fills a half the other left empty, BOTH halves are in the file
    //       afterwards. False if the later row replaces the earlier one: a config holding a
    //       video wallpaper for monitor 64 and, further down, a bare name for monitor 64 then
    //       loses the video path at the next save, deleted for good by the mechanism whose
    //       only job is to preserve it.
    // WHY-THIS-SHAPE: both file orders are exercised, because a replace-the-held-row build
    //       loses a different half in each -- name-last loses the wallpaper, wallpaper-last
    //       loses the name -- so a single-order check is half-blind. The SURVIVING values are
    //       asserted, not the absence of a loss report: the report is the second-order symptom
    //       and the wallpaper path is the thing the user loses.
    // CONTROL: the half-by-half merge in WallpaperManager::restoreSaved()
    //       (backend/daemon/wallpaper_manager.cpp) replaced with `*held = entry;
    //       ++duplicateIds;` -> "FAIL: a later name-only row does not delete the wallpaper on
    //       the row it merges into", printing "monitor 64 holds path '' output 'DP-5'";
    //       "FAIL: a later wallpaper row does not delete the screen name on the row it merges
    //       into", printing "monitor 65 holds path '/other.mp4' output ''"; and "FAIL: every
    //       kept row a later row really overwrote is reported as a loss, and no other", the
    //       detail reading "4 entries dropped" where one row really lost a wallpaper. The
    //       two kept counts go red with them, the halves those rows lost no longer being kept
    //       at all. Re-observed 2026-08-14. Reverted.
    const bool wallpaperSurvived =
        onDisk.count(64) == 1 && onDisk.at(64).path == "/clip.mp4" && onDisk.at(64).output == "DP-5";
    check(wallpaperSurvived, "a later name-only row does not delete the wallpaper on the row it merges into");
    if (!wallpaperSurvived) {
        std::fprintf(stderr, "  monitor 64 holds path '%s' output '%s'\n",
                     onDisk.count(64) ? onDisk.at(64).path.c_str() : "(no row)",
                     onDisk.count(64) ? onDisk.at(64).output.c_str() : "(no row)");
    }
    const bool nameSurvived =
        onDisk.count(65) == 1 && onDisk.at(65).path == "/other.mp4" && onDisk.at(65).output == "DP-6";
    check(nameSurvived, "a later wallpaper row does not delete the screen name on the row it merges into");
    if (!nameSurvived) {
        std::fprintf(stderr, "  monitor 65 holds path '%s' output '%s'\n",
                     onDisk.count(65) ? onDisk.at(65).path.c_str() : "(no row)",
                     onDisk.count(65) ? onDisk.at(65).output.c_str() : "(no row)");
    }
    // The half that really does collide still resolves last-wins, so the merge above is not
    // just "never overwrite anything".
    const bool nameCollisionResolved = onDisk.count(66) == 1 && onDisk.at(66).output == "DP-8";
    check(nameCollisionResolved, "two kept rows naming one monitor differently leave the later name in the file");
    if (!nameCollisionResolved) {
        std::fprintf(stderr, "  monitor 66 holds output '%s'\n",
                     onDisk.count(66) ? onDisk.at(66).output.c_str() : "(no row)");
    }

    // GOAL: a connector name is written on exactly one row, even when the row carrying it is
    //       one this build cannot apply and so never bound. False if both rows keep it: the
    //       file then names one screen twice, permanently -- the load side skips out-of-range
    //       ids entirely, so no later run of this build can ever notice, report or repair it.
    // WHY-THIS-SHAPE: every row in the file is scanned for the name and the ids are compared
    //       as a list. Asking instead which monitor "DP-3" resolves to -- in the snapshot or
    //       by looking up row 1 -- is true of the duplicating build as well, because the
    //       second copy sits on a row nothing in this build ever reads back. The kept row's
    //       own wallpaper is asserted still present, so clearing the name by deleting the row
    //       does not pass either.
    // CONTROL: two mutations together, and it takes both -- `m_outputs.count(it->output) != 0
    //       ||` deleted from the reconciliation loop at the end of
    //       WallpaperManager::restoreSaved() AND `|| m_outputs.count(row.output) != 0` deleted
    //       from the name-clearing loop in WallpaperManager::persistableLocked() (both in
    //       backend/daemon/wallpaper_manager.cpp) -> "FAIL: a connector name a live screen
    //       holds is written on that screen's row and no other", printing "rows carrying
    //       'DP-3': 1 40". Either alone leaves the file with one copy and this check green,
    //       which is the point of the pair: the load-side rule decides it and the save-side
    //       rule is the backstop for a name a client claims later. What separates them is the
    //       REPORT -- deleting the load-side rule alone reddens the displaced-names check
    //       above, which reads "2 screen names displaced" against 3, because only the load can
    //       count what it took away. Observed 2026-08-14. Both reverted.
    std::vector<int> rowsCarryingName;
    for (const auto& [id, row] : onDisk) {
        if (row.output == "DP-3") rowsCarryingName.push_back(id);
    }
    const bool nameOnLiveRowOnly = rowsCarryingName == std::vector<int>{1};
    check(nameOnLiveRowOnly, "a connector name a live screen holds is written on that screen's row and no other");
    if (!nameOnLiveRowOnly) {
        std::string ids;
        for (const int id : rowsCarryingName) ids += " " + std::to_string(id);
        std::fprintf(stderr, "  rows carrying 'DP-3':%s\n", ids.empty() ? " (none)" : ids.c_str());
    }
    const bool keptRowIntact = onDisk.count(40) == 1 && onDisk.at(40).path == "/kept.png";
    check(keptRowIntact, "the kept row keeps its wallpaper when the name it carried is taken from it");
    if (!keptRowIntact) {
        std::fprintf(stderr, "  monitor 40 holds '%s'\n",
                     onDisk.count(40) ? onDisk.at(40).path.c_str() : "(no row)");
    }

    // GOAL: Fix 2's property -- a connector name appears on at most one row of the written
    //       file even when NO live screen holds it, i.e. when both rows carrying it are ones
    //       this build cannot apply. False if both are written: a later build whose monitor
    //       limit admits both ids binds both, one displaces the other, and the screen that
    //       loses stops being findable by the only name the plugin knows it by.
    // WHY-THIS-SHAPE: the live-screen case above cannot establish this. It is cleared by a
    //       different clause -- the name is in m_outputs -- so a build with no duplicate
    //       suppression at all passes it. Two kept rows with no live claimant is the only
    //       shape that reaches the clause under test. Both rows' wallpapers are asserted
    //       present as well, so clearing the duplicate by dropping the whole row does not
    //       pass.
    //       The row it survives on is asserted too, and is the LATER of the two: last-wins is
    //       the rule every other collision here follows, and the two places that can settle
    //       this one disagree about it -- the load walks the kept rows backwards and keeps the
    //       later, the save walks them in monitor-id order and would keep the earlier. Only
    //       naming the row says which one ran.
    // CONTROL: the last-claim reconciliation loop at the end of
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp) emptied ->
    //       "FAIL: a connector name no live screen holds is written on the later kept row and
    //       no other", printing "rows carrying 'DP-9': 42" -- the save's own duplicate
    //       suppression still leaves one copy, but the earlier row's. Deleting
    //       `|| !written.insert(row.output).second` from the name-clearing loop in
    //       WallpaperManager::persistableLocked() as well -> the same FAIL printing "rows
    //       carrying 'DP-9': 42 43", which is the shape with neither defence. Re-observed
    //       2026-08-15 against the reworked loop. Both reverted.
    std::vector<int> rowsCarryingDuplicate;
    for (const auto& [id, row] : onDisk) {
        if (row.output == "DP-9") rowsCarryingDuplicate.push_back(id);
    }
    const bool duplicateNameWrittenOnce = rowsCarryingDuplicate == std::vector<int>{43};
    check(duplicateNameWrittenOnce, "a connector name no live screen holds is written on the later kept row and no "
                                    "other");
    if (!duplicateNameWrittenOnce) {
        std::string ids;
        for (const int id : rowsCarryingDuplicate) ids += " " + std::to_string(id);
        std::fprintf(stderr, "  rows carrying 'DP-9':%s\n", ids.empty() ? " (none)" : ids.c_str());
    }
    const bool bothDuplicateRowsIntact = onDisk.count(42) == 1 && onDisk.at(42).path == "/dup-a.png" &&
                                         onDisk.count(43) == 1 && onDisk.at(43).path == "/dup-b.png";
    check(bothDuplicateRowsIntact, "the kept row that lost a contested connector name keeps its wallpaper");
    if (!bothDuplicateRowsIntact) std::fprintf(stderr, "  ids on disk: %s\n", idsOf(onDisk).c_str());

    // GOAL: the storage half of the same rule -- a name the stored format cannot hold reaches
    //       the file on no row at all, whatever id it arrived on, while the row that carried it
    //       keeps its wallpaper. False if such a name is written back: it then rides every save
    //       for the life of the install, and no build that reads this format will ever bind it.
    // WHY-THIS-SHAPE: every row in the file is scanned for a name this build would refuse,
    //       rather than looking up monitor 47 and asking whether its output is empty. The
    //       property is about the whole file: a build that moved the name onto some other row
    //       satisfies the narrow question and not this one. The wallpaper on that row is
    //       asserted in the same breath, because dropping the row entirely would also empty the
    //       list, and that is a wallpaper lost rather than a name refused.
    // CONTROL: the carried case in WallpaperManager::restoreSaved()
    //       (backend/daemon/wallpaper_manager.cpp) gated on valid UTF-8 rather than on the whole
    //       rule -- the code before this fix -> "FAIL: a name the stored format cannot hold is
    //       written back on no row at all", printing "rows carrying a name this build refuses:
    //       47 (257 bytes)". Observed 2026-08-15.
    std::vector<int> rowsWithRefusedName;
    for (const auto& [id, row] : onDisk) {
        if (!row.output.empty() && !wallpaperx::outputNameError(row.output).empty()) {
            rowsWithRefusedName.push_back(id);
        }
    }
    const bool noRefusedNameWritten = rowsWithRefusedName.empty();
    check(noRefusedNameWritten, "a name the stored format cannot hold is written back on no row at all");
    if (!noRefusedNameWritten) {
        std::string ids;
        for (const int id : rowsWithRefusedName) {
            ids += " " + std::to_string(id) + " (" + std::to_string(onDisk.at(id).output.size()) + " bytes)";
        }
        std::fprintf(stderr, "  rows carrying a name this build refuses:%s\n", ids.empty() ? " (none)" : ids.c_str());
    }
    const bool refusedNameRowIntact = onDisk.count(47) == 1 && onDisk.at(47).path == "/kept47.png";
    check(refusedNameRowIntact, "the kept row carrying a name this build cannot bind keeps its wallpaper");
    if (!refusedNameRowIntact) {
        std::fprintf(stderr, "  monitor 47 holds '%s'\n",
                     onDisk.count(47) ? onDisk.at(47).path.c_str() : "(no row)");
    }

    // GOAL: the count of kept names the report promises back is the number of names the save
    //       really left in the file. False whenever a name is taken off a kept row somewhere
    //       the report cannot see: the save rebuilds the file on every write and is const, so
    //       a name it drops there is dropped in silence, while the health text -- recomputed
    //       from the rows still held -- goes on offering it to the user on every boot.
    // WHY-THIS-SHAPE: the report is compared against the FILE, not against a literal. A
    //       literal for each is what the two exact-count checks above already do, and both of
    //       them stay green when the two sides disagree: they are computed from the same
    //       fixture by hand and a build that clears a name at save time satisfies whichever
    //       one it was written for. Only reading back what was written can catch the two
    //       drifting apart.
    // CONTROL: the last-claim reconciliation loop at the end of
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp) emptied,
    //       which puts both decisions back inside the const save -> "FAIL: every kept name the
    //       report promises back is really in the written file", printing "the report promises
    //       6 kept names, the file holds 5, the fixture expects 5". The kept row that still
    //       carries a name after that deletion but is left out of the count is monitor 40,
    //       which persistenceDetail()'s own live-name guard excludes. Re-observed 2026-08-15
    //       against the reworked loop. Reverted.
    // COVERAGE-ASSERT: the comparison is only evidence if the file really holds kept names, so
    //       the count it read is asserted to be the expected one rather than merely equal.
    int namesOnKeptRows = 0;
    for (const auto& [id, row] : onDisk) {
        if (id >= WallpaperManager::kMaxMonitors && !row.output.empty()) ++namesOnKeptRows;
    }
    if (!keptNameCountReadable) {
        // The graded readability check is above; comparing a count the harness could not read
        // against a row count here would print this goal's message for a report, not a daemon.
        notVerified(kGoals, "the kept-name count could not be read out of the persistence report");
    } else {
        const bool reportMatchesFile = *promisedKeptNames == namesOnKeptRows &&
                                       namesOnKeptRows == static_cast<int>(keptNameIds.size());
        check(reportMatchesFile, "every kept name the report promises back is really in the written file");
        if (!reportMatchesFile) {
            std::fprintf(stderr, "  the report promises %d kept names, the file holds %d, the fixture expects %zu\n",
                         *promisedKeptNames, namesOnKeptRows, keptNameIds.size());
        }
    }

    // GOAL: once a client claims a screen name a kept row was carrying, the report stops
    //       offering that name back. False if the count still includes it: the very next save
    //       takes the name off the kept row -- it must, or the file would name one screen
    //       twice -- while the health text goes on telling the user that name returns on a
    //       build with a larger monitor limit. It says so for the life of the process, and the
    //       number then drops at the next start with nothing said about why.
    // WHY-THIS-SHAPE: the name is claimed through setMonitorOutput AFTER restoreSaved() has
    //       run. That is the whole check: the load reconciles every collision the file ARRIVED
    //       with, so a phase that only writes a fixture naming one screen twice and reads the
    //       count cannot go red here no matter what the counting rule is -- a client binding
    //       the name later is the only way to reach it. Graded as a DIFFERENCE across the one
    //       claim rather than a literal, for the reason the live-screen check above gives: a
    //       literal is reddened by any fixture change that shifts the total and would then
    //       print this message about a rule it never touched. The FILE is read back beside the
    //       text because the defect is the two disagreeing -- a check on the text alone is
    //       equally satisfied by a build that stopped writing the name and stopped counting it.
    // CONTROL: `&& m_outputs.count(entry.output) == 0` deleted from the kept-name loop in
    //       WallpaperManager::persistenceDetail() (backend/daemon/wallpaper_manager.cpp) ->
    //       "FAIL: a screen name a client claims after startup stops being counted among the
    //       names that come back", printing "6 kept names promised before the claim, 6 after
    //       it; expected 1 fewer", and "FAIL: after a client claims a kept row's name the report
    //       counts exactly the kept names the file holds", printing "the report promises 6 kept
    //       names, the file holds 5". Those two are the ONLY checks in the phase that mutation
    //       reddens: everywhere else the load has already stripped the name, so the guard is
    //       inert -- which is why the live-screen check above needs both mutations and this one
    //       needs only this. Observed 2026-08-14. Reverted.
    // COVERAGE-ASSERT: the name is asserted to be held by a kept row and by no live screen at
    //       the moment it is claimed, and to be bound to the claiming monitor afterwards.
    //       Without those the difference is 0 for a correct build and the mutated one alike,
    //       and this establishes nothing.
    const std::string claimedName = "DP-5";
    constexpr int claimingMonitor = 5;
    const WallpaperManager::Snapshot beforeClaim = manager.snapshot();
    const std::optional<int> keptNamesBefore = keptNameCount(beforeClaim.health.persistence.detail);
    // Graded before it is used, and skipped rather than folded into the check below: an empty
    // result means the harness could not read the report, and folding it in prints a message
    // about the fixture and the daemon for a failure that is neither.
    const bool beforeCountReadable = keptNamesBefore.has_value();
    check(beforeCountReadable, "the kept-name count can be read out of the report taken before the claim");
    if (!beforeCountReadable) {
        notVerified(kGoals, "the kept-name count could not be read out of the report taken before the claim");
        std::fprintf(stderr, "  persistence detail: %s\n", beforeClaim.health.persistence.detail.c_str());
        return;
    }
    const bool claimIsOfAKeptName = *keptNamesBefore > 0 && beforeClaim.outputs.count(claimedName) == 0 &&
                                    onDisk.count(64) == 1 && onDisk.at(64).output == claimedName;
    check(claimIsOfAKeptName, "the name claimed after startup is one a kept row holds and no live screen does");
    if (!claimIsOfAKeptName) {
        std::fprintf(stderr, "  %d kept names promised, live claimants %zu, monitor 64 output '%s'\n",
                     *keptNamesBefore, beforeClaim.outputs.count(claimedName),
                     onDisk.count(64) ? onDisk.at(64).output.c_str() : "(no row)");
        return;
    }

    manager.setMonitorOutput(claimingMonitor, claimedName);
    const WallpaperManager::Snapshot afterClaim = manager.snapshot();
    const std::optional<int> keptNamesAfter = keptNameCount(afterClaim.health.persistence.detail);
    const bool claimBound = afterClaim.outputs.count(claimedName) == 1 &&
                            afterClaim.outputs.at(claimedName) == claimingMonitor;
    check(claimBound, "the name claimed after startup really binds to the monitor that asked for it");
    // Graded, not merely used to return: an ungraded return would leave the three checks below
    // unrun with no pass, fail or skip recorded for any of them.
    const bool afterCountReadable = keptNamesAfter.has_value();
    check(afterCountReadable, "the kept-name count can be read out of the report taken after the claim");
    if (!claimBound || !afterCountReadable) {
        notVerified(kGoals, "the claim this phase makes did not bind, or its report could not be read, so what the "
                            "count does after a claim was not established");
        std::fprintf(stderr, "  persistence detail after the claim: %s\n",
                     afterClaim.health.persistence.detail.c_str());
        return;
    }

    const bool claimedNameGivenUp = *keptNamesBefore - *keptNamesAfter == 1;
    check(claimedNameGivenUp, "a screen name a client claims after startup stops being counted among the names that "
                              "come back");
    if (!claimedNameGivenUp) {
        std::fprintf(stderr, "  %d kept names promised before the claim, %d after it; expected 1 fewer\n",
                     *keptNamesBefore, *keptNamesAfter);
    }

    waitForRows(file, [&claimedName, claimingMonitor](const std::map<int, RowInFile>& rows) {
        return rows.count(claimingMonitor) == 1 && rows.at(claimingMonitor).output == claimedName;
    });
    std::string claimWhy;
    const std::map<int, RowInFile> afterClaimOnDisk = rowsInFile(file, claimWhy);
    std::vector<int> rowsCarryingClaimedName;
    for (const auto& [id, row] : afterClaimOnDisk) {
        if (row.output == claimedName) rowsCarryingClaimedName.push_back(id);
    }
    const bool claimedNameOnLiveRowOnly = rowsCarryingClaimedName == std::vector<int>{claimingMonitor};
    check(claimedNameOnLiveRowOnly, "a name claimed after startup is written on the claiming screen's row and no "
                                    "other");
    if (!claimedNameOnLiveRowOnly) {
        std::string ids;
        for (const int id : rowsCarryingClaimedName) ids += " " + std::to_string(id);
        std::fprintf(stderr, "  %s; rows carrying '%s':%s\n", claimWhy.empty() ? "read back" : claimWhy.c_str(),
                     claimedName.c_str(), ids.empty() ? " (none)" : ids.c_str());
    }
    const bool keptRowKeptItsWallpaper =
        afterClaimOnDisk.count(64) == 1 && afterClaimOnDisk.at(64).path == "/clip.mp4";
    check(keptRowKeptItsWallpaper, "the kept row keeps its wallpaper when a client claims the name it carried");
    if (!keptRowKeptItsWallpaper) {
        std::fprintf(stderr, "  monitor 64 holds '%s'\n",
                     afterClaimOnDisk.count(64) ? afterClaimOnDisk.at(64).path.c_str() : "(no row)");
    }

    int namesOnKeptRowsAfterClaim = 0;
    for (const auto& [id, row] : afterClaimOnDisk) {
        if (id >= WallpaperManager::kMaxMonitors && !row.output.empty()) ++namesOnKeptRowsAfterClaim;
    }
    const bool stillMatchesFile = *keptNamesAfter == namesOnKeptRowsAfterClaim &&
                                  namesOnKeptRowsAfterClaim == static_cast<int>(keptNameIds.size()) - 1;
    check(stillMatchesFile, "after a client claims a kept row's name the report counts exactly the kept names the "
                            "file holds");
    if (!stillMatchesFile) {
        std::fprintf(stderr, "  the report promises %d kept names, the file holds %d\n", *keptNamesAfter,
                     namesOnKeptRowsAfterClaim);
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6i -- Step 10: the cost of a client re-asserting a name it already gave.
// ---------------------------------------------------------------------------- //
void runIdempotentRenameChecks() {
    constexpr const char* kGoal = "S10.13 (naming a screen the name it already has does not rewrite the config)";
    const ScratchDir scratch("renameidempotent");
    if (!scratch.ok()) {
        notVerified(kGoal, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        notVerified(kGoal, "the config directory this phase writes into could not be created: " + ec.message());
        return;
    }

    // Pointed at the config file before the manager exists, so no save of this phase can land
    // uncounted.
    countRenamesOnto(file);
    WallpaperManager::Config managerConfig;
    managerConfig.configFile = file;
    WallpaperManager manager(managerConfig);

    // GOAL: SetMonitorOutput is idempotent in cost as well as in outcome -- a call that
    //       changes no binding performs no write. False if it rewrites the file: the method is
    //       documented idempotent so a client is invited to re-assert every screen's name on
    //       each display change, and each redundant call parks the daemon's DBus event-loop
    //       thread until the write settles.
    // WHY-THIS-SHAPE: successful rename(2) calls onto the config file are counted, which is
    //       what ConfigStore::save() finishes with. Neither obvious alternative works here.
    //       Comparing the file's bytes cannot see a redundant save at all, because it writes
    //       exactly what was already there. Comparing modification times -- what the system
    //       tier uses across a process restart -- is only as fine as the filesystem's
    //       timestamp granularity, and the two calls here are microseconds apart in one
    //       process, so on a filesystem with coarse timestamps it would read "unchanged" for
    //       a daemon that wrote twice. A rename count is exact on every filesystem. What makes
    //       an exact number legitimate here rather than an assertion on the scheduler is that
    //       every count is taken from a QUIESCED daemon, and quiescence is established rather
    //       than assumed: after each call this phase waits for the name to reach the file and
    //       then requires that it had already landed when the call returned. A call that must
    //       write NOTHING is sampled at once, and what makes that sample safe is the call after
    //       it: waiting for that write re-reads the count, so a write the redundant call made
    //       late shows up as a count that moved.
    // CONTROL: the `== BindResult::Unchanged` early return deleted from
    //       WallpaperManager::setMonitorOutput (backend/daemon/wallpaper_manager.cpp) ->
    //       "FAIL: naming a screen the name it already has does not write the config at all",
    //       printing "2 write(s) landed for 1 change". Observed 2026-08-14. Reverted.
    // COVERAGE-ASSERT: a count that never moves is what a dead interposition and a daemon that
    //       never saves both produce, so the first call is asserted to write exactly once and
    //       a later call that DOES change the binding is asserted to write again -- an early
    //       return that swallowed every save fails those instead of passing this.

    // Polls for a name to reach the file, up to a bound far longer than persist()'s own
    // deadline. Returning false means the write never arrived, which is a verdict on the
    // daemon; arriving only after the call returned is a verdict on the host, and the callers
    // below tell the two apart by re-reading the count.
    const auto waitForOutput = [](const std::filesystem::path& path, int monitorId, const std::string& name) {
        return waitForRows(path, [monitorId, &name](const std::map<int, RowInFile>& rows) {
            return rows.count(monitorId) == 1 && rows.at(monitorId).output == name;
        });
    };

    manager.setMonitorOutput(3, "DP-A");
    const unsigned afterFirst = renamesCounted();
    if (!waitForOutput(file, 3, "DP-A")) {
        check(false, "naming a screen writes that name into the config file");
        std::string why;
        const std::map<int, RowInFile> afterFirstRows = rowsInFile(file, why);
        std::fprintf(stderr, "  %s; monitor 3 output '%s'\n", why.empty() ? "read back" : why.c_str(),
                     afterFirstRows.count(3) ? afterFirstRows.at(3).output.c_str() : "(no row)");
        return;
    }
    // GOAL: a SetMonitorOutput that returned normally has already finished its write, so every
    //       count taken below is of a quiesced daemon. False if the daemon reports success on a
    //       write still in flight: "no write at all" and "a write that has not landed yet" then
    //       read identically and nothing this phase counts means anything.
    // WHY-THIS-SHAPE: a check, where this used to be a skip reading "this host finished the write
    //       only after the call had returned". That host condition stopped existing at 6fa5be3
    //       (2026-08-17), which made setMonitorOutput throw unless its own write landed, so
    //       returning normally and having landed became one fact -- a moved count is now a daemon
    //       defect, not a slow disk, and skipping would retire the phase over it. Kept rather than
    //       simply deleted because every count below rests on this and a build that gave the
    //       guarantee up would otherwise make them quietly meaningless.
    // CONTROL: kWriteDeadline set to 0 and a 300 ms sleep added ahead of store.save()
    //       (backend/daemon/config_writer.cpp) -- a host whose write always lands after the call
    //       returns, which is exactly what the old skip claimed to cover. The shipped daemon does
    //       not reach here at all under it: "FAIL: the 're-asserting a name a screen already has'
    //       phase threw: ... a write has not come back, the choice may not be on disk yet". With
    //       the throw ALSO taken out of setMonitorOutput -- the pre-6fa5be3 shape -- the same
    //       mutation reddens this check: "FAIL: a SetMonitorOutput that returned had already
    //       finished its write", printing "0 write(s) counted when the call returned, 1 once the
    //       file held the name". All three reverted. Observed 2026-08-19.
    const unsigned settledAfterFirst = renamesCounted();
    const bool firstQuiesced = afterFirst == settledAfterFirst;
    check(firstQuiesced, "a SetMonitorOutput that returned had already finished its write");
    if (!firstQuiesced) {
        std::fprintf(stderr, "  %u write(s) counted when the call returned, %u once the file held the name; every "
                             "count below would be of a daemon that is still writing\n",
                     afterFirst, settledAfterFirst);
        return;
    }
    if (afterFirst == 0) {
        // The counter is incremented after the real rename(2) returns, so a read taken in that
        // gap sees the new file and the old count. Re-read briefly before declaring anything.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (renamesCounted() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (renamesCounted() != 0) {
            notVerified(kGoal, "this host counted the daemon's first config write only after the file already held "
                               "the name, so every count below would be of a daemon that is still writing");
            return;
        }
        // The name is on disk and no write was ever counted, so the daemon saved and the
        // INSTRUMENT is what did not see it. Red as a harness fault rather than a skip: a skip
        // retires this goal and everything below it -- including "naming a screen the name it
        // already has does not write the config at all" -- while a build that rewrote the file
        // on every redundant call would ship. It is not a FAIL against the daemon either; the
        // fault is this file's interposed rename() no longer sitting on ConfigStore::save()'s
        // write path.
        //
        // CONTROL: the increment in this file's interposed rename() disabled, which is a dead
        // instrument over a working daemon -> "FAIL (harness): the interposed rename() counted
        // no write ...", the summary line "1 fault(s) in this test file, not in ConfigStore",
        // and exit 1. With this arm put back to notVerified() and the same dead counter, the
        // run printed "NOT VERIFIED -- S10.13 ..." and recorded no fault, retiring every check
        // below this line. Observed 2026-08-15. Both reverted.
        harnessFailure("the interposed rename() counted no write although the daemon's save reached the config file, "
                       "so this phase's instrument no longer observes ConfigStore::save()");
        return;
    }
    check(afterFirst == 1, "naming a screen a name it did not have writes the config exactly once");

    manager.setMonitorOutput(3, "DP-A");
    const unsigned afterRepeat = renamesCounted();
    check(afterRepeat == afterFirst, "naming a screen the name it already has does not write the config at all");
    if (afterRepeat != afterFirst) {
        std::fprintf(stderr, "  %u write(s) landed for %u change\n", afterRepeat, afterFirst);
    }

    manager.setMonitorOutput(3, "DP-B");
    const unsigned afterChange = renamesCounted();
    if (!waitForOutput(file, 3, "DP-B")) {
        check(false, "naming a screen a different name writes that name into the config file");
        return;
    }
    // The same guarantee for the second write, and it carries more here: this sample is also what
    // makes the redundant call above safe to have sampled at once. A skip in its place, like the
    // one this replaced, would retire "naming a screen the name it already has does not write the
    // config at all" over a state the daemon no longer allows.
    // CONTROL: the throw taken out of WallpaperManager::setMonitorOutput and the config writer's
    //       worker made to sleep 3 s -- past its deadline -- before store.save() for the DP-B
    //       entry only, so THIS write lands late and the first does not -> "FAIL: a
    //       SetMonitorOutput that changed a name had also already finished its write", printing
    //       "1 write(s) counted when the call returned, 2 once the file held the name". Both
    //       reverted. Observed 2026-08-19.
    const unsigned settledAfterChange = renamesCounted();
    const bool changeQuiesced = afterChange == settledAfterChange;
    check(changeQuiesced, "a SetMonitorOutput that changed a name had also already finished its write");
    if (!changeQuiesced) {
        std::fprintf(stderr, "  %u write(s) counted when the call returned, %u once the file held the name; a write "
                             "the redundant call made cannot be told from a late one\n",
                     afterChange, settledAfterChange);
        return;
    }
    check(afterChange == afterRepeat + 1, "naming a screen a different name still writes the config");
    if (afterChange != afterRepeat + 1) {
        std::fprintf(stderr, "  %u write(s) landed after a change, %u before it\n", afterChange, afterRepeat);
    }

    // The other half of the same rule, on a daemon whose last write did not land. The skip
    // above is what makes a client's re-assertion free; here it is what would make the
    // re-assertion useless, so it must not apply.
    constexpr const char* kRetryGoal = "S10.13's retry clause (re-asserting a name after a failed write attempts the "
                                       "write again)";
    const std::filesystem::path readOnly = scratch.path() / "ro";
    std::filesystem::create_directories(readOnly, ec);
    if (ec) {
        notVerified(kRetryGoal, "the directory this phase makes unwritable could not be created: " + ec.message());
        return;
    }
    std::filesystem::permissions(readOnly, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
        notVerified(kRetryGoal, "the directory this phase writes into could not be made read-only: " + ec.message());
        return;
    }
    // X.3, as in the failed-save phase: root ignores the mode bits and so do some
    // filesystems. A host that cannot make a write fail must say so rather than grading the
    // daemon on a state it was never put in.
    { std::ofstream probe(readOnly / "probe"); }
    if (std::filesystem::exists(readOnly / "probe")) {
        std::filesystem::remove(readOnly / "probe", ec);
        notVerified(kRetryGoal, "this process can write into a directory it made read-only (running as root, or a "
                                "filesystem that ignores mode bits), so no save can be made to fail");
        return;
    }

    // GOAL: S10.13's retry clause -- when the last config write FAILED, re-asserting the name a
    //       screen already has attempts the write again. False if the idempotence skip applies:
    //       the in-memory map is updated before the write is tried, so it agrees with the
    //       request even when the file does not, and the header invites clients to re-assert
    //       names, which would then be the one retry a client can reach and would do nothing.
    // WHY-THIS-SHAPE: the directory is opened again between the two calls and the check is
    //       that the name REACHES THE FILE. Counting write attempts is what the healthy half
    //       above does, but it cannot be reused here: the interposed counter only sees a
    //       rename that succeeded, and no write into an unwritable directory ever gets that
    //       far, so a failed retry and no retry at all count identically. The third call, on a
    //       daemon that is healthy again, is asserted to write NOTHING -- without it a build
    //       that simply deleted the skip would pass this -- and that count is taken after a
    //       later name change has reached the file, so a write the redundant call made late
    //       has been counted by the time it is read.
    // CONTROL: `&& m_writer.outcome().onDisk` deleted from the early return in
    //       WallpaperManager::setMonitorOutput (backend/daemon/wallpaper_manager.cpp), which is
    //       where the old persistFailedLocked() test now lives -> "FAIL: re-asserting a name after
    //       a failed write writes it to the file", printing "0 write(s) landed for the retry, 0
    //       before it; the config file is empty or absent; monitor 3 output '(no row)'", and
    //       "FAIL: the persistence health recovers once a retried write lands". Eight red in all;
    //       the other six are the agreement cluster below, which grades the same shortcut.
    //       Re-observed 2026-08-18. Reverted.
    // CONTROL, for the last check of this phase: the whole `== BindResult::Unchanged` early
    //       return deleted from the same function, so every call writes -> "FAIL: once the
    //       retry has landed, re-asserting the same name writes nothing again", printing "3
    //       write(s) landed for the redundant call and the change after it, 1 for the retry".
    //       Observed 2026-08-14. Reverted.
    const std::filesystem::path blockedFile = readOnly / "wallpaperx" / "config.json";
    countRenamesOnto(blockedFile);
    WallpaperManager::Config blockedConfig;
    blockedConfig.configFile = blockedFile;
    WallpaperManager blocked(blockedConfig);

    // Returns what the call threw, or an empty string. Every SetMonitorOutput below goes through
    // it: a name that did not reach the file is now reported by throwing, so a bare call would
    // abandon this phase through main()'s per-phase handler and print one opaque line in place of
    // every check under it.
    const auto name = [&blocked](int monitorId, const std::string& outputName) {
        try {
            blocked.setMonitorOutput(monitorId, outputName);
        } catch (const std::exception& e) {
            return std::string(e.what());
        } catch (...) {
            return std::string("an exception that is not a std::exception");
        }
        return std::string();
    };

    // GOAL: naming a screen reports a name that did not reach the config file. False if it
    //       returns success: DbusService::setMonitorOutput's declared contract
    //       (backend/daemon/dbus_service.h) is that an accepted reply means the mapping is on
    //       disk rather than merely queued, and a name held only in memory is gone at the next
    //       restart -- where the plugin, having been told the name was stored, has no reason to
    //       set it again. What the error denies is durability only: the binding must still stand
    //       for this run, so a client is not left believing the screen is unnamed.
    // WHY-THIS-SHAPE: both halves are asserted together, because each alone admits a wrong build.
    //       Asserting only the throw is satisfied by a daemon that also discarded the binding;
    //       asserting only the binding is what the pre-fix daemon did while replying success. The
    //       error text is required to carry the write's own reason rather than merely being
    //       non-empty -- a bare "failed" sends the user nowhere.
    // CONTROL: two, both in WallpaperManager::setMonitorOutput
    //       (backend/daemon/wallpaper_manager.cpp), both reverted. (a) the `throw
    //       std::runtime_error(health.persistence.detail)` deleted, which is the code before this
    //       fix -> "FAIL: a screen name that never reached the config file is reported as an
    //       error", printing "the call returned normally". (b) an `m_outputs.erase(outputName)`
    //       added just before that throw -- the plausible wrong fix, reporting the failure by
    //       rolling the binding back -> "FAIL: a screen name that could not be written is still
    //       bound for this run", printing "DP-A resolves to (nothing)", with the first check
    //       green. The pair is what separates "the daemon reported" from "the daemon gave up".
    //       (c) persist()'s `whyNot` out-parameter ignored and `m_health.persistence.detail` read
    //       raw instead -- the shape 6fa5be3 shipped, which this check caught -> "FAIL: a screen
    //       name that never reached the config file is reported as an error", printing "it threw:
    //       <scratch>/ro/wallpaperx/config.json", the bare path with no cause. Nothing RECORDS a
    //       completed write's error into that field: applyPersistOutcomeLocked() composes it into
    //       a caller's copy at snapshot time and no further. Fixed by carrying the writer's own
    //       error out of persist(), which is the reason belonging to this call rather than the
    //       daemon-wide aggregate this method deliberately stopped reading.
    const std::string blockedThrew = name(3, "DP-A");
    const bool reportedTheLoss = blockedThrew.find("Permission denied") != std::string::npos;
    check(reportedTheLoss, "a screen name that never reached the config file is reported as an error");
    if (!reportedTheLoss) {
        std::fprintf(stderr, "  %s\n",
                     blockedThrew.empty() ? "the call returned normally" : ("it threw: " + blockedThrew).c_str());
    }
    const std::map<std::string, int> boundNow = blocked.snapshot().outputs;
    const bool stillBound = boundNow.count("DP-A") == 1 && boundNow.at("DP-A") == 3;
    check(stillBound, "a screen name that could not be written is still bound for this run");
    if (!stillBound) {
        std::fprintf(stderr, "  DP-A resolves to %s\n",
                     boundNow.count("DP-A") ? std::to_string(boundNow.at("DP-A")).c_str() : "(nothing)");
    }

    if (blocked.snapshot().health.persistence.status != wallpaperx::HealthStatus::Fail) {
        notVerified(kRetryGoal, "the save into a directory this process cannot write did not report a failure, so "
                                "there is no failed write for a re-assertion to retry");
        return;
    }
    const unsigned beforeRetry = renamesCounted();

    std::filesystem::permissions(readOnly, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
    if (ec) {
        notVerified(kRetryGoal, "the directory could not be made writable again: " + ec.message());
        return;
    }
    const std::string retryThrew = name(3, "DP-A");
    check(retryThrew.empty(), "a re-assertion whose write lands reports no error");
    if (!retryThrew.empty()) std::fprintf(stderr, "  the retry threw: %s\n", retryThrew.c_str());
    // Waited for, then counted: a retry whose write landed a moment after persist() stopped
    // waiting is still a retry, and sampling at the instant the call returned would report it
    // as a daemon that never tried. The count is only compared as "more than before", so
    // taking it after the wait cannot weaken it -- a build that never retries never produces
    // the row either, and the wait then expires and this check fails as it should.
    const bool retryLanded = waitForOutput(blockedFile, 3, "DP-A");
    const unsigned afterRetry = renamesCounted();
    std::string retryWhy;
    const std::map<int, RowInFile> retryRows = rowsInFile(blockedFile, retryWhy);
    const bool retried =
        retryLanded && afterRetry > beforeRetry && retryRows.count(3) == 1 && retryRows.at(3).output == "DP-A";
    check(retried, "re-asserting a name after a failed write writes it to the file");
    if (!retried) {
        std::fprintf(stderr, "  %u write(s) landed for the retry, %u before it; %s; monitor 3 output '%s'\n",
                     afterRetry, beforeRetry, retryWhy.empty() ? "read back" : retryWhy.c_str(),
                     retryRows.count(3) ? retryRows.at(3).output.c_str() : "(no row)");
    }
    check(blocked.snapshot().health.persistence.status == wallpaperx::HealthStatus::Ok,
          "the persistence health recovers once a retried write lands");

    name(3, "DP-A");
    // A name CHANGE is made after the redundant call and waited for, and the total is graded
    // only once that change is on disk. Reading the count at the instant the redundant call
    // returned would grade an unsynchronised moment -- persist() abandons its wait after two
    // seconds, so on a host that hit the deadline a write the redundant call made lands after
    // the sample and reads as no write at all, leaving this check green for the build it exists
    // to catch. The changing call is the barrier: DP-C can only be on disk at the moment its
    // own call returned if the worker was never behind, so anything the redundant call queued
    // was written and counted. A host where it was behind never gets there -- the count moves
    // during the wait below and the phase skips.
    name(3, "DP-C");
    const unsigned afterHealthyChange = renamesCounted();
    if (!waitForOutput(blockedFile, 3, "DP-C")) {
        check(false, "a name change made after a retried write reaches the file");
        return;
    }
    if (afterHealthyChange != renamesCounted()) {
        notVerified(kRetryGoal, "this host finished the daemon's config write only after the call had returned, so a "
                                "write the redundant call made cannot be told from a late one");
        return;
    }
    const bool repeatWroteNothing = afterHealthyChange == afterRetry + 1;
    check(repeatWroteNothing, "once the retry has landed, re-asserting the same name writes nothing again");
    if (!repeatWroteNothing) {
        std::fprintf(stderr, "  %u write(s) landed for the redundant call and the change after it, %u for the retry\n",
                     afterHealthyChange, afterRetry);
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6j -- Step 10: the repair a start performs is written at that start, so the
// next one has nothing left to repair.
// ---------------------------------------------------------------------------- //
void runStartupRepairChecks() {
    constexpr const char* kGoals = "S10.14 (a start that repairs the config writes the repair, so a second start with "
                                   "no client acting reports nothing and no empty row survives)";
    const ScratchDir scratch("startuprepair");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths) return;
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path& file = paths->config;
    const std::filesystem::path& present = paths->wallpapers[0];

    // Three rows, chosen so the load has to repair something and NO client call could ever
    // be the thing that writes the repair down: a screen renamed by a later row, and a row
    // for an id this build cannot apply whose only content is the name the live screen has
    // just taken. The second is left holding nothing at all.
    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(1, "static", present.string(), "DP-3"));
    monitors.push_back(configRow(1, "", "", "DP-4"));
    monitors.push_back(configRow(40, "", "", "DP-4"));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    std::string firstWhy;
    std::map<int, RowInFile> afterFirstStart;
    std::string firstDetail;
    {
        WallpaperManager::Config firstConfig;
        firstConfig.configFile = file;
        WallpaperManager first(firstConfig);
        const std::vector<int> restored = first.restoreSaved();
        check(restored == std::vector<int>{1}, "the one row this build can apply is posted for rendering");
        // The two calls backend/daemon/main.cpp makes once it owns the bus name, in that
        // order. restoreSaved() computes the repair and holds it -- writing it there would
        // land on a running daemon's config and over the bytes preserveUnreadableConfig()
        // copies aside -- so a phase that read the file after it alone would grade a startup
        // sequence the daemon does not perform.
        first.preserveUnreadableConfig();
        first.writeRepairedConfig();
        firstDetail = first.snapshot().health.persistence.detail;
        // Read with no client call of any kind in between. That is the whole point: a save
        // driven by a SetWallpaper or a SetMonitorOutput would establish nothing about
        // whether the START wrote anything. Waiting is sound for the same reason: nothing but
        // the start can put anything in this file, so whatever the wait sees the start wrote.
        const std::string presentPath = present.string();
        waitForRows(file, [&presentPath](const std::map<int, RowInFile>& rows) {
            return rows.count(1) == 1 && rows.at(1).path == presentPath && rows.at(1).output == "DP-4";
        });
        afterFirstStart = rowsInFile(file, firstWhy);
    }

    // COVERAGE-ASSERT: everything below is a statement about a repair, so the first start is
    // asserted to have really performed and reported one. Without this, a build that found
    // nothing to repair would satisfy every check in this phase by doing nothing at all.
    const std::optional<int> displacedCount = clauseCountOrSkip(firstDetail, " screen names displaced", kGoals);
    if (!displacedCount.has_value()) return;
    const bool firstStartRepaired = *displacedCount == 2;
    check(firstStartRepaired, "the first start reports the repair it performed");
    if (!firstStartRepaired) std::fprintf(stderr, "  persistence detail: %s\n", firstDetail.c_str());

    // The empty-row rule below is graded on a key being ABSENT from this read, and a read that
    // failed has no keys at all -- so a start that wrote nothing, or a truncated file, would
    // satisfy it. Established first, and separately, so the failure line says which of the two
    // broke rather than merging a read fault into a verdict on the daemon.
    //
    // CONTROL: this phase's read pointed at a path that does not exist -> "FAIL: the config
    // this phase reads back was really written", printing "the config file is empty or absent;
    // ids on disk: (none)", and nothing below it graded. With this precondition disabled and
    // the same broken read, "a kept row left holding nothing is not written back" reported
    // PASS on the empty map and only the content check went red. Observed 2026-08-15. Both
    // reverted.
    const bool firstReadOk = firstWhy.empty() && afterFirstStart.count(1) == 1;
    check(firstReadOk, "the config this phase reads back was really written");
    if (!firstReadOk) {
        std::fprintf(stderr, "  %s; ids on disk: %s\n", firstWhy.empty() ? "read back" : firstWhy.c_str(),
                     idsOf(afterFirstStart).c_str());
        return;
    }

    // GOAL: the repair a start performs is on disk when that start finishes, with no client
    //       call. False if the write waits for the next save: a client that only re-asserts
    //       names it already holds writes nothing, so the next save may never come -- and the
    //       config keeps the state the daemon has already rejected, which the user meets again
    //       as the same health note on every boot.
    // WHY-THIS-SHAPE: the file is read inside the first manager's lifetime and before any
    //       client call, after a bounded wait for the repaired content, and it is graded on the
    //       row's CONTENT, not on the file changing. Reading at the instant writeRepairedConfig()
    //       returned is what a simpler version would do, and persist() abandons its wait after
    //       two seconds -- so on a loaded host that read prints this FAIL against a daemon whose
    //       write was merely still in flight. A wait that expires leaves the check red.
    //       Asking whether monitor 1 carries "DP-4" is true of the unrepaired file as well --
    //       the fixture's second row says exactly that -- so only asking for the wallpaper and
    //       the name on one row separates the file this run wrote from the file it read.
    // CONTROL: `if (repaired) m_repair = persistableLocked();` disabled at the end of
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp), so
    //       writeRepairedConfig() has nothing to write -> "FAIL: the repair a start performs is
    //       on disk before any client call", printing "monitor 1 holds type '' path '' output
    //       'DP-4'; ids on disk: 1 40", and "FAIL: a second start with no client acting has
    //       nothing left to repair", printing a second-start detail carrying "2 screen names
    //       displaced by a later entry" again. Observed 2026-08-15. Reverted.
    const bool repairOnDisk = afterFirstStart.count(1) == 1 && afterFirstStart.at(1).path == present.string() &&
                              afterFirstStart.at(1).output == "DP-4";
    check(repairOnDisk, "the repair a start performs is on disk before any client call");
    if (!repairOnDisk) {
        std::fprintf(stderr, "  %s; monitor 1 holds type '%s' path '%s' output '%s'; ids on disk: %s\n",
                     firstWhy.empty() ? "read back" : firstWhy.c_str(),
                     afterFirstStart.count(1) ? afterFirstStart.at(1).type.c_str() : "(no row)",
                     afterFirstStart.count(1) ? afterFirstStart.at(1).path.c_str() : "(no row)",
                     afterFirstStart.count(1) ? afterFirstStart.at(1).output.c_str() : "(no row)",
                     idsOf(afterFirstStart).c_str());
    }

    // GOAL: a kept row that has been stripped of its last content is not written back. False
    //       if it is: the row reaches the file with no type, no path and no name, and the NEXT
    //       start reads that emptiness as a wallpaper no build can apply -- a loss the user
    //       never suffered, reported again on every boot, and self-sustaining because the
    //       empty row is written out once more each time.
    // WHY-THIS-SHAPE: monitor 40's row is the one shape that reaches this rule -- its name is
    //       the only thing on it and the live screen takes that name -- and the check asks
    //       whether the file has a row for it at all, rather than whether that row is empty.
    //       "The row is empty" is satisfied by the build that writes it.
    // CONTROL: the empty-row skip in WallpaperManager::persistableLocked()
    //       (backend/daemon/wallpaper_manager.cpp) put back to an unconditional
    //       `entries.push_back(std::move(row));` -> "FAIL: a kept row left holding nothing is
    //       not written back", printing "ids on disk: 1 40", and "FAIL: a second start with no
    //       client acting has nothing left to repair", whose detail then carried "1 entries
    //       dropped, no build can apply them" -- the phantom loss this rule exists to stop.
    //       Observed 2026-08-14. Reverted.
    const bool emptyRowNotWritten = afterFirstStart.count(40) == 0;
    check(emptyRowNotWritten, "a kept row left holding nothing is not written back");
    if (!emptyRowNotWritten) {
        std::fprintf(stderr, "  ids on disk: %s; monitor 40 holds type '%s' path '%s' output '%s'\n",
                     idsOf(afterFirstStart).c_str(), afterFirstStart.at(40).type.c_str(),
                     afterFirstStart.at(40).path.c_str(), afterFirstStart.at(40).output.c_str());
    }

    // GOAL: after one start has repaired the config, a second start on the same file -- with
    //       no client acting between them -- finds nothing to repair and says nothing about
    //       one. False if any repair clause comes back: the note the user reads is then a
    //       permanent fixture of every boot rather than a report of something that happened,
    //       which is how a real loss stops being noticeable.
    // WHY-THIS-SHAPE: a second WallpaperManager over the same path, asserted to have restored
    //       the wallpaper, and graded on the ABSENCE of all three repair clauses rather than
    //       on the detail being any particular string. The detail legitimately carries the
    //       config path and may carry standing facts about kept rows, so an equality check
    //       would be brittle in exactly the direction that hides a regression. The restore is
    //       asserted first so that a second start which read nothing at all -- an empty or
    //       unparseable file -- cannot pass by having no clauses to print.
    // CONTROL: as for the two checks above; each of the two mutations that redden them
    //       reddens this one too, with a different clause. Observed 2026-08-14. Reverted.
    WallpaperManager::Config secondConfig;
    secondConfig.configFile = file;
    WallpaperManager second(secondConfig);
    const std::vector<int> secondRestored = second.restoreSaved();
    const WallpaperManager::Snapshot secondSnap = second.snapshot();
    const std::string& secondDetail = secondSnap.health.persistence.detail;
    const bool secondStartRead = secondRestored == std::vector<int>{1} && secondSnap.outputs.count("DP-4") == 1;
    check(secondStartRead, "the second start reads the repaired config back");
    const std::string clausesFound = repairClausesIn(secondDetail);
    const bool secondStartQuiet = clausesFound.empty();
    check(secondStartQuiet, "a second start with no client acting has nothing left to repair");
    if (!secondStartRead || !secondStartQuiet) {
        std::fprintf(stderr, "  second start restored %zu row(s), %zu name(s);%s; persistence detail: %s\n",
                     secondRestored.size(), secondSnap.outputs.size(),
                     clausesFound.empty() ? " no repair clause" : clausesFound.c_str(), secondDetail.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6j2 -- S10.17: the startup repair does not park the thread that is about to
// start serving.
// ---------------------------------------------------------------------------- //
void runStartupRepairIsNotBlockingChecks() {
    constexpr const char* kGoals = "S10.17 (the startup repair hands its write to the writer and returns, so a config "
                                   "directory that has stopped answering cannot park the daemon between claiming the "
                                   "bus name and serving requests)";
    const ScratchDir scratch("repairnotblocking");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;
    const std::filesystem::path& file = paths->config;
    const std::filesystem::path& present = paths->wallpapers[0];
    const ScopedEnv stateHome("XDG_STATE_HOME", (scratch.path() / "state").string());

    std::string fixtureWhy;
    if (!blockingWriteFixtureWorks(scratch.path() / "wallpaperx" / "probe.json", fixtureWhy)) {
        notVerified(kGoals, fixtureWhy);
        return;
    }

    // A screen renamed by a later row: a repair no client call could ever be the thing that
    // writes down, which is why the write happens at startup at all.
    nlohmann::json monitors = nlohmann::json::array();
    monitors.push_back(configRow(1, "static", present.string(), "DP-3"));
    monitors.push_back(configRow(1, "", "", "DP-4"));
    nlohmann::json root;
    root["version"] = 1;
    root["monitors"] = monitors;
    if (!writeFile(file, root.dump())) {
        notVerified(kGoals, "the config file this phase reads could not be written");
        return;
    }

    BlockingWrite blocked(file);
    if (!blocked.arm()) {
        notVerified(kGoals, std::string("a fifo could not be created at ") + blocked.temp() +
                                ", so no write can be made to block on this host");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();
    manager.preserveUnreadableConfig();

    // GOAL: S10.17 -- writeRepairedConfig() returns without waiting for the write to land.
    //       False if it waits: backend/daemon/main.cpp calls it after requestName() and before
    //       the event loop and the HTTP server exist, so on a config directory that has stopped
    //       answering the daemon owns the bus name for the whole deadline while dispatching to
    //       nobody, and the plugin's polls are refused outright.
    // WHY-THIS-SHAPE: the write is really parked, on a fifo, and the elapsed time is compared
    //       against a quarter of the daemon's own deadline. Two cheaper versions were rejected.
    //       Timing this call on a healthy tmpfs measures nothing -- the write takes microseconds,
    //       so a build that waits for it returns just as fast. And asserting only that the repair
    //       reaches the file cannot see the wait at all, since a blocking write also arrives.
    //       The parked write's bytes are graded beside the timing, because "returned instantly"
    //       is trivially true of a build that queues nothing: that check is what stops this
    //       phase being satisfied by losing the repair altogether.
    // CONTROL: writeRepairedConfig() (backend/daemon/wallpaper_manager.cpp) calling persist()
    //       rather than queuePersist(), which is the shape before this fix -> "FAIL: the
    //       startup repair does not park the daemon on the config write", printing "it took
    //       2000 ms against a 2000 ms deadline", with the queued-bytes check still green.
    //       Observed 2026-08-15.
    const auto start = StallClock::now();
    manager.writeRepairedConfig();
    const double elapsedMs = millisSince(start);

    // Released after the measurement, and read back: a write that never reached the fifo means
    // the repair was not queued at all, which the fixture probe above has already ruled out as
    // a fault of this host.
    const BlockingWrite::Released released = blocked.release(std::chrono::milliseconds(10000));
    const bool queued = released.ok;
    check(queued, "the startup repair hands a write to the writer");
    if (!queued) std::fprintf(stderr, "  %s\n", released.why.c_str());

    const bool prompt = elapsedMs < kSaveDeadlineMs / 4;
    check(prompt, "the startup repair does not park the daemon on the config write");
    if (!prompt) {
        std::fprintf(stderr, "  it took %.0f ms against a %.0f ms deadline\n", elapsedMs, kSaveDeadlineMs);
    }

    if (queued) {
        // COVERAGE-ASSERT: the write that was parked is the repair, not some empty snapshot --
        // "returned promptly" would otherwise be satisfied by a build that queued nothing worth
        // writing.
        const nlohmann::json wrote = nlohmann::json::parse(released.bytes, nullptr, false);
        const bool carriedRepair = !wrote.is_discarded() && wrote.is_object() && wrote.contains("monitors") &&
                                   wrote["monitors"].is_array() && wrote["monitors"].size() == 1 &&
                                   wrote["monitors"][0].value("path", std::string()) == present.string() &&
                                   wrote["monitors"][0].value("output", std::string()) == "DP-4";
        check(carriedRepair, "the write the startup repair queued carries the repaired row");
        if (!carriedRepair) {
            std::fprintf(stderr, "  the fifo carried %zu bytes: %s\n", released.bytes.size(),
                         released.bytes.c_str());
        }
    }
    std::printf("config_store_test: a startup repair against a parked write returned in %.0f ms\n", elapsedMs);
}

// ---------------------------------------------------------------------------- //
// Phase 6k -- Step 10: a health snapshot taken while the first save is publishing
// its worker. The workload the ThreadSanitizer tier needs; see the block below for
// what this does and does not assert on an ordinary build.
// ---------------------------------------------------------------------------- //
void runConcurrentHealthReadChecks() {
    constexpr const char* kGoal = "S10.15 (the first save publishes its worker where every reader of it is "
                                  "synchronised, so a /health request beside it is not a data race)";
    const ScratchDir scratch("concurrenthealth");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoal, 1);
    if (!paths) return;
    const std::filesystem::path& file = paths->config;
    const std::filesystem::path& wallpaper = paths->wallpapers[0];

    WallpaperManager::Config managerConfig;
    managerConfig.configFile = file;
    WallpaperManager manager(managerConfig);

    // GOAL: the daemon's first save and a concurrent health request do not race on the worker
    //       the save publishes. False if that pointer is published unsynchronised: the HTTP
    //       thread reads it under the state lock while the bus thread writes it under nothing,
    //       which is undefined behaviour and, on a real toolchain, a torn or stale read of a
    //       shared_ptr -- either a missed persistence status or a double free of the control
    //       block.
    // WHY-THIS-SHAPE: THIS PHASE ASSERTS NOTHING ABOUT THE RACE, deliberately. It exists to
    //       produce the overlap, and the instrument that grades it is ThreadSanitizer: build
    //       with -DWALLPAPERX_SANITIZE=thread and run this binary. An assertion here would be
    //       an assertion on the scheduler -- a race that does not fire on this run is
    //       indistinguishable from no race, so a green check would mean nothing and a red one
    //       would be flaky. What the phase establishes instead is that the overlap really
    //       happened -- the reader was already running before the save began, and completed at
    //       least one snapshot while it was in flight -- and a run where it did not is a SKIP,
    //       not a pass and not a failure. The reader is started and confirmed running BEFORE
    //       requestWallpaper, rather than both being launched together, because the pointer is
    //       published exactly once, on the first save -- two threads racing to start would
    //       usually miss that instant.
    // CONTROL: the publication in ConfigWriter::queue() (backend/daemon/config_writer.cpp) put
    //       back to its unsynchronised shape -- `startLocked(); worker = m_worker;` moved out of
    //       the m_mutex scope -- and the tree rebuilt with -DWALLPAPERX_SANITIZE=thread ->
    //       "WARNING: ThreadSanitizer: data race" twice, naming ConfigWriter::startLocked()
    //       writing m_worker from this thread against ConfigWriter::everQueued() and
    //       outcomeLocked() reading it under WallpaperManager::applyPersistOutcomeLocked() under
    //       snapshot(); ctest reported config_store as Failed rather than Skipped, the sanitizer
    //       replacing the exit code. Every check in this phase stayed GREEN in that run (589
    //       snapshots, 220 of them in flight), which is the point of the block above: the
    //       sanitizer is the oracle here, not the assertions. With the lock restored the same
    //       build is clean again. Re-observed 2026-08-18 after the writer moved out of the
    //       manager. Reverted.
    // NOTE, on which lock: the pointer is now published under ConfigWriter's own m_mutex, not
    //       under m_stateMutex. The property is unchanged -- one lock covers every reader of
    //       that pointer -- but the writer no longer takes any lock belonging to the manager,
    //       which is why setMonitorOutput can and does call m_writer.outcome() with m_stateMutex
    //       held.
    // CONTROL, for the overlap being reported as a skip rather than a pass: a three-second
    //       sleep added to this reader's loop, so no read can complete inside the save window
    //       -> exit 77 with "2 snapshot(s) in total, 0 of them while the save was in flight"
    //       and "NOT VERIFIED -- S10.15 ... nothing overlapped it for the sanitizer to grade",
    //       and no check failing. The production-side version of the same state -- the save made
    //       while m_stateMutex is held, so no health read can complete beside it -- still cannot
    //       be used as a control, but only through the wrapper: WallpaperManager::persist() takes
    //       m_stateMutex on its failure path and would self-deadlock. m_writer.write() itself no
    //       longer takes any manager lock, so it holds nobody out of snapshot(). Observed
    //       2026-08-14, re-reasoned 2026-08-18. Reverted.
    // COVERAGE-ASSERT: a snapshot is counted as an overlap only when the save was in flight
    //       both before the read began and after it ended, and a run where that count is zero
    //       is reported as a SKIP rather than a pass. So a
    //       future edit that leaves the reader not actually reading, or serialised behind the
    //       save, cannot leave the sanitizer tier with nothing to sanitize while this phase
    //       reads green. It is not graded as a failure: whether another thread is handed CPU
    //       inside a two-microsecond window is a property of the host, not of the daemon, and
    //       a check() there fails a correct build on a loaded single-CPU runner.
    std::atomic<unsigned> snapshotsTaken{0};
    std::atomic<unsigned> snapshotsDuringSave{0};
    std::atomic<bool> saveInFlight{false};
    bool readerStarted = false;
    bool saveRefused = false;
    std::string refusal;
    // Written by the reader only, read only after ~jthread has joined it -- the join is the
    // happens-before edge, so no lock is needed.
    std::string readerThrew;
    {
        // The stop_token, not a flag of our own: every check below can be reached by a path
        // that leaves the loop running, and ~jthread requests the stop on all of them --
        // including one that throws something no catch here names. A hand-rolled flag has to
        // be set on each of those paths, and the destructor would then join a loop nobody
        // stopped.
        //
        // CONTROL: a bare `return;` added on the path out of this block, i.e. one exit that
        // sets no flag. With the token the binary finished normally (exit 0, every later phase
        // still run); with the loop back on a hand-rolled `std::atomic<bool>` it hung until
        // killed at 30 s, printing nothing at all -- no phase name, no diagnostic, the failure
        // mode this shape exists to remove. Observed 2026-08-14. Both reverted.
        //
        // CONTROL, for the handler: `throw std::runtime_error("CONTROL")` added at the top of
        // the loop body -> "FAIL: the health read beside the first save completed without
        // throwing", printing "WallpaperManager::snapshot() threw: CONTROL", and the phases
        // after this one still ran. With the catch arms rethrowing instead, the same throw
        // ended the binary in std::terminate: exit 134, "terminate called after throwing an
        // instance of 'int'", no FAIL line, and none of the later phases run. Observed
        // 2026-08-15. Both reverted.
        std::jthread reader([&](std::stop_token stopping) {
            // An exception escaping a thread's callable is std::terminate, which destroys the
            // diagnostics this phase exists to print. Reported, not swallowed: it goes red as a
            // FAIL below and the overlap counts of that run are discarded.
            try {
                while (!stopping.stop_requested()) {
                    // Sampled on BOTH sides of the snapshot. The flag rises once and falls once
                    // for the whole phase, so true before it began and true after it ended is
                    // what proves the save was in flight across the read's entire interval. The
                    // single sample taken after the call also counts a snapshot that finished
                    // all its work before the save started, which is the one thing this counter
                    // exists to rule out.
                    //
                    // CONTROL: a two-second sleep between the snapshot and the second sample,
                    // and three seconds of the save window held open after requestWallpaper --
                    // a read whose whole workload ran before the save began and which is still
                    // alive when the flag rises -> "2 snapshot(s) in total, 0 of them while the
                    // save was in flight" and "NOT VERIFIED -- S10.15 ...", exit 77. With the
                    // second sample alone deciding, the same run reported "2 health snapshots
                    // taken beside the first save, 1 of them while it was in flight" and "all
                    // checks passed" -- an overlap certified for a read that never had one.
                    // Observed 2026-08-15. All reverted.
                    const bool inFlightBefore = saveInFlight.load(std::memory_order_acquire);
                    const WallpaperManager::Snapshot taken = manager.snapshot();
                    (void)taken;
                    ++snapshotsTaken;
                    if (inFlightBefore && saveInFlight.load(std::memory_order_acquire)) ++snapshotsDuringSave;
                }
            } catch (const std::exception& e) {
                readerThrew = e.what();
            } catch (...) {
                readerThrew = "an exception that is not a std::exception";
            }
        });

        // A deadline with a real failure branch: without it a reader that never ran would
        // leave the save unobserved and this phase would report a concurrency property it
        // never exercised.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (snapshotsTaken.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        readerStarted = snapshotsTaken.load() > 0;
        if (readerStarted) {
            saveInFlight.store(true, std::memory_order_release);
            try {
                manager.requestWallpaper("static", wallpaper.string(), 1);
            } catch (const std::exception& e) {
                saveRefused = true;
                refusal = e.what();
            }
            saveInFlight.store(false, std::memory_order_release);
        }
    }

    if (!readerThrew.empty()) {
        check(false, "the health read beside the first save completed without throwing");
        std::fprintf(stderr, "  WallpaperManager::snapshot() threw: %s\n", readerThrew.c_str());
        return;
    }
    if (!readerStarted) {
        notVerified(kGoal, "no health snapshot was taken within five seconds, so nothing ran beside the save");
        return;
    }
    if (saveRefused) {
        notVerified(kGoal, "the save this phase races against was refused: " + refusal);
        return;
    }
    if (snapshotsDuringSave.load() == 0) {
        std::fprintf(stderr, "  %u snapshot(s) in total, %u of them while the save was in flight\n",
                     snapshotsTaken.load(), snapshotsDuringSave.load());
        notVerified(kGoal, "no health snapshot completed while the first save was in flight, so nothing overlapped it "
                           "for the sanitizer to grade");
        return;
    }
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return rows.count(1) == 1; });
    std::string why;
    const std::map<int, RowInFile> onDisk = rowsInFile(file, why);
    const bool saveLanded = onDisk.count(1) == 1;
    check(saveLanded, "the save this phase races against really happened");
    if (!saveLanded) {
        std::fprintf(stderr, "  %s; ids on disk: %s\n", why.empty() ? "read back" : why.c_str(),
                     idsOf(onDisk).c_str());
    }
    std::printf("config_store_test: %u health snapshots taken beside the first save, %u of them while it was in "
                "flight\n",
                snapshotsTaken.load(), snapshotsDuringSave.load());
}

// ---------------------------------------------------------------------------- //
// Phase 6m -- Step 10: naming a screen answers for its own write, not for the
// daemon's overall persistence health.
// ---------------------------------------------------------------------------- //
void runNameAnswersForItsOwnWriteChecks() {
    constexpr const char* kGoals = "S10.13's reporting half (naming a screen fails only when THAT name did not reach "
                                   "the config file, not when some other row's write failed)";
    const ScratchDir scratch("nameownwrite");
    if (!scratch.ok()) {
        notVerified(kGoals, "a scratch directory could not be created");
        return;
    }
    // A filename that cannot be written into JSON at all, on a monitor this phase never names.
    // It is the one cause of a daemon-wide persistence failure a client can actually produce.
    const std::filesystem::path oddlyNamed = scratch.path() / std::string("bad\x80name.png");
    if (!writeWallpaperFile(oddlyNamed) || wallpaperx::isValidUtf8(oddlyNamed.string())) {
        notVerified(kGoals, "this host would not create a file whose name is not valid UTF-8, so no unrelated "
                            "persistence failure can be produced for a screen name to be judged against");
        return;
    }
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);

    // GOAL: naming a screen answers for its own write. False if it answers for the daemon's
    //       persistence health: that field is an aggregate carrying every other row's trouble --
    //       here, a different monitor's un-encodable filename -- so a client whose name WAS
    //       written is told it was lost, and the plugin's documented response to that error is to
    //       ask the user to run the app again on a screen that is already named.
    // WHY-THIS-SHAPE: the unrelated failure is produced by a real un-encodable path on a
    //       different monitor rather than by contriving a health value, because that is the only
    //       route a client has to it and it is what makes the aggregate non-empty. The name is
    //       asserted to be IN THE FILE beside the no-throw: "it did not throw" alone is equally
    //       true of a build that stopped writing names at all, which would be a worse defect
    //       wearing this check's green.
    // CONTROL: WallpaperManager::setMonitorOutput (backend/daemon/wallpaper_manager.cpp) put back
    //       to reading the aggregate -- `DaemonHealth h = m_health; applyPersistOutcomeLocked(h);
    //       if (h.persistence.status == Fail) throw;` -- which is the code before this fix ->
    //       "FAIL: naming a screen succeeds when that name reached the file, whatever else is
    //       wrong", printing "it threw: <config path>; monitor 1 output 'DP-OK'" -- the name in
    //       the file on the same line as the error saying it was not.
    manager.requestWallpaper("static", oddlyNamed.string(), 0);
    waitForRows(file, [](const std::map<int, RowInFile>& rows) { return !rows.empty() || true; });
    const WallpaperManager::Snapshot beforeNaming = manager.snapshot();
    if (beforeNaming.health.persistence.status != wallpaperx::HealthStatus::Fail) {
        notVerified(kGoals, "the un-encodable filename did not make the daemon's persistence health fail, so there is "
                            "no unrelated failure for a screen name to be judged against");
        return;
    }

    std::string threw;
    try {
        manager.setMonitorOutput(1, "DP-OK");
    } catch (const std::exception& e) {
        threw = e.what();
    } catch (...) {
        threw = "an exception that is not a std::exception";
    }
    const bool landed = waitForRows(file, [](const std::map<int, RowInFile>& rows) {
        return rows.count(1) == 1 && rows.at(1).output == "DP-OK";
    });
    const bool answeredForItself = threw.empty() && landed;
    check(answeredForItself, "naming a screen succeeds when that name reached the file, whatever else is wrong");
    if (!answeredForItself) {
        std::string why;
        const std::map<int, RowInFile> onDisk = rowsInFile(file, why);
        std::fprintf(stderr, "  %s; monitor 1 output '%s'\n",
                     threw.empty() ? "the call returned normally" : ("it threw: " + threw).c_str(),
                     onDisk.count(1) ? onDisk.at(1).output.c_str() : "(no row)");
    }
    // COVERAGE-ASSERT: the daemon is still reporting the unrelated failure. Without it, a build
    // that had simply stopped noticing the un-encodable path would satisfy the check above by
    // having nothing to be confused by.
    const WallpaperManager::Snapshot afterNaming = manager.snapshot();
    const bool stillReportsTheOther = afterNaming.health.persistence.status == wallpaperx::HealthStatus::Fail;
    check(stillReportsTheOther, "the unrelated failure this phase is about is still reported after the name lands");
    if (!stillReportsTheOther) {
        std::fprintf(stderr, "  persistence reads %s (%s)\n",
                     wallpaperx::toString(afterNaming.health.persistence.status),
                     afterNaming.health.persistence.detail.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6p -- Step 10: the surviving alarm is escaped once, not once per read.
// ---------------------------------------------------------------------------- //
void runStickyAlarmEscapingChecks() {
    constexpr const char* kGoals = "S6.40's persistence half (a path that reaches the health field is escaped once, "
                                   "whatever else the daemon reports about it)";
    const ScratchDir scratch("alarmescape");
    if (!scratch.ok()) {
        notVerified(kGoals, "a scratch directory could not be created");
        return;
    }
    // A backslash in the config directory's name. It is a legal byte in a Linux filename and the
    // one character whose escaping is visibly not idempotent: sanitizeForLog turns it into two,
    // and two into four.
    const std::filesystem::path oddDir = scratch.path() / "conf\\dir";
    const std::filesystem::path file = oddDir / "wallpaperx" / "config.json";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        notVerified(kGoals, "this host would not create a directory whose name contains a backslash, so double "
                            "escaping cannot be made visible");
        return;
    }
    // One malformed entry, so a copy is owed; and nowhere to put it, so the copy fails and the
    // sticky alarm is what health reports.
    const std::filesystem::path blocked = scratch.path() / "not-a-directory";
    if (!writeFile(blocked, "this is a file, not a state directory") ||
        !writeFile(file, "{\"version\":1,\"monitors\":[{\"monitor_id\":\"two\",\"type\":\"static\","
                         "\"path\":\"/photos/right.png\"}]}")) {
        notVerified(kGoals, "the fixture this phase needs could not be written");
        return;
    }
    const ScopedEnv stateHome("XDG_STATE_HOME", blocked.string());
    bool occupied = true;
    for (int n = 0; n < 10 && occupied; ++n) {
        occupied = writeFile(file.parent_path() /
                                 ("config.json.bad" + (n == 0 ? std::string() : "." + std::to_string(n))),
                             "an earlier broken config " + std::to_string(n));
    }
    if (!occupied) {
        notVerified(kGoals, "the ten occupied copy names beside the config could not be written, so the copy cannot "
                            "be made to fail");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();
    manager.preserveUnreadableConfig();

    // GOAL: the path in the health detail is escaped exactly once. False if a report re-escapes
    //       an already-escaped string: one backslash in the user's config path is shown as two,
    //       and the path they are being sent to look at is not a path that exists. This is the
    //       defect the codebase already names -- "escaping twice turns one backslash in a path
    //       into two in what the user reads" -- reached by a different route.
    // WHY-THIS-SHAPE: read from a manager that has been asked to preserve and NOT asked to save,
    //       because the branch at issue is the one that reports the surviving alarm when no
    //       writer exists yet; once a save has been made, the detail is composed from raw text
    //       and escaped once. The expected rendering is computed with production's own
    //       sanitizeForLog rather than written out by hand, so the check states "escaped once"
    //       rather than pinning today's escape syntax.
    // COVERAGE-ASSERT: the alarm really is the thing being reported -- status Fail and no writer
    //       -- so this cannot pass by reading a detail that never went near the branch.
    //
    // CONTROL: report(HealthStatus::Fail, health.persistence.detail) restored in place of the
    //       status-only assignment in applyPersistOutcomeLocked()'s "nothing has been asked of
    //       the file yet" branch -- the shape bcac12f shipped, which this check caught. report()
    //       sanitizes, over a string recordHealthLocked() had already sanitized, and
    //       sanitizeForLog is not idempotent -> "FAIL: the config path in the surviving alarm is
    //       escaped once, not twice", printing a directory named `conf\dir` as `conf\\\\dir`:
    //       an alarm naming a path that does not exist. Fixed by carrying only the severity,
    //       since the detail was already correct when it was recorded.
    const WallpaperManager::Snapshot snap = manager.snapshot();
    const bool alarmShowing = snap.health.persistence.status == wallpaperx::HealthStatus::Fail &&
                              snap.health.persistence.detail.find("not copied") != std::string::npos;
    check(alarmShowing, "the surviving alarm is what this phase reads");
    if (!alarmShowing) {
        std::fprintf(stderr, "  persistence: %s / %s\n", wallpaperx::toString(snap.health.persistence.status),
                     snap.health.persistence.detail.c_str());
        return;
    }
    const std::string escapedOnce = sanitizeForLog(file.string(), 1024, LogFraming::Bare);
    const std::string escapedTwice = sanitizeForLog(escapedOnce, 1024, LogFraming::Bare);
    const bool escapedOnlyOnce = snap.health.persistence.detail.find(escapedOnce) != std::string::npos &&
                                 snap.health.persistence.detail.find(escapedTwice) == std::string::npos;
    check(escapedOnlyOnce, "the config path in the surviving alarm is escaped once, not twice");
    if (!escapedOnlyOnce) {
        std::fprintf(stderr, "  detail holds: %s\n  escaped once would be: %s\n  escaped twice is: %s\n",
                     snap.health.persistence.detail.c_str(), escapedOnce.c_str(), escapedTwice.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6o -- Step 10: the three channels that answer "is the file up to date"
// answer the same way about one write.
// ---------------------------------------------------------------------------- //

// What each channel said about the same write, sampled around one re-assertion of a name the
// manager already holds. Kept together because the property under test is that they AGREE:
// sampled apart, three separate checks each pass while the daemon contradicts itself.
struct FileAnswers {
    bool healthOk = false;   // the persistence health field, read before the call
    bool replyOk = false;    // the reply SetMonitorOutput would give a client
    unsigned writes = 0;     // whether the idempotent shortcut let a write through
    std::string threw;
};

FileAnswers askEveryChannel(WallpaperManager& manager, const std::filesystem::path& file, int monitorId,
                            const std::string& outputName) {
    FileAnswers out;
    out.healthOk = manager.snapshot().health.persistence.status == wallpaperx::HealthStatus::Ok;
    countRenamesOnto(file);
    try {
        manager.setMonitorOutput(monitorId, outputName);
    } catch (const std::exception& e) {
        out.threw = e.what();
    } catch (...) {
        out.threw = "an exception that is not a std::exception";
    }
    out.replyOk = out.threw.empty();
    out.writes = renamesCounted();
    return out;
}

void runOneAnswerAboutTheFileChecks() {
    constexpr const char* kGoals = "S10.13/S10.16's shared premise (the health field, the reply to SetMonitorOutput "
                                   "and the idempotent re-assert shortcut cannot disagree about one write)";

    // GOAL: the three channels that answer "does the config file hold what live state holds"
    //       give the same answer about the same write. False whenever they are derived
    //       separately: three review rounds found that defect in three costumes -- a name
    //       reported lost that was written, a name reported stored that was refused, and a name
    //       reported durable while the write that would have made it so had never completed. The
    //       damage is always the same shape: the client acts on one channel and the daemon
    //       believes another.
    // WHY-THIS-SHAPE: all three are sampled around ONE call, in several states, and compared to
    //       each other rather than each to a constant. Three separate checks -- health is Fail
    //       here, the reply throws there -- is what this file already had, and it passed
    //       throughout the two rounds in which the channels disagreed. The re-assertion is of a
    //       name the manager ALREADY holds, because that is the only call that reaches the
    //       shortcut; a new name would bind and write whatever the outcome said. States with a
    //       failed preservation or a dropped wallpaper are deliberately excluded: those make
    //       health Fail for losses that are not "the file is out of date", so the equivalence
    //       being asserted would not hold and the check would be grading the wrong thing.
    //       Each state also carries the answer that is TRUE of it, and both channels are graded
    //       against that as well as against each other. Agreement alone is not enough and was
    //       measured not to be: with one shared derivation the channels agree by construction, so
    //       a mutation of that derivation makes them consistently wrong and an agreement-only
    //       check stays green. Agreement guards against the derivation being split again;
    //       correctness guards against the one derivation being wrong.
    // CONTROL, for the correctness half: `outstanding` in ConfigWriter::outcomeLocked()
    //       (backend/daemon/config_writer.cpp) forced to false, which is the pre-bcac12f reading
    //       that consulted only completed writes -> "FAIL: the file is reported out of date, when
    //       a write is outstanding and the caller gave up on it", printing "health said up-to-date
    //       and the reply said up-to-date, but the file does not hold what live state holds", plus
    //       the stalled-write health check two phases down -- two red in the run, no skips.
    //       Re-observed 2026-08-19, after this phase moved its parked call onto an abandonable
    //       thread; unchanged.
    // CONTROL, for the agreement half: the shortcut in WallpaperManager::setMonitorOutput given
    //       back an inference of its own -- `m_writer.lastResult().error.empty()`, i.e. only a
    //       COMPLETED write with an error counts, in place of `m_writer.outcome().onDisk` --
    //       which is the shape that let two callers disagree -> six red, this phase's four led by
    //       "FAIL: every channel agrees about the write, when a write is outstanding and the
    //       caller gave up on it", printing "health said not-up-to-date, the reply said
    //       up-to-date, and 0 write(s) were made", and the same pair for the refused-write state.
    //       The two phases below it go red on the same edit -- one mutation, all three of the
    //       states this cluster is about. Re-observed 2026-08-19, after this phase moved its
    //       parked call onto an abandonable thread; still six, still no skips.
    // COVERAGE-ASSERT: every one of the three states is accounted for at the end -- compared, or
    //       skipped with a printed reason. A state that stops being reachable with no reason given
    //       (an early return added later, a fixture that silently does nothing) breaks the sum and
    //       reddens, so the phase cannot shrink to one trivial comparison and stay green; a state
    //       this host genuinely cannot stage says so and the run is a skip, not a pass, because
    //       every arm that stops staging one also calls notVerified().
    int compared = 0;
    int statesSkipped = 0;
    const auto compare = [&compared](const FileAnswers& answers, bool expectedOnDisk, const char* what) {
        ++compared;
        const bool agree = answers.healthOk == answers.replyOk;
        check(agree, (std::string("every channel agrees about the write, when ") + what).c_str());
        if (!agree) {
            std::fprintf(stderr, "  health said %s, the reply said %s, and %u write(s) were made%s%s\n",
                         answers.healthOk ? "up-to-date" : "not-up-to-date",
                         answers.replyOk ? "up-to-date" : "not-up-to-date", answers.writes,
                         answers.threw.empty() ? "" : "; it threw: ", answers.threw.c_str());
        }
        // What is actually true of this state, so a build in which every channel is wrong the
        // same way cannot pass by agreeing with itself.
        const bool correct = answers.healthOk == expectedOnDisk && answers.replyOk == expectedOnDisk;
        check(correct, (std::string(expectedOnDisk ? "the file is reported up to date, when "
                                                   : "the file is reported out of date, when ") +
                        what)
                           .c_str());
        if (!correct) {
            std::fprintf(stderr, "  health said %s and the reply said %s, but the file %s\n",
                         answers.healthOk ? "up-to-date" : "not-up-to-date",
                         answers.replyOk ? "up-to-date" : "not-up-to-date",
                         expectedOnDisk ? "holds what live state holds" : "does not hold what live state holds");
        }
        // The other half of the same agreement: a daemon that believes the file is up to date
        // must take the shortcut, and one that does not must try again. Without this, "they
        // agree" is satisfied by a build whose shortcut ignores both of them.
        const bool shortcutMatches = answers.healthOk ? answers.writes == 0 : answers.writes == 0 || !answers.replyOk;
        check(shortcutMatches,
              (std::string("the re-assert shortcut follows the same answer, when ") + what).c_str());
        if (!shortcutMatches) {
            std::fprintf(stderr, "  %u write(s) for a daemon reporting %s\n", answers.writes,
                         answers.healthOk ? "up-to-date" : "not-up-to-date");
        }
    };

    {
        const ScratchDir scratch("oneanswer");
        const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
        if (!paths.has_value()) return;
        const std::filesystem::path& file = paths->config;

        std::string fixtureWhy;
        if (!blockingWriteFixtureWorks(scratch.path() / "wallpaperx" / "probe.json", fixtureWhy)) {
            notVerified(kGoals, fixtureWhy);
            return;
        }

        WallpaperManager::Config config;
        config.configFile = file;
        // shared_ptr because state two parks a request on a thread this phase may abandon, and
        // that thread must be able to keep the manager alive rather than be left holding a
        // destroyed one.
        auto manager = std::make_shared<WallpaperManager>(config);
        manager->setMonitorOutput(3, "DP-A");
        if (!waitForRows(file, [](const std::map<int, RowInFile>& rows) {
                return rows.count(3) == 1 && rows.at(3).output == "DP-A";
            })) {
            notVerified(kGoals, "the first name never reached the file, so there is no up-to-date state to compare "
                                "the others against");
            return;
        }
        // State one: quiescent and healthy. It is the state in which the shortcut is SUPPOSED to
        // fire, so it is what stops this phase being satisfied by a daemon that never takes it.
        compare(askEveryChannel(*manager, file, 3, "DP-A"), /*expectedOnDisk=*/true, "the file is up to date");

        // State two: a write parked on a fifo, waited out, and given up on. The state the old
        // predicate got wrong -- it read completed writes only, so it called the file up to date
        // while nothing had landed.
        BlockingWrite blocked(file);

        // GOAL (a property of this harness, X.3): staging the outstanding-write state is bounded
        //       by something this phase owns, so a build whose save never comes back reddens or
        //       skips with a reason instead of hanging. False whenever the only thing that can
        //       release this phase is the daemon's own kWriteDeadline.
        // WHY-THIS-SHAPE: two guards, because the bound can be lost two ways. The request is made
        //       on a thread the phase abandons at kParkedCallPatience -- calling requestWallpaper()
        //       inline is the shorter version and is what this phase used to do: with kWriteDeadline
        //       raised it never returns, so the binary hangs, printing no phase name, no FAIL line
        //       and no reason. That is not hypothetical here, it is paid history -- a control run on
        //       the stalled-save phase below had to be re-observed at 30 s instead of the hour it
        //       wanted, because THIS phase hung first. The second guard is cheaper and fires before
        //       anything is staged: the comparison below grades elapsed time against kSaveDeadlineMs,
        //       so a deadline that is not comfortably inside the patience makes "waited out its
        //       deadline" and "was abandoned" the same observation, and the state is then skipped
        //       with a reason rather than measured.
        // CONTROL: (a) kWriteDeadline (backend/daemon/config_writer.cpp) raised to 300 s -> this
        //       phase printed "NOT VERIFIED -- ... requestWallpaper had not returned 20 s after a
        //       write that cannot finish, so the outstanding-write state was never staged", and the
        //       whole binary finished in 63 s at exit 1, red on the stalled-save phase below which
        //       is what that mutation is FOR. With the same mutation and this call put back inline
        //       -- the shorter shape -- the binary printed nothing on stdout at all and was killed
        //       at 90 s, exit 124. Both reverted. (b) kSaveDeadlineMs above raised to 30000 ->
        //       "NOT VERIFIED -- ... the save deadline this phase grades against (30000 ms) is not
        //       comfortably inside the 20000 ms this phase is willing to wait for a parked call",
        //       in 7 s and with no fifo write staged at all. Reverted. Observed 2026-08-19.
        if (!blocked.arm()) {
            ++statesSkipped;
            notVerified(kGoals, std::string("a fifo could not be created at ") + blocked.temp() +
                                    ", so no write can be left outstanding on this host");
        } else if (kSaveDeadlineMs >= 0.5 * parkedCallPatienceMs()) {
            ++statesSkipped;
            notVerified(kGoals, "the save deadline this phase grades against (" +
                                    std::to_string(static_cast<int>(kSaveDeadlineMs)) +
                                    " ms) is not comfortably inside the " +
                                    std::to_string(static_cast<int>(parkedCallPatienceMs())) +
                                    " ms this phase is willing to wait for a parked call, so a write left "
                                    "outstanding could not be told from one this phase merely gave up on");
        } else {
            const AbandonedOrReturned parked =
                requestOnAbandonableThread(manager, paths->wallpapers[0].string(), 0, kParkedCallPatience);
            if (!parked.returned) {
                // Released before the reason is printed: the abandoned thread is still inside the
                // manager, and letting its write through is what gets it out before this process
                // ends. Its result is not read -- the state was never staged, so there is nothing
                // here to grade.
                blocked.release(std::chrono::milliseconds(10000));
                ++statesSkipped;
                notVerified(kGoals, "requestWallpaper had not returned " +
                                        std::to_string(static_cast<int>(parkedCallPatienceMs() / 1000)) +
                                        " s after a write that cannot finish, so the outstanding-write state was "
                                        "never staged and nothing below it was measured");
            } else {
                const FileAnswers outstanding = askEveryChannel(*manager, file, 3, "DP-A");
                const BlockingWrite::Released released = blocked.release(std::chrono::milliseconds(10000));
                if (!released.ok || parked.elapsedMs < kSaveDeadlineMs * 0.9 || !parked.threw.empty()) {
                    ++statesSkipped;
                    notVerified(kGoals,
                                !released.ok ? "no config write reached the fifo: " + released.why
                                : !parked.threw.empty()
                                    ? "the parked request threw (" + parked.threw +
                                          "), so the daemon this would compare is not in the state the comparison "
                                          "is about; whether a stalled write may fail its request is graded by the "
                                          "stalled-save phase"
                                    : "the save returned in " + std::to_string(static_cast<int>(parked.elapsedMs)) +
                                          " ms rather than waiting out its deadline, so no write was ever "
                                          "outstanding");
                } else {
                    compare(outstanding, /*expectedOnDisk=*/false,
                            "a write is outstanding and the caller gave up on it");
                }
            }
        }
    }

    // State three: a config the daemon could not read and could not move aside, so every write
    // is refused before a writer thread exists. A separate manager because the refusal is decided
    // in the constructor and cannot be entered from a healthy one.
    const ScratchDir refusedScratch("oneanswerrefused");
    if (!refusedScratch.ok()) {
        ++statesSkipped;
        notVerified(kGoals, "a scratch directory for the refused-write state could not be created");
    } else {
        const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(refusedScratch).string());
        const std::filesystem::path refusedFile = refusedScratch.path() / "wallpaperx" / "config.json";
        std::error_code ec;
        std::filesystem::create_directories(refusedFile.parent_path(), ec);
        if (ec || ::mkfifo(refusedFile.c_str(), 0644) != 0) {
            ++statesSkipped;
            notVerified(kGoals, "a named pipe could not be created at the config path, so no write can be refused "
                                "before a writer exists");
        } else {
            WallpaperManager::Config refusedConfig;
            refusedConfig.configFile = refusedFile;
            WallpaperManager refused(refusedConfig);
            refused.restoreSaved();
            refused.preserveUnreadableConfig();
            try {
                refused.setMonitorOutput(4, "DP-B");
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch) -- graded below
            }
            compare(askEveryChannel(refused, refusedFile, 4, "DP-B"), /*expectedOnDisk=*/false,
                    "every write is refused before a writer exists");
        }
    }

    check(compared + statesSkipped == 3, "all three states this phase sets up were compared or skipped with a reason");
    if (compared + statesSkipped != 3) {
        std::fprintf(stderr, "  %d of 3 states compared, %d skipped with a stated reason, and the rest vanished "
                             "without one\n",
                     compared, statesSkipped);
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6n -- Step 10: re-asserting a name the daemon refused to write.
// ---------------------------------------------------------------------------- //
void runRefusedNameRetryChecks() {
    constexpr const char* kGoals = "S10.13's retry clause on a REFUSED write (a name the daemon declined to write is "
                                   "reported, and the identical retry is reported too rather than reading as success)";
    const ScratchDir scratch("refusedname");
    if (!scratch.ok()) {
        notVerified(kGoals, "a scratch directory could not be created");
        return;
    }
    const ScopedEnv stateHome("XDG_STATE_HOME", stateHomeOf(scratch).string());
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // A named pipe: load() cannot read it AND preserveUnreadableConfig() will not move it aside
    // (it is not an ordinary file), so the daemon refuses every write for the run -- the one
    // refusal that is decided before any writer thread exists.
    if (ec || ::mkfifo(file.c_str(), 0644) != 0) {
        notVerified(kGoals, "a named pipe could not be created at the config path, so no write can be refused before "
                            "a writer exists");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    manager.restoreSaved();
    manager.preserveUnreadableConfig();

    const auto name = [&manager](int monitorId, const std::string& outputName) {
        try {
            manager.setMonitorOutput(monitorId, outputName);
        } catch (const std::exception& e) {
            return std::string(e.what());
        } catch (...) {
            return std::string("an exception that is not a std::exception");
        }
        return std::string();
    };

    // GOAL: a refusal decided before any writer exists is still a failed write, so re-asserting
    //       the same name reports the failure again. False if the second call reads as success:
    //       the daemon told the client the same name was lost and then durable, with nothing on
    //       disk having changed -- and re-asserting is the only retry a client has, so the second
    //       answer is the one it acts on. The config here can never be written for the life of
    //       the run, so a success is never true.
    // WHY-THIS-SHAPE: the refusal is a config the daemon could not read AND could not move
    //       aside, which is the one refusal decided in ConfigWriter::queue() before a writer
    //       thread exists -- the worker-side failure the read-only-directory phase already covers
    //       cannot reach this arm, because there a writer does exist and its result carries the
    //       error. The IDENTICAL name is re-asserted, because that is the call the idempotence
    //       skip applies to; a different name would take the binding path and prove nothing.
    // CONTROL: `m_notWritten = m_refusal;` deleted from ConfigWriter::queue()'s refusal branch
    //       (backend/daemon/config_writer.cpp), so a refusal decided before any writer exists is
    //       invisible to outcome() -- the successor to the deleted persistFailedLocked()'s
    //       `if (m_configUnread) return true;` -> both checks below red, "FAIL: re-asserting a
    //       name the daemon refused to write is refused again, not reported as stored" printing
    //       "the retry returned normally", and the first call red the same way. The coverage
    //       assert stays GREEN, correctly: the refusal still stops the write, only the report is
    //       lost. Seven red in all. Re-observed 2026-08-18.
    const std::string firstThrew = name(3, "DP-A");
    const bool firstRefused = !firstThrew.empty();
    check(firstRefused, "a name the daemon will not write is reported as an error");
    if (!firstRefused) std::fprintf(stderr, "  the first call returned normally\n");

    const std::string retryThrew = name(3, "DP-A");
    const bool retryRefused = !retryThrew.empty();
    check(retryRefused, "re-asserting a name the daemon refused to write is refused again, not reported as stored");
    if (!retryRefused) std::fprintf(stderr, "  the retry returned normally\n");

    // COVERAGE-ASSERT: nothing was written over the user's config, which is what makes both
    // answers above true rather than merely consistent. A build that had quietly replaced the
    // fifo with a config of its own would make "refused" a lie in the other direction.
    check(!isRegularFile(file), "the config the daemon refused to write over is still the file the user left there");
}

// ---------------------------------------------------------------------------- //
// Phase 7 -- S8.6: a save that cannot happen fails loudly and harmlessly.
// ---------------------------------------------------------------------------- //
void runFailedSaveChecks() {
    const ScratchDir scratch("readonly");
    if (!scratch.ok()) {
        notVerified("S8.6 (a failed save does not fail SetWallpaper, and does not go unreported)",
                    "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path readOnly = scratch.path() / "ro";
    const std::filesystem::path wallpaper = scratch.path() / "wallpaper.png";
    std::error_code ec;
    std::filesystem::create_directories(readOnly, ec);
    if (ec || !writeWallpaperFile(wallpaper)) {
        notVerified("S8.6", "the read-only directory this phase needs could not be created");
        return;
    }
    std::filesystem::permissions(readOnly, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);

    // X.3: root ignores the mode bits, and so do some filesystems. A phase that cannot
    // make the write fail must say so -- asserting "the save failed" where the save in
    // fact succeeded would report the daemon as broken for a property of the host.
    { std::ofstream probe(readOnly / "probe"); }
    if (std::filesystem::exists(readOnly / "probe")) {
        std::filesystem::remove(readOnly / "probe", ec);
        notVerified("S8.6", "this process can write into a directory it made read-only (running as root, or a "
                            "filesystem that ignores mode bits), so no save can be made to fail");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = readOnly / "wallpaperx" / "config.json";
    WallpaperManager manager(config);

    // GOAL: S8.6, both clauses -- a save that fails does NOT fail the request, and does
    //       NOT leave the daemon reporting health. Neither implies the other, and the
    //       dangerous direction is a build that satisfies the first by swallowing the
    //       error: the user is told the wallpaper is set, and nothing anywhere says it
    //       will not survive a reboot (PLAN.md decision 4).
    // WHY-THIS-SHAPE: driven through WallpaperManager rather than over the bus. The save
    //       is inside requestWallpaper(), and DbusService::setWallpaper adds only the
    //       throw-to-DBus-error translation -- so a bus round trip would test sdbus's
    //       error mapping, which S6.10's checks already cover, while costing this file a
    //       session bus it otherwise does not need.
    // CONTROL: two, both reverted, both re-observed 2026-08-18. (a) the Fail in
    //       applyPersistOutcomeLocked()'s `!outcome.onDisk` branch changed to Ok, i.e. the
    //       error swallowed where health is now composed -> "FAIL: a save that failed leaves the
    //       persistence health check reporting failure" while the no-throw check stayed green,
    //       which is the pair of results the two clauses exist to distinguish. (b) a `throw
    //       std::runtime_error(why)` added on persist()'s failure path -> "FAIL: a config write
    //       that cannot happen does not fail the request that triggered it", printing
    //       "requestWallpaper threw: .../ro/wallpaperx: could not be created: Permission denied",
    //       and nine phases abandoned through main()'s handler.
    // WAS: (a) named deleting the recordHealth call in WallpaperManager::persist(). Re-run
    //       2026-08-18 that reddens NOTHING -- since bcac12f the health is derived from the
    //       writer's own result at snapshot time, so removing the recording changes no answer
    //       this phase can see. The check is not vacuous; the mutation named for it had gone
    //       inert, which is why it is restated at the composition site above.
    std::string thrown;
    try {
        manager.requestWallpaper("static", wallpaper.string(), 0);
    } catch (const std::exception& e) {
        thrown = e.what();
    }
    check(thrown.empty(), "a config write that cannot happen does not fail the request that triggered it");
    if (!thrown.empty()) std::fprintf(stderr, "  requestWallpaper threw: %s\n", thrown.c_str());

    const WallpaperManager::Snapshot snap = manager.snapshot();
    check(snap.monitors.count(0) == 1 && snap.monitors.at(0).path == wallpaper.string(),
          "the wallpaper is applied even though it could not be persisted");
    check(snap.health.persistence.status == wallpaperx::HealthStatus::Fail,
          "a save that failed leaves the persistence health check reporting failure");
    check(!snap.health.persistence.detail.empty(),
          "the persistence failure says what went wrong rather than failing silently");
    std::printf("config_store_test: persistence health after a refused write: %s (%s)\n",
                wallpaperx::toString(snap.health.persistence.status), snap.health.persistence.detail.c_str());

    // The other half of decision 4's health contract, and the reason `disabled` is Ok
    // rather than Fail: /health aggregates worst-wins, so a daemon that never opted in
    // must not report itself broken.
    WallpaperManager::Config off;
    WallpaperManager disabledManager(off);
    const WallpaperManager::Snapshot offSnap = disabledManager.snapshot();
    check(offSnap.health.persistence.status == wallpaperx::HealthStatus::Ok &&
              offSnap.health.persistence.detail == "disabled",
          "a manager with persistence disabled reports persistence ok with detail 'disabled'");
}

// Points the process's standard error at a file for as long as it is alive, and puts the real
// one back. The only way to read what the config writer says: it reports with write(2) on the
// descriptor, deliberately -- that thread outlives main() when a write is stuck, so a FILE* torn
// down by exit is not something it can hold -- and nothing in WallpaperManager's interface
// carries the line.
class CapturedStderr {
public:
    explicit CapturedStderr(std::filesystem::path file) : m_file(std::move(file)) {
        m_saved = ::dup(STDERR_FILENO);
        const int fd = ::open(m_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return;
        m_active = m_saved >= 0 && ::dup2(fd, STDERR_FILENO) >= 0;
        ::close(fd);
    }
    ~CapturedStderr() { restore(); }

    CapturedStderr(const CapturedStderr&) = delete;
    CapturedStderr& operator=(const CapturedStderr&) = delete;

    bool ok() const { return m_active; }
    std::string text() const { return readFile(m_file); }

    // Idempotent, and called explicitly before anything is printed: a check that failed while
    // the redirection was still up would write its own diagnosis into the capture instead of
    // to the reader.
    void restore() {
        if (m_active) {
            ::dup2(m_saved, STDERR_FILENO);
            m_active = false;
        }
        if (m_saved >= 0) {
            ::close(m_saved);
            m_saved = -1;
        }
    }

private:
    std::filesystem::path m_file;
    int m_saved = -1;
    bool m_active = false;
};

// ---------------------------------------------------------------------------- //
// Phase 7b -- Step 10: a failed write is announced by the thread that learns it.
// ---------------------------------------------------------------------------- //
void runFailedWriteIsAnnouncedChecks() {
    constexpr const char* kGoals = "S10.17's reporting half (a config write that failed is said on the console by the "
                                   "writer itself, which is the only report that cannot race the write it describes)";
    const ScratchDir scratch("writesaysso");
    if (!scratch.ok()) {
        notVerified(kGoals, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path readOnly = scratch.path() / "ro";
    const std::filesystem::path wallpaper = scratch.path() / "wallpaper.png";
    std::error_code ec;
    std::filesystem::create_directories(readOnly, ec);
    if (ec || !writeWallpaperFile(wallpaper)) {
        notVerified(kGoals, "the read-only directory this phase needs could not be created");
        return;
    }
    std::filesystem::permissions(readOnly, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    // X.3: root ignores the mode bits, and so do some filesystems. With no failed write there is
    // nothing for the daemon to announce, and every check below would grade a healthy save.
    { std::ofstream probe(readOnly / "probe"); }
    if (std::filesystem::exists(readOnly / "probe")) {
        std::filesystem::remove(readOnly / "probe", ec);
        notVerified(kGoals, "this process can write into a directory it made read-only (running as root, or a "
                            "filesystem that ignores mode bits), so no save can be made to fail");
        return;
    }

    // GOAL: S10.17's reporting half -- a write that failed reaches the console. False if it is
    //       only in GetStatus: the startup repair is QUEUED and not waited on (S10.17, which must
    //       stay true), so main()'s own snapshot is taken while the write is still in flight and
    //       reports a clean start. A user has no reason to open GetStatus on a daemon that said
    //       nothing, and the rows the repair was meant to fix are gone by the next restart.
    // WHY-THIS-SHAPE: the process's standard error is redirected to a file, because the report is
    //       made with write(2) from the writer's own thread and no interface of the manager
    //       carries it. The captured line is required to name THIS phase's own scratch directory
    //       rather than merely to contain "config not saved": other phases in this binary drive
    //       failing writes too, and a substring check would be satisfied by one of theirs. The
    //       capture is polled with a bound and a failure branch -- a wait that expires fails the
    //       check, it never falls through into treating the stall as an answer.
    // CONTROL: the `::write(STDERR_FILENO, line.data(), line.size())` block deleted from the
    //       writer thread's loop in ConfigWriter::startLocked()
    //       (backend/daemon/config_writer.cpp), which is the code before this fix -> "FAIL: a
    //       config write that failed is announced on standard error", printing "nothing naming
    //       this phase's own directory was written to standard error in 5000 ms; the capture
    //       holds: (nothing)", and nothing else red. Re-observed 2026-08-18.
    const std::filesystem::path configFile = readOnly / "wallpaperx" / "config.json";
    const std::filesystem::path capturedTo = scratch.path() / "stderr.txt";
    std::string captured;
    bool captureWorked = false;
    {
        CapturedStderr capture(capturedTo);
        if (capture.ok()) {
            captureWorked = true;
            WallpaperManager::Config config;
            config.configFile = configFile;
            WallpaperManager manager(config);
            manager.requestWallpaper("static", wallpaper.string(), 0);
            // The writer announces before it publishes its result, so a healthy build has already
            // written the line by the time the call returns. Polled anyway, with a bound: the
            // announcement is on another thread, and the bound expiring is graded as a failure
            // below rather than being allowed to look like a pass.
            const auto deadline = StallClock::now() + std::chrono::milliseconds(5000);
            for (;;) {
                captured = capture.text();
                if (captured.find(scratch.path().string()) != std::string::npos) break;
                if (StallClock::now() >= deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        capture.restore();
    }
    if (!captureWorked) {
        harnessFailure("this phase could not redirect its own standard error, so what the config writer says was "
                       "never read");
        return;
    }

    const bool announced = captured.find("config not saved") != std::string::npos &&
                           captured.find(scratch.path().string()) != std::string::npos;
    check(announced, "a config write that failed is announced on standard error");
    if (!announced) {
        std::fprintf(stderr, "  nothing naming this phase's own directory was written to standard error in 5000 ms; "
                             "the capture holds: %s\n",
                     captured.empty() ? "(nothing)" : sanitizeForLog(captured).c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 8 -- S8.10: the save the DBus thread performs is the cheap one.
// ---------------------------------------------------------------------------- //

// Writes `text` through a temp file and rename, exactly as ConfigStore::save() does, with
// the durability the design rejected switched on. Its only job is to show the counter this
// phase reads is live: a build where the interposed fsync stopped binding would count zero
// for the daemon too, and the phase would be green on nothing.
bool writeDurably(const std::filesystem::path& file, const std::string& text) {
    const std::string temp = file.string() + ".probe";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const char* data = text.data();
    size_t left = text.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, data, left);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        data += n;
        left -= static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    return ::rename(temp.c_str(), file.c_str()) == 0;
}

void runSaveCostChecks() {
    constexpr const char* kGoal = "S8.10 (the save on SetWallpaper does not make a DBus handler slow)";

    const ScratchDir onDisk("savecost");
    // preparePhase, not a bare onDisk.ok(): the closing check reads back the path the daemon
    // persisted -- which is what establishes the zero-fsync count above was measured over a real
    // save -- and a temporary directory whose name is not valid UTF-8 makes the writer skip that
    // entry by design (S8.7).
    const std::optional<PhasePaths> paths = preparePhase(onDisk, kGoal, 1);
    if (!paths.has_value()) return;
    const std::filesystem::path persistedFile = paths->config;
    const std::filesystem::path probeFile = onDisk.path() / "wallpaperx" / "probe.json";
    const std::filesystem::path wallpaper = paths->wallpapers.front();
    std::error_code ec;
    std::filesystem::create_directories(persistedFile.parent_path(), ec);
    if (ec) {
        notVerified(kGoal, "the directory this phase's probe writer needs could not be created");
        return;
    }

    WallpaperManager::Config persisting;
    persisting.configFile = persistedFile;
    WallpaperManager persistingManager(persisting);

    // One warm-up, so the graded request is not also paying for the directory creation.
    persistingManager.requestWallpaper("static", wallpaper.string(), 0);
    // GOAL: S8.10 -- the save a SetWallpaper performs on the DBus event-loop thread is the
    //       write+rename variant, not the durable one the design rejected (9.3 us against
    //       823.8 us, PLAN.md Step 8). False if the handler flushes to the platter before
    //       replying: every caller then waits on the disk.
    // WHY-THIS-SHAPE: the durability syscalls are counted, not the elapsed time. Timing the
    //       save against a write+rename this file performs is the obvious version and does not
    //       work: its bound is the geometric mean of two disk costs, while the
    //       quantity it grades also carries a fixed CPU cost -- the JSON build -- that no
    //       yardstick in the phase measures and that does not shrink when the disk gets
    //       faster. On a host with a fast fsync the bound falls below what correct code
    //       already costs, so it accuses the daemon of a property of the storage; it also
    //       had to skip itself entirely under a sanitizer, which instruments the daemon's
    //       side and not the probe's. A count is exact on every filesystem, tmpfs included,
    //       and under every build.
    //       COVERAGE LOST, stated plainly: nothing here now bounds the save's absolute
    //       latency, only its durability class. A save made slow some other way -- an
    //       accidentally quadratic serialiser, a synchronous network path -- would not be
    //       caught by this phase.
    // CONTROL: `::fsync(fd);` added before the `if (::close(fd) != 0)` in
    //       ConfigStore::save() -> "FAIL: the save a SetWallpaper performs issues no fsync
    //       or fdatasync", printing "  1 durability syscall(s) during one requestWallpaper",
    //       and nothing else red. Reverted.
    // COVERAGE-ASSERT: two, because a count of zero is what a dead instrument and a correct
    //       daemon both produce. (a) the same counter is asserted to MOVE for a write this
    //       file makes durable on purpose, so a build where the interposed fsync stopped
    //       binding fails instead of passing; (b) the config file's contents are asserted
    //       afterwards, so a manager that ignored its configFile -- and so called nothing at
    //       all -- cannot pass either. CONTROL for (a): this file's interposing `fsync`
    //       renamed so it no longer binds -> "FAIL: a write this file makes durable on
    //       purpose is counted, so the counter can see an fsync at all". Renamed back.
    const unsigned before = g_fsyncCalls.load();
    persistingManager.requestWallpaper("static", wallpaper.string(), 0);
    const unsigned duringSave = g_fsyncCalls.load() - before;
    check(duringSave == 0, "the save a SetWallpaper performs issues no fsync or fdatasync");
    if (duringSave != 0) {
        std::fprintf(stderr, "  %u durability syscall(s) during one requestWallpaper\n", duringSave);
    }

    // X.3: a missing file here is the DAEMON not having saved, which is a failure and must
    // not be reported as this file's probe writer failing. A probe write that fails is
    // genuinely this side of the boundary and stays a skip.
    const std::string payload = readFile(persistedFile);
    if (payload.empty()) {
        check(false, "the request this phase grades wrote the config file");
        return;
    }
    const unsigned beforeProbe = g_fsyncCalls.load();
    if (!writeDurably(probeFile, payload)) {
        notVerified(kGoal, "the probe writer could not write into the scratch directory, so the counter this phase "
                           "relies on could not be shown to be live");
        return;
    }
    const unsigned duringProbe = g_fsyncCalls.load() - beforeProbe;
    check(duringProbe > 0,
          "a write this file makes durable on purpose is counted, so the counter can see an fsync at all");
    if (duringProbe == 0) {
        std::fprintf(stderr, "  the interposed fsync counted nothing for a write that calls it directly, so the "
                             "zero above establishes nothing about the daemon\n");
    }

    std::string why;
    const std::map<int, std::string> onDiskPaths = pathsInFile(persistedFile, why);
    const bool wrote = onDiskPaths.size() == 1 && onDiskPaths.count(0) == 1 &&
                       onDiskPaths.at(0) == wallpaper.string();
    check(wrote, "the graded request really did write the config file, so a save is what was observed");
    if (!wrote) std::fprintf(stderr, "  %s\n", why.empty() ? "the file holds something else" : why.c_str());
}

// ---------------------------------------------------------------------------- //
// Phases 9-11 -- S8.28-S8.33: a config write that blocks on the filesystem.
// ---------------------------------------------------------------------------- //

// StallClock, millisSince() and kSaveDeadlineMs are declared above the startup-repair phase,
// which is the first one to time a parked write.
//
// BlockingWrite, blockingWriteFixtureWorks() and isRegularFile() live in
// tests/client/blocking_config_write.h, shared with daemon_system_test: the fixture's
// correctness rests on details a second copy would drift away from silently.

// requestOnAbandonableThread(), AbandonedOrReturned and kParkedCallPatience are declared
// beside kSaveDeadlineMs above: the "one answer about the config file" phase parks a write
// too and needs the same bound.

void runStalledSaveChecks() {
    constexpr const char* kGoals = "S8.29/S8.31/S8.32 (a config write that never comes back)";

    const ScratchDir scratch("stalledsave");
    // preparePhase ahead of the baseline guard below, not instead of it: on a host whose
    // temporary directory is not valid UTF-8 the writer must skip these entries (S8.7), and the
    // baseline guard would then report the miss as "the healthy save did not happen", sending a
    // reader to the daemon's save path for a property of the machine. The guard stays for every
    // other cause, which is the one it was written for.
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 2);
    if (!paths.has_value()) return;
    const std::filesystem::path configFile = paths->config;
    const std::filesystem::path healthyWallpaper = paths->wallpapers[0];
    const std::filesystem::path stalledWallpaper = paths->wallpapers[1];

    WallpaperManager::Config config;
    config.configFile = configFile;
    auto manager = std::make_shared<WallpaperManager>(config);

    // A healthy save first, and it is a precondition rather than a check: what S8.32 forbids
    // is health showing the last save that WORKED while a later one is stuck, and that is
    // only a wrong answer when there is a right one for it to show. A phase that stalled the
    // very first save would assert the same words against a manager that had never persisted
    // anything.
    manager->requestWallpaper("static", healthyWallpaper.string(), 0);
    const WallpaperManager::Snapshot healthy = manager->snapshot();
    std::string why;
    const std::map<int, std::string> savedFirst = pathsInFile(configFile, why);
    if (healthy.health.persistence.status != wallpaperx::HealthStatus::Ok || savedFirst.count(0) != 1) {
        // X.3: the save path being broken is graded by the phases written for it. Here it is
        // a baseline that did not appear, so this phase establishes nothing and says so.
        notVerified(kGoals, "the healthy save this phase needs as a baseline did not happen (persistence reads " +
                                std::string(wallpaperx::toString(healthy.health.persistence.status)) + ", " +
                                (why.empty() ? "the file holds no entry for monitor 0" : why) + ")");
        return;
    }

    std::string fixtureWhy;
    if (!blockingWriteFixtureWorks(scratch.path() / "wallpaperx" / "probe.json", fixtureWhy)) {
        notVerified(kGoals, fixtureWhy);
        return;
    }
    BlockingWrite blocked(configFile);
    if (!blocked.arm()) {
        notVerified(kGoals, std::string("a fifo could not be created at ") + blocked.temp() +
                                ", so no write can be made to block on this host");
        return;
    }

    // GOAL: S8.29 -- a save whose write never returns releases the caller, and does so
    //       inside the two-second deadline rather than when the filesystem answers. False if
    //       the caller is still in requestWallpaper() when the dead mount is the only thing
    //       that could release it: that thread is the daemon's bus thread, and a SetWallpaper
    //       that never returns is a daemon that answers nothing until its watchdog kills it.
    //       False in the other direction too if the request FAILS -- a wallpaper the daemon
    //       applied must not be reported as rejected because the config write is slow.
    // WHY-THIS-SHAPE: the call is made on a thread this phase abandons, and the wait on it is
    //       bounded and has a failure branch. Calling requestWallpaper() inline here is the
    //       shorter version and is unusable: with the deadline removed it never returns, so
    //       the check does not fail, it hangs -- and a hung test prints no FAIL line, no
    //       reason and nothing a reader can act on. The elapsed time is asserted against a
    //       window (waited, but not forever) rather than a point, because how long past the
    //       deadline the wakeup lands is the scheduler's business.
    // CONTROL: three, all reverted, all against `kWriteDeadline` in ConfigWriter::write()'s
    //       settled.wait_for() (backend/daemon/config_writer.cpp). (a) raised to 300 s -> "FAIL: a
    //       save whose write cannot complete still releases the caller", printing
    //       "requestWallpaper has not returned 20.0 s after a write that cannot finish", the rest
    //       of this phase skipped with that reason, and the snapshot-wins phase reporting NOT
    //       VERIFIED rather than failing -- one red, exactly as before. Whole binary: 63 s.
    //       WAS: this said 30 s, "not the hour this used to say", because at an hour the binary
    //       hung before reaching here inside the "one answer about the config file" phase, which
    //       was bounded by nothing but this constant. True when written 2026-08-18; that phase now
    //       has a bound of its own, so this mutation can be as large as it likes.
    //       (b) `throw std::runtime_error("stalled")` added
    //       after the wait_for -> "FAIL: a config write that cannot complete does not fail the
    //       request that triggered it", printing "requestWallpaper threw: stalled". (c) for the
    //       upper bound specifically, replaced with 10 seconds, so the caller is released but far
    //       too late -> "FAIL: the caller of a stalled save waits for the deadline and not for the
    //       filesystem", printing "requestWallpaper took 10000 ms against a 2000 ms deadline", and
    //       nothing else red. (c) re-observed 2026-08-18, (a) at 300 s on 2026-08-19.
    const AbandonedOrReturned stalled =
        requestOnAbandonableThread(manager, stalledWallpaper.string(), 1, kParkedCallPatience);
    check(stalled.returned, "a save whose write cannot complete still releases the caller");
    if (!stalled.returned) {
        // Printed from the constant, not from a literal 20, so raising the patience cannot leave
        // this line naming a wait nobody made.
        std::fprintf(stderr, "  requestWallpaper has not returned %.1f s after a write that cannot finish; the "
                             "checks after this one are skipped rather than measured against a thread that is "
                             "still inside the manager\n",
                     parkedCallPatienceMs() / 1000.0);
        return;
    }
    check(stalled.threw.empty(), "a config write that cannot complete does not fail the request that triggered it");
    if (!stalled.threw.empty()) std::fprintf(stderr, "  requestWallpaper threw: %s\n", stalled.threw.c_str());
    const bool bounded = stalled.elapsedMs < 2 * kSaveDeadlineMs;
    check(bounded, "the caller of a stalled save waits for the deadline and not for the filesystem");
    if (!bounded) {
        std::fprintf(stderr, "  requestWallpaper took %.0f ms against a %.0f ms deadline\n", stalled.elapsedMs,
                     kSaveDeadlineMs);
    }

    // GOAL: S8.32 -- while a write is outstanding past the deadline, health reports THAT,
    //       and not the last save that succeeded. False if persistence still reads ok: the
    //       bus thread has stopped waiting on that write, so a stale ok is the only thing
    //       anyone would ever see, and an operator watching /health would be told the config
    //       is being saved while nothing has reached the disk since the mount died.
    // WHY-THIS-SHAPE: polled to a deadline with a failure branch, and the verdict is the
    //       LAST observation, not "did any observation match". A single snapshot taken right
    //       after the stalled call cannot be used: the stall is reported once the write is
    //       older than the deadline, and the caller is released at almost exactly that
    //       instant, so which side of the comparison that snapshot lands on is a race. The
    //       status and the wording are both asserted -- a `fail` with the previous save's
    //       detail would send an operator to look at a disk error that is not there.
    // CONTROL: ConfigWriter::stalledPastDeadline() (backend/daemon/config_writer.cpp) forced to
    //       false, which is where the old `if (writingSince && ... > kLoadDeadline)` test now
    //       lives -> "FAIL: health reports a write that has not come back rather than the last
    //       save that did", printing "persistence reads fail (... a write has not come back, the
    //       choice may not be on disk yet)", and nothing else red. Re-observed 2026-08-18.
    // WAS: this said the mutation printed "persistence reads ok" -- the healthy save above. True
    //       when written 2026-08-12; bcac12f then made an outstanding write a failure in its own
    //       right, so what this block uniquely supplies is now the WORDING, not the status. The
    //       check asserts both, which is why it still reddens.
    wallpaperx::HealthStatus sawStatus = wallpaperx::HealthStatus::Pending;
    std::string sawDetail;
    bool namesTheStall = false;
    const auto pollDeadline = StallClock::now() + std::chrono::seconds(8);
    while (StallClock::now() < pollDeadline) {
        const WallpaperManager::Snapshot snap = manager->snapshot();
        sawStatus = snap.health.persistence.status;
        sawDetail = snap.health.persistence.detail;
        namesTheStall = sawStatus == wallpaperx::HealthStatus::Fail &&
                        sawDetail.find("has not answered a write, the filesystem may be stalled") != std::string::npos;
        if (namesTheStall) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(namesTheStall, "health reports a write that has not come back rather than the last save that did");
    if (!namesTheStall) {
        std::fprintf(stderr, "  persistence reads %s (%s)\n", wallpaperx::toString(sawStatus), sawDetail.c_str());
    } else {
        // Asked only once there IS a stalled-write report: ungated, this reads the last good
        // save's detail instead, which also names the file, so it would pass while saying
        // nothing about the report it is named for.
        // CONTROL: the file name dropped from the stall detail in applyPersistOutcomeLocked()
        //       -> "FAIL: the stalled-write report names the file that is not answering", and
        //       nothing else red. Reverted.
        check(sawDetail.find(configFile.string()) != std::string::npos,
              "the stalled-write report names the file that is not answering");
    }

    // GOAL: S8.31 -- once a wait has run out, a later save does not pay the deadline again.
    //       False if every SetWallpaper during an outage costs another two seconds: the
    //       answer is already known, and a desktop that changes wallpaper on a timer would
    //       spend the outage with its bus thread asleep.
    // WHY-THIS-SHAPE: run on THIS thread, unlike the stalled call above, and that is safe
    //       for the reason the check exists: the worst a build without the short-circuit can
    //       do is wait one more deadline and return, which is a red check rather than a hang.
    //       The bound is a quarter of the deadline, not "faster than the first call": the
    //       second save does no I/O at all on this thread, so anything near the deadline is
    //       the defect, and a relative bound would be satisfied by 1.9 s.
    // CONTROL: the `!worker->stalled &&` short-circuit dropped from ConfigWriter::write()'s wait
    //       (backend/daemon/config_writer.cpp), so every save waits -> "FAIL: a second save during
    //       a stall does not wait for the deadline all over again", printing "the second save took
    //       2000 ms against a 2000 ms deadline", and nothing else red. Re-observed 2026-08-18
    //       after the wait moved out of the manager. Reverted.
    // COVERAGE-ASSERT: the first save's own elapsed time is asserted to be at least three
    //       quarters of the deadline. Without it this comparison is green on a build that
    //       waits for nothing at all -- both calls return instantly and the phase reports
    //       that the deadline is not paid twice, having never seen it paid once. CONTROL for
    //       it: `kWriteDeadline` (backend/daemon/config_writer.cpp) replaced with 0 seconds ->
    //       "FAIL: the first save really did wait for the deadline, so the comparison above has a
    //       baseline", printing "the first save took 0 ms", while the check it guards stayed
    //       green at 0 ms. Re-observed 2026-08-18. Reverted.
    const auto secondStart = StallClock::now();
    manager->requestWallpaper("static", healthyWallpaper.string(), 2);
    const double secondMs = millisSince(secondStart);
    const bool cheap = secondMs < kSaveDeadlineMs / 4;
    check(cheap, "a second save during a stall does not wait for the deadline all over again");
    if (!cheap) {
        std::fprintf(stderr, "  the second save took %.0f ms against a %.0f ms deadline\n", secondMs,
                     kSaveDeadlineMs);
    }
    const bool firstPaidIt = stalled.elapsedMs >= 0.75 * kSaveDeadlineMs;
    check(firstPaidIt, "the first save really did wait for the deadline, so the comparison above has a baseline");
    if (!firstPaidIt) {
        std::fprintf(stderr, "  the first save took %.0f ms, which is not a deadline being paid\n",
                     stalled.elapsedMs);
    }
    std::printf("config_store_test: a stalled save cost %.0f ms, the next one %.0f ms\n", stalled.elapsedMs,
                secondMs);
}

// ---------------------------------------------------------------------------- //
// Phase 11b -- S10.16: the config writer that could not be started.
// ---------------------------------------------------------------------------- //
void runNoWriterThreadChecks() {
    constexpr const char* kGoals = "S10.16 (a config writer whose thread could not be created is reported as failed "
                                   "persistence and retried, never remembered as a writer that exists)";
    const ScratchDir scratch("nowriterthread");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;
    const std::filesystem::path configFile = paths->config;
    const std::filesystem::path wallpaper = paths->wallpapers.front();

    WallpaperManager::Config config;
    config.configFile = configFile;
    // Constructed BEFORE the limit below drops. The constructor reads the config on a thread
    // of its own, and a manager that could not read its config refuses every save afterwards
    // for a reason that has nothing to do with this goal.
    WallpaperManager manager(config);

    rlimit original{};
    if (::getrlimit(RLIMIT_NPROC, &original) != 0) {
        notVerified(kGoals, "this host would not report RLIMIT_NPROC, so thread creation cannot be made to fail");
        return;
    }
    rlimit exhausted = original;
    exhausted.rlim_cur = 1;
    if (::setrlimit(RLIMIT_NPROC, &exhausted) != 0) {
        notVerified(kGoals, "RLIMIT_NPROC could not be lowered here, so thread creation cannot be made to fail");
        return;
    }
    // A guard, not a call at the end: the limit is process-wide, so anything that left this
    // phase early -- a throw out of the manager, a return added later -- would take every
    // following phase's threads with it and report the wreckage as daemon failures.
    struct RestoreLimit {
        rlimit original;
        bool restored = false;
        ~RestoreLimit() { restore(); }
        void restore() {
            if (!restored) restored = ::setrlimit(RLIMIT_NPROC, &original) == 0;
        }
    } restoreLimit{original};

    // X.3: a privileged process ignores RLIMIT_NPROC entirely. A phase that cannot make
    // thread creation fail must say so -- everything below would otherwise grade an ordinary
    // healthy save and report it as this goal being satisfied.
    bool fixtureWorks = false;
    try {
        std::thread probe([] {});
        probe.join();
    } catch (const std::system_error&) {
        fixtureWorks = true;
    }
    if (!fixtureWorks) {
        notVerified(kGoals, "this process still creates threads with RLIMIT_NPROC lowered (running as root, or with "
                            "CAP_SYS_RESOURCE), so no config write can be made to have no thread to run on");
        return;
    }

    const auto call = [&manager, &wallpaper](int monitorId, double& elapsedMs) {
        std::string threw;
        const auto start = StallClock::now();
        try {
            manager.requestWallpaper("static", wallpaper.string(), monitorId);
        } catch (const std::exception& e) {
            threw = e.what();
        } catch (...) {
            threw = "an exception that is not a std::exception";
        }
        elapsedMs = millisSince(start);
        return threw;
    };

    double firstMs = 0;
    const std::string firstThrew = call(0, firstMs);
    const WallpaperManager::Snapshot afterFirst = manager.snapshot();
    double secondMs = 0;
    const std::string secondThrew = call(1, secondMs);

    // Naming a screen while no writer can start, and then re-asserting the SAME name once one
    // can. The order is the whole point and cannot be relaxed: any other save in between writes
    // the binding out as part of the whole live state, so the re-assertion would land on a file
    // that already agreed with it and this would grade nothing.
    const auto name = [&manager](int monitorId, const std::string& outputName) {
        try {
            manager.setMonitorOutput(monitorId, outputName);
        } catch (const std::exception& e) {
            return std::string(e.what());
        } catch (...) {
            return std::string("an exception that is not a std::exception");
        }
        return std::string();
    };
    const std::string nameThrew = name(4, "DP-Z");

    restoreLimit.restore();
    const bool limitRestored = restoreLimit.restored;
    std::string reassertThrew;
    bool reassertLanded = false;
    if (limitRestored) {
        reassertThrew = name(4, "DP-Z");
        reassertLanded = waitForRows(configFile, [](const std::map<int, RowInFile>& rows) {
            return rows.count(4) == 1 && rows.at(4).output == "DP-Z";
        });
    }

    // GOAL: S10.16 -- with no thread available for the config writer, the wallpaper is still
    //       accepted and the daemon says persistence failed. False in two directions, and the
    //       second is the damaging one: a request that throws reaches the client as a render
    //       error, and a request that succeeds silently leaves persistence reading ok while
    //       nothing will ever be written again for the life of the process.
    // WHY-THIS-SHAPE: RLIMIT_NPROC is lowered around two real saves rather than a thread
    //       failure being simulated. The simpler alternative -- a test-only hook that makes
    //       the manager pretend the thread failed -- is production surface a test would own,
    //       which this project forbids, and it would grade the pretence rather than what
    //       std::thread does here. The limit is restored before anything is printed, and the
    //       phase skips rather than grades when the limit does not bite.
    // CONTROL: the fix reverted whole in ConfigWriter::startLocked()
    //       (backend/daemon/config_writer.cpp) -- `m_worker` published before the std::thread that
    //       services it AND the catch replaced by a rethrow, which together are the pre-fix shape
    //       -> the four checks below red: "a save with no thread for the writer does not fail the
    //       request" printing "requestWallpaper threw: Resource temporarily unavailable", "the
    //       daemon reports persistence as failed when it has no thread to write on" printing
    //       "persistence reads ok", "a later save does not wait on a writer that was never
    //       started" printing "the second save took 2000 ms", and "once threads are available
    //       again a save reaches the file" printing "ids on disk: (none)"; eight red in all.
    //       Both halves are needed: with the catch left in place the early publish alone reddens
    //       five OTHER checks and leaves these two green, because nothing escapes and the first
    //       save's timed-out wait sets the stall flag that makes the second cheap. Re-observed
    //       2026-08-18. Reverted.
    // CONTROL, for the two recovery checks: `m_refusing = true;` with the unreadable-config
    //       wording added beside that same catch -- the "refuse every write for the session"
    //       repair the review warned against -> "FAIL: once threads are available again a save
    //       reaches the file" and "FAIL: persistence reports itself working again once a save has
    //       landed", printing "not saved, this run could not read it and will not overwrite it".
    //       The two checks above stay green; the two name checks below now go red as well, since
    //       they require the reason to still name the missing thread. Re-observed 2026-08-18.
    check(firstThrew.empty(), "a save with no thread for the writer does not fail the request");
    if (!firstThrew.empty()) std::fprintf(stderr, "  requestWallpaper threw: %s\n", firstThrew.c_str());
    const bool reported = afterFirst.health.persistence.status == wallpaperx::HealthStatus::Fail &&
                          afterFirst.health.persistence.detail.find("no thread") != std::string::npos;
    check(reported, "the daemon reports persistence as failed when it has no thread to write on");
    if (!reported) {
        std::fprintf(stderr, "  persistence reads %s (%s)\n",
                     wallpaperx::toString(afterFirst.health.persistence.status),
                     afterFirst.health.persistence.detail.c_str());
    }

    // A second save is what separates "reported once" from "the daemon still believes it has a
    // writer": with the pointer published before the thread, every later save queues a
    // snapshot for nobody and pays the full deadline waiting for it.
    const bool cheap = secondMs < kSaveDeadlineMs / 4;
    check(cheap, "a later save does not wait on a writer that was never started");
    if (!cheap) {
        std::fprintf(stderr, "  the second save took %.0f ms against a %.0f ms deadline%s%s\n", secondMs,
                     kSaveDeadlineMs, secondThrew.empty() ? "" : ", and threw: ", secondThrew.c_str());
    }

    if (!limitRestored) {
        notVerified(kGoals, "RLIMIT_NPROC could not be put back, so whether persistence recovers was not asked");
        return;
    }

    // GOAL: S10.16's retry half -- a writer that could never be started counts as a FAILED write,
    //       so re-asserting the name a screen already has attempts the write again. False if it
    //       counts as "nothing has been asked of the file yet": SetMonitorOutput's idempotence
    //       skip then applies, the in-memory map agrees with the request while the file does not,
    //       and re-asserting -- the only retry a client can reach, and the one the header invites
    //       -- returns success having written nothing.
    // WHY-THIS-SHAPE: the failing call is a SetMonitorOutput rather than a SetWallpaper, because
    //       only the name path has an idempotence skip to be wrongly taken; a wallpaper request
    //       re-saves unconditionally and would be green under either build. The re-assertion is
    //       the FIRST call after threads are available again, since any other save writes the
    //       binding out with the rest of the live state and would hide the defect. The failing
    //       call is asserted to report, so this cannot be satisfied by a build that never noticed
    //       the writer was missing.
    // CONTROL: the `if (!m_notWritten.empty())` branch disabled in
    //       ConfigWriter::outcomeLocked() (backend/daemon/config_writer.cpp), so a refusal decided
    //       before any writer exists is invisible again -> eleven red across five phases, here
    //       "FAIL: naming a screen with no thread for the writer reports the name did not reach
    //       the file" printing "the call returned normally", and "FAIL: re-asserting a name whose
    //       writer could never start writes it to the file" printing "the config file is empty or
    //       absent; monitor 4 output '(no row)'". Re-observed 2026-08-18.
    // CONTROL, for the other direction -- a stale refusal must stop outranking the writer once a
    //       snapshot has been handed over: `if (worker) m_notWritten.clear();` disabled in
    //       ConfigWriter::queue() -> exactly two red, "FAIL: re-asserting a name whose writer
    //       could never start writes it to the file", printing "monitor 4 output 'DP-Z'; the
    //       re-assertion threw: ... no thread available: Resource temporarily unavailable" -- the
    //       name in the file and the daemon still reporting the refusal that preceded it -- and
    //       "FAIL: persistence reports itself working again once a save has landed". Re-observed
    //       2026-08-18.
    const bool nameReported = nameThrew.find("no thread") != std::string::npos;
    check(nameReported, "naming a screen with no thread for the writer reports the name did not reach the file");
    if (!nameReported) {
        std::fprintf(stderr, "  %s\n",
                     nameThrew.empty() ? "the call returned normally" : ("it threw: " + nameThrew).c_str());
    }
    std::string retryWhy;
    const std::map<int, RowInFile> retryRows = rowsInFile(configFile, retryWhy);
    const bool retried = reassertLanded && reassertThrew.empty() && retryRows.count(4) == 1 &&
                         retryRows.at(4).output == "DP-Z";
    check(retried, "re-asserting a name whose writer could never start writes it to the file");
    if (!retried) {
        std::fprintf(stderr, "  %s; monitor 4 output '%s'%s%s\n", retryWhy.empty() ? "read back" : retryWhy.c_str(),
                     retryRows.count(4) ? retryRows.at(4).output.c_str() : "(no row)",
                     reassertThrew.empty() ? "" : "; the re-assertion threw: ", reassertThrew.c_str());
    }
    // The whole point of not remembering a writer that does not exist: the failure is
    // transient, so a save made once threads are available again has to land.
    manager.requestWallpaper("static", wallpaper.string(), 2);
    waitForRows(configFile, [](const std::map<int, RowInFile>& rows) { return rows.count(2) == 1; });
    std::string why;
    std::size_t rowsWritten = 0;
    const std::map<int, RowInFile> onDisk = rowsInFile(configFile, why, &rowsWritten);
    const bool recovered = onDisk.count(2) == 1 && onDisk.at(2).path == wallpaper.string();
    check(recovered, "once threads are available again a save reaches the file");
    if (!recovered) {
        std::fprintf(stderr, "  %s; ids on disk: %s\n", why.empty() ? "read back" : why.c_str(),
                     idsOf(onDisk).c_str());
    }
    const WallpaperManager::Snapshot afterRecovery = manager.snapshot();
    const bool healthy = afterRecovery.health.persistence.status == wallpaperx::HealthStatus::Ok;
    check(healthy, "persistence reports itself working again once a save has landed");
    if (!healthy) {
        std::fprintf(stderr, "  persistence reads %s (%s)\n",
                     wallpaperx::toString(afterRecovery.health.persistence.status),
                     afterRecovery.health.persistence.detail.c_str());
    }
    std::printf("config_store_test: with no thread for the writer, two saves cost %.0f ms and %.0f ms\n", firstMs,
                secondMs);
}

// ---------------------------------------------------------------------------- //
// Phase 9c -- S10.16's allocation half: the writer thread that could not be built.
// ---------------------------------------------------------------------------- //
void runWriterAllocationFailureChecks() {
    constexpr const char* kGoals = "S10.16's allocation half (a config writer whose thread could not be ALLOCATED is "
                                   "reported as failed persistence and does not kill the daemon)";
#if !WALLPAPERX_ALLOCATION_INSTRUMENT
    notVerified(kGoals, "this build replaces no allocator, so no allocation can be made to fail where the config "
                        "writer is started -- the instrument is compiled out under a sanitizer, whose runtime owns "
                        "operator new");
#else
    const ScratchDir scratch("writeralloc");
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;

    // GOAL: S10.16's allocation half -- an allocation failure where the config writer is started
    //       is reported as failed persistence and the request still succeeds. False if it escapes:
    //       std::thread's constructor allocates the callable's state before it asks the kernel for
    //       anything, so on a machine out of memory the throw is std::bad_alloc, not
    //       std::system_error, and a handler that names only the second lets it out of
    //       requestWallpaper -- out of the DBus handler, and out of main() at startup, killing
    //       the daemon over a config file it could not write.
    // WHY-THIS-SHAPE: the failing allocation is found by sweeping WHICH allocation fails rather
    //       than by naming one. Nothing outside the process can say which allocation on the
    //       calling thread belongs to std::thread's constructor -- it is libstdc++'s internal
    //       state object -- so the check asks whether ANY of the first few hundred produces the
    //       reported outcome, which is a question with a definite answer under either build.
    //       RLIMIT_NPROC, which the phase above uses, cannot serve here: it makes the kernel
    //       refuse the thread, which is a std::system_error the pre-fix handler already caught,
    //       so both builds behave identically under it. Each sweep step gets a fresh manager,
    //       because a manager whose writer started never starts one again.
    // CONTROL: `catch (const std::exception& e)` put back to `catch (const std::system_error& e)`
    //       in ConfigWriter::startLocked() (backend/daemon/config_writer.cpp), which is the code
    //       before this fix, re-observed 2026-08-18 -> "FAIL: an allocation failure where the config
    //       writer is started is reported, not thrown at the client", printing "no allocation in
    //       the first 400 reported a writer that could not be built; 16 were injected, and one
    //       escaped requestWallpaper at allocation 0: std::bad_alloc" -- the escape being the
    //       daemon death the fix removes. On the shipped build the sweep finds it at attempt 9,
    //       and sixteen of the four hundred fault anything at all, so the bound is generous
    //       rather than tuned.
    // COVERAGE-ASSERT: the number of allocations the sweep actually faulted is asserted to be
    //       non-zero, so an instrument that stopped firing -- an armed flag never set, a compiled-
    //       out replacement -- fails loudly instead of sweeping four hundred no-ops and passing.
    constexpr long long kSweep = 400;
    g_allocProbeArmed.store(true, std::memory_order_relaxed);
    struct DisarmProbe {
        ~DisarmProbe() {
            t_failAfter = -1;
            g_allocProbeArmed.store(false, std::memory_order_relaxed);
        }
    } disarmProbe;

    // Arms on construction and disarms on destruction, which is what makes the disarm run during
    // the unwind the injected throw starts -- before the handler below allocates a string of its
    // own out of an allocator that would otherwise still be armed.
    struct ArmedFor {
        explicit ArmedFor(long long n) { t_failAfter = n; }
        ~ArmedFor() { t_failAfter = -1; }
    };

    bool reported = false;
    long long injections = 0;
    long long foundAt = -1;
    std::string escapedAt;
    std::string reportedDetail;
    for (long long n = 0; n < kSweep && !reported; ++n) {
        WallpaperManager::Config config;
        config.configFile = paths->config;
        WallpaperManager manager(config);
        std::string threw;
        t_injected = false;
        try {
            ArmedFor armed(n);
            manager.requestWallpaper("static", paths->wallpapers[0].string(), 0);
        } catch (const std::exception& e) {
            threw = e.what();
        } catch (...) {
            threw = "an exception that is not a std::exception";
        }
        // Nothing failed on this step: n landed past the end of the call, so there is no
        // observation to make and none is counted.
        if (!t_injected) continue;
        ++injections;
        if (!threw.empty()) {
            if (escapedAt.empty()) {
                escapedAt = "one escaped requestWallpaper at allocation " + std::to_string(n) + ": " + threw;
            }
            continue;
        }
        const std::string detail = manager.snapshot().health.persistence.detail;
        if (detail.find("no thread available") != std::string::npos &&
            detail.find("bad_alloc") != std::string::npos) {
            reported = true;
            foundAt = n;
            reportedDetail = detail;
        }
    }

    check(reported, "an allocation failure where the config writer is started is reported, not thrown at the client");
    if (!reported) {
        std::fprintf(stderr, "  no allocation in the first %lld reported a writer that could not be built; %lld were "
                             "injected%s%s\n",
                     kSweep, injections, escapedAt.empty() ? "" : ", and ", escapedAt.c_str());
    }
    check(injections > 0, "the allocation instrument this phase depends on actually faulted an allocation");
    if (injections == 0) {
        std::fprintf(stderr, "  the sweep armed %lld times and nothing ever threw, so nothing below it was graded\n",
                     kSweep);
    }
    std::printf("config_store_test: the config writer's own allocation was faulted at attempt %lld of %lld, and the "
                "daemon reported: %s\n",
                foundAt, kSweep, reportedDetail.empty() ? "(nothing)" : reportedDetail.c_str());
#endif
}

void runSaveVisibleOnReturnChecks() {
    constexpr const char* kGoals = "S8.28 (a save is on disk when the call that made it returns)";

    const ScratchDir scratch("savewait");
    // preparePhase, not a bare scratch.ok(): the bytes pulled out of the fifo are matched against
    // the wallpaper path this phase requested, and a temporary directory whose name is not valid
    // UTF-8 makes the writer skip that entry by design (S8.7) -- so the parked write would
    // legitimately carry an empty monitor list and the check would read as the daemon queueing
    // the wrong state.
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 1);
    if (!paths.has_value()) return;
    const std::filesystem::path configFile = paths->config;
    const std::filesystem::path wallpaper = paths->wallpapers.front();

    std::string fixtureWhy;
    if (!blockingWriteFixtureWorks(scratch.path() / "wallpaperx" / "probe.json", fixtureWhy)) {
        notVerified(kGoals, fixtureWhy);
        return;
    }
    BlockingWrite blocked(configFile);
    if (!blocked.arm()) {
        notVerified(kGoals, std::string("a fifo could not be created at ") + blocked.temp() +
                                ", so no write can be made slow on this host");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = configFile;
    WallpaperManager manager(config);

    // Held for well under the deadline, so this write COMPLETES rather than being abandoned:
    // the property is about the wait ending because the write landed.
    constexpr auto kHold = std::chrono::milliseconds(700);
    BlockingWrite::Released released;
    // jthread, so an exception out of the call below joins this rather than reaching
    // std::terminate through a live std::thread and destroying the diagnostics with it. Every
    // wait inside release() is bounded, so that join cannot hang.
    std::jthread releaser([&] {
        std::this_thread::sleep_for(kHold);
        released = blocked.release(std::chrono::milliseconds(10000));
    });

    // GOAL: S8.28 -- the caller of a save waits for the write, so the reply to SetWallpaper
    //       means the choice is on disk. False if the write is merely queued: a wallpaper set
    //       just before a shutdown is then lost, and every check in this suite that sets a
    //       wallpaper and reads the file back becomes a race it usually wins.
    // WHY-THIS-SHAPE: the write is made to take a known 700 ms and the CAUSAL ORDER is
    //       asserted -- requestWallpaper did not return before the write was let go. Two
    //       cheaper versions were rejected. Setting a wallpaper on a healthy tmpfs and
    //       reading the file back is what the rest of this suite already does, and it cannot
    //       fail reliably: the write takes microseconds, so removing the wait leaves a race
    //       the check wins nearly every time. Comparing against when the WRITER CLOSED is
    //       the other one, and it is an assertion on the scheduler: that instant is observed
    //       by this thread's peer, which may not be scheduled until after the main thread has
    //       already returned, so a correct build would go red at random. When the reader
    //       attached is observed before the write can possibly have finished, and is
    //       therefore a lower bound no interleaving can move.
    // CONTROL: `kWriteDeadline` in ConfigWriter::write()'s settled.wait_for()
    //       (backend/daemon/config_writer.cpp) replaced with 0 seconds, i.e. the write left fully
    //       asynchronous -> "FAIL: requestWallpaper returns only after the write it queued has
    //       landed", printing "it returned 700 ms before the write was even let go". The same
    //       mutation takes seven other checks with it, including three phases abandoned by a
    //       SetMonitorOutput that now throws on a write it cannot confirm -- collateral of the
    //       kind this phase exists to turn into a deliberate check. Re-observed 2026-08-18.
    //       Reverted.
    const auto start = StallClock::now();
    manager.requestWallpaper("static", wallpaper.string(), 0);
    const auto returnedAt = StallClock::now();
    const double elapsedMs = millisSince(start);
    releaser.join();

    if (!released.ok) {
        notVerified(kGoals, "the write never reached the fifo at " + blocked.temp() + " (" + released.why +
                                "), so this phase never had a slow write to time");
        return;
    }

    // The bytes the parked write was carrying, so "the caller waited" is about THIS request's
    // write and not about some other one that happened to be in flight.
    // CONTROL: `worker->pending = rows;` in ConfigWriter::queue()
    //       (backend/daemon/config_writer.cpp) replaced with an empty vector, i.e. a save that
    //       queues the wrong state -> "FAIL: the write this phase released is the one the request
    //       queued", printing "the fifo carried 37 bytes", among 40 red checks in all.
    //       Re-observed 2026-08-18 after the writer moved out of the manager. Reverted.
    const nlohmann::json wrote = nlohmann::json::parse(released.bytes, nullptr, false);
    const bool carriedThisRequest =
        !wrote.is_discarded() && wrote.is_object() && wrote.contains("monitors") && wrote["monitors"].is_array() &&
        wrote["monitors"].size() == 1 && wrote["monitors"][0].value("path", std::string()) == wallpaper.string();
    check(carriedThisRequest, "the write this phase released is the one the request queued");
    if (!carriedThisRequest) {
        std::fprintf(stderr, "  the fifo carried %zu bytes: %s\n", released.bytes.size(), released.bytes.c_str());
    }

    const bool waitedForIt = returnedAt >= released.at;
    check(waitedForIt, "requestWallpaper returns only after the write it queued has landed");
    if (!waitedForIt) {
        std::fprintf(stderr, "  it returned %.0f ms before the write was even let go\n",
                     std::chrono::duration<double, std::milli>(released.at - returnedAt).count());
    }
    std::printf("config_store_test: a save held for %lld ms took %.0f ms to return\n",
                static_cast<long long>(kHold.count()), elapsedMs);
}

void runLatestSnapshotWinsChecks() {
    constexpr const char* kGoals = "S8.33 (the snapshot queued behind a stalled write is replaced, not queued)";

    const ScratchDir scratch("latestwins");
    // preparePhase, not a bare scratch.ok(): every check below identifies a snapshot by the
    // wallpaper path in it, and a temporary directory whose name is not valid UTF-8 makes the
    // writer skip those entries by design (S8.7). Without this the phase spends its full ten
    // second wait for a row that is legitimately never coming and then blames the daemon for
    // dropping the newest save.
    const std::optional<PhasePaths> paths = preparePhase(scratch, kGoals, 3);
    if (!paths.has_value()) return;
    const std::filesystem::path configFile = paths->config;
    const std::filesystem::path stuck = paths->wallpapers[0];
    const std::filesystem::path superseded = paths->wallpapers[1];
    const std::filesystem::path newest = paths->wallpapers[2];

    std::string fixtureWhy;
    if (!blockingWriteFixtureWorks(scratch.path() / "wallpaperx" / "probe.json", fixtureWhy)) {
        notVerified(kGoals, fixtureWhy);
        return;
    }
    BlockingWrite blocked(configFile);
    if (!blocked.arm()) {
        notVerified(kGoals, std::string("a fifo could not be created at ") + blocked.temp() +
                                ", so no write can be made to block on this host");
        return;
    }
    // COVERAGE-ASSERT for the write count below: a count of zero is what a dead instrument
    // and a daemon that wrote nothing both produce, so the counter is shown to move for a
    // rename this phase performs itself before it is trusted with the daemon's.
    // CONTROL: the `++g_renameCount` line removed from the interposed rename() -> "FAIL: the
    //       rename counter this phase reads is live" and, below it, "the snapshot a newer one
    //       superseded is never written" printing "0 writes landed". Reverted.
    const std::filesystem::path probeFrom = scratch.path() / "probe.from";
    const std::filesystem::path probeTo = scratch.path() / "probe.to";
    countRenamesOnto(probeTo);
    const bool probed = writeFile(probeFrom, "probe") && ::rename(probeFrom.c_str(), probeTo.c_str()) == 0;
    check(probed && renamesCounted() == 1, "the rename counter this phase reads is live");
    if (!probed || renamesCounted() != 1) {
        std::fprintf(stderr, "  a rename this phase performed itself counted %u, so the count below establishes "
                             "nothing\n",
                     renamesCounted());
    }
    // Pointed at the config file before the manager exists, so no save of this phase can land
    // uncounted.
    countRenamesOnto(configFile);

    WallpaperManager::Config config;
    config.configFile = configFile;
    auto manager = std::make_shared<WallpaperManager>(config);

    const AbandonedOrReturned first = requestOnAbandonableThread(manager, stuck.string(), 0, std::chrono::seconds(20));
    if (!first.returned) {
        // Graded by runStalledSaveChecks, which is where that property lives. Here it is a
        // precondition: with a thread still inside the manager, nothing below can be set up.
        notVerified(kGoals, "the first save never returned, so the two snapshots this phase queues behind it were "
                            "never queued");
        return;
    }
    // Both land while the first write is stuck in the fifo, so the second is a snapshot the
    // third supersedes before either can reach the disk.
    manager->requestWallpaper("static", superseded.string(), 1);
    manager->requestWallpaper("static", newest.string(), 2);

    const BlockingWrite::Released released = blocked.release(std::chrono::milliseconds(10000));
    if (!released.ok) {
        notVerified(kGoals, "the parked write could not be read back (" + released.why + ")");
        return;
    }

    // GOAL: S8.33, first half -- the write that was stuck is the one that was queued when it
    //       started, so the two later snapshots really were queued BEHIND it. False if the
    //       stuck write already carried them: this phase would then be counting writes of a
    //       state that was never superseded, and the count below would mean nothing.
    // WHY-THIS-SHAPE: read out of the fifo rather than off the disk, because these are bytes
    //       that never reach the disk -- the write lands in the pipe and its rename moves the
    //       fifo, not a file. It is also the only way to see a superseded snapshot's content
    //       at all.
    // CONTROL: `worker->pending = rows;` in ConfigWriter::queue()
    //       (backend/daemon/config_writer.cpp) replaced with an empty vector -> "FAIL: the write
    //       that stalled is the snapshot that was current when it started", printing "the fifo
    //       carried 37 bytes". Re-observed 2026-08-18. The first-wins and queue mutations
    //       below both leave this one GREEN -- the fifo carries the first snapshot under
    //       either -- which is why it is asserted as a precondition here rather than left to
    //       the checks that need it.
    const nlohmann::json parked = nlohmann::json::parse(released.bytes, nullptr, false);
    const bool parkedIsTheFirst = !parked.is_discarded() && parked.is_object() && parked.contains("monitors") &&
                                  parked["monitors"].is_array() && parked["monitors"].size() == 1 &&
                                  parked["monitors"][0].value("path", std::string()) == stuck.string();
    check(parkedIsTheFirst, "the write that stalled is the snapshot that was current when it started");
    if (!parkedIsTheFirst) {
        std::fprintf(stderr, "  the fifo carried %zu bytes: %s\n", released.bytes.size(), released.bytes.c_str());
    }

    // GOAL: S8.33 -- the snapshot that lands once the filesystem answers is the NEWEST one,
    //       and the one it superseded is never written at all. False in the first direction
    //       if a save queued during an outage is dropped, which loses the user's last choice;
    //       false in the second if snapshots queue up, which spends one write per change on a
    //       filesystem that has just proved it is slow.
    // WHY-THIS-SHAPE: the file's contents AND the number of times anything was renamed onto
    //       it. Contents alone cannot tell the two apart -- a queue that writes every
    //       snapshot in order also ends with the newest one on disk, so a content-only check
    //       is green under both. The count is read only after the newest content is on disk,
    //       which is what makes it race-free rather than a grace period: under a queue the
    //       superseded write necessarily precedes the newest one, so by the time this reads
    //       the count, a third rename would already have been queued by the kernel.
    //       The wait for the content has a failure branch and never falls through into the
    //       count, and every read of the file is guarded by S_ISREG: the released write
    //       renames the FIFO onto config.json, and an unguarded read of that path would block
    //       this thread for good.
    // CONTROL: two, both reverted. (a) `worker->pending = rows;` in ConfigWriter::queue()
    //       (backend/daemon/config_writer.cpp) replaced with `if (!worker->pending)
    //       worker->pending = rows;`, i.e. first-wins -> "FAIL: the snapshot that lands after a
    //       stalled write is the newest one", printing "  monitor 2 is missing from the file",
    //       and nothing else red. Re-observed 2026-08-18. (b) `pending` turned into a
    //       std::deque<std::vector<PersistedWallpaper>> the worker pops the front of, i.e. a
    //       real queue -> "FAIL: the snapshot a newer one superseded is never written",
    //       printing "3 writes landed on config.json, expected 2", while the content check
    //       stayed green -- which is the pair of results these two checks exist to tell apart.
    std::map<int, std::string> landed;
    std::string landedWhy;
    const auto deadline = StallClock::now() + std::chrono::seconds(10);
    bool newestLanded = false;
    while (StallClock::now() < deadline) {
        if (isRegularFile(configFile)) {
            landedWhy.clear();
            landed = pathsInFile(configFile, landedWhy);
            newestLanded = landed.count(2) == 1;
            if (newestLanded) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const bool newestWon = newestLanded && landed.size() == 3 && landed.at(0) == stuck.string() &&
                           landed.at(1) == superseded.string() && landed.at(2) == newest.string();
    check(newestWon, "the snapshot that lands after a stalled write is the newest one");
    if (!newestWon) {
        std::string wrong = "the file holds a different set of monitors";
        if (!newestLanded) {
            wrong = landedWhy.empty() ? "monitor 2 is missing from the file"
                                      : "the config file never arrived: " + landedWhy;
        }
        std::fprintf(stderr, "  %s\n", wrong.c_str());
        // The count below is only meaningful once the newest write has landed; without it a
        // low count is "nothing has been written yet", not "nothing extra was written".
        return;
    }

    const unsigned writes = renamesCounted();
    check(writes == 2, "the snapshot a newer one superseded is never written");
    if (writes != 2) {
        std::fprintf(stderr, "  %u writes landed on %s, expected 2 (the stalled one and the newest one)\n", writes,
                     configFile.filename().string().c_str());
    }
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "config_store_test";

    // GOAL: this file manipulates $HOME and $XDG_CONFIG_HOME in process and builds
    //       ConfigStores from paths it computes, so it is one edit away from writing into
    //       the developer's own wallpaper config. False if a run creates or changes the file
    //       defaultConfigPath() names under this process's real environment.
    // WHY-THIS-SHAPE: witnessed and compared rather than asserted absent -- persistence is a
    //       shipped feature, so a developer running this suite may legitimately have a real
    //       config.json, and "it does not exist" would be red on their machine. Sampled
    //       before the first ScopedEnv exists, so it is the real environment that is asked.
    // CONTROL: a one-byte write to that exact path inserted just before the comparison ->
    //       "FAIL: this test wrote nothing into the developer's own wallpaper config",
    //       printing "/home/.../.config/wallpaperx/config.json did not exist before this run
    //       and does now". Removed, and the file deleted.
    const std::filesystem::path realConfig = wallpaperx::defaultConfigPath();
    bool realConfigExisted = false;
    bool witnessTaken = false;
    std::string realConfigBefore;
    if (!realConfig.empty()) {
        std::error_code ec;
        const bool present = std::filesystem::exists(realConfig, ec);
        // A stat this process could not perform is not evidence the file was absent.
        // Folded together, a failed probe here and a successful one at the end would print
        // "this test wrote nothing into the developer's own wallpaper config" as a FAIL
        // against a suite that had touched nothing.
        witnessTaken = !ec;
        realConfigExisted = present && !ec;
        realConfigBefore = realConfigExisted ? readFile(realConfig) : std::string();
    }

    // The daemon keeps a preserved copy of a rejected config under $XDG_STATE_HOME, and a
    // phase that forgets to point that somewhere of its own would write into the developer's
    // real state directory. One scratch directory for the whole run is the backstop; the
    // phases that read a copy back set their own inside it.
    //
    // CONTROL: this pair deleted, with the phases' own ScopedEnvs deleted too -> "FAIL: this
    // test wrote nothing into the developer's own state directory", naming
    // $HOME/.local/state/wallpaperx/config.json.bad. Observed 2026-08-15. Reverted.
    const ScratchDir stateGuard("stateguard");
    if (!stateGuard.ok()) {
        harnessFailure("no scratch directory for XDG_STATE_HOME could be created, so a preserved copy this run "
                       "makes would land in the real state directory");
        return 1;
    }
    const ScopedEnv stateHome("XDG_STATE_HOME", (stateGuard.path() / "state").string());
    const std::filesystem::path realStateDir = [] {
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            return std::filesystem::path(home) / ".local" / "state" / "wallpaperx";
        }
        return std::filesystem::path();
    }();
    bool realStateDirExisted = false;
    if (!realStateDir.empty()) {
        std::error_code ec;
        realStateDirExisted = std::filesystem::exists(realStateDir, ec) && !ec;
    }

    // X.4: this file drives a WallpaperManager, whose constructor can throw on a machine
    // whose shader directory is missing. Without a handler the process would abort and ctest
    // would report an opaque SIGABRT instead of the FAIL lines above. One handler per phase
    // rather than one around all of them: a throw in phase 2 would otherwise delete phases 3
    // to 8 from the run with nothing in the output saying they had not been reached, and a
    // reader would see one named failure and reasonably conclude everything else was checked.
    //
    // GOAL: X.3 -- what escapes a phase body is a fault of the code under test, so the
    //       handler below counts it as one. False if a host limit reaches here: $TMPDIR naming
    //       a directory that does not exist would otherwise throw out of every phase's first
    //       line and print 16 FAIL lines against ConfigStore for a machine with nowhere to
    //       put a temporary directory.
    // WHY-THIS-SHAPE: fixed at the setup call (ScratchDir's temp_directory_path now uses the
    //       error_code overload, so the phase reaches its own notVerified arm), NOT by
    //       adding a `catch (const std::filesystem::filesystem_error&) -> notVerified` arm
    //       here. That arm cannot tell the two apart: ConfigStore throwing a
    //       filesystem_error of its own is the same C++ type, and demoting it to a skip
    //       would convert real defects into green.
    // CONTROL: ConfigStore's constructor (backend/daemon/config_store.cpp) made to throw a
    //       std::filesystem::filesystem_error -> 16 `FAIL: the "..." phase threw: filesystem
    //       error: CONTROL: ...` lines and exit 1, not a skip. Reverted.
    struct Phase {
        const char* name;
        void (*run)();
    };
    static constexpr Phase kPhases[] = {
        {"where the config file may live", runLocationChecks},
        {"loading arbitrary file content", runLoadCorpusChecks},
        {"the save/load round trip", runRoundTripChecks},
        {"a path that is not valid UTF-8", runNonUtf8Checks},
        {"a malformed entry", runMalformedEntryChecks},
        {"restore does not write", runRestoreChecks},
        {"a config that could not be read", runUnreadableConfigChecks},
        {"when the preserved copy must not be made", runPreservedCopyChecks},
        {"a config that cannot be preserved anywhere", runPreservationExhaustedChecks},
        {"the copy made for malformed entries", runMalformedEntryCopyChecks},
        {"the copy made before a repair is written", runRepairCopyChecks},
        {"preservation on a daemon with no state directory", runPreservationWithoutHomeChecks},
        {"what a move is, and what a failed copy leaves", runCopyMechanicsChecks},
        {"a copy kept when the original would not go", runPreservedCopySurvivesFailedRemovalChecks},
        {"a repair recomputed on every start", runPreservationReuseChecks},
        {"entries this build cannot apply", runUnappliedEntryChecks},
        {"the connector name as a stored field", runOutputFieldChecks},
        {"the connector names read back at startup", runOutputRestoreChecks},
        {"the connector names a restore could not keep", runOutputLossReportChecks},
        {"a kept row a live monitor has taken over", runKeptWallpaperSupersededChecks},
        {"a kept row a client takes the monitor of", runKeptWallpaperDisplacedAtRuntimeChecks},
        {"two rows for one monitor this build can apply", runDuplicateLiveRowChecks},
        {"one monitor claimed by a row this build can apply and one it cannot", runCrossContainerDuplicateChecks},
        {"what owning a monitor id is allowed to mean", runStrictOwnershipChecks},
        {"a carried screen name a client takes over", runKeptNameTakenOverAtRuntimeChecks},
        {"the same loss caused twelve times", runRepeatedRuntimeLossesAreBoundedChecks},
        {"which kept row keeps a name three rows claim", runKeptNameLastWinsChecks},
        {"the rows kept for a build with more screens", runPreservedRowChecks},
        {"re-asserting a name a screen already has", runIdempotentRenameChecks},
        {"the repair a start performs is written at that start", runStartupRepairChecks},
        {"the startup repair against a config directory that has stopped answering",
         runStartupRepairIsNotBlockingChecks},
        {"a health read beside the first save", runConcurrentHealthReadChecks},
        {"naming a screen beside an unrelated persistence failure", runNameAnswersForItsOwnWriteChecks},
        {"one answer about the config file, asked three ways", runOneAnswerAboutTheFileChecks},
        {"the escaping of an alarm that outlives its write", runStickyAlarmEscapingChecks},
        {"re-asserting a name the daemon refused to write", runRefusedNameRetryChecks},
        {"a save that cannot succeed", runFailedSaveChecks},
        {"a failed write nobody waited for", runFailedWriteIsAnnouncedChecks},
        {"the cost of the save", runSaveCostChecks},
        {"a write that never comes back", runStalledSaveChecks},
        {"a writer thread that could not be created", runNoWriterThreadChecks},
        {"a writer thread that could not be allocated", runWriterAllocationFailureChecks},
        {"the wait a save is given", runSaveVisibleOnReturnChecks},
        {"the snapshot that wins a stall", runLatestSnapshotWinsChecks},
    };
    for (const Phase& phase : kPhases) {
        try {
            phase.run();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: the \"%s\" phase threw: %s\n", phase.name, e.what());
            ++g_failures;
        }
    }

    if (realConfig.empty()) {
        // Printed, not silent: with no usable HOME there is nothing to protect, and a
        // reader must not mistake the guard's silence for it having run.
        std::printf("config_store_test: this process has no usable XDG_CONFIG_HOME or HOME, so there is no real "
                    "config file for this run to have touched.\n");
    } else if (!witnessTaken) {
        notVerified("the developer-config isolation guard",
                    "the real config file could not be probed before the run, so there is no baseline to compare "
                    "against");
    } else {
        std::error_code ec;
        const bool existsNow = std::filesystem::exists(realConfig, ec) && !ec;
        const bool untouched =
            existsNow == realConfigExisted && (!existsNow || readFile(realConfig) == realConfigBefore);
        check(untouched, "this test wrote nothing into the developer's own wallpaper config");
        if (!untouched) {
            std::fprintf(stderr, "  %s %s before this run and %s\n", realConfig.string().c_str(),
                         realConfigExisted ? "held something else" : "did not exist",
                         existsNow ? "does now" : "is gone");
        }
    }

    // The same guard for the directory a preserved copy goes to. Graded on the copies, not on
    // the directory's existence: another WallpaperX process may legitimately have made one
    // before this run started, and a phase whose ScopedEnv is missing writes a NEW one.
    if (realStateDir.empty()) {
        std::printf("config_store_test: this process has no usable HOME, so there is no real state directory for "
                    "this run to have touched.\n");
    } else {
        std::error_code ec;
        const bool existsNow = std::filesystem::exists(realStateDir, ec) && !ec;
        const bool untouched = realStateDirExisted || !existsNow;
        check(untouched, "this test wrote nothing into the developer's own state directory");
        if (!untouched) {
            std::fprintf(stderr, "  %s did not exist before this run and does now, holding: %s\n",
                         realStateDir.string().c_str(),
                         joined(preservedCopyNames(std::filesystem::path("config.json"), realStateDir)).c_str());
        }
    }

    // Asked before the not-verified count, so a run that is both broken and incomplete
    // reports as broken rather than hiding failures behind ctest's "Skipped". Both red
    // channels are summarised: a fault this file recorded against itself -- an instrument that
    // stopped observing what it grades -- must not be reported as a skip either.
    const bool checksFailed = wallpaperx::testing::reportCheckFailures();
    const bool harnessBroken = wallpaperx::testing::reportHarnessFaults("in this test file, not in ConfigStore");
    if (checksFailed || harnessBroken) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
