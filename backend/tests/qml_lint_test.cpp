// SPDX-License-Identifier: GPL-2.0-or-later

// The QML the Plasma wallpaper package ships -- backend/kde/kpackage/contents/ui/main.qml and
// contents/ui/config.qml -- put through Qt's QML linter.
//
// What this is for. Plasma loads these files; nothing else in this tree ever compiles them, so
// without this entry they are unchecked text. `qmllint` is the only tool available here that
// resolves org.kde.plasma.plasmoid and can say that a property does not exist.
//
// What the tool does and does not establish, measured on qmllint 6.11.1 rather than assumed,
// because the difference decides how main.qml has to be written to be checkable at all:
//
//   - a declarative binding to a property the type does not have (`nosuchproperty: 1`) is a
//     `missing-property` warning;
//   - a *read* of a property the file does not declare (`root.wallpaperPat` inside a binding
//     or a function) is a `missing-property` warning, with a did-you-mean;
//   - a *write* to a misspelled property inside a JavaScript function body is NOT reported.
//     Copying main.qml and changing `root.answered = true` to `root.answerd = true` gives
//     `{"success":true,"warnings":[]}` and this entry stays green. QML accepts the write at
//     load time too -- it lands on the wrapper, not on the property -- so that typo is
//     invisible from here and from the desktop both.
//
// So the tool checks names in bindings and in reads, and main.qml is shaped to put every
// displayed fact in exactly those places: three properties written from JavaScript, and
// `screenName`, `wallpaperPath` and `screenState` derived from them in declarative bindings
// that this entry does check. That shape is what the check below is worth; a future edit that
// moves a displayed fact back into a JavaScript assignment moves it out of coverage without
// turning anything red, and nothing here can detect that.
//
// It grades one half of GOALS.md's S10.9 (and S10.12 for the package's other QML file). The
// other half -- that the root element is WallpaperItem from org.kde.plasma.plasmoid -- is
// graded by the S9.4 check in backend/tests/kpackage_metadata_test.cpp, which reads the file
// as text and so answers on a host with no Qt at all. That is deliberate and is why this file
// does not repeat it: the two halves need different preconditions, ctest's SKIP_RETURN_CODE is
// all-or-nothing per entry, and a single binary holding both would report "Skipped" on a
// Qt-less host over a root-element verdict that had actually run. Two ctest entries, one per
// precondition, is the only shape that reports each half honestly.
//
// That the two halves are genuinely different properties was checked rather than assumed:
// with main.qml's root changed from WallpaperItem to Item, qmllint -W0 still exits 0 (the now
// unused import is only an Info-level finding), and kpackage_metadata's S9.4 is what goes red.
// Neither entry subsumes the other.
//
// The trap this file is mostly built around. `qmllint` reports a module it cannot import the
// same way it reports a real defect -- a warning, and in --json mode `success: false`. A host
// with no Plasma installed would therefore look exactly like a broken package. Worse is the
// silent direction: if the linter ever stopped resolving a module's *types* while still
// importing it, every property check would quietly become unanswerable and this file would
// report green forever. So the linter is put through a self-test on generated probe files --
// one pair per file under lint, importing what that file imports -- before its verdict is
// believed at all, and the verdict is withheld, as a skip and never as a pass, if that
// self-test does not discriminate.
//
// Registered outside the if(WALLPAPERX_BUILD_KDE_PLUGIN) guard for the reason
// kpackage_metadata is: it links nlohmann_json alone, needs no Qt to *build*, and the
// sanitizer tree and the hosted runner both configure with that option OFF. It carries
// SKIP_RETURN_CODE 77 because a host with no qmllint has not established S10.9 and must not
// report that it has.
//
// This test writes only inside a directory it creates under $TMPDIR and removes; it reads the
// package's QML and never writes it.

#include "support/forked_child.h"
#include "tests/client/scratch_dir.h"
#include "tests/file_presence.h"
#include "tests/test_report.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// The pass / fail / skip contract, and the harness-fault channel that names a defect in this
// file or in the tool it drives rather than in the QML it reads, both live in
// tests/test_report.h. The present/missing/unknowable classification lives in
// tests/file_presence.h, shared with kpackage_metadata_test, which grades the same package.
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::Presence;
using wallpaperx::testing::presenceOf;

// The property the oracle self-test asks the linter to reject. Long and project-specific so
// that no future version of any of these types can acquire it and turn the self-test green by
// accident -- which would leave the linter's blindness undetected.
constexpr std::string_view kProbeUnknownProperty = "wallpaperxQmllintProbeNoSuchProperty";

// qmllint's warning category for anything to do with resolving an import. It covers both
// "this module is not installed" and "this file could not be opened", which is why file
// presence is established separately below rather than inferred from this id.
constexpr std::string_view kImportCategory = "import";

// The category a misspelled property produces. The self-test requires this exact id, not
// merely "some warning": a linter that had lost type resolution still emits `import`
// warnings, and accepting any warning would let that state pass for a working oracle.
constexpr std::string_view kMissingPropertyCategory = "missing-property";

// A single lint of a single small file takes ~50 ms on this host (measured, both package
// files, qmllint 6.11.1). The ceiling is three orders of magnitude above that because it is
// there to catch a wedged process, not a slow one: a bound close to the measurement would
// turn a loaded host red, which is an assertion on the scheduler.
constexpr int kLintTimeoutSeconds = 120;

// --------------------------------------------------------------------------- //
// Running qmllint
// --------------------------------------------------------------------------- //

struct ProcessResult {
    bool execFailed = false;   // the binary could not be started at all
    int execErrno = 0;
    bool exitedNormally = false;
    int exitCode = -1;
    int signalNumber = 0;
    bool timedOut = false;  // it was still running at the deadline and was killed
    std::string out;
    std::string harnessProblem;  // non-empty: this process could not run it, whatever qmllint is
};

