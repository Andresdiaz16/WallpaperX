// SPDX-License-Identifier: GPL-2.0-or-later

// The daemon's install (S14.6, S14.7, S14.8): what `cmake --install` puts under a prefix, whether the
// installed systemd user unit starts the installed binary and restarts it the way Step 14 needs,
// and whether that binary, run from the prefix, draws with the shaders installed beside it.
//
// It installs into scratch directories only, and runs the installed daemon with a HOME of its own
// on the private bus ctest gives it; main() refuses any other bus. It never starts the unit.

#include "daemon/dbus_service.h"

#include <string>
#include <string_view>

namespace {
// SpawnedDaemon execs WALLPAPERX_DAEMON_BINARY; here that is the copy this test installed, known
// only at run time.
std::string g_installedDaemon;
}  // namespace
#define WALLPAPERX_DAEMON_BINARY g_installedDaemon

#include "support/test_image.h"
#include "tests/client/daemon_client.h"
#include "tests/client/http_probe.h"
#include "tests/client/scratch_dir.h"
#include "tests/name_watcher.h"
#include "tests/spawned_daemon.h"
#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using wallpaperx::testing::check;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::getStatus;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::NameWatcher;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::ScratchDir;
using wallpaperx::testing::SpawnedDaemon;
using wallpaperx::testing::StatusReply;
using wallpaperx::testing::wallpaperStatus;

namespace fs = std::filesystem;

