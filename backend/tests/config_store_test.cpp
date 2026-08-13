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
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
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

extern "C" int rename(const char* oldPath, const char* newPath) {
    using Fn = int (*)(const char*, const char*);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "rename"));
    if (real == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    const int result = real(oldPath, newPath);
    if (result == 0 && newPath != nullptr) {
        std::lock_guard<std::mutex> lock(g_renameMutex);
        if (!g_renameTarget.empty() && g_renameTarget == newPath) ++g_renameCount;
    }
    return result;
}

namespace {

// The pass / fail / skip contract lives in tests/test_report.h: exit 77 for a goal this host
// could not exercise (ctest turns it into SKIP through SKIP_RETURN_CODE), exit 1 for a check
// that ran and did not hold, and failures outranking skips so a run that is both broken and
// incomplete cannot hide behind "Skipped".
using wallpaperx::testing::check;
using wallpaperx::testing::g_failures;
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

// Every preserved-copy name sitting beside the config file, sorted. Named by prefix rather
// than by asking whether `config.json.bad` exists: preservation now numbers its copies, so
// a check written against the one name would be satisfied by a build that wrote `.bad.1`
// instead -- which is still a copy the goal says must not have been made.
std::vector<std::string> preservedCopyNames(const std::filesystem::path& file) {
    std::vector<std::string> names;
    const std::string prefix = file.filename().string() + ".bad";
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(file.parent_path(), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(prefix, 0) == 0) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
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
// Phase 6 -- S8.5 and S8.4: restore reads, and never writes.
// ---------------------------------------------------------------------------- //
void runRestoreChecks() {
    const ScratchDir scratch("restore");
    if (!scratch.ok()) {
        notVerified("S8.5 (restore does not write) and S8.4 (an unreadable entry is not forgotten)",
                    "a scratch directory could not be created");
        return;
    }
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

    // GOAL: S8.5 -- a start that only restores does not write the config file. False if
    //       its contents or its mtime move. Not hypothetical: restore records the same
    //       per-monitor state requestWallpaper() records, and requestWallpaper() is where
    //       the save hook is attached, so one line in the wrong place saves on startup --
    //       and S8.4 (the unreadable entry surviving) depends on this not happening.
    // WHY-THIS-SHAPE: bytes AND mtime, against a file written in a formatting save() does
    //       not produce. Comparing only the mtime would miss a rewrite inside the
    //       filesystem's timestamp granularity; comparing only the parsed entries would
    //       miss a rewrite that preserved them, which is precisely what an accidental save
    //       on restore would do -- it would look identical in every way except that the
    //       file had been replaced.
    // CONTROL: `m_store.save(persistableLocked());` added at the end of
    //       WallpaperManager::restoreSaved() -> "FAIL: restoring does not rewrite the config
    //       file", and nothing else red. The mtime half stayed green under it, because this
    //       host's scratch filesystem stamps both writes with the same coarse time -- which
    //       is the reason both comparisons are here rather than either alone. The mtime half
    //       is what carries the equivalent check in daemon_system_test, where the file was
    //       written by the daemon's own save() and a second save reproduces it byte for
    //       byte. Reverted.
    check(readFile(file) == original, "constructing a manager reads the config file without rewriting it");

    const std::vector<int> restored = manager.restoreSaved();
    check(restored == std::vector<int>({0, 1}), "restoreSaved returns the monitors to post, in file order");
    check(readFile(file) == original, "restoring does not rewrite the config file");

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
        check(mtimeAfter == mtimeBefore, "restoring does not touch the config file's mtime");
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
    if (!scratch.ok()) {
        notVerified(kGoal, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path kept = std::filesystem::path(file.string() + ".bad");
    const std::filesystem::path wallpaper = scratch.path() / "wallpaper.png";
    // A hand edit that broke the syntax: one trailing comma in an otherwise complete file
    // holding two wallpapers the user picked. What makes this the case worth pinning is that
    // the bytes are still valuable -- the paths are readable in it -- so losing them is data
    // loss and not merely a reset.
    const std::string handEdited =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a.png\"},]}";
    if (!writeFile(file, handEdited) || !writeWallpaperFile(wallpaper)) {
        notVerified(kGoal, "the damaged config file this phase reads could not be written");
        return;
    }

    WallpaperManager::Config config;
    config.configFile = file;
    WallpaperManager manager(config);
    // main.cpp's order: construct, restore, and only then preserve -- the constructor is
    // read-only, and preserveUnreadableConfig() is the one call that writes to the config
    // directory, made after the bus name is held. Nothing in this file can establish that
    // ordering, because there is no bus here; daemon_system_test's preservation-ownership
    // phase is where it is graded.
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
    // CONTROL: the `std::filesystem::rename(m_store.file(), kept, ec)` call deleted from
    //       WallpaperManager::preserveConfigTo(), leaving the move branch a no-op -> "FAIL: a
    //       config that could not be read is kept as config.json.bad" and "FAIL: the
    //       preserved copy is byte-identical to the file the daemon could not read", printing
    //       "  no config.json.bad exists". Reverted.
    const WallpaperManager::Snapshot afterLoad = manager.snapshot();
    check(afterLoad.health.persistence.status == wallpaperx::HealthStatus::Fail,
          "a config that could not be read leaves the persistence check reporting failure");
    check(afterLoad.health.persistence.detail.find(".bad") != std::string::npos,
          "the persistence failure says where the unreadable config was kept");
    if (afterLoad.health.persistence.detail.find(".bad") == std::string::npos) {
        std::fprintf(stderr, "  persistence detail: %s\n", afterLoad.health.persistence.detail.c_str());
    }

    manager.requestWallpaper("static", wallpaper.string(), 1);

    const bool keptExists = std::filesystem::exists(kept);
    check(keptExists, "a config that could not be read is kept as config.json.bad");
    check(keptExists && readFile(kept) == handEdited,
          "the preserved copy is byte-identical to the file the daemon could not read");
    if (!keptExists) {
        std::fprintf(stderr, "  no config.json.bad exists\n");
    } else if (readFile(kept) != handEdited) {
        std::fprintf(stderr, "  kept: %s\n  written: %s\n", readFile(kept).c_str(), handEdited.c_str());
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
    //       "  preserved copies beside it: config.json.bad". (b) the same, hoisted above the
    //       S_ISREG return -> the same three for the named-pipe case. (c) the `if
    //       (m_configUnread)` early return deleted from WallpaperManager::persist() -> all
    //       three cases red on "still holds the bytes it held before the daemon ran" and on
    //       "the daemon says it refused to save over it", with the mode-000 file rewritten
    //       to hold monitor 1 alone -- the two paths the goal names, measured separately.
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

        const std::vector<std::string> copies = preservedCopyNames(file);
        check(copies.empty(),
              (std::string("a config.json that is ") + c.what + " is left where it is, under its own name").c_str());
        if (!copies.empty()) {
            std::fprintf(stderr, "  preserved copies beside it: %s\n", joined(copies).c_str());
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
    // CONTROL: two, both reverted. (a) the `+ (n == 0 ? "" : "." + std::to_string(n))`
    //       suffix dropped from WallpaperManager::preserveConfigTo()'s loop, so every copy
    //       competes for one name -> "FAIL: the second unreadable config is preserved under
    //       a name of its own" with "  copies beside it: config.json.bad", "FAIL: the daemon
    //       says where the second unreadable config was kept" ("not kept, no free name"),
    //       and "FAIL: a wallpaper set after the second unreadable config is persisted into
    //       a fresh config.json" -- the run stops saving, which is the second half of the
    //       damage. (b) the `if (std::filesystem::exists(kept, ec)) continue;` arm deleted,
    //       leaving the rename unconditional -> "FAIL: a second unreadable config does not
    //       overwrite the copy the first one preserved", printing the throwaway's bytes
    //       where the user's config had been, beside the name-of-its-own check.
    const ScratchDir scratch("twicebad");
    if (!scratch.ok()) {
        notVerified(kGoal, "a scratch directory for the second-bad-start case could not be created");
        return;
    }
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path kept = std::filesystem::path(file.string() + ".bad");
    const std::filesystem::path keptSecond = std::filesystem::path(file.string() + ".bad.1");
    const std::filesystem::path wallpaper = scratch.path() / "wallpaper.png";
    const std::string usersOwn =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/kept.png\"},]}";
    const std::string throwaway = "{\"version\":1,\"monitors\":[{,";
    if (!writeFile(kept, usersOwn) || !writeFile(file, throwaway) || !writeWallpaperFile(wallpaper)) {
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
        std::fprintf(stderr, "  copies beside it: %s\n", joined(preservedCopyNames(file)).c_str());
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
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path wallpaper = scratch.path() / "wallpaper.png";
    // One more than the daemon's bound would leave a free name, one fewer would not test
    // it: this is exactly the count at which the search runs out.
    constexpr int kCopies = 10;
    const std::string usersOwn =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/kept.png\"},"
        "{\"monitor_id\":1,\"type\":\"static\",\"path\":\"/photos/right.png\"},]}";
    bool staged = writeWallpaperFile(wallpaper) && writeFile(file, usersOwn);
    for (int n = 0; n < kCopies && staged; ++n) {
        staged = writeFile(std::filesystem::path(file.string() + ".bad" + (n == 0 ? "" : "." + std::to_string(n))),
                           "an earlier broken config");
    }
    if (!staged) {
        notVerified(kGoal, "the ten occupied copy names this case needs could not be written");
        return;
    }

    // GOAL: S8.22 -- when there is nowhere left to preserve it to, an unreadable config is
    //       LEFT ALONE and the run stops saving, rather than being destroyed to make room.
    //       False if the next SetWallpaper rewrites the file: the user has ten preserved
    //       copies and the eleventh start silently discards the live one, which is the only
    //       file the daemon will read on the next start.
    // WHY-THIS-SHAPE: the number of occupied names is the daemon's own bound, and the check
    //       is made after a SetWallpaper rather than after the constructor. A check that
    //       only asserted "no config.json.bad.10 was created" would pass for a build that
    //       gave up on preserving and then went on to overwrite the file, which is the
    //       damaging half; and one made before the save would pass for any build at all,
    //       since the constructor writes nothing on this path.
    // CONTROL: two, both reverted. (a) `kMaxPreservedCopies` in wallpaper_manager.cpp raised
    //       to 11, so a free name exists after all -> all three checks red, "  11 copies
    //       beside it (expected 10)" naming config.json.bad.10. (b) `m_configUnread = true;`
    //       deleted from the constructor's error branch -> "FAIL: with every copy name
    //       taken, the config the daemon could not read is not saved over", printing the
    //       file rewritten to hold monitor 1 alone, with the copy-count check still green:
    //       that pair is what separates "gave up on preserving" from "went on to destroy
    //       it", and only the second is data loss.
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

    const std::vector<std::string> copies = preservedCopyNames(file);
    check(static_cast<int>(copies.size()) == kCopies,
          "with every copy name taken, the config the daemon could not read is still there");
    if (static_cast<int>(copies.size()) != kCopies) {
        std::fprintf(stderr, "  %zu copies beside it (expected %d): %s\n", copies.size(), kCopies,
                     joined(copies).c_str());
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
    if (!scratch.ok()) {
        notVerified(kGoal, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path kept = std::filesystem::path(file.string() + ".bad");
    const std::filesystem::path wallpaper = scratch.path() / "wallpaper.png";
    // Parses, so nothing here is an unreadable config: one entry the daemon restores and
    // one it cannot make sense of. A rejected entry never becomes an entry at all, so the
    // keep-for-a-newer-build path cannot carry it and the first save drops it -- which is
    // what makes the copy the user's only record of it.
    const std::string firstBroken =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/left.png\"},"
        "{\"monitor_id\":\"two\",\"type\":\"static\",\"path\":\"/photos/right.png\"}]}";
    const std::string secondBroken =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/later.png\"},"
        "{\"monitor_id\":1.5,\"type\":\"static\",\"path\":\"/photos/other.png\"}]}";
    if (!writeWallpaperFile(wallpaper) || !writeFile(file, firstBroken)) {
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
    // CONTROL: the `move` argument of preserveConfigTo() forced true on the malformed-entry
    //       path, i.e. a rename where a copy is owed -> "FAIL: a config with malformed
    //       entries is still there after the copy is made", and that alone. Note what stayed
    //       GREEN under it: "a wallpaper set after a config with malformed entries is
    //       persisted", because the save simply recreates the file the rename took away --
    //       which is why the loss has to be caught by the file's absence rather than by the
    //       daemon misbehaving afterwards. The two second-start checks stayed green too: the
    //       numbered search still sends the second file to a name of its own whether it is
    //       moved or copied, so what the second start grades is the naming, not the mode.
    //       Reverted.
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
    // CONTROL: the `+ (n == 0 ? "" : "." + std::to_string(n))` suffix dropped from
    //       preserveConfigTo()'s loop, so both starts compete for config.json.bad -> "FAIL:
    //       the second start's malformed config is copied under a name of its own" with
    //       "  copies beside it: config.json.bad", and "FAIL: the daemon says where the
    //       second malformed config was copied" printing "1 malformed entries ignored; not
    //       copied: no free name". The first start's copy stayed intact under that mutation,
    //       so the pair separates "the second file was lost" from "the first was destroyed".
    //       Reverted.
    const std::filesystem::path keptSecond = std::filesystem::path(file.string() + ".bad.1");
    const bool secondCopied = readFile(keptSecond) == secondBroken;
    check(secondCopied, "the second start's malformed config is copied under a name of its own");
    if (!secondCopied) {
        std::fprintf(stderr, "  copies beside it: %s\n", joined(preservedCopyNames(file)).c_str());
    }
    const std::string reported = secondManager.snapshot().health.persistence.detail;
    const bool said = reported.find(keptSecond.filename().string()) != std::string::npos;
    check(said, "the daemon says where the second malformed config was copied");
    if (!said) std::fprintf(stderr, "  persistence detail: %s\n", reported.c_str());
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
    //       on-disk ids are compared as an exact set after a save made for a DIFFERENT
    //       monitor. Only that write can drop them, so a check made straight after the
    //       restore would pass for a build that forgets them at the first SetWallpaper. The
    //       count in the health detail is derived from the fixture, not spelled out, so a
    //       build that reports a fixed number cannot pass.
    // CONTROL: `m_unapplied.push_back(entry);` deleted from restoreSaved()'s skip branch
    //       (its state before today) -> "FAIL: the entries this build cannot apply are all
    //       still in the file after another monitor is set" with "  ids on disk: 1 3", and
    //       "FAIL: the persistence detail says how many entries could not be applied".
    //       Reverted.
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

    const WallpaperManager::Snapshot snap = manager.snapshot();
    const std::string expectedCount = std::to_string(unusableIds.size()) + " entries this build cannot apply";
    const bool reported = snap.health.persistence.detail.find(expectedCount) != std::string::npos;
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
    const std::string expectedDropped =
        std::to_string(droppedIds.size()) + " entries dropped, no build can apply them";
    const bool droppedReported = snap.health.persistence.detail.find(expectedDropped) != std::string::npos;
    check(droppedReported, "the persistence detail says how many entries no build can apply");
    if (!droppedReported) {
        std::fprintf(stderr, "  persistence detail: %s\n  expected '%s'\n", snap.health.persistence.detail.c_str(),
                     expectedDropped.c_str());
    }

    manager.requestWallpaper("static", other.string(), 1);
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
    // CONTROL: two, both reverted. (a) `recordHealth(&DaemonHealth::persistence,
    //       HealthStatus::Fail, saved.error); return;` replaced by a bare `return;` in
    //       WallpaperManager::persist(), i.e. the error swallowed -> "FAIL: a save that
    //       failed leaves the persistence health check reporting failure" while the
    //       no-throw check stayed green, which is the pair of results the two clauses
    //       exist to distinguish. (b) that same line replaced with a `throw
    //       std::runtime_error(saved.error)` -> "FAIL: a config write that cannot happen
    //       does not fail the request that triggered it", printing "requestWallpaper threw:
    //       .../ro/wallpaperx: could not be created: Permission denied", with the health
    //       check red beside it since the throw skipped the recording.
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
    if (!onDisk.ok()) {
        notVerified(kGoal, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path persistedFile = onDisk.path() / "wallpaperx" / "config.json";
    const std::filesystem::path probeFile = onDisk.path() / "wallpaperx" / "probe.json";
    const std::filesystem::path wallpaper = onDisk.path() / "wallpaper.png";
    std::error_code ec;
    std::filesystem::create_directories(persistedFile.parent_path(), ec);
    if (ec || !writeWallpaperFile(wallpaper)) {
        notVerified(kGoal, "the fixture files this phase needs could not be written");
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
    //       save against a write+rename this file performs is the obvious version and is
    //       what shipped: its bound is the geometric mean of two disk costs, while the
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

using StallClock = std::chrono::steady_clock;

double millisSince(StallClock::time_point start) {
    return std::chrono::duration<double, std::milli>(StallClock::now() - start).count();
}

// The deadline a save is given, restated rather than shared: kLoadDeadline sits in an
// anonymous namespace in wallpaper_manager.cpp. A copy here means a change to it fails these
// phases loudly instead of silently re-pointing them at whatever the new value is.
constexpr double kSaveDeadlineMs = 2000.0;

// BlockingWrite, blockingWriteFixtureWorks() and isRegularFile() live in
// tests/client/blocking_config_write.h, shared with daemon_system_test: the fixture's
// correctness rests on details a second copy would drift away from silently.

// One requestWallpaper(), made on a thread this phase can walk away from.
//
// The obvious version -- call it inline -- turns a build whose wait is unbounded into a
// HANG, and a hung test prints nothing at all: no FAIL line, no reason, just a ctest
// timeout thirty seconds later. This one turns that build into a check that goes red.
// The manager is held by shared_ptr and copied into the thread, so a call that never
// returns keeps alive the object it is inside rather than being abandoned on top of a
// destroyed one.
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

void runStalledSaveChecks() {
    constexpr const char* kGoals = "S8.29/S8.31/S8.32 (a config write that never comes back)";

    const ScratchDir scratch("stalledsave");
    if (!scratch.ok()) {
        notVerified(kGoals, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path configFile = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path healthyWallpaper = scratch.path() / "healthy.png";
    const std::filesystem::path stalledWallpaper = scratch.path() / "stalled.png";
    if (!writeWallpaperFile(healthyWallpaper) || !writeWallpaperFile(stalledWallpaper)) {
        notVerified(kGoals, "the wallpaper files this phase needs could not be written");
        return;
    }

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
    // CONTROL: two, both reverted. (a) `kLoadDeadline` in the settled.wait_for() call
    //       replaced with `std::chrono::hours(1)` -> "FAIL: a save whose write cannot
    //       complete still releases the caller", printing "requestWallpaper has not returned
    //       20.0 s after a write that cannot finish", the rest of this phase skipped with
    //       that reason, and the snapshot-wins phase reporting NOT VERIFIED rather than
    //       failing. (b) `throw std::runtime_error("stalled")` added after
    //       the wait_for -> "FAIL: a config write that cannot complete does not fail the
    //       request that triggered it", printing "requestWallpaper threw: stalled". (c) for
    //       the upper bound specifically, `kLoadDeadline` replaced with 10 seconds, so the
    //       caller is released but far too late -> "FAIL: the caller of a stalled save waits
    //       for the deadline and not for the filesystem", printing "requestWallpaper took
    //       10000 ms against a 2000 ms deadline", and nothing else red.
    const AbandonedOrReturned stalled =
        requestOnAbandonableThread(manager, stalledWallpaper.string(), 1, std::chrono::seconds(20));
    check(stalled.returned, "a save whose write cannot complete still releases the caller");
    if (!stalled.returned) {
        std::fprintf(stderr, "  requestWallpaper has not returned 20.0 s after a write that cannot finish; the "
                             "checks after this one are skipped rather than measured against a thread that is "
                             "still inside the manager\n");
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
    // CONTROL: the `if (writingSince && ... > kLoadDeadline)` block deleted from
    //       applyPersistOutcomeLocked() -> "FAIL: health reports a write that has not come
    //       back rather than the last save that did", printing "persistence reads ok
    //       (/tmp/.../config.json)" -- the healthy save above, which is the exact wrong
    //       answer this goal names. Reverted.
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
    // CONTROL: `if (m_persist->stalled) return;` deleted from persist() -> "FAIL: a second
    //       save during a stall does not wait for the deadline all over again", printing
    //       "the second save took 2000 ms against a 2000 ms deadline". Reverted.
    // COVERAGE-ASSERT: the first save's own elapsed time is asserted to be at least three
    //       quarters of the deadline. Without it this comparison is green on a build that
    //       waits for nothing at all -- both calls return instantly and the phase reports
    //       that the deadline is not paid twice, having never seen it paid once. CONTROL for
    //       it: `kLoadDeadline` in the settled.wait_for() replaced with 0 seconds -> "FAIL:
    //       the first save really did wait for the deadline, so the comparison above has a
    //       baseline", printing "the first save took 0 ms", while the check it guards stayed
    //       green at 0 ms. Reverted.
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

void runSaveVisibleOnReturnChecks() {
    constexpr const char* kGoals = "S8.28 (a save is on disk when the call that made it returns)";

    const ScratchDir scratch("savewait");
    if (!scratch.ok()) {
        notVerified(kGoals, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path configFile = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path wallpaper = scratch.path() / "slow.png";
    if (!writeWallpaperFile(wallpaper)) {
        notVerified(kGoals, "the wallpaper file this phase needs could not be written");
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
    // CONTROL: `kLoadDeadline` in persist()'s settled.wait_for() replaced with 0 seconds,
    //       i.e. the write left fully asynchronous -> "FAIL: requestWallpaper returns only
    //       after the write it queued has landed", printing "it returned 700 ms before the
    //       write was even let go". The same mutation also reddened one of the older checks
    //       that sets a wallpaper and reads the file back ("a monitor whose file was
    //       unreadable at restore is still in the file after another monitor is set"), which
    //       is the accident this phase exists to turn into a deliberate check. Reverted.
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
    // CONTROL: `m_persist->pending = entries;` in persist() replaced with an empty vector,
    //       i.e. a save that queues the wrong state -> "FAIL: the write this phase released is
    //       the one the request queued", printing "the fifo carried 37 bytes", alongside five
    //       older persistence checks. Reverted.
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

// Points the interposed rename() at one destination and zeroes its count.
void countRenamesOnto(const std::filesystem::path& target) {
    std::lock_guard<std::mutex> lock(g_renameMutex);
    g_renameTarget = target.string();
    g_renameCount = 0;
}

unsigned renamesCounted() {
    std::lock_guard<std::mutex> lock(g_renameMutex);
    return g_renameCount;
}

void runLatestSnapshotWinsChecks() {
    constexpr const char* kGoals = "S8.33 (the snapshot queued behind a stalled write is replaced, not queued)";

    const ScratchDir scratch("latestwins");
    if (!scratch.ok()) {
        notVerified(kGoals, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path configFile = scratch.path() / "wallpaperx" / "config.json";
    const std::filesystem::path stuck = scratch.path() / "stuck.png";
    const std::filesystem::path superseded = scratch.path() / "superseded.png";
    const std::filesystem::path newest = scratch.path() / "newest.png";
    if (!writeWallpaperFile(stuck) || !writeWallpaperFile(superseded) || !writeWallpaperFile(newest)) {
        notVerified(kGoals, "the wallpaper files this phase needs could not be written");
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
    // CONTROL: `m_persist->pending = entries;` in persist() replaced with an empty vector ->
    //       "FAIL: the write that stalled is the snapshot that was current when it started",
    //       printing "the fifo carried 37 bytes". Reverted. The first-wins and queue mutations
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
    // CONTROL: two, both reverted. (a) `m_persist->pending = entries;` in persist() replaced
    //       with `if (!m_persist->pending) m_persist->pending = entries;`, i.e. first-wins ->
    //       "FAIL: the snapshot that lands after a stalled write is the newest one", printing
    //       "  monitor 2 is missing from the file". (b) `pending` turned into a
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

    // X.4: this file drives a WallpaperManager, whose constructor can throw on a machine
    // whose shader directory is missing. Without a handler the process would abort and ctest
    // would report an opaque SIGABRT instead of the FAIL lines above. One handler per phase
    // rather than one around all of them: a throw in phase 2 used to delete phases 3 to 8
    // from the run with nothing in the output saying they had not been reached, so a reader
    // saw one named failure and reasonably concluded everything else had been checked.
    //
    // GOAL: X.3 -- what escapes a phase body is a fault of the code under test, so the
    //       handler below counts it as one. False if a host limit reaches here: $TMPDIR
    //       naming a directory that does not exist used to throw out of every phase's first
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
        {"entries this build cannot apply", runUnappliedEntryChecks},
        {"a save that cannot succeed", runFailedSaveChecks},
        {"the cost of the save", runSaveCostChecks},
        {"a write that never comes back", runStalledSaveChecks},
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

    // Asked before the not-verified count, so a run that is both broken and incomplete
    // reports as broken rather than hiding failures behind ctest's "Skipped".
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