// fork/execv with the child's stdout on a pipe. Not popen(): that routes through /bin/sh, so
// a qmllint path or a package path containing a space, a quote or a `$` would be re-split by
// the shell -- and the paths here are a CMake cache entry and a source directory, neither of
// which this file gets to constrain.
//
// Exec failure is reported through a second, close-on-exec pipe rather than through the exit
// code alone: an exit code from the child is indistinguishable from qmllint itself choosing
// the same number, and the two must not be confused -- one is a host without the tool (a
// skip), the other is a tool behaving in a way this file does not model (red).
//
// GOAL: a wedged linter is named and killed, never waited on until ctest's own timeout fires.
//       False if this function can block forever, which is what X.1 forbids: the run would
//       report "qml_lint timed out" with no line saying the linter was the thing that hung,
//       and would leave the child behind holding whatever it holds.
// WHY-THIS-SHAPE: poll() against one deadline computed before the drain, rather than the
//       simpler blocking read(). A blocking read cannot be interrupted from here, and giving
//       each read its own fresh timeout would restart the budget on every chunk, so a child
//       that dribbles one byte per second would never expire. The deadline is absolute for
//       that reason. The reap is polled against that same deadline rather than blocking:
//       end-of-file on the capture pipe is not proof the child exited -- a linter that closes
//       or redirects its stdout and then wedges produces exactly that -- so a blocking waitpid
//       would hang here on the ordinary, un-killed path. SIGKILL is sent on every path that
//       expires, and only the wait that follows a SIGKILL is allowed to block.
// CONTROL: three, all with kLintTimeoutSeconds cut to 3 for the run and
//       WALLPAPERX_QMLLINT_EXECUTABLE pointed at a stand-in.
//       (1) A script that sleeps ten minutes with its stdout still open -> `FAIL (harness):
//       ... did not finish within 3s on ... and was killed`, exit 1, and nothing left running.
//       (2) The end-of-file variant the blocking reap could not survive: a script that
//       redirects its own stdout to a file and then sleeps, so the capture pipe reports
//       end-of-file while the child lives. With the polled reap, the same named harness fault
//       at the deadline; with the reap written as a plain blocking waitpid, the run never
//       returned -- killed externally at 60s, and it left the sleeper orphaned.
//       (3) Both `::kill` sites deleted, stand-in (1): the run never returned either, again
//       leaving the sleeper behind. Deleting only the first is *not* sufficient to hang it --
//       the reap's own kill covers that -- so the first exists to bound the exec-errno read
//       for a child wedged before execv, not to make the reap safe. All reverted.
ProcessResult runCapturingStdout(const std::vector<std::string>& argv) {
    ProcessResult result;
    if (argv.empty()) {
        result.harnessProblem = "asked to run an empty command line";
        return result;
    }

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::pipe(outPipe) != 0) {
        result.harnessProblem = std::string("pipe() failed: ") + std::strerror(errno);
        return result;
    }
    if (::pipe2(errPipe, O_CLOEXEC) != 0) {
        result.harnessProblem = std::string("pipe2() failed: ") + std::strerror(errno);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        return result;
    }

    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    // argv is alive for the whole call and execv does not write through these, so casting
    // away const is safe here; the POSIX signature simply predates const.
    for (const std::string& a : argv) raw.push_back(const_cast<char*>(a.c_str()));
    raw.push_back(nullptr);

    const pid_t parentBeforeFork = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        result.harnessProblem = std::string("fork() failed: ") + std::strerror(errno);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        return result;
    }
    if (pid == 0) {
        ::close(outPipe[0]);
        ::close(errPipe[0]);
        // X.8: this child must not outlive a parent that dies without unwinding -- a
        // sanitizer report or an abort runs no destructor, and a surviving qmllint would
        // hold whatever it had open. Same helper as the daemon and renderer fork sites.
        if (!wallpaperx::testing::armAgainstParentDeath(parentBeforeFork)) {
            ::_exit(wallpaperx::testing::kChildArmFailedExit);
        }
        // Every process this harness starts is one it kills, or breaks on purpose in a control
        // run; their cores are the host's journal noise rather than evidence.
        wallpaperx::testing::suppressCoreDump();
        // Its own process group, so the timeout below can kill anything it spawned rather
        // than only the process this test can see. Also called in the parent, since either
        // side may run first and the second call simply fails.
        ::setpgid(0, 0);
        // Only stdout is redirected. qmllint's stderr is left attached to this test's, so
        // anything it says outside its JSON reaches the ctest log instead of vanishing.
        if (::dup2(outPipe[1], STDOUT_FILENO) < 0) ::_exit(wallpaperx::testing::kChildDup2FailedExit);
        ::close(outPipe[1]);
        ::execv(raw[0], raw.data());
        const int e = errno;
        const ssize_t ignored = ::write(errPipe[1], &e, sizeof(e));
        static_cast<void>(ignored);
        ::_exit(wallpaperx::testing::kChildExecFailedExit);
    }

    ::close(outPipe[1]);
    ::close(errPipe[1]);
    static_cast<void>(::setpgid(pid, pid));

    // EINTR is retried on every wait rather than ending the loop. Stopping short would hand
    // the parser a truncated JSON document, and a truncated document is not distinguishable
    // from a tool this file does not understand -- so a signal arriving mid-read would be
    // reported as "could not interpret qmllint", quietly, on some runs and not others.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kLintTimeoutSeconds);
    char buffer[4096];
    std::string drainProblem;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timedOut = true;
            break;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{};
        pfd.fd = outPipe[0];
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, static_cast<int>(left));
        if (ready < 0) {
            if (errno == EINTR) continue;
            drainProblem = std::string("poll() on qmllint's output failed: ") + std::strerror(errno);
            break;
        }
        if (ready == 0) {
            result.timedOut = true;
            break;
        }
        const ssize_t n = ::read(outPipe[0], buffer, sizeof(buffer));
        if (n > 0) {
            result.out.append(buffer, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            // Named, not merged into the end-of-file case. A truncated capture read as a
            // complete one reaches the JSON reader, which reports "a qmllint predating Qt 6's
            // --json answers exactly like this" -- an accusation against the host's Qt for
            // what is a pipe error in this process.
            drainProblem = std::string("read() on qmllint's output failed: ") + std::strerror(errno);
            break;
        }
    }
    // Killed here, before the exec-errno read below, so that read cannot block on a child that
    // wedged before reaching execv. The group first and the process second: a linter that had
    // spawned a helper would otherwise leave that helper running with the pipe still open.
    if (result.timedOut || !drainProblem.empty()) {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
    }
    ::close(outPipe[0]);

    // Bounded in practice rather than by a deadline: errPipe[1] is close-on-exec and the
    // parent closed its copy, so exec, _exit, or the SIGKILL above all produce end-of-file.
    int execErr = 0;
    ssize_t got = 0;
    while ((got = ::read(errPipe[0], &execErr, sizeof(execErr))) < 0 && errno == EINTR) {
    }
    const int errPipeErrno = got < 0 ? errno : 0;
    ::close(errPipe[0]);
    if (got == static_cast<ssize_t>(sizeof(execErr))) {
        result.execFailed = true;
        result.execErrno = execErr;
    } else if (got < 0 && drainProblem.empty()) {
        // Otherwise indistinguishable from "the child never wrote an errno", which would
        // report a failed execv as a normal qmllint run.
        drainProblem = std::string("read() on the exec-error pipe failed: ") + std::strerror(errPipeErrno);
    }

    // Polled against the same deadline, not blocking. End-of-file on the capture pipe means
    // every copy of its write end is closed, which a child that redirects its own stdout and
    // then wedges also achieves -- so the un-killed path needs a bound of its own.
    int status = 0;
    bool reaped = false;
    for (;;) {
        // EINTR is retried rather than treated as the child being gone: giving up here would
        // leave a zombie and report a signal the child never received.
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            reaped = true;
            break;
        }
        if (done < 0) {
            if (errno == EINTR) continue;
            result.harnessProblem = std::string("waitpid() failed: ") + std::strerror(errno);
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = true;
            break;
        }
        const timespec nap{0, 10 * 1000 * 1000};
        ::nanosleep(&nap, nullptr);
    }
    if (!reaped) {
        // Reachable only with result.timedOut set, so the wait below always follows a SIGKILL.
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                result.harnessProblem = std::string("waitpid() failed: ") + std::strerror(errno);
                return result;
            }
        }
    }
    if (WIFEXITED(status)) {
        result.exitedNormally = true;
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signalNumber = WTERMSIG(status);
    }
    if (!drainProblem.empty() && result.harnessProblem.empty()) result.harnessProblem = drainProblem;
    // A child that died in the preamble -- arming, dup2 -- never ran qmllint at all, and its
    // exit code says so. Named here rather than left to be read as a verdict from a program
    // that never started. The exec case is carried by the errno pipe above instead, which is
    // why it is not in this map.
    if (result.exitedNormally && !result.execFailed && !result.timedOut) {
        if (const char* preamble = wallpaperx::testing::forkPreambleFault(result.exitCode);
            preamble != nullptr && result.exitCode != wallpaperx::testing::kChildExecFailedExit) {
            result.harnessProblem = preamble;
        }
    }
    return result;
}

