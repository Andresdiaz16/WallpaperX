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
#include "tests/client/scratch_dir.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using wallpaperx::ConfigStore;
using wallpaperx::LoadResult;
using wallpaperx::LogFraming;
using wallpaperx::PersistedWallpaper;
using wallpaperx::SaveResult;
using wallpaperx::sanitizeForLog;
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

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++g_failures;
    }
}

// Goals this host could not exercise at all, as opposed to goals it exercised and that
// held. Reported as exit 77, which ctest turns into a SKIP through SKIP_RETURN_CODE --
// the same channel daemon_system_test uses, and for the same reason: a phase that returned
// early after printing a reason would otherwise leave ctest reporting PASS for coverage
// that never ran.
int g_notVerified = 0;
constexpr int kNotVerifiedExitCode = 77;

void notVerified(const char* goals, const std::string& reason) {
    std::printf("config_store_test: NOT VERIFIED -- %s could not be exercised: %s. This run establishes nothing "
                "about them, and is reported to ctest as a skip rather than as a pass.\n",
                goals, reason.c_str());
    std::fflush(stdout);
    ++g_notVerified;
}

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
        // force. CONTROL: observed red in the other direction first -- with the load still
        // accepting NUL, this row reported 1 entry and no error where it now expects none.
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
    // CONTROL: the `std::filesystem::rename(m_store.file(), kept, ec)` block deleted from
    //       WallpaperManager's constructor -> "FAIL: a config that could not be read is kept
    //       as config.json.bad" and "FAIL: the preserved copy is byte-identical to the file
    //       the daemon could not read", printing "  no config.json.bad exists". Reverted.
    const WallpaperManager::Snapshot afterLoad = manager.snapshot();
    check(afterLoad.health.persistence.status == wallpaperx::HealthStatus::Fail,
          "a config that could not be read leaves the persistence check reporting failure");
    check(afterLoad.health.persistence.detail.find(".bad") != std::string::npos,
          "the persistence failure says where the unreadable config was kept");
    if (afterLoad.health.persistence.detail.find(".bad") == std::string::npos) {
        std::fprintf(stderr, "  persistence detail: %s\n", afterLoad.health.persistence.detail.c_str());
    }

    manager.restoreSaved();
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
    const std::string config = "{\"version\":1,\"monitors\":["
                               "{\"monitor_id\":3,\"type\":\"static\",\"path\":" +
                               nlohmann::json(present.string()).dump() +
                               "},"
                               "{\"monitor_id\":40,\"type\":\"static\",\"path\":\"/a.png\"},"
                               "{\"monitor_id\":5,\"type\":\"slideshow\",\"path\":\"/b.png\"},"
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

    manager.requestWallpaper("static", other.string(), 1);
    std::string why;
    const std::map<int, std::string> onDisk = pathsInFile(file, why);
    std::vector<int> ids;
    for (const auto& entry : onDisk) ids.push_back(entry.first);
    const std::vector<int> expectedIds{1, 3, 5, 6, 40};
    const bool survived = ids == expectedIds;
    check(survived, "the entries this build cannot apply are all still in the file after another monitor is set");
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

}  // namespace

int main() {
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
        {"entries this build cannot apply", runUnappliedEntryChecks},
        {"a save that cannot succeed", runFailedSaveChecks},
        {"the cost of the save", runSaveCostChecks},
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
    if (g_failures > 0) {
        std::fprintf(stderr, "config_store_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    if (g_notVerified > 0) {
        std::printf("config_store_test: every check that ran passed, but %d goal(s) could not be exercised on this "
                    "host (listed above) -- reported to ctest as a skip, not a pass.\n",
                    g_notVerified);
        return kNotVerifiedExitCode;
    }
    std::printf("config_store_test: all checks passed\n");
    return 0;
}