namespace {

constexpr const char* kUnitName = "wallpaperx-daemon.service";

// Where this build tree was configured to install each file, relative to the prefix. Read from the
// configuration rather than assumed, so a layout overridden at configure time is not graded as a
// broken install. main() refuses an absolute one.
// CONTROL: by hand 2026-09-24, build-qa reconfigured with the unit in lib/systemd/user and the
//       binary in sbin -> green; with unitPath() made a literal share/systemd/user as well -> ten
//       unit and manifest lines red. The tree was configured back afterwards.
fs::path unitPath(const fs::path& prefix) { return prefix / WALLPAPERX_TEST_UNIT_DIR / kUnitName; }
fs::path binaryPath(const fs::path& prefix) { return prefix / WALLPAPERX_TEST_BINDIR / "wallpaperx-daemon"; }
fs::path shaderDir(const fs::path& prefix) { return prefix / WALLPAPERX_TEST_SHADER_DIR; }

// Empty when systemd-analyze was not found at configure time. main() reports that once; every
// verify below is skipped on it.
constexpr bool kHaveAnalyze = sizeof(WALLPAPERX_SYSTEMD_ANALYZE) > 1;

// runCaptured's answer when this harness could not run the command at all, which is never the
// command's fault.
constexpr int kCouldNotRun = -2;

// Runs `argv` to completion with `extraEnv` added, stdout and stderr captured together. The exit
// status, -1 when it did not exit normally, or kCouldNotRun with the reason in `output`.
int runCaptured(const std::vector<std::string>& argv, const std::vector<std::string>& extraEnv, std::string& output) {
    // An anonymous memory file, so capturing needs no writable directory.
    const int outFd = ::memfd_create("wpx_install_out", MFD_CLOEXEC);
    if (outFd < 0) {
        output = std::string("no capture file could be created: ") + std::strerror(errno);
        return kCouldNotRun;
    }

    std::vector<std::string> env = wallpaperx::testing::mergedEnvironment(extraEnv);
    std::vector<char*> envp;
    for (std::string& entry : env) envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::vector<std::string> args = argv;
    std::vector<char*> argp;
    for (std::string& arg : args) argp.push_back(arg.data());
    argp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outFd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outFd, STDERR_FILENO);
    pid_t pid = -1;
    const int spawned = ::posix_spawnp(&pid, argp[0], &actions, nullptr, argp.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    if (spawned != 0 || ::waitpid(pid, &status, 0) != pid) {
        ::close(outFd);
        output = "could not run " + argv[0];
        return kCouldNotRun;
    }
    output.clear();
    ::lseek(outFd, 0, SEEK_SET);
    char buf[4096];
    for (ssize_t n; (n = ::read(outFd, buf, sizeof(buf))) > 0;) output.append(buf, static_cast<size_t>(n));
    ::close(outFd);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Every `Key=value` line of a unit file, by key, in file order. Section headers and comments are
// skipped: every key graded here is unique to its section.
std::multimap<std::string, std::string> unitKeys(const std::string& text) {
    std::multimap<std::string, std::string> keys;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        keys.emplace(line.substr(0, eq), line.substr(eq + 1));
    }
    return keys;
}

std::optional<std::string> onlyValue(const std::multimap<std::string, std::string>& keys, const std::string& key) {
    if (keys.count(key) != 1) return std::nullopt;
    return keys.find(key)->second;
}

// The program an ExecStart value names, read as systemd.service(5) "Command lines" reads it: the
// leading executable prefixes (@-:+!) dropped, one word, double-quoted or bare, and `%%` taken as
// `%`. nullopt for anything else this unit should never hold -- arguments, an escape, a quote in a
// bare word, a specifier other than `%%`.
std::optional<std::string> execProgram(const std::string& value) {
    const size_t at = value.find_first_not_of("@-:+!");
    if (at == std::string::npos) return std::nullopt;
    std::string word;
    if (value[at] == '"') {
        const size_t close = value.find('"', at + 1);
        if (close == std::string::npos || close + 1 != value.size()) return std::nullopt;
        word = value.substr(at + 1, close - at - 1);
        if (word.find('\\') != std::string::npos) return std::nullopt;
    } else {
        word = value.substr(at);
        if (word.find_first_of(" \t\"'\\") != std::string::npos) return std::nullopt;
    }
    std::string path;
    for (size_t i = 0; i < word.size(); ++i) {
        if (word[i] != '%') {
            path += word[i];
        } else if (i + 1 < word.size() && word[i + 1] == '%') {
            path += '%';
            ++i;
        } else {
            return std::nullopt;
        }
    }
    return path;
}

// A systemd time span in milliseconds, for the forms this unit could use: a bare number (seconds)
// or one number with ms, s, sec, m or min.
std::optional<double> spanMillis(const std::string& value) {
    size_t digits = 0;
    while (digits < value.size() && (std::isdigit(static_cast<unsigned char>(value[digits])) || value[digits] == '.')) {
        ++digits;
    }
    if (digits == 0) return std::nullopt;
    const double number = std::stod(value.substr(0, digits));
    const std::string unit = value.substr(digits);
    if (unit.empty() || unit == "s" || unit == "sec") return number * 1000.0;
    if (unit == "ms" || unit == "msec") return number;
    if (unit == "m" || unit == "min") return number * 60000.0;
    return std::nullopt;
}

// Whether `text` appears in the part of `output` from "CMake Error" on, all whitespace removed
// from both: CMake re-wraps its messages, and splits a newline in a path onto a fresh indented
// line. Looking anywhere in the output would be satisfied by the "-- Installing:" lines.
bool errorNames(const std::string& output, const std::string& text) {
    const size_t at = output.find("CMake Error");
    if (at == std::string::npos) return false;
    auto squeeze = [](std::string s) {
        std::erase_if(s, [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
        return s;
    };
    return squeeze(output.substr(at)).find(squeeze(text)) != std::string::npos;
}

struct Install {
    bool ran = false;  // false: already reported as a harness fault, grade nothing
    bool ok = false;
    std::string output;
};

// `wrapper` runs in front of cmake, for an install under a different umask.
Install install(const fs::path& prefix, const std::vector<std::string>& env = {},
                const std::vector<std::string>& wrapper = {}, const std::string& buildDir = WALLPAPERX_BUILD_DIR) {
    Install result;
    std::vector<std::string> argv = wrapper;
    argv.insert(argv.end(), {WALLPAPERX_CMAKE_COMMAND, "--install", buildDir, "--prefix", prefix.string(),
                             "--component", "daemon"});
    const int status = runCaptured(argv, env, result.output);
    if (status == kCouldNotRun) {
        harnessFailure("cmake --install into " + prefix.string() + " could not be run: " + result.output);
        return result;
    }
    result.ran = true;
    result.ok = status == 0;
    return result;
}

// S14.6: every shader the build produced is installed, byte for byte. False with nothing
// compared when the build produced none, which is the harness's problem.
void checkShaders(const fs::path& installedShaders) {
    // GOAL: S14.6 -- the install carries every shader the daemon can load, unchanged, so the
    //       installed daemon draws the same pictures as the one in the build tree.
    // WHY-THIS-SHAPE: compared against the build's own shader output directory, not against a
    //       list written here, so a shader added to the build is covered without editing this.
    // CONTROL: in backend/CMakeLists.txt, via control_run.sh, reverted: the shader install(DIRECTORY)
    //       rule deleted -> this line red five times, with both render lines and the /health line;
    //       its PATTERN narrowed to "fullscreen*.spv" -> this line red twice (the ycbcr_convert
    //       pair) and the video render line. Observed 2026-09-24.
    // COVERAGE-ASSERT: the number of shaders compared is asserted non-zero, so a shader directory
    //       that holds none cannot pass by comparing nothing. Controlled in renderer/CMakeLists.txt:
    //       WALLPAPERX_SHADER_OUTPUT_DIR set one level above where the shaders are compiled ->
    //       "FAIL (harness): no .spv was found ...", with both render lines red. Observed
    //       2026-09-24 via control_run.sh; reverted.
    int compared = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(WALLPAPERX_SHADER_OUTPUT_DIR, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() != ".spv") continue;
        ++compared;
        const fs::path installed = installedShaders / it->path().filename();
        const bool same = fs::is_regular_file(installed) && readFile(installed) == readFile(it->path());
        check(same, "every shader the build produced is installed, byte for byte");
        if (!same) std::fprintf(stderr, "  missing or different: %s\n", installed.c_str());
    }
    if (compared == 0) {
        harnessFailure(std::string("no .spv was found in the build's shader directory ") + WALLPAPERX_SHADER_OUTPUT_DIR);
    }
    std::printf("daemon_install_test: compared %d installed shader(s) with the build's\n", compared);
}

// S14.6 and S14.8: the unit's single ExecStart, read as systemd reads it, is `expectedExec`.
bool execStartNames(const std::string& unitText, const fs::path& expectedExec, const std::string& label) {
    const std::optional<std::string> exec = onlyValue(unitKeys(unitText), "ExecStart");
    const std::optional<std::string> program = exec ? execProgram(*exec) : std::nullopt;
    const bool names = program.has_value() && *program == expectedExec.string();
    check(names, (label + ": the installed unit's ExecStart names the installed binary").c_str());
    if (!names) {
        std::fprintf(stderr, "  ExecStart: %s\n  expected:  %s\n", exec ? exec->c_str() : "(missing or repeated)",
                     expectedExec.c_str());
    }
    return names;
}

// S14.6 and S14.7: the unit names the installed binary and restarts it on failure, with a start
// limit it can actually reach.
void checkUnit(const fs::path& unit, const fs::path& expectedExec) {
    const std::string text = readFile(unit);
    const auto keys = unitKeys(text);

    // GOAL: S14.6 -- the installed unit starts the binary installed beside it, named by the
    //       prefix the install actually used, including one given only to `cmake --install`.
    // WHY-THIS-SHAPE: exactly one ExecStart, unquoted as systemd would, then an exact match. A
    //       prefix substituted at configure time -- the obvious template -- names /usr/local
    //       here, not the scratch prefix given at install time.
    // CONTROL: in backend/CMakeLists.txt, both install-time cmake_path calls (bin and shader
    //       directory) based on "/usr/local" instead of the install prefix -> this line red, with
    //       the verify line, the DESTDIR ExecStart line, every odd prefix's three lines, every
    //       refused row but backslash, and both absolute-bin-directory lines. Observed 2026-09-24
    //       via control_run.sh; reverted.
    execStartNames(text, expectedExec, "plain prefix");

    // GOAL: S14.7 -- the unit restarts the daemon when it exits nonzero (75 after a lost device)
    //       and not when it is stopped.
    // CONTROL: `Restart=no` in backend/packaging/wallpaperx-daemon.service.in -> this line red,
    //       alone (systemd accepts the unit). Observed 2026-09-24 via control_run.sh; reverted.
    const std::optional<std::string> restart = onlyValue(keys, "Restart");
    check(restart.value_or("") == "on-failure", "the installed unit keeps Restart=on-failure");

    // GOAL: S14.7 -- systemd can reach the unit's start limit, so a daemon that dies at every
    //       start is eventually given up on. False when StartLimitBurst restarts, RestartSec
    //       apart, take longer than StartLimitIntervalSec: systemd's own default of 5 in 10 s
    //       is exactly that with RestartSec=3.
    // WHY-THIS-SHAPE: computed from the three values in the file, with systemd's defaults for
    //       any that are absent (100 ms, 5, 10 s), so dropping a key is graded, not skipped. An
    //       unparseable value is a failure: the unit is ours to keep readable.
    // CONTROL: in backend/packaging/wallpaperx-daemon.service.in, via control_run.sh, reverted:
    //       `StartLimitBurst=100` -> "... start limit is reachable" red, alone; `StartLimitBurst=ten`
    //       -> "... can be read" red, with every verify line. Observed 2026-09-24.
    const std::optional<double> restartSec = spanMillis(onlyValue(keys, "RestartSec").value_or("100ms"));
    const std::optional<double> interval = spanMillis(onlyValue(keys, "StartLimitIntervalSec").value_or("10s"));
    std::optional<long> burst;
    try {
        burst = std::stol(onlyValue(keys, "StartLimitBurst").value_or("5"));
    } catch (const std::exception&) {
    }
    const bool parsed = restartSec && interval && burst;
    check(parsed, "the installed unit's RestartSec, StartLimitBurst and StartLimitIntervalSec can be read");
    if (parsed) {
        const bool reachable = static_cast<double>(*burst) * *restartSec < *interval;
        check(reachable, "the installed unit's start limit is reachable: StartLimitBurst x RestartSec < "
                         "StartLimitIntervalSec");
        std::printf("daemon_install_test: start limit %ld starts x %.0f ms apart against a %.0f ms window\n", *burst,
                    *restartSec, *interval);
    }
}

// S14.6: systemd itself accepts the unit. `verify` exits 0 on a unit it could not parse and only
// prints, so any output at all is a failure. Needs kHaveAnalyze.
void checkVerify(const fs::path& unit, const std::string& label = "") {
    // GOAL: S14.6 -- systemd parses the installed unit without complaint.
    // WHY-THIS-SHAPE: silence is graded, not the exit status: measured, `systemd-analyze --user
    //       verify` on `Restart=on-failur` prints "Failed to parse Restart=on-failur, ignoring:
    //       Invalid argument" and exits 0, so a status check passes a unit systemd ignores a line of.
    // CONTROL: `Restart=on-failur` in backend/packaging/wallpaperx-daemon.service.in -> this line
    //       red for the plain prefix and all five odd prefixes, with "keeps Restart=on-failure".
    //       Observed 2026-09-24 via control_run.sh; reverted.
    std::string output;
    const int status = runCaptured({WALLPAPERX_SYSTEMD_ANALYZE, "--user", "verify", unit.string()}, {}, output);
    if (status == kCouldNotRun) {
        harnessFailure(label + "systemd-analyze could not be run: " + output);
        return;
    }
    const bool quiet = status == 0 && output.find_first_not_of(" \t\r\n") == std::string::npos;
    check(quiet, (label + "systemd-analyze --user verify accepts the installed unit, printing nothing").c_str());
    if (!quiet) std::fprintf(stderr, "  exit %d, printed:\n%s\n", status, output.c_str());
}

// S14.6: the unit is in install_manifest_daemon.txt, so an uninstall driven by the manifest
// removes it with the rest.
void checkManifest(const fs::path& finalPrefix, const fs::path& stagedPrefix, const std::string& label) {
    std::set<std::string> listed;
    std::ifstream in(fs::path(WALLPAPERX_BUILD_DIR) / "install_manifest_daemon.txt");
    for (std::string line; std::getline(in, line);) listed.insert(line);

    // GOAL: S14.6 -- the unit, written by hand rather than by install(FILES), is still recorded in
    //       the install manifest, in the same form as CMake's own entries (without DESTDIR on the
    //       CMake measured here), so a manifest-driven uninstall or package list includes it.
    // WHY-THIS-SHAPE: the expected entry is derived from how the manifest lists CMake's own files
    //       (every entry but the unit's), not written as "no DESTDIR" here: which form CMake uses
    //       is CMake's choice, and the property is that the unit matches it. Not from the binary's
    //       entry alone, which a binary installed to the wrong directory would take away.
    // CONTROL: in backend/CMakeLists.txt, via control_run.sh, reverted: the unit's
    //       `list(APPEND CMAKE_INSTALL_MANIFEST_FILES ...)` deleted -> this line red for the plain
    //       prefix and with DESTDIR; the unit appended with DESTDIR in front -> red with DESTDIR
    //       only. Observed 2026-09-24.
    bool stagedForm = false;
    bool finalForm = false;
    for (const std::string& line : listed) {
        if (fs::path(line).filename() == kUnitName) continue;
        if (line.rfind(stagedPrefix.string() + "/", 0) == 0) stagedForm = true;
        if (line.rfind(finalPrefix.string() + "/", 0) == 0) finalForm = true;
    }
    if (stagedForm == finalForm && stagedPrefix != finalPrefix) {
        harnessFailure(label + ": install_manifest_daemon.txt lists CMake's own files " +
                       (stagedForm ? "both with and without DESTDIR" : "under neither form of the prefix") +
                       ", so how it records paths cannot be read from it");
        return;
    }
    const fs::path base = stagedForm ? stagedPrefix : finalPrefix;
    const fs::path unit = unitPath(base);
    const bool recorded = listed.count(unit.string()) != 0;
    check(recorded, (label + ": the install manifest lists the unit the way it lists CMake's own files").c_str());
    if (!recorded) std::fprintf(stderr, "  expected the line %s\n", unit.c_str());
}

using UnitCopies = std::map<std::string, fs::file_time_type>;

// Adds every file under `dir` whose name contains the unit's to `copies`. False when something
// could not be read. What vanished while being walked is skipped, not an error: other ctest
// entries create and remove directories in this tree meanwhile, and a directory that no longer
// exists cannot hold a copy the install left. The root must exist.
bool collectUnitCopies(const fs::path& dir, bool root, UnitCopies& copies, size_t& scanned) {
    const auto gone = [](const std::error_code& ec) { return ec == std::errc::no_such_file_or_directory; };
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return !root && gone(ec);
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        ++scanned;
        const fs::file_status status = it->symlink_status(ec);
        if (ec) {
            if (gone(ec)) continue;
            return false;
        }
        if (fs::is_directory(status)) {
            if (!collectUnitCopies(it->path(), false, copies, scanned)) return false;
            continue;
        }
        if (it->path().filename().string().find(kUnitName) == std::string::npos) continue;
        const fs::file_time_type when = it->last_write_time(ec);
        if (ec) {
            if (gone(ec)) continue;
            return false;
        }
        copies[it->path().string()] = when;
    }
    return !ec || (!root && gone(ec));
}

// Every file in the build tree whose name contains the unit's, with its modification time; nullopt
// when the tree could not be walked.
std::optional<UnitCopies> unitCopiesInBuildTree(const fs::path& buildDir = WALLPAPERX_BUILD_DIR) {
    UnitCopies copies;
    size_t scanned = 0;
    if (!collectUnitCopies(buildDir, true, copies, scanned) || scanned == 0) return std::nullopt;
    return copies;
}

// S14.8: prefixes systemd can represent only quoted, each installed and read back three ways.
void checkOddPrefixes(const fs::path& root) {
    for (const char* name : {"with space", "pct%n", "dol$HOME", "brace${HOME}x", "all of %i $x ;#@~"}) {
        const std::string label = std::string("prefix \"") + name + "\"";
        const fs::path prefix = root / "odd" / name;
        const Install installed = install(prefix);
        if (!installed.ran) continue;
        check(installed.ok, (label + ": cmake --install succeeds").c_str());
        const fs::path unit = unitPath(prefix);
        if (!installed.ok || !fs::is_regular_file(unit)) {
            std::fprintf(stderr, "%s\n", installed.output.c_str());
            check(false, (label + ": the install writes the unit").c_str());
            continue;
        }
        const fs::path binary = binaryPath(prefix);
        if (!fs::is_regular_file(binary)) {
            check(false, (label + ": the install puts the binary in <prefix>/" WALLPAPERX_TEST_BINDIR).c_str());
            continue;
        }

        // GOAL: S14.8 -- an install prefix holding a space, `%` or `$` yields a unit whose
        //       ExecStart systemd reads as the installed binary, and that verify accepts.
        // WHY-THIS-SHAPE: three answers, because each alone can be wrong. The unquoted string is
        //       this file's reading of systemd's rules; verify's silence says only that the path
        //       systemd parsed is some executable. So the binary is then moved aside, and verify
        //       must name exactly its path as missing: a parse that landed on any other file
        //       would stay silent or name that file.
        // CONTROL: in backend/CMakeLists.txt, through control_run.sh, reverted: the bare-path
        //       allowlist widened with a space -> "with space" red on all three lines; `%` not
        //       doubled -> "pct%n" and "all of" red on all three; `$` doubled -> the three `$`
        //       prefixes red on all three. Re-observed 2026-09-24. By hand here, reverted: the binary
        //       copied aside instead of moved -> only the moved-aside line red, for all five.
        execStartNames(readFile(unit), binary, label);
        if (!kHaveAnalyze) continue;
        checkVerify(unit, label + ": ");

        const fs::path aside = binary.parent_path() / "moved-aside";
        std::error_code ec;
        fs::rename(binary, aside, ec);
        if (ec) {
            harnessFailure("could not move " + binary.string() + " aside: " + ec.message());
            continue;
        }
        std::string output;
        const int status = runCaptured({WALLPAPERX_SYSTEMD_ANALYZE, "--user", "verify", unit.string()}, {}, output);
        fs::rename(aside, binary, ec);
        if (ec) harnessFailure("could not put " + binary.string() + " back: " + ec.message());
        if (status == kCouldNotRun) {
            harnessFailure(label + ": systemd-analyze could not be run: " + output);
            continue;
        }
        const std::string named = "Command " + binary.string() + " is not executable";
        const bool exact = output.find(named) != std::string::npos;
        check(exact, (label + ": with the binary moved aside, verify names exactly its path as missing").c_str());
        if (!exact) std::fprintf(stderr, "  expected \"%s\", verify printed:\n%s\n", named.c_str(), output.c_str());
    }
}

// S14.8: a prefix systemd cannot represent fails the install, naming the path, and no unit is
// installed under it.
void checkRefusedPrefixes(const fs::path& root) {
    const std::vector<std::pair<std::string, std::string>> prefixes = {
        {"double quote", "quo\"x"}, {"single quote", "apo'x"}, {"backslash", "bs\\x"},
        {"newline", "nl\nx"},       {"tab", "tab\tx"},         {"byte 1", "ctl\x01x"},
        {"byte 127", "del\x7fx"},
    };
    for (const auto& [label, name] : prefixes) {
        const fs::path prefix = root / "refused" / name;
        const Install installed = install(prefix);
        if (!installed.ran) continue;

        // GOAL: S14.8 -- a prefix holding a character systemd cannot take in an executable path
        //       stops the install with an error naming the path, instead of installing a unit
        //       that can never load.
        // WHY-THIS-SHAPE: the path is looked for in the error only (errorNames above). The unit
        //       step runs before anything is copied, so every row, backslash included, is refused
        //       by it.
        // CONTROL: in backend/CMakeLists.txt, via control_run.sh, reverted, re-observed 2026-09-24:
        //       the path dropped from the refusal message -> all seven "naming the path" lines red,
        //       backslash's included; code 34 (") alone removed -> the double-quote rows only; the
        //       refused list emptied -> every row but backslash red on both lines. A backslash row
        //       stays refused even then, by CMake's own file(WRITE): it creates the parent
        //       directories reading the backslash as a separator and then cannot open the path.
        const bool named = !installed.ok && errorNames(installed.output, prefix.string());
        check(named, ("prefix with a " + label + ": the install fails with an error naming the path").c_str());
        if (!named) std::fprintf(stderr, "  install output:\n%s\n", installed.output.c_str());
        std::error_code ec;
        const bool noUnit = !fs::exists(unitPath(prefix), ec);
        check(noUnit, ("prefix with a " + label + ": no unit is installed").c_str());
    }
}

// S14.6: DESTDIR stages the files and leaves ExecStart naming where they will finally live.
void checkDestdir(const fs::path& root) {
    const fs::path destdir = root / "destdir";
    // Never created: the install writes only under DESTDIR.
    const fs::path finalPrefix = "/opt/wallpaperx-install-test";
    const Install staged = install(finalPrefix, {"DESTDIR=" + destdir.string()});
    if (!staged.ran) return;
    check(staged.ok, "cmake --install with DESTDIR succeeds");
    if (!staged.ok) std::fprintf(stderr, "%s\n", staged.output.c_str());
    const fs::path stagedPrefix = destdir / finalPrefix.relative_path();
    check(fs::is_regular_file(binaryPath(stagedPrefix)), "with DESTDIR, the binary lands under it");

    // GOAL: S14.6 -- the unit, written by the install step's own code rather than install(FILES),
    //       honours DESTDIR as CMake's own rules do.
    // CONTROL: `$ENV{DESTDIR}` dropped from `_wx_staged_unit` in backend/CMakeLists.txt -> this line
    //       red, with "with DESTDIR succeeds" and "the binary lands under it": the unit step, which
    //       runs first, failed writing under /opt (not writable here, so nothing reached it).
    //       Observed 2026-09-24 via control_run.sh; reverted.
    const fs::path unit = unitPath(stagedPrefix);
    const bool haveUnit = fs::is_regular_file(unit);
    check(haveUnit, "with DESTDIR, the unit lands under it");
    if (staged.ok) checkManifest(finalPrefix, stagedPrefix, "with DESTDIR");

    // GOAL: S14.6 -- a staged install names the final path, not the staging one: a package
    //       built with DESTDIR must start /opt/..., not /build/destdir/opt/....
    // CONTROL: in backend/CMakeLists.txt, both install-time cmake_path calls based on
    //       `$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}` -> this line red, alone. Observed 2026-09-24 via
    //       control_run.sh; reverted.
    if (haveUnit) execStartNames(readFile(unit), binaryPath(finalPrefix), "with DESTDIR");
}

// S14.6: the unit is 0644 whatever the installing user's umask, as install(FILES) would make it.
void checkUnitModeUnderUmask(const fs::path& root) {
    const fs::path prefix = root / "umask077";
    const Install installed = install(prefix, {}, {"/bin/sh", "-c", "umask 077 && exec \"$0\" \"$@\""});
    if (!installed.ran) return;
    check(installed.ok, "cmake --install under umask 077 succeeds");
    if (!installed.ok) {
        std::fprintf(stderr, "%s\n", installed.output.c_str());
        return;
    }
    // GOAL: S14.6 -- an install run under a restrictive umask still leaves the unit readable by
    //       everyone (0644), so a systemd user manager and a package built from it can read it.
    // WHY-THIS-SHAPE: the umask is set by a shell in front of cmake, not by umask() here, which is
    //       process-wide and would also govern every file this test's other threads create.
    // CONTROL: the unit's file(CHMOD) deleted from backend/CMakeLists.txt -> this line red, alone
    //       (the unit came out 0600). Observed 2026-09-24 via control_run.sh; reverted.
    struct stat st {};
    const fs::path unit = unitPath(prefix);
    if (::stat(unit.c_str(), &st) != 0) {
        check(false, "under umask 077, the install writes the unit");
        return;
    }
    const mode_t mode = st.st_mode & 07777;
    check(mode == 0644, "under umask 077, the installed unit is mode 0644");
    if (mode != 0644) std::fprintf(stderr, "  mode %04o\n", static_cast<unsigned>(mode));
}

// S14.6: a build configured with an absolute bin directory, whose compiled-in path from the binary
// to the shaders holds only under the configured prefix. Configured here, never built: the check
// runs before anything is copied, and the install then stops at the missing binary.
void checkAbsoluteBindir(const fs::path& root) {
    const fs::path build = root / "absbin-build";
    const fs::path configured = root / "absbin-configured";
    const fs::path bindir = root / "absbin-bin";
    std::string output;
    const int status =
        runCaptured({WALLPAPERX_CMAKE_COMMAND, "-S", WALLPAPERX_BACKEND_SOURCE_DIR, "-B", build.string(), "-G",
                     WALLPAPERX_CMAKE_GENERATOR, "-DWALLPAPERX_BUILD_KDE_PLUGIN=" WALLPAPERX_BUILD_KDE_PLUGIN_SETTING,
                     "-DCMAKE_INSTALL_PREFIX=" + configured.string(), "-DCMAKE_INSTALL_BINDIR=" + bindir.string()},
                    {}, output);
    if (status == kCouldNotRun) {
        harnessFailure("cmake could not be run to configure a scratch build: " + output);
        return;
    }
    check(status == 0, "a build configured with an absolute CMAKE_INSTALL_BINDIR configures");
    if (status != 0) {
        std::fprintf(stderr, "%s\n", output.c_str());
        return;
    }

    // GOAL: S14.6 -- given no override, the unit goes to share/systemd/user, which systemd --user
    //       searches under $XDG_DATA_HOME, so a ~/.local install is found; lib/systemd/user is not.
    // WHY-THIS-SHAPE: read from this scratch configure's cache, which set no unit directory, not
    //       from this build tree's: every other check follows this tree's configured layout, so a
    //       legitimate override there must not read as a changed default here.
    // CONTROL: in backend/CMakeLists.txt, the cache default made "lib/systemd/user" -> this line
    //       red, alone: this tree keeps its cached value, so every other check still passes.
    //       Observed 2026-09-24 via control_run.sh; reverted.
    std::string defaultUnitDir;
    {
        std::istringstream cache(readFile(build / "CMakeCache.txt"));
        constexpr std::string_view kKey = "WALLPAPERX_SYSTEMD_USER_UNIT_DIR:STRING=";
        for (std::string line; std::getline(cache, line);) {
            if (line.rfind(kKey, 0) == 0) defaultUnitDir = line.substr(kKey.size());
        }
    }
    check(defaultUnitDir == "share/systemd/user", "given no override, the unit directory is share/systemd/user");
    if (defaultUnitDir != "share/systemd/user") std::fprintf(stderr, "  it is \"%s\"\n", defaultUnitDir.c_str());

    // GOAL: S14.6 -- an install whose prefix would put the shaders somewhere the daemon's
    //       compiled-in path does not lead is refused before anything is copied, naming where the
    //       shaders would have gone; otherwise it installs a daemon that cannot start.
    // WHY-THIS-SHAPE: "the install fails" alone is satisfied by this unbuilt tree whatever the
    //       check does, so the error must name the would-be shader directory, and nothing at all
    //       may exist under the prefix afterwards.
    // CONTROL: in backend/CMakeLists.txt, the shader-path comparison made `if(FALSE)` -> both lines
    //       below red, alone. Observed 2026-09-24 via control_run.sh; reverted.
    const fs::path other = root / "absbin-other";
    const Install moved = install(other, {}, {}, build.string());
    if (!moved.ran) return;
    const bool named = !moved.ok && errorNames(moved.output, (other / "share" / "wallpaperx" / "shaders").string());
    check(named, "absolute bin directory, another prefix: the install is refused, naming where the shaders would go");
    if (!named) std::fprintf(stderr, "  install output:\n%s\n", moved.output.c_str());
    std::error_code ec;
    check(!fs::exists(other, ec), "absolute bin directory, another prefix: nothing is installed under it");

    // GOAL: S14.6 -- the same build installed with its configured prefix is not refused: the
    //       check refuses only the installs it exists for, and the unit names the absolute bin
    //       directory.
    // WHY-THIS-SHAPE: the unit being written is the evidence the check passed, since it is written
    //       after the check; the install still fails afterwards, on the binary this tree never built.
    // CONTROL: in backend/CMakeLists.txt, the install-time relative path computed from
    //       "${CMAKE_INSTALL_PREFIX}/bin" instead of the bin directory actually used -- a check that
    //       assumes a relative bin directory -> this line red, alone. Observed 2026-09-24 via
    //       control_run.sh; reverted.
    const Install kept = install(configured, {}, {}, build.string());
    if (!kept.ran) return;
    // This scratch configure's unit directory, not this build tree's.
    const fs::path unit = configured / defaultUnitDir / kUnitName;
    const bool accepted = fs::is_regular_file(unit);
    check(accepted, "absolute bin directory, the configured prefix: the shader-path check lets it through");
    if (!accepted) {
        std::fprintf(stderr, "  install output:\n%s\n", kept.output.c_str());
        return;
    }
    execStartNames(readFile(unit), bindir / "wallpaperx-daemon", "absolute bin directory");
}

// The port the daemon reports it bound, from GetStatus's config_server detail; -1 if none.
int boundPort(const StatusReply& status) {
    const nlohmann::json& json = status.json();
    if (!json.contains("config_server") || !json["config_server"].is_object()) return -1;
    const std::string detail = json["config_server"].value("detail", std::string());
    const size_t colon = detail.rfind(':');
    if (colon == std::string::npos) return -1;
    try {
        return std::stoi(detail.substr(colon + 1));
    } catch (const std::exception&) {
        return -1;
    }
}

// S14.6: the installed binary, run with no override, finds the installed shaders and draws.
void checkInstalledDaemon(NameWatcher& names, const fs::path& root, const fs::path& prefix) {
    constexpr const char* kGoals = "S14.6's run of the installed daemon";
    const fs::path home = root / "home";
    const std::string blue = wallpaperx::testing::writeSolidPng((root / "blue.png").string(), 8, 8, {0, 0, 255, 255});
    g_installedDaemon = binaryPath(prefix).string();

    FrameReadyLog frames;
    auto proxy = wallpaperx::testing::subscribeFrameReady(frames);

    SpawnedDaemon daemon({"WALLPAPERX_VALIDATION=0", "HOME=" + home.string(),
                          "XDG_CONFIG_HOME=" + (home / ".config").string(),
                          "XDG_STATE_HOME=" + (home / ".local" / "state").string()},
                         SpawnedDaemon::Stdout::Captured, {}, 0, {"--config-port", "0"});
    if (!names.waitFor(true)) {
        check(false, "the installed daemon starts and acquires the bus name");
        return;
    }

    // GOAL: S14.6 -- those shaders are enough to draw: a still, and a video, whose conversion
    //       shaders are separate files.
    // WHY-THIS-SHAPE: the non-throwing SetWallpaper, graded here: a throw would unwind past every
    //       later install phase into main's catch.
    // CONTROL: in backend/CMakeLists.txt, via control_run.sh, reverted: no shaders installed -> both
    //       lines red; only fullscreen*.spv installed -> only the video line. By hand here, with a
    //       FAIL marker added to the last phase, reverted: the type made "bogus" -> this line red
    //       and the marker printed; the same call made the throwing setWallpaper -> "plain
    //       install: unexpected exception" and the marker printed. Observed 2026-09-24.
    const bool stillSent = wallpaperx::testing::trySetWallpaper(*proxy, "static", blue, 0, std::chrono::seconds(10));
    check(stillSent && frames.wait(0, 1), "the installed daemon draws a still with the installed shaders");
    const std::string clip = std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_bands.mp4";
    std::error_code ec;
    if (!fs::exists(clip, ec)) {
        notVerified(kGoals, "the video fixture " + clip + " was not generated, so the video shaders were not drawn with");
    } else {
        const bool videoSent =
            wallpaperx::testing::trySetWallpaper(*proxy, "video", clip, 1, std::chrono::seconds(10));
        check(videoSent && frames.waitAtLeast(1, 2), "the installed daemon plays a video with the installed shaders");
    }
    const StatusReply status = getStatus(*proxy, std::chrono::seconds(10));
    for (const char* monitor : {"0", "1"}) {
        const nlohmann::json* entry = wallpaperStatus(status, monitor);
        if (entry != nullptr && entry->contains("last_error")) {
            std::fprintf(stderr, "  monitor %s: %s\n", monitor, entry->value("last_error", std::string()).c_str());
        }
    }

    // GOAL: S14.6 -- the installed daemon, with WALLPAPERX_SHADER_DIR unset, loads the shaders
    //       installed under its own prefix -- not the build tree's, which exist on this machine
    //       and would hide an install that left its shaders behind.
    // WHY-THIS-SHAPE: the directory is read from /health, where the daemon reports the one it
    //       resolved; a successful render alone cannot tell the two directories apart while the
    //       build tree still exists. Asked after the renders: the directory is resolved by the
    //       first one, and /health says pending until then.
    // CONTROL: defaultShaderDir() in backend/daemon/wallpaper_manager.cpp made to return the build
    //       tree's directory whatever the executable's location -> this line red, alone: both
    //       renders stayed green on the build tree's shaders. Observed 2026-09-24; reverted.
    const int port = boundPort(status);
    if (port <= 0) {
        check(false, "the installed daemon reports the config port it bound");
    } else {
        httplib::Client client = wallpaperx::testing::makeClient(port);
        const wallpaperx::testing::HttpReply health = wallpaperx::testing::get(client, "/health");
        const nlohmann::json shaders = health.hasJsonObject() ? health.json.value("checks", nlohmann::json::object())
                                                                     .value("shaders", nlohmann::json::object())
                                                               : nlohmann::json::object();
        const std::string detail = shaders.value("detail", std::string());
        const fs::path expected = shaderDir(prefix);
        const bool installedOnes = shaders.value("status", std::string()) == "ok" && fs::equivalent(detail, expected, ec);
        check(installedOnes, "/health reports the installed daemon using the installed shader directory");
        if (!installedOnes) {
            std::fprintf(stderr, "  /health shaders: %s\n  expected:        %s\n", shaders.dump().c_str(),
                         expected.c_str());
        }
    }

    const int exitCode = daemon.shutdown(SIGTERM);
    if (daemon.sanitizerReported()) {
        // Its exit status is then the sanitizer's, and grading it would blame the stop path.
        check(false, "the installed daemon exits 0 on SIGTERM -- not graded: the daemon's sanitizer reported "
                     "warnings, and the exit status is the sanitizer's");
    } else {
        check(exitCode == 0, "the installed daemon exits 0 on SIGTERM");
    }
    if (!names.waitFor(false)) harnessFailure("the bus name is still owned after the installed daemon was reaped");
}

// S14.6 for an ordinary prefix: the files, the unit, what the install leaves in the build tree and
// its manifest, and the installed daemon run.
void checkPlainInstall(const fs::path& root) {
    const fs::path prefix = root / "prefix";
    const auto copiesBefore = unitCopiesInBuildTree();
    const Install installed = install(prefix);
    if (!installed.ran) return;
    check(installed.ok, "cmake --install --component daemon into a scratch prefix succeeds");
    if (!installed.ok) {
        std::fprintf(stderr, "%s\n", installed.output.c_str());
        return;
    }

    // GOAL: S14.6 -- the install writes the unit only to its destination and leaves no copy of it
    //       in the build tree, which may not be the installing user's to write, and which two
    //       installs to different prefixes would otherwise both rewrite.
    // WHY-THIS-SHAPE: every file named like the unit anywhere in the build tree, before and after.
    //       Not a diff of the whole tree: other ctest entries run in this tree at the same time,
    //       so that would grade their writes. For the same reason the walk skips what vanishes
    //       under it, where recursive_directory_iterator stops at the first ENOENT. The walk must
    //       have seen files at all, and a root it cannot open is a harness fault.
    // CONTROL: in backend/CMakeLists.txt, the unit also written to
    //       ${CMAKE_CURRENT_BINARY_DIR}/packaging/wallpaperx-daemon.service, as the install did
    //       before -> this line red, alone. Re-observed 2026-09-24 via control_run.sh with this walk;
    //       reverted. By hand, the same day: four processes creating and deleting directory trees
    //       inside the build tree during 30 runs -> 26 runs a "could not be walked" harness fault
    //       with recursive_directory_iterator, 0 with this walk; the after-walk pointed at a root
    //       that does not exist -> "FAIL (harness): the build tree ... could not be walked".
    const auto copiesAfter = unitCopiesInBuildTree();
    if (!copiesBefore || !copiesAfter) {
        harnessFailure(std::string("the build tree ") + WALLPAPERX_BUILD_DIR + " could not be walked");
    } else {
        check(*copiesBefore == *copiesAfter, "the install leaves no copy of the unit in the build tree");
        for (const auto& [path, when] : *copiesAfter) {
            if (copiesBefore->count(path) == 0 || copiesBefore->at(path) != when) {
                std::fprintf(stderr, "  written: %s\n", path.c_str());
            }
        }
    }
    checkManifest(prefix, prefix, "plain prefix");

    // CONTROL: in backend/CMakeLists.txt, via control_run.sh, reverted, both observed 2026-09-24.
    //       The daemon installed to sbin -> this line, the verify line, the installed-daemon start,
    //       the DESTDIR binary line and each odd prefix's binary line red. `_wx_unit_dir` set to
    //       lib/systemd/user -> the unit line below and every other "writes the unit" / "lands
    //       under it" line red, with both manifest lines and the absolute-bin-directory accept line.
    const fs::path binary = binaryPath(prefix);
    check(fs::is_regular_file(binary) && ::access(binary.c_str(), X_OK) == 0,
          "the install puts an executable wallpaperx-daemon in <prefix>/" WALLPAPERX_TEST_BINDIR);
    checkShaders(shaderDir(prefix));
    const fs::path unit = unitPath(prefix);
    const bool haveUnit = fs::is_regular_file(unit);
    check(haveUnit, "the install puts the systemd user unit in <prefix>/" WALLPAPERX_TEST_UNIT_DIR);
    if (haveUnit) {
        checkUnit(unit, binary);
        if (kHaveAnalyze) checkVerify(unit);
    }
    NameWatcher names;
    checkInstalledDaemon(names, root, prefix);
}

// One phase, with its own catch: an unexpected throw is reported against it, and the phases after
// it still run.
void runPhase(const char* name, const std::function<void()>& phase) {
    try {
        phase();
    } catch (const std::exception& e) {
        check(false, (std::string(name) + ": unexpected exception: " + e.what()).c_str());
    }
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "daemon_install_test";
    // What the goal is about: the installed daemon given no override. Before any thread exists.
    ::unsetenv("WALLPAPERX_SHADER_DIR");

    // S14.9, graded in name_watcher.h: before anything is installed or spawned.
    if (const std::optional<int> refused = wallpaperx::testing::refuseUnlessPrivateBus()) return *refused;

    // Every check installs to prefixes of its own, and an absolute location would not follow them.
    for (const fs::path configured : {fs::path(WALLPAPERX_TEST_UNIT_DIR), fs::path(WALLPAPERX_TEST_BINDIR),
                                      fs::path(WALLPAPERX_TEST_SHADER_DIR)}) {
        if (configured.is_absolute()) {
            notVerified("every goal in this file", "this tree was configured to install to " + configured.string() +
                                                       ", an absolute path, so a scratch prefix cannot hold the install");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
    }

    if (!kHaveAnalyze) {
        notVerified("S14.6's and S14.8's systemd-analyze halves",
                    "systemd-analyze was not found when this build was configured");
    }

    try {
        ScratchDir root("install");
        if (!root.ok()) {
            notVerified("every goal in this file", "a scratch directory could not be created");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        runPhase("plain install", [&] { checkPlainInstall(root.path()); });
        runPhase("DESTDIR", [&] { checkDestdir(root.path()); });
        runPhase("umask", [&] { checkUnitModeUnderUmask(root.path()); });
        runPhase("odd prefixes", [&] { checkOddPrefixes(root.path()); });
        runPhase("refused prefixes", [&] { checkRefusedPrefixes(root.path()); });
        runPhase("absolute bin directory", [&] { checkAbsoluteBindir(root.path()); });
    } catch (const std::exception& e) {
        check(false, (std::string("unexpected exception: ") + e.what()).c_str());
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test's harness, not in the install")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