// --------------------------------------------------------------------------- //
// Reading qmllint's machine-readable report
// --------------------------------------------------------------------------- //

struct LintWarning {
    std::string id;
    std::string type;  // "warning", "info", ...
    std::string message;
    int line = 0;
    int column = 0;

    // `-W0` is `--max-warnings 0`, and Info-level findings do not count against it: the
    // sibling config.qml reports `Unused import [unused-imports]` as Info and still exits 0.
    // So severity, not presence, is what the goal is stated in terms of.
    bool countsAgainstMaxWarnings() const { return type != "info"; }
};

struct LintReport {
    bool usable = false;         // the fields below were read; otherwise `problem` says why not
    bool toolSaidSuccess = false;  // qmllint's own verdict for the file
    std::vector<LintWarning> warnings;
    std::string problem;
};

std::string describe(const LintWarning& w) {
    return "[" + w.id + "/" + w.type + "] " + std::to_string(w.line) + ":" + std::to_string(w.column) + " " +
           w.message;
}

// Parses `qmllint --json -` output. Every field is checked for presence and type rather than
// read with value(): a report shape this file does not understand must arrive as a named
// "could not interpret" rather than as an exception, or as zero warnings -- and zero warnings
// is the silent direction, since it reads as a clean file.
LintReport parseLintJson(const std::string& text, const std::filesystem::path& expectedFile) {
    LintReport report;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        // The Qt 5 hint is here rather than left to the reader: a qmllint predating --json
        // writes usage text to stderr and nothing to stdout, which arrives as an empty
        // document, and that is the likeliest cause of this branch on a host that has some
        // qmllint. It belongs on this branch alone -- a tool that answered in JSON this file
        // could not use is a different problem, and the hint would be a false lead there.
        report.problem = std::string("its --json output did not parse (a qmllint predating Qt 6's --json answers "
                                     "exactly like this): ") +
                         e.what();
        return report;
    }
    if (!doc.is_object() || !doc.contains("files") || !doc.at("files").is_array()) {
        report.problem = "its --json output has no \"files\" array";
        return report;
    }
    const nlohmann::json& files = doc.at("files");
    if (files.size() != 1) {
        report.problem = "its --json output described " + std::to_string(files.size()) +
                         " files for a single-file run; this reader expects exactly one";
        return report;
    }
    const nlohmann::json& file = files.front();
    if (!file.is_object()) {
        report.problem = "its --json output's \"files\" entry is not an object";
        return report;
    }
    // Required, not opportunistic. It is the only field that establishes what the report is
    // about, so treating its absence as "no comparison to make" would grade whatever file the
    // tool actually read as though it were this one -- green for the wrong reason, and silently.
    if (!file.contains("filename") || !file.at("filename").is_string()) {
        report.problem = "its --json output has no string \"filename\" for the file, so nothing establishes which "
                         "file it read";
        return report;
    }
    const std::filesystem::path reported = file.at("filename").get<std::string>();
    std::error_code absEc;
    const std::filesystem::path expected = std::filesystem::absolute(expectedFile, absEc);
    if (reported.lexically_normal() != (absEc ? expectedFile : expected).lexically_normal()) {
        report.problem = "it reported on \"" + reported.string() + "\" when asked about \"" +
                         expectedFile.string() + "\"";
        return report;
    }
    if (!file.contains("success") || !file.at("success").is_boolean()) {
        report.problem = "its --json output has no boolean \"success\" for the file";
        return report;
    }
    report.toolSaidSuccess = file.at("success").get<bool>();
    if (!file.contains("warnings") || !file.at("warnings").is_array()) {
        report.problem = "its --json output has no \"warnings\" array for the file";
        return report;
    }
    for (const nlohmann::json& w : file.at("warnings")) {
        if (!w.is_object() || !w.contains("id") || !w.at("id").is_string() || !w.contains("type") ||
            !w.at("type").is_string()) {
            report.problem = "one of its warnings has no string \"id\" and \"type\"; this reader classifies by "
                             "category, so it cannot tell a missing module from a real defect without them";
            return report;
        }
        LintWarning parsed;
        parsed.id = w.at("id").get<std::string>();
        parsed.type = w.at("type").get<std::string>();
        if (w.contains("message") && w.at("message").is_string()) parsed.message = w.at("message").get<std::string>();
        if (w.contains("line") && w.at("line").is_number_integer()) parsed.line = w.at("line").get<int>();
        if (w.contains("column") && w.at("column").is_number_integer()) parsed.column = w.at("column").get<int>();
        report.warnings.push_back(std::move(parsed));
    }
    report.usable = true;
    return report;
}

std::string g_qmllint;

