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
//     temp_directory_path() (or, for the one measurement that needs a real disk, under the
//     build directory) and removes again. A test that touched $HOME/.config/wallpaperx
//     would be restoring its own wallpapers onto a developer's desktop, which has happened.
//   - A phase that cannot be set up on this host prints a reason and is reported to ctest
//     as a skip (exit 77), never as a pass.

#include "daemon/config_store.h"
#include "daemon/log_escape.h"
#include "daemon/wallpaper_manager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using wallpaperx::ConfigStore;
using wallpaperx::LoadResult;
using wallpaperx::LogFraming;
using wallpaperx::PersistedWallpaper;
using wallpaperx::SaveResult;
using wallpaperx::sanitizeForLog;
using wallpaperx::WallpaperManager;

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

// A directory this process creates and removes. `base` is a parameter because one phase
// needs a real disk (temp_directory_path() is tmpfs on most hosts, where the measurement
// it makes cannot discriminate) while every other phase wants the fastest scratch space
// available.
class TempDir {
public:
    TempDir(const std::filesystem::path& base, const char* tag) {
        static int counter = 0;
        m_path = base / ("wpx_config_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + "_" +
                         std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
        m_ok = !ec;
    }

    ~TempDir() {
        std::error_code ec;
        // A phase makes a directory read-only on purpose; without this, remove_all fails
        // and the scratch space leaks for every later run.
        std::filesystem::permissions(m_path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(m_path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory(ec)) {
                std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::add, ec);
            }
        }
        std::filesystem::remove_all(m_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    bool ok() const { return m_ok; }
    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
    bool m_ok = false;
};

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
    const TempDir scratch(std::filesystem::temp_directory_path(), "location");
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
    // COVERAGE-ASSERT: the table's size is asserted against the number of cases actually
    //       run, so a future edit that returns early out of the loop fails instead of going
    //       quiet.
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

    int exercised = 0;
    for (const LocationCase& c : cases) {
        const ScopedEnv xdgEnv("XDG_CONFIG_HOME", c.xdg);
        const ScopedEnv homeEnv("HOME", c.home);
        const std::filesystem::path got = wallpaperx::defaultConfigPath();
        check(got == c.expected, c.what);
        if (got != c.expected) {
            std::fprintf(stderr, "  expected '%s', got '%s'\n", c.expected.string().c_str(), got.string().c_str());
        }
        ++exercised;
    }
    check(exercised == static_cast<int>(cases.size()), "every location case was exercised");

    // The other half of S8.9: a disabled store is silent, not merely path-less. A save()
    // that fell back to a relative name would land in the working directory, which under
    // ctest is the build tree -- the exact "config file left where nobody looks" the
    // decision exists to prevent.
    const std::filesystem::path cwd = std::filesystem::current_path();
    const ConfigStore disabled{std::filesystem::path{}};
    check(!disabled.enabled(), "a ConfigStore built from an empty path reports itself disabled");
    const SaveResult saved = disabled.save({PersistedWallpaper{0, "static", "/tmp/x.png"}});
    check(saved.error.empty(), "saving through a disabled store is not an error");
    check(!std::filesystem::exists(cwd / "config.json") && !std::filesystem::exists(cwd / "wallpaperx") &&
              !std::filesystem::exists(cwd / ".config"),
          "a disabled store writes nothing into the working directory");
    const LoadResult loaded = disabled.load();
    check(loaded.entries.empty() && loaded.error.empty() && !loaded.fileExisted,
          "loading through a disabled store reports nothing, not an error");
}

// ---------------------------------------------------------------------------- //
// Phase 2 -- S8.2: load() survives any file content at all.
// ---------------------------------------------------------------------------- //
void runLoadCorpusChecks() {
    const TempDir scratch(std::filesystem::temp_directory_path(), "corpus");
    if (!scratch.ok()) {
        notVerified("S8.2 (no config content stops the daemon)", "a scratch directory could not be created");
        return;
    }

    std::string huge(100u * 1024u * 1024u, 'x');
    std::string deep = "{\"version\":1,\"monitors\":";
    deep.append(100000, '[');
    deep.append(100000, ']');
    deep += "}";

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
    // COVERAGE-ASSERT: the number of cases run is asserted against the table's size.
    struct LoadCase {
        const char* what;
        std::string content;
        int expectedEntries;
        int expectedMalformed;
        bool expectError;
    };
    const std::vector<LoadCase> cases{
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
        {"a NUL byte in the middle of the file", std::string("{\"version\":1,\"mon\0itors\":[]}", 27), 0, 0, true},
        {"100 MB of no JSON whatsoever", std::move(huge), 0, 0, true},
        // Parses fine and holds one element -- an array 100 000 deep -- which is one
        // malformed entry, not a broken file. The case is here for the parser's stack, not
        // for its verdict: a recursive-descent parser (or a recursive destructor) would
        // take the process down rather than return anything at all.
        {"100 000 levels of nesting", std::move(deep), 0, 1, false},
        {"a well-formed file",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a.png\"},"
         "{\"monitor_id\":3,\"type\":\"static\",\"path\":\"/b.png\"}]}",
         2, 0, false},
    };

    const std::filesystem::path file = scratch.path() / "wallpaperx" / "config.json";
    const ConfigStore store(file);
    int exercised = 0;
    for (const LoadCase& c : cases) {
        if (!writeFile(file, c.content)) {
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
    check(exercised == static_cast<int>(cases.size()), "every load case was exercised");

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
    const TempDir scratch(std::filesystem::temp_directory_path(), "roundtrip");
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
    for (size_t i = 0; i < cases.size() && i < loaded.entries.size(); ++i) {
        const bool identical = loaded.entries[i].path == cases[i].second;
        check(identical, cases[i].first);
        if (!identical) {
            std::fprintf(stderr, "  in:  %s\n  out: %s\n", hex(cases[i].second).c_str(),
                         hex(loaded.entries[i].path).c_str());
        }
    }
}

// ---------------------------------------------------------------------------- //
// Phase 4 -- S8.7: an invalid-UTF-8 path costs only its own monitor.
// ---------------------------------------------------------------------------- //
void runNonUtf8Checks() {
    const TempDir scratch(std::filesystem::temp_directory_path(), "utf8");
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
    const TempDir scratch(std::filesystem::temp_directory_path(), "malformed");
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
    const TempDir scratch(std::filesystem::temp_directory_path(), "restore");
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
    // below able to see a rewrite that happens to preserve the same entries.
    const std::string original = "{\"monitors\":[{\"path\":\"" + missing +
                                 "\",\"type\":\"static\",\"monitor_id\":0},{\"path\":\"" + present.string() +
                                 "\",\"type\":\"static\",\"monitor_id\":1}],\"version\":1}";
    if (!writeFile(file, original)) {
        notVerified("S8.5 and S8.4", "the config file this phase reads could not be written");
        return;
    }
    std::error_code ec;
    const auto mtimeBefore = std::filesystem::last_write_time(file, ec);

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
    // CONTROL: `persist(persistableLocked());` added at the end of
    //       WallpaperManager::restoreSaved(), outside the lock -> "FAIL: restoring does not
    //       rewrite the config file" and "FAIL: restoring does not touch the config file's
    //       mtime", and nothing else red. Reverted.
    check(readFile(file) == original, "constructing a manager reads the config file without rewriting it");

    const std::vector<int> restored = manager.restoreSaved();
    check(restored == std::vector<int>({0, 1}), "restoreSaved returns the monitors to post, in file order");
    check(readFile(file) == original, "restoring does not rewrite the config file");
    const auto mtimeAfter = std::filesystem::last_write_time(file, ec);
    check(!ec && mtimeAfter == mtimeBefore, "restoring does not touch the config file's mtime");

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
// Phase 7 -- S8.6: a save that cannot happen fails loudly and harmlessly.
// ---------------------------------------------------------------------------- //
void runFailedSaveChecks() {
    const TempDir scratch(std::filesystem::temp_directory_path(), "readonly");
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

using Clock = std::chrono::steady_clock;

double microsOf(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(d).count();
}

double median(std::vector<double> samples) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

// Writes `text` through a temp file and rename, exactly as ConfigStore::save() does, with
// the durability the design rejected switched on or off. This is the test's own writer,
// not the daemon's: it is what establishes that the difference between the two variants is
// visible AT ALL on this host's filesystem, and it must stay independent of the production
// code so that a mutation of that code cannot move the yardstick it is measured against.
bool writeVariant(const std::filesystem::path& file, const std::string& text, bool durable) {
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
    if (durable && ::fsync(fd) != 0) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    if (::rename(temp.c_str(), file.c_str()) != 0) return false;
    if (durable) {
        const int dirFd = ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dirFd < 0) return false;
        ::fsync(dirFd);
        ::close(dirFd);
    }
    return true;
}

void runSaveCostChecks() {
    constexpr int kIterations = 200;
    constexpr const char* kGoal = "S8.10 (the save on SetWallpaper does not make a DBus handler slow)";

#ifdef WALLPAPERX_TIMING_INSTRUMENTED
    // Not a tolerance widened until the sanitized run fits -- that would blunt the check
    // for every ordinary build too. The two sides of this comparison are instrumented
    // differently (see backend/CMakeLists.txt), so a sanitized run measures the tool, and
    // measured 133.8 us against a 123.5 us threshold on code that is correct.
    notVerified(kGoal, "this build is sanitized, so the daemon's save runs instrumented while the write+rename this "
                       "phase measures it against does not -- the comparison would grade the sanitizer");
    return;
#else

    // The build directory, not temp_directory_path(): /tmp is tmpfs on most hosts, and on
    // tmpfs an fsync costs nothing, so the durable variant this phase measures itself
    // against would be indistinguishable from the cheap one and the phase could establish
    // nothing. Falls back to /tmp only so the gate below reports the reason rather than the
    // phase failing to start.
    TempDir onDisk(std::filesystem::current_path(), "savecost");
    if (!onDisk.ok()) {
        notVerified(kGoal, "a scratch directory could not be created in the build directory");
        return;
    }
    const std::filesystem::path persistedFile = onDisk.path() / "wallpaperx" / "config.json";
    const std::filesystem::path probeFile = onDisk.path() / "wallpaperx" / "probe.json";
    const std::filesystem::path wallpaper = onDisk.path() / "wallpaper.png";
    std::error_code ec;
    std::filesystem::create_directories(persistedFile.parent_path(), ec);
    if (ec || !writeWallpaperFile(wallpaper)) {
        notVerified(kGoal, "the fixture files this phase measures against could not be written");
        return;
    }

    WallpaperManager::Config persisting;
    persisting.configFile = persistedFile;
    WallpaperManager persistingManager(persisting);

    WallpaperManager::Config none;
    WallpaperManager silentManager(none);

    // One warm-up of each, so the first sample does not carry directory creation, page
    // faults on first touch, or the allocator's first growth.
    persistingManager.requestWallpaper("static", wallpaper.string(), 0);
    silentManager.requestWallpaper("static", wallpaper.string(), 0);
    // X.3 in both directions: a missing file here is the DAEMON not having saved, which is
    // a failure and must not be reported as this file's probe writer failing (measured: with
    // persist() removed from requestWallpaper, the two conditions folded into one reported
    // NOT VERIFIED and blamed the probe writer). A probe write that fails is genuinely this
    // side of the boundary and stays a skip.
    const std::string payload = readFile(persistedFile);
    if (payload.empty()) {
        check(false, "the warm-up request wrote the config file whose cost this phase measures");
        return;
    }
    if (!writeVariant(probeFile, payload, false) || !writeVariant(probeFile, payload, true)) {
        notVerified(kGoal, "the probe writer could not write into the scratch directory, so the cheap and durable "
                           "variants cannot be compared");
        return;
    }

    std::vector<double> withSave, withoutSave, cheap, durable;
    withSave.reserve(kIterations);
    withoutSave.reserve(kIterations);
    cheap.reserve(kIterations);
    durable.reserve(kIterations);
    bool probeFailed = false;
    // Interleaved, not four separate loops: a background flush, a CPU frequency change or
    // another ctest job landing mid-run would otherwise be charged entirely to whichever
    // variant was running at the time. PLAN.md's own measurement interleaved for this
    // reason and this reproduces it.
    for (int i = 0; i < kIterations; ++i) {
        const auto t0 = Clock::now();
        persistingManager.requestWallpaper("static", wallpaper.string(), 0);
        const auto t1 = Clock::now();
        silentManager.requestWallpaper("static", wallpaper.string(), 0);
        const auto t2 = Clock::now();
        probeFailed = probeFailed || !writeVariant(probeFile, payload, false);
        const auto t3 = Clock::now();
        probeFailed = probeFailed || !writeVariant(probeFile, payload, true);
        const auto t4 = Clock::now();
        withSave.push_back(microsOf(t1 - t0));
        withoutSave.push_back(microsOf(t2 - t1));
        cheap.push_back(microsOf(t3 - t2));
        durable.push_back(microsOf(t4 - t3));
    }
    if (probeFailed) {
        notVerified(kGoal, "a probe write failed part way through the measurement, so the yardstick is incomplete");
        return;
    }

    const double medianWith = median(withSave);
    const double medianWithout = median(withoutSave);
    const double medianCheap = median(cheap);
    const double medianDurable = median(durable);
    const double added = medianWith - medianWithout;
    std::printf("config_store_test: SetWallpaper's non-GPU work: %.1f us with persistence, %.1f us without "
                "(added %.1f us). This host's own write+rename: %.1f us; write+fsync+rename+fsync(dir): %.1f us.\n",
                medianWith, medianWithout, added, medianCheap, medianDurable);

    // GOAL: S8.10 -- the save the DBus handler performs is the write+rename variant
    //       (9.3 us measured) and not the durable one (823.8 us) the design rejected.
    //       False if a SetWallpaper's non-GPU work costs what a durable write costs.
    // WHY-THIS-SHAPE: the bound is not a constant. A literal microsecond threshold is
    //       either a machine-specific number that fails on a slower disk or a number wide
    //       enough to admit an fsync, and "a check that cannot tell 9 us from 824 us is not
    //       testing this". So the test measures BOTH variants itself, in the same
    //       directory, and asserts the daemon's added cost sits on the cheap side of their
    //       geometric midpoint. Both yardsticks are written by this file, so a mutation of
    //       ConfigStore cannot move them -- if the gate were computed from the daemon's own
    //       cost, making the daemon durable would make the phase SKIP instead of fail,
    //       which is the vacuous-green direction.
    // CONTROL: an fsync of the temp file and an fsync of the directory added around
    //       ConfigStore::save()'s rename -- PLAN.md's variant B -> "FAIL: the persistence a
    //       SetWallpaper pays for is the write+rename variant, not the durable one", with
    //       "added 877.2 us against a 96.2 us threshold (cheap 10.9 us, durable 848.2 us)".
    //       Reverted; the shipped code measures 28-40 us added on this host, roughly three
    //       times the bare write+rename because save() also builds the JSON and calls
    //       create_directories, and an order of magnitude under the threshold.
    // COVERAGE-ASSERT: the measurement is only meaningful if the daemon really wrote, so
    //       the file's contents are asserted afterwards -- a configFile the manager ignored
    //       would otherwise show a delta of zero and pass with flying colours.
    if (medianCheap <= 0.0 || medianDurable < 10.0 * medianCheap) {
        notVerified(kGoal, "this filesystem makes a durable write no more expensive than a plain one (measured " +
                               std::to_string(medianDurable) + " us against " + std::to_string(medianCheap) +
                               " us), so no measurement here can tell the two designs apart");
        return;
    }
    const double threshold = std::sqrt(medianCheap * medianDurable);
    const bool cheapSide = added < threshold;
    check(cheapSide,
          "the persistence a SetWallpaper pays for is the write+rename variant, not the durable one");
    if (!cheapSide) {
        std::fprintf(stderr, "  added %.1f us against a %.1f us threshold (cheap %.1f us, durable %.1f us)\n", added,
                     threshold, medianCheap, medianDurable);
    }

    std::string why;
    const std::map<int, std::string> onDiskPaths = pathsInFile(persistedFile, why);
    const bool wrote = onDiskPaths.size() == 1 && onDiskPaths.count(0) == 1 &&
                       onDiskPaths.at(0) == wallpaper.string();
    check(wrote, "the measured requests really did write the config file, so the measurement is of a save");
    if (!wrote) std::fprintf(stderr, "  %s\n", why.empty() ? "the file holds something else" : why.c_str());
#endif
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
    std::string realConfigBefore;
    if (!realConfig.empty()) {
        std::error_code ec;
        realConfigExisted = std::filesystem::exists(realConfig, ec) && !ec;
        realConfigBefore = realConfigExisted ? readFile(realConfig) : std::string();
    }

    try {
        runLocationChecks();
        runLoadCorpusChecks();
        runRoundTripChecks();
        runNonUtf8Checks();
        runMalformedEntryChecks();
        runRestoreChecks();
        runFailedSaveChecks();
        runSaveCostChecks();
    } catch (const std::exception& e) {
        // X.4: this file drives a WallpaperManager, whose constructor can throw on a
        // machine whose shader directory is missing. Without this the process would abort
        // and ctest would report an opaque SIGABRT instead of the FAIL lines above.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++g_failures;
    }

    if (realConfig.empty()) {
        // Printed, not silent: with no usable HOME there is nothing to protect, and a
        // reader must not mistake the guard's silence for it having run.
        std::printf("config_store_test: this process has no usable XDG_CONFIG_HOME or HOME, so there is no real "
                    "config file for this run to have touched.\n");
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