// One qmllint run over one file. Distinguishes three outcomes the callers must not merge: the
// tool could not be run (a host condition), the tool ran but said something unreadable (this
// file is out of date with it), and the tool answered.
//
// GOAL: every file this entry grades -- the generated probes and the package's QML alike -- is
//       linted under the same rules, so the probes' proof applies to the file they vouch for.
//       False if a `.qmllint.ini` can change the answer for one and not the other. This closes
//       one of qmllint's two suppression channels; the other, a directive comment inside the
//       graded file, is closed by checkNoLintDirectives below, and the transfer rests on both.
// WHY-THIS-SHAPE: `--ignore-settings` on every run, rather than trusting that no settings file
//       exists. qmllint reads a `.qmllint.ini` from the linted file's own directory or any
//       ancestor; the probes are written under $TMPDIR and the package's QML lives in the
//       source tree, so the two have different ancestor chains and a settings file at or above
//       the repository would silence the real check while leaving the oracle's proof intact --
//       a green run establishing nothing. Writing the probes beside main.qml would equalise
//       them too, but this test may not write into the source tree.
// CONTROL: a copy of backend/ under /tmp with `[Warnings] MissingProperty=disable` at its root
//       and a bogus property added to the copied main.qml, built with
//       WALLPAPERX_BACKEND_SOURCE_DIR pointing at it. With `--ignore-settings` removed the run
//       passed green over the bogus property; with it restored the run reported `FAIL:
//       backend/kde/kpackage/contents/ui/main.qml passes qmllint -W0`. The repository itself
//       was not touched.
LintReport lintFile(const std::filesystem::path& file, std::string& hostProblem) {
    const ProcessResult run = runCapturingStdout({g_qmllint, "--ignore-settings", "--json", "-", file.string()});
    LintReport report;
    if (run.timedOut) {
        // Deliberately a harness fault and not a skip. Every other failure path here becomes
        // notVerified() and therefore exit 77, which ctest grades as not-a-failure -- and a
        // hang graded as not-a-failure is the state X.1 exists to forbid.
        harnessFailure(g_qmllint + " did not finish within " + std::to_string(kLintTimeoutSeconds) + "s on " +
                       file.string() + " and was killed; the fault is the linter or this harness, not the package");
        report.problem = "did not finish within " + std::to_string(kLintTimeoutSeconds) + "s and was killed";
        hostProblem = report.problem;
        return report;
    }
    if (!run.harnessProblem.empty()) {
        report.problem = run.harnessProblem;
        hostProblem = "this process could not run " + g_qmllint + ": " + run.harnessProblem;
        return report;
    }
    if (run.execFailed) {
        report.problem = "could not be started";
        hostProblem = g_qmllint + " was found when this build was configured but cannot be run now: " +
                      std::strerror(run.execErrno);
        return report;
    }
    if (!run.exitedNormally) {
        report.problem = "died on signal " + std::to_string(run.signalNumber);
        hostProblem = g_qmllint + " died on signal " + std::to_string(run.signalNumber);
        return report;
    }
    report = parseLintJson(run.out, file);
    if (!report.usable) {
        // Exit code is deliberately not the oracle. In --json mode qmllint exits 0 even for a
        // file with real warnings (measured: a missing-property run exits 0), and 255 only
        // for a parse error -- so grading on the exit status would pass a file full of
        // defects. The JSON body is the only reliable answer, and being unable to read it is
        // reported as such rather than papered over with the status.
        report.problem += " (qmllint exited " + std::to_string(run.exitCode) + ")";
    }
    return report;
}

// --------------------------------------------------------------------------- //
// Phases
// --------------------------------------------------------------------------- //

// One file the package ships and this entry lints, with the material its oracle needs.
//
// The imports are written here rather than read out of the file. Reading them would need a
// QML parser in a binary that deliberately has none, and would buy nothing the goal wants: the
// probe exists to ask whether *this host* can resolve what the package needs, so a stale list
// makes a host check stale, not a package check vacuous. The direction that matters is safe --
// if the file gains an import this list lacks and the host cannot resolve it, the run reports
// a failure naming the module rather than a silent pass. main.qml's own import of the plasmoid
// module is checked against the file's text by kpackage_metadata's S9.4.
// A type the probe declares as its root, and the module it comes from. One per module, because
// a probe rooted on a single type only ever proves the linter reads that type's module: a host
// that imports QtQuick.Controls but has lost its type information still answers correctly about
// an Item from QtQuick, and config.qml would be announced clean on the strength of it.
struct ProbeType {
    const char* type;
    const char* module;  // named in the failure text, so a blind linter is attributed
};

constexpr ProbeType kMainProbeTypes[] = {
    {"WallpaperItem", "org.kde.plasma.plasmoid"},
    {"Rectangle", "QtQuick"},
};
constexpr ProbeType kConfigProbeTypes[] = {
    {"Item", "QtQuick"},
    {"Control", "QtQuick.Controls"},
    {"ColumnLayout", "QtQuick.Layouts"},
    {"Heading", "org.kde.kirigami"},
    {"DataSource", "org.kde.plasma.plasma5support"},
};

struct PackageQmlFile {
    const char* relative;          // under backend/kde/kpackage/
    const char* probeImports;      // one `import` line per module the file needs
    const ProbeType* probeTypes;   // one per module in probeImports
    std::size_t probeTypeCount;
    const char* probeTag;          // distinguishes this entry's probe files in the scratch dir
};

constexpr PackageQmlFile kPackageQml[] = {
    {"contents/ui/main.qml", "import QtQuick\nimport org.kde.plasma.plasmoid\n", kMainProbeTypes,
     std::size(kMainProbeTypes), "main"},
    {"contents/ui/config.qml",
     "import QtQuick\nimport QtQuick.Controls\nimport QtQuick.Layouts\n"
     "import org.kde.kirigami\nimport org.kde.plasma.plasma5support\n",
     kConfigProbeTypes, std::size(kConfigProbeTypes), "config"},
};

// "WallpaperItem (org.kde.plasma.plasmoid), Rectangle (QtQuick)" -- for the line a passing run
// prints, which is what tells a reader of a green log what was actually proven.
std::string describeProbeTypes(const PackageQmlFile& entry) {
    std::string out;
    for (std::size_t t = 0; t < entry.probeTypeCount; ++t) {
        if (!out.empty()) out += ", ";
        out += std::string(entry.probeTypes[t].type) + " (" + entry.probeTypes[t].module + ")";
    }
    return out;
}

std::filesystem::path g_packageDir;

// How many of the package's QML files this run issued a lint verdict for, and how many it
// reached the linter for and then had to withhold. Asserted against each other in main(), so
// a run that quietly stopped grading one of them is visible.
int g_filesGraded = 0;
int g_verdictsWithheld = 0;

bool writeFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    return out.good();
}

enum class OracleVerdict {
    Discriminates,   // proven to resolve this file's modules and to reject an unknown property
    ModuleMissing,   // this host cannot resolve one of them -- a skip
    Unusable,        // qmllint could not be run or understood -- a skip, with the reason
    Blind,           // it ran, resolved the modules, and did not notice the bogus property
};

// GOAL: S10.9/S10.12's lint half is answerable on this host at all -- that qmllint really
//       resolves the modules the file under lint imports and really rejects a property those
//       modules' types do not have. False if it imports them but cannot see into their types,
//       which is the state in which the package's QML lints green while establishing nothing.
// WHY-THIS-SHAPE: a generated clean/dirty probe pair per *module* the file imports, rather than
//       trusting a green run on the file itself. The obvious simpler alternative -- "qmllint
//       exists, so run it and believe the answer" -- is exactly the vacuous check this project
//       keeps producing: a linter that silently stopped resolving a module's types emits *no*
//       warning, so the hollowed-out oracle and a correct file are the same observation. Two
//       probes are needed, not one: the dirty probe proves it can say "not clean", the clean
//       probe proves it can say "clean", and only the pair rules out an oracle stuck on one
//       answer. One pair per module and not one per file, because a probe rooted on a single
//       type proves type resolution for that type's module alone -- a host that can import
//       QtQuick.Controls but has lost its type information still rejects the bogus property on
//       a QtQuick Item, and config.qml would be announced clean on the strength of a module it
//       does not use for that. Per file rather than once, because the two files import
//       different modules and a host that can answer about main.qml and not about config.qml
//       must grade the first and skip the second, not blame the package for either. The bogus
//       property is asserted by category (`missing-property`), not by matching the English
//       message and not merely as "some warning" -- a linter that has lost type resolution
//       still emits `import` warnings, which "some warning" would accept as a working oracle.
// CONTROL: stand-in linters replayed as WALLPAPERX_QMLLINT_EXECUTABLE against an unmodified
//       tree -- the input is patched, not this file's own constants, because the states being
//       controlled are properties of the host's qmllint. One replaying Qt 6.11.1's real
//       "Failed to import" body -> the module-missing skip naming the module, exit 77. One
//       answering `success:true, warnings:[]` to everything -- the blind linter, the whole
//       reason this phase exists -> `... accepted a property this test invented ...`, exit 77
//       with the package's verdict withheld, NOT a green pass. One warning on the bare clean
//       probe -> the "could not be attributed to the file" skip. One reporting a different
//       filename than it was asked about -> the filename-mismatch skip. One whose report omits
//       "filename" entirely -> `its --json output has no string "filename" ...`, exit 77;
//       with that field read only when present, the same stand-in produced a green pass over
//       two files it had never looked at. /bin/true (no output at all) -> `its --json output
//       did not parse`, exit 77. And, for the per-module claim above, one that answers
//       truthfully except on a QtQuick.Controls Control -> main.qml linted and announced clean
//       while config.qml alone was withheld, naming QtQuick.Controls, exit 77; with
//       config.qml's row cut back to a single Item probe the same stand-in passed green.
//       Nothing in the source tree was altered for any of them.
// COVERAGE-ASSERT: this is a self-test of the tool, not of production code -- no change to
//       backend/kde/kpackage/ can turn it red, and it is written so that its failure is a skip
//       or a harness fault, never an accusation against the package. Its job is to make the
//       next phase's green mean something, and it is what stops this file degrading into a
//       check that cannot fail.
OracleVerdict proveOracleDiscriminates(const PackageQmlFile& entry, const std::filesystem::path& scratch,
                                       std::string& reason) {
    std::size_t modulesProven = 0;
    for (std::size_t t = 0; t < entry.probeTypeCount; ++t) {
        const ProbeType& probe = entry.probeTypes[t];
        // No comments in the generated QML: qmllint parses a comment whose first word is
        // `qmllint` as a lint directive, and a malformed one is itself a warning.
        const std::string cleanSource = std::string(entry.probeImports) + probe.type + " {\n}\n";
        const std::string dirtySource = std::string(entry.probeImports) + probe.type + " {\n    " +
                                        std::string(kProbeUnknownProperty) + ": 1\n}\n";
        const std::string suffix = std::string(entry.probeTag) + "_" + probe.type + ".qml";
        const std::filesystem::path cleanPath = scratch / ("probe_clean_" + suffix);
        const std::filesystem::path dirtyPath = scratch / ("probe_unknown_property_" + suffix);
        if (!writeFile(cleanPath, cleanSource) || !writeFile(dirtyPath, dirtySource)) {
            reason = "the probe QML files for " + std::string(entry.relative) + " could not be written under " +
                     scratch.string();
            return OracleVerdict::Unusable;
        }

        std::string hostProblem;
        const LintReport clean = lintFile(cleanPath, hostProblem);
        if (!clean.usable) {
            reason = hostProblem.empty() ? (g_qmllint + ": " + clean.problem) : hostProblem;
            return OracleVerdict::Unusable;
        }

        // Asked of the probe, not of the package's file, and this is the load-bearing ordering.
        // qmllint reports a file it cannot *open* with the same `import` category as a module
        // it cannot *import*, so classifying the real file's own warnings would turn a deleted
        // main.qml into "this host has no Plasma" -- a skip, which ctest grades as
        // not-a-failure. The probe is a file this process just wrote, so an import warning on
        // it can only be a module.
        for (const LintWarning& w : clean.warnings) {
            if (w.countsAgainstMaxWarnings() && w.id == kImportCategory) {
                reason = "qmllint at " + g_qmllint + " cannot resolve a module " + std::string(entry.relative) +
                         " imports, so it cannot answer anything about that file (" + describe(w) + ")";
                return OracleVerdict::ModuleMissing;
            }
        }
        for (const LintWarning& w : clean.warnings) {
            if (w.countsAgainstMaxWarnings()) {
                reason = "qmllint at " + g_qmllint + " reports a bare `" + probe.type + " { }` as not clean (" +
                         describe(w) + "), so a warning on " + std::string(entry.relative) +
                         " could not be attributed to " + std::string(entry.relative);
                return OracleVerdict::Unusable;
            }
        }

        const LintReport dirty = lintFile(dirtyPath, hostProblem);
        if (!dirty.usable) {
            reason = hostProblem.empty() ? (g_qmllint + ": " + dirty.problem) : hostProblem;
            return OracleVerdict::Unusable;
        }
        bool rejected = false;
        for (const LintWarning& w : dirty.warnings) {
            if (w.countsAgainstMaxWarnings() && w.id == kMissingPropertyCategory &&
                w.message.find(kProbeUnknownProperty) != std::string::npos) {
                rejected = true;
                break;
            }
        }
        if (!rejected) {
            reason = "qmllint at " + g_qmllint + " accepted a property this test invented (" +
                     std::string(kProbeUnknownProperty) + ") on a " + probe.type + " root, so it is not reading " +
                     probe.module + "'s types and a clean run on " + std::string(entry.relative) +
                     ", which imports that module, would establish nothing";
            return OracleVerdict::Blind;
        }
        ++modulesProven;
    }
    // COVERAGE-ASSERT: the loop's own integrity. Every module in the row must have been put
    // through a probe pair; a `continue` slipped above the grading, or an empty type list on a
    // future row, would otherwise return Discriminates having proven nothing at all.
    if (modulesProven != entry.probeTypeCount || entry.probeTypeCount == 0) {
        reason = "this test proved the linter on " + std::to_string(modulesProven) + " of the " +
                 std::to_string(entry.probeTypeCount) + " module(s) " + std::string(entry.relative) +
                 " imports -- the fault is in backend/tests/qml_lint_test.cpp, not in the package";
        return OracleVerdict::Unusable;
    }
    return OracleVerdict::Discriminates;
}

// GOAL: S10.9's lint half for main.qml, S10.12's for the package's other QML -- the file
//       passes `qmllint -W0`. False if it produces any finding that counts against
//       `--max-warnings 0`: a misspelled property in a binding or a read, an unresolvable
//       type, a syntax error. The named consequence is a plugin that loads and silently shows
//       the wrong thing where the misspelling is.
// WHY-THIS-SHAPE: `--json` and a category-by-category verdict, not `-W0` and its exit status.
//       Two concrete reasons the simpler form fails. First, `-W0` exits 255 both for a real
//       defect and for a host with no Plasma module installed, so continuous integration --
//       which installs no Qt and configures with the KDE plugin off -- would report the
//       package as broken; the id on each warning is the only thing that tells those apart.
//       Second, in --json mode qmllint exits 0 even for a file with a missing-property
//       warning (measured on 6.11.1), so a check written to read the status in that mode
//       would pass on exactly the defect the goal names.
// CONTROL: `nosuchproperty: 1` added to the Rectangle in
//       backend/kde/kpackage/contents/ui/main.qml, in a copy of backend/ under /tmp linted
//       through WALLPAPERX_BACKEND_SOURCE_DIR -- observed `FAIL:
//       backend/kde/kpackage/contents/ui/main.qml passes qmllint -W0` followed by
//       `[missing-property/warning] Could not find property "nosuchproperty".` (line and
//       column omitted here on purpose: they track main.qml's layout and would go stale on any
//       edit above the Rectangle). The same property on the WallpaperItem root itself: same
//       failure, `Could not find property "nosuchrootprop"`, which is what proves the
//       linter resolves the Plasma module rather than passing vacuously over it. The same on
//       config.qml's Text root: `FAIL: backend/kde/kpackage/contents/ui/config.qml passes
//       qmllint -W0`. And the mutation that does NOT go red, recorded so the limit is on file
//       rather than rediscovered: `root.answered = true` changed to `root.answerd = true` in
//       main.qml's apply() -- qmllint answers `success:true, warnings:[]` and this check
//       passes. qmllint does not type-check an assignment target inside a JavaScript function
//       body. Every mutation was made in the /tmp copy; the repository was not touched.
void runLintCheck(const PackageQmlFile& entry, const std::filesystem::path& file, bool oracleProven) {
    if (!oracleProven) {
        std::fprintf(stderr, "  the lint verdict for %s is withheld: the linter did not prove it could answer "
                             "(reported above)\n",
                     entry.relative);
        return;
    }

    std::string hostProblem;
    const LintReport report = lintFile(file, hostProblem);
    if (!report.usable) {
        // The oracle answered twice already, so a failure here is this run losing the tool
        // mid-flight rather than the package being wrong.
        notVerified("the lint half of S10.9/S10.12",
                    (hostProblem.empty() ? (g_qmllint + ": " + report.problem) : hostProblem) + " (asked about " +
                        entry.relative + ")");
        ++g_verdictsWithheld;
        return;
    }

    std::vector<const LintWarning*> failing;
    for (const LintWarning& w : report.warnings) {
        if (w.countsAgainstMaxWarnings()) failing.push_back(&w);
    }

    // The verdict is issued before the harness cross-check below, and that ordering is the
    // check. The two readings disagreeing does not make this reading unknown -- `failing` is
    // already computed -- so withholding the verdict there would let a real misspelled
    // property present as a fault in this test file and nothing else, sending the reader to
    // fix a harness that is not what broke.
    const std::string verdict = "backend/kde/kpackage/" + std::string(entry.relative) + " passes qmllint -W0";
    check(failing.empty(), verdict.c_str());
    for (const LintWarning* w : failing) {
        std::fprintf(stderr, "  %s\n", describe(*w).c_str());
    }
    ++g_filesGraded;

    // qmllint's own `success` and this file's severity rule must agree. They are two readings
    // of the same run, and a version whose severities this file misclassifies would otherwise
    // show up as a confident verdict rather than as "the harness is out of date". Attributed
    // to the harness, not to the package, and red rather than a skip -- a reader that has
    // stopped tracking the tool must not go on issuing verdicts.
    // CONTROL: a stand-in linter that answers truthfully for the probes and then claims
    //       `success:true` for main.qml while emitting a `type:"warning"` finding -> both
    //       `FAIL: backend/kde/kpackage/contents/ui/main.qml passes qmllint -W0` and `FAIL
    //       (harness): this file's reading of qmllint's severities disagrees ...`, exit 1,
    //       with the "linted clean" announcement suppressed.
    const bool severityDisagreement = report.toolSaidSuccess != failing.empty();
    if (severityDisagreement) {
        harnessFailure("this file's reading of qmllint's severities disagrees with qmllint's own verdict for " +
                       file.string() + " (it said success=" + (report.toolSaidSuccess ? "true" : "false") +
                       ", this file counted " + std::to_string(failing.size()) +
                       " finding(s) against --max-warnings 0) -- the fault is in "
                       "backend/tests/qml_lint_test.cpp, not in the package");
    }
    if (!failing.empty() || severityDisagreement) return;

    // Announced, not merely implied. A run that had silently degraded to "qmllint not found"
    // would be a ctest SKIP rather than a pass, but this line is what tells a reader of a
    // *passing* log which tool actually looked and that it was proven able to say no first.
    std::printf("qml_lint_test: %s linted %s clean, after proving on generated probes that it rejects an unknown "
                "property on one type from each module that file imports: %s.\n",
                g_qmllint.c_str(), file.string().c_str(), describeProbeTypes(entry).c_str());
    std::fflush(stdout);
}

// 1-based line numbers of every comment whose first word is `qmllint`, which is qmllint's own
// rule for recognising a lint directive. Both comment styles and trailing position count; all
// three were measured to suppress a warning on 6.11.1.
std::vector<int> findLintDirectiveLines(std::string_view text) {
    constexpr std::string_view kToken = "qmllint";
    std::vector<int> lines;
    int line = 1;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
            continue;
        }
        if (text[i] != '/' || i + 1 >= text.size()) continue;
        if (text[i + 1] != '/' && text[i + 1] != '*') continue;
        std::size_t j = i + 2;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
        if (j + kToken.size() > text.size() || text.substr(j, kToken.size()) != kToken) continue;
        const std::size_t after = j + kToken.size();
        const bool wordEnds = after >= text.size() || (std::isalnum(static_cast<unsigned char>(text[after])) == 0 &&
                                                       text[after] != '_');
        if (wordEnds) lines.push_back(line);
    }
    return lines;
}

// GOAL: the file being graded cannot silence the check that grades it. False if a shipped QML
//       file carries a `// qmllint disable ...` comment: qmllint reads directive comments out
//       of the linted source itself, so a file could turn off `missing-property` and then be
//       announced "linted clean" over exactly the defect this entry exists to catch.
// WHY-THIS-SHAPE: an assertion in this file rather than another qmllint flag. There is no
//       flag -- `--ignore-settings` closes the external `.qmllint.ini` channel and does not
//       touch the in-file one, which is the channel the graded file controls. The match is
//       qmllint's own rule (a comment whose first word is `qmllint`), not the narrower word
//       `disable`, because the directive vocabulary belongs to the tool and a narrower match
//       would go quiet the moment it grows. The scan has no QML parser, so a comment-shaped
//       run of bytes inside a string literal would be reported; that direction is a named
//       FAIL on a real line, while the direction it forecloses -- a directive nobody notices
//       -- is a silent green, and this package ships no such string.
// CONTROL: `// qmllint disable missing-property` added above a bogus property in main.qml, in
//       a copy of backend/ under /tmp linted through WALLPAPERX_BACKEND_SOURCE_DIR -> observed
//       `FAIL: backend/kde/kpackage/contents/ui/main.qml carries no qmllint directive comment`
//       and the line printed. Without this check the same copy passed green over the bogus
//       property, with qmllint reporting `success:true, warnings:[]`. The repository was not
//       touched.
void checkNoLintDirectives(const PackageQmlFile& entry, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        notVerified("the lint half of S10.9/S10.12",
                    "whether " + std::string(entry.relative) + " carries a qmllint directive could not be "
                    "determined: it is there but could not be opened at " + file.string());
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        notVerified("the lint half of S10.9/S10.12",
                    "whether " + std::string(entry.relative) + " carries a qmllint directive could not be "
                    "determined: reading " + file.string() + " failed part way through");
        return;
    }
    const std::vector<int> directives = findLintDirectiveLines(text);
    const std::string verdict =
        "backend/kde/kpackage/" + std::string(entry.relative) + " carries no qmllint directive comment";
    check(directives.empty(), verdict.c_str());
    for (const int at : directives) {
        std::fprintf(stderr, "  line %d begins a comment with the word qmllint, which the linter reads as a directive "
                             "-- the file would then be grading itself\n",
                     at);
    }
}

// GOAL: every QML file the package ships is one this entry lints. False if a file is added
//       under contents/ui/ -- at the top level or in a subdirectory -- and nothing here looks
//       at it, which is the state config.qml was in until this table gained a second row.
// WHY-THIS-SHAPE: the directory is enumerated and compared against the table, rather than the
//       table simply being iterated. Iterating alone cannot notice a file nobody added a row
//       for, which is precisely how the gap arises: Step 11 fills config.qml in, Step 12 adds
//       another, and the entry called `qml_lint` stays green over unlinted QML. Globbing the
//       directory *instead* of the table was rejected: each row also carries the modules and
//       root type its oracle needs, which cannot be derived without a QML parser this binary
//       has none of, and an empty glob would silently grade nothing. The walk recurses, and
//       the comparison key is the path relative to the package root rather than the bare
//       filename, because a component under contents/ui/components/ is the conventional Plasma
//       layout and a non-recursive walk would neither lint it nor report it as unlinted.
//       The error test sits *after* the loop, not inside it: increment(error_code&) both sets
//       the code and turns the iterator into the end iterator, so a failed read ends the loop
//       and a test in the body can only ever observe the code left by a successful step.
// COVERAGE-ASSERT: this is the guard on the table's completeness; g_filesGraded below is the
//       guard on the table actually being walked.
// CONTROL: an extra file `contents/ui/stray.qml` added to a copy of backend/ under /tmp and
//       linted through WALLPAPERX_BACKEND_SOURCE_DIR -> `FAIL: every .qml under contents/ui is
//       one this test lints` and `  contents/ui/stray.qml is not in this file's table`. Same
//       observation with the stray placed at `contents/ui/components/stray.qml`, which the
//       walk only reaches by recursing. And for the truncated-walk branch, an LD_PRELOAD
//       stand-in whose readdir fails with EIO after the first entry -> `NOT VERIFIED ...
//       walking ... failed part way through: Input/output error`, exit 77; with the error test
//       written inside the loop the same run reported the check as passing. The repository
//       itself was not touched.
void checkTableCoversTheDirectory() {
    const std::filesystem::path uiDir = g_packageDir / "contents" / "ui";
    std::error_code ec;
    std::vector<std::string> unlisted;
    std::string walkProblem;
    const std::filesystem::recursive_directory_iterator end;
    std::filesystem::recursive_directory_iterator it(uiDir, ec);
    if (ec) {
        notVerified("the completeness of this entry's file list",
                    "the package's contents/ui directory could not be read -- " + uiDir.string() + ": " +
                        ec.message());
        return;
    }
    for (; it != end; it.increment(ec)) {
        if (it->path().extension() != ".qml") continue;
        std::error_code relEc;
        const std::filesystem::path rel = std::filesystem::relative(it->path(), uiDir, relEc);
        if (relEc) {
            walkProblem = it->path().string() + " could not be named relative to " + uiDir.string() + ": " +
                          relEc.message();
            break;
        }
        const std::string relative = "contents/ui/" + rel.generic_string();
        const bool listed = std::any_of(std::begin(kPackageQml), std::end(kPackageQml),
                                        [&](const PackageQmlFile& e) { return relative == e.relative; });
        if (!listed) unlisted.push_back(relative);
    }
    // A stray file already seen is reported as a failure even if the walk then broke; only a
    // walk that broke with nothing to report becomes a skip, so a truncated enumeration is
    // never graded as a complete, clean one.
    if (unlisted.empty() && (ec || !walkProblem.empty())) {
        notVerified("the completeness of this entry's file list",
                    "walking " + uiDir.string() + " failed part way through: " +
                        (ec ? ec.message() : walkProblem));
        return;
    }
    check(unlisted.empty(), "every .qml under contents/ui is one this test lints");
    for (const std::string& name : unlisted) {
        std::fprintf(stderr, "  %s is not in this file's table, so nothing ever lints it\n", name.c_str());
    }
    if (ec || !walkProblem.empty()) {
        std::fprintf(stderr, "  and the walk of %s did not finish (%s), so there may be more\n", uiDir.string().c_str(),
                     (ec ? ec.message() : walkProblem).c_str());
    }
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "qml_lint_test";

    const std::filesystem::path backendDir(WALLPAPERX_BACKEND_SOURCE_DIR);
    g_packageDir = backendDir / "kde" / "kpackage";
    g_qmllint = WALLPAPERX_QMLLINT_EXECUTABLE;

    // The source tree not being where the build recorded it is this run failing to set up, not
    // the package being wrong, so it is a named skip. A tree that *is* there but has no
    // main.qml is the opposite -- that is the package being incomplete, and the phase below
    // says so.
    // CONTROL: built this file by hand with WALLPAPERX_BACKEND_SOURCE_DIR="/nonexistent/backend"
    //       -- observed the NOT VERIFIED line and exit 77, with no FAIL against the package.
    std::error_code dirEc;
    if (!std::filesystem::is_directory(backendDir, dirEc) || dirEc) {
        notVerified("the lint half of S10.9/S10.12",
                    "the backend source directory baked in at build time is not there now: " + backendDir.string());
        std::printf("qml_lint_test: nothing could be checked -- reported to ctest as a skip, not a pass.\n");
        return wallpaperx::testing::kNotVerifiedExitCode;
    }

    int exitCode = 0;
    // Files whose oracle proved the linter could answer about them, so a verdict was owed.
    int reachedTheLinter = 0;
    try {
        checkTableCoversTheDirectory();

        for (const PackageQmlFile& entry : kPackageQml) {
            const std::filesystem::path file = g_packageDir / entry.relative;

            // Established before qmllint is consulted, and this ordering is the check. qmllint
            // reports a file it cannot open as an `import` warning -- the same category as a
            // missing module -- so asking it first would let a deleted main.qml be classified
            // as "this host has no Plasma" and reported as a skip. ctest grades a skip as
            // not-a-failure, so the package's entry point vanishing would be the quietest
            // possible result. A missing file is a package defect for both entries, and stays
            // a FAIL here even though kpackage_metadata's S9.4/S9.5 also red on it: two
            // entries agreeing costs a duplicate line, while converting either into a skip
            // would make this one report not-a-failure over an absent file.
            // CONTROL: moved backend/kde/kpackage/contents/ui/main.qml aside in a /tmp copy of
            //       backend/ -- observed `FAIL: the package ships contents/ui/main.qml, one of
            //       the files this goal is about`, exit 1, not a skip. Same for config.qml.
            std::string why;
            bool haveFile = false;
            const std::string shipped =
                "the package ships " + std::string(entry.relative) + ", one of the files this goal is about";
            switch (presenceOf(file, why)) {
                case Presence::Unknowable:
                    notVerified("the lint half of S10.9/S10.12", "whether the package ships " +
                                                                     std::string(entry.relative) +
                                                                     " could not be determined -- " + why);
                    break;
                case Presence::Missing:
                    check(false, shipped.c_str());
                    std::fprintf(stderr, "  expected it at %s\n", file.string().c_str());
                    break;
                case Presence::Present:
                    haveFile = true;
                    break;
            }
            if (!haveFile) continue;

            // Text-only, so it runs before the linter is consulted and still answers on a host
            // with no qmllint at all -- which is the host on which a directive would otherwise
            // sit unnoticed for as long as the entry keeps skipping.
            checkNoLintDirectives(entry, file);

            if (g_qmllint.empty()) {
                // Nothing was wrong with the package; this host simply cannot look. Named
                // rather than generic, because "qmllint not found" is the state in which this
                // whole file would otherwise pass forever while establishing nothing.
                // CONTROL: built this file by hand with WALLPAPERX_QMLLINT_EXECUTABLE="" --
                //       observed this line and exit 77, which ctest reads as Skipped, not
                //       Passed. Built again with it pointing at /nonexistent/qmllint (found at
                //       configure time, gone by run time) -- observed `... was found when this
                //       build was configured but cannot be run now: No such file or
                //       directory`, also 77.
                notVerified("the lint half of S10.9/S10.12",
                            "no qmllint was found when this build was configured, so nothing here could compile or "
                            "resolve the modules " + std::string(entry.relative) + " imports (Qt's qmllint is not on "
                            "PATH on every host; backend/CMakeLists.txt looks in Qt's own bin directory too)");
                continue;
            }

            wallpaperx::testing::ScratchDir scratch("qmllint");
            if (!scratch.ok()) {
                notVerified("the lint half of S10.9/S10.12",
                            "this machine has nowhere to write the probe QML files the linter is checked against "
                            "(std::filesystem::temp_directory_path, honouring $TMPDIR)");
                continue;
            }

            std::string reason;
            const OracleVerdict oracle = proveOracleDiscriminates(entry, scratch.path(), reason);
            switch (oracle) {
                case OracleVerdict::Discriminates:
                    ++reachedTheLinter;
                    break;
                case OracleVerdict::ModuleMissing:
                case OracleVerdict::Unusable:
                case OracleVerdict::Blind:
                    notVerified("the lint half of S10.9/S10.12", reason);
                    break;
            }
            runLintCheck(entry, file, oracle == OracleVerdict::Discriminates);
        }
    } catch (const std::exception& e) {
        // Attributed here, not to the package: an exception escaping the above comes from the
        // JSON reader or the filesystem calls in this file at least as often as it says
        // anything about backend/kde/kpackage/.
        harnessFailure(std::string("this test threw: ") + e.what());
    }

    // COVERAGE-ASSERT: every file whose oracle proved the linter could answer must end with
    // either a verdict or a printed withholding -- the loop's own integrity, which is a
    // runtime count and not a property of the source. A `continue` slipped above the grading
    // call, or a row dropped part way through the walk, leaves this unequal and red instead of
    // quietly unchecked. Compared against the files that reached the linter rather than
    // against the table's size, because a host that cannot resolve one file's modules
    // legitimately grades fewer, and that is a skip with a reason, not a hollowed-out run.
    // CONTROL: `continue` inserted immediately above the runLintCheck call -> `FAIL (harness):
    //       this run reached the linter for 2 of the package's QML files but accounted for
    //       only 0`, exit 1. Reverted.
    if (g_filesGraded + g_verdictsWithheld != reachedTheLinter) {
        harnessFailure("this run reached the linter for " + std::to_string(reachedTheLinter) +
                       " of the package's QML files but accounted for only " +
                       std::to_string(g_filesGraded + g_verdictsWithheld) +
                       " -- the fault is in backend/tests/qml_lint_test.cpp, not in the package");
    }

    // Failures asked about first, so a run that is both broken and incomplete reports as
    // broken -- ctest's "Skipped" would otherwise hide red.
    const bool checksFailed = wallpaperx::testing::reportCheckFailures();
    const bool harnessFaulted =
        wallpaperx::testing::reportHarnessFaults("in this test file or in the tool it drives, not in the package");
    if (checksFailed || harnessFaulted) {
        exitCode = 1;
    } else {
        exitCode = wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // The other half of S10.9 -- that the root element is WallpaperItem from
    // org.kde.plasma.plasmoid -- is graded by the S9.4 check in
    // backend/tests/kpackage_metadata_test.cpp, a separate ctest entry that reads the file as
    // text and therefore still runs when this one skips. Printed on every path so a reader of
    // a "Skipped" line is not left believing S10.9 went entirely unchecked.
    std::printf("qml_lint_test: this entry grades only the lint half of S10.9/S10.12; S10.9's root-element half is "
                "the kpackage_metadata entry (S9.4), which needs no Qt and runs even when this one skips.\n");
    return exitCode;
}
