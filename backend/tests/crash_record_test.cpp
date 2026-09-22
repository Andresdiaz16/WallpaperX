// SPDX-License-Identifier: GPL-2.0-or-later

// The on-disk half of the crash-loop breaker: the record the render thread keeps while a
// wallpaper is being applied, the playing record that names the video a death happened in, and
// what the fatal-signal handler does to each.
//
// Everything that involves a signal runs in a forked child, for one reason: a check that gets
// it wrong takes the process down instead of reporting, and a dead harness prints no
// diagnostics at all. The child dies; the parent reads what it left on disk and grades that.
//
// No GPU, no bus, no daemon. The state directory is a scratch directory this file owns, so a
// run never reads or writes the developer's own $XDG_STATE_HOME -- which is what the
// production default resolves to, and what a manager built with a scratch config alone still
// uses.

#include "daemon/crash_record.h"

#include "support/forked_child.h"
#include "tests/client/scratch_dir.h"
#include "tests/test_report.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using wallpaperx::ApplyingRecord;
using wallpaperx::CrashRecord;
using wallpaperx::crashRecordPath;
using wallpaperx::describeCrash;
using wallpaperx::installFatalSignalHandlers;
using wallpaperx::readCrashRecord;
using wallpaperx::removeCrashRecord;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::ScratchDir;

namespace {

// What every forked child records. The monitor id is arbitrary but must match what the parent
// reads back: readCrashRecord() compares the id in the file against the one asked for, so a
// mismatch here would be indistinguishable from the handler never writing.
constexpr int kMonitorId = 3;
constexpr const char* kType = "static";
constexpr const char* kPath = "/pictures/the-one-that-crashed.png";

// The child could not put a record on disk at all, so nothing it did afterwards means
// anything. Distinct from forked_child.h's preamble codes and from any signal death.
constexpr int kChildRecordFailedExit = 120;

// How long a child gets to reach the point the parent is waiting for. Generous: this is the
// bound past which the run is declared broken, not a measurement of anything.
constexpr auto kChildTimeout = std::chrono::seconds(20);

#if defined(WALLPAPERX_SIGNAL_EXIT_IS_THE_SIGNAL)
constexpr bool kDeathSignalObservable = WALLPAPERX_SIGNAL_EXIT_IS_THE_SIGNAL;
#else
constexpr bool kDeathSignalObservable = true;
#endif

// ---------------------------------------------------------------------------- //
// The forked child
// ---------------------------------------------------------------------------- //

enum class Mode {
    FaultOnApplying,  // a real memory fault on the thread holding the record
    AbortOnApplying,  // SIGABRT on that thread -- a one-digit signal number
    AbortElsewhere,   // SIGABRT on a thread that holds no record
    KilledMidApply,   // SIGKILL from the parent while the record is live
    CleanExit,        // the record is destroyed the ordinary way
    ChainPrevious,    // a handler installed before ours must still run
};

// The write end of the pipe the ChainPrevious child's own SIGABRT handler reports through.
// A global because a sigaction handler takes no context, and write() is async-signal-safe.
int g_chainFd = -1;

void notePreviousHandlerRan(int) {
    const char byte = 'p';
    const ssize_t ignored = ::write(g_chainFd, &byte, 1);
    (void)ignored;
}

// A store to a page mapped PROT_NONE: a genuine SIGSEGV rather than a raise() of one.
//
// mmap and not a null dereference, because a compiler that can see the null can replace the
// store with a trap -- SIGILL, not SIGSEGV -- and the check below would then be asserting on
// what the optimiser felt like emitting.
[[noreturn]] void faultNow() {
    void* page = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) _exit(kChildRecordFailedExit);
    *static_cast<volatile char*>(page) = 1;
    _exit(kChildRecordFailedExit);  // unreachable: the store above faults
}

[[noreturn]] void sleepForever() {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

[[noreturn]] void runChildBody(Mode mode, const std::filesystem::path& dir, int readyFd, int chainFd) {
    if (mode == Mode::ChainPrevious) {
        g_chainFd = chainFd;
        struct sigaction previous {};
        previous.sa_handler = notePreviousHandlerRan;
        sigemptyset(&previous.sa_mask);
        previous.sa_flags = 0;
        ::sigaction(SIGABRT, &previous, nullptr);
    }
    installFatalSignalHandlers();

    std::promise<void> recordLive;
    std::future<void> live = recordLive.get_future();

    std::thread applying([&] {
        ApplyingRecord record(dir, kMonitorId, kType, kPath);
        // Nothing below says anything unless the record actually reached the disk. Exiting
        // with a code of its own keeps "the harness could not set this up" apart from "the
        // handler did the wrong thing", which is otherwise the same missing file.
        std::error_code ec;
        if (!std::filesystem::exists(crashRecordPath(dir, kMonitorId), ec)) _exit(kChildRecordFailedExit);
        recordLive.set_value();

        switch (mode) {
            case Mode::FaultOnApplying:
                faultNow();
            case Mode::AbortOnApplying:
            case Mode::ChainPrevious:
                std::abort();
            case Mode::AbortElsewhere:
            case Mode::KilledMidApply:
                sleepForever();  // holds the record open for whatever kills this process
            case Mode::CleanExit:
                break;
        }
    });

    live.wait();
    switch (mode) {
        case Mode::AbortElsewhere:
            std::abort();  // the main thread dies; the applying thread is still holding a record
        case Mode::KilledMidApply: {
            const char byte = 'r';
            const ssize_t ignored = ::write(readyFd, &byte, 1);
            (void)ignored;
            sleepForever();
        }
        case Mode::CleanExit:
            applying.join();
            // _Exit and not return: this process is a fork of the harness and holds copies of
            // its ScratchDir objects, whose destructors would delete the very directory the
            // parent is about to read.
            std::_Exit(0);
        case Mode::FaultOnApplying:
        case Mode::AbortOnApplying:
        case Mode::ChainPrevious:
            sleepForever();  // the applying thread ends the whole process
    }
    std::_Exit(0);
}

// waitpid with a deadline. A blocking wait on a child that wedged would hang this run, and a
// hang is graded by ctest's timeout as a failure of nothing in particular.
bool waitForExit(pid_t pid, int& status) {
    const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
    for (;;) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) return true;
        if (done < 0 && errno != EINTR) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

struct ChildResult {
    bool reaped = false;
    int status = 0;
    bool previousHandlerRan = false;
};

// Runs one child to its end and returns how it died. `ok` is false when the harness itself
// could not get the child to the point being graded -- never when the child did something
// unexpected, which is a verdict and belongs in a check().
ChildResult runChild(Mode mode, const std::filesystem::path& dir, bool& ok) {
    ok = false;
    ChildResult out;

    int ready[2] = {-1, -1};
    int chain[2] = {-1, -1};
    if (::pipe2(ready, O_CLOEXEC) != 0) {
        harnessFailure("could not create the readiness pipe for a forked child");
        return out;
    }
    if (::pipe2(chain, O_CLOEXEC) != 0) {
        ::close(ready[0]);
        ::close(ready[1]);
        harnessFailure("could not create the handler-chaining pipe for a forked child");
        return out;
    }

    const pid_t parentBeforeFork = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(ready[0]);
        ::close(ready[1]);
        ::close(chain[0]);
        ::close(chain[1]);
        harnessFailure("fork failed");
        return out;
    }
    if (pid == 0) {
        ::close(ready[0]);
        ::close(chain[0]);
        if (!wallpaperx::testing::armAgainstParentDeath(parentBeforeFork)) {
            _exit(wallpaperx::testing::kChildArmFailedExit);
        }
        // Every mode here ends in SIGSEGV or SIGABRT on purpose. Without this each run leaves
        // a core in the host's collector and a crash notification on the developer's desktop.
        wallpaperx::testing::suppressCoreDump();
        runChildBody(mode, dir, ready[1], chain[1]);
    }

    ::close(ready[1]);
    ::close(chain[1]);

    bool killedOnCue = true;
    if (mode == Mode::KilledMidApply) {
        // The record has to be live when the signal lands, and "live" is a fact the child
        // reports, not an interval to sleep through. A sleep long enough to look safe here is
        // an assertion about how fast this machine happens to be.
        struct pollfd waitOn {
            ready[0], POLLIN, 0
        };
        const int reported = ::poll(&waitOn, 1, static_cast<int>(kChildTimeout.count()) * 1000);
        if (reported <= 0) {
            harnessFailure("a forked child never reported that its record was on disk, so nothing was killed "
                           "mid-apply and the checks below would be graded against an empty directory");
            killedOnCue = false;
        }
        // Killed either way: an unreported child is still a process holding a scratch directory
        // open. The flag above is what stops its outcome being read as a verdict.
        ::kill(pid, SIGKILL);
    }

    out.reaped = waitForExit(pid, out.status);
    if (!out.reaped) {
        ::kill(pid, SIGKILL);
        int discarded = 0;
        waitForExit(pid, discarded);
        harnessFailure("a forked child did not exit within the harness's bound");
    }

    // After the reap, so the byte is either in the pipe or was never written.
    char byte = 0;
    int flags = ::fcntl(chain[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(chain[0], F_SETFL, flags | O_NONBLOCK);
    out.previousHandlerRan = ::read(chain[0], &byte, 1) == 1;

    ::close(ready[0]);
    ::close(chain[0]);

    if (out.reaped && WIFEXITED(out.status)) {
        if (const char* fault = wallpaperx::testing::forkPreambleFault(WEXITSTATUS(out.status));
            fault != nullptr) {
            harnessFailure(fault);
            return out;
        }
        if (WEXITSTATUS(out.status) == kChildRecordFailedExit) {
            harnessFailure("a forked child could not put a record on disk, so its outcome grades nothing");
            return out;
        }
    }
    ok = out.reaped && killedOnCue;
    return out;
}

std::string howItDied(const ChildResult& result) {
    if (!result.reaped) return "never exited";
    if (WIFSIGNALED(result.status)) {
        return "signal " + std::to_string(WTERMSIG(result.status));
    }
    return "exit " + std::to_string(WEXITSTATUS(result.status));
}

bool diedOf(const ChildResult& result, int sig) {
    return result.reaped && WIFSIGNALED(result.status) && WTERMSIG(result.status) == sig;
}

// ---------------------------------------------------------------------------- //
// Phases
// ---------------------------------------------------------------------------- //

// GOAL: S13.11 -- a fatal signal on the thread applying a wallpaper leaves a record naming
//       that wallpaper and that signal, and the process still dies of that signal. False if
//       the record is missing, unstamped, names another wallpaper, or if the handler swallows
//       the signal so the service manager sees a clean exit.
// WHY-THIS-SHAPE: two signals, not one. The handler formats the number by hand and has a
//       separate branch for two digits, so SIGSEGV (11) alone leaves the one-digit branch
//       ungraded and SIGABRT (6) alone leaves the other. The SIGSEGV case is a real store to a
//       PROT_NONE page rather than raise(SIGSEGV): raise() would grade the handler against a
//       signal the thread sent itself, which is not the case the breaker exists for.
// CONTROL: in backend/daemon/crash_record.cpp, the `::write(fd, line, n)` in onFatalSignal()
//       deleted -> "FAIL: the record left by a fault on the applying thread is stamped with
//       SIGSEGV" and the same for SIGABRT, four FAIL lines in all, ctest Failed. Reverted.
void runStampedChecks(int sig, Mode mode, const char* what) {
    ScratchDir scratch("crashstamp");
    if (!scratch.ok()) {
        notVerified("S13.11", std::string("no scratch state directory for the ") + what + " case");
        return;
    }

    bool ok = false;
    const ChildResult result = runChild(mode, scratch.path(), ok);
    if (!ok) return;  // runChild already said which part of the harness failed

    if (kDeathSignalObservable) {
        check(diedOf(result, sig),
              (std::string(what) + " on the applying thread still ends the process with that signal").c_str());
        if (!diedOf(result, sig)) {
            std::fprintf(stderr, "  child died: %s, expected signal %d\n", howItDied(result).c_str(), sig);
        }
    }

    const std::optional<CrashRecord> record = readCrashRecord(scratch.path(), kMonitorId);
    check(record.has_value(), (std::string(what) + " on the applying thread leaves a record behind").c_str());
    if (!record) return;

    check(record->signal.has_value() && *record->signal == sig,
          (std::string("the record left by ") + what + " on the applying thread is stamped with it").c_str());
    if (record->signal && *record->signal != sig) {
        std::fprintf(stderr, "  record says signal %d, expected %d\n", *record->signal, sig);
    }
    check(record->type == kType && record->path == kPath,
          (std::string("the record left by ") + what + " names the wallpaper that was being applied").c_str());

    const std::string reason = describeCrash(*record);
    const char* name = ::sigabbrev_np(sig);
    check(name != nullptr && reason.find(std::string("SIG") + name) != std::string::npos,
          (std::string("the reason a client reads after ") + what + " names that signal").c_str());
    std::printf("crash_record_test: after %s -- %s\n", what, reason.c_str());
}

// GOAL: S13.12 -- a fatal signal on any other thread while a wallpaper is being applied leaves
//       NO record. False if the next start refuses a wallpaper because an unrelated thread
//       died beside it.
// WHY-THIS-SHAPE: the abort has to land while the record is live, so the applying thread
//       parks on a promise it publishes after the constructor returns rather than on a sleep
//       long enough to look safe. The positive direction is runStampedChecks above: the same
//       abort moved onto the applying thread does leave a stamped record, so a build that
//       never wrote one at all would fail there rather than pass here.
// CONTROL: in backend/daemon/crash_record.cpp, the `::unlink(path)` that onFatalSignal()'s
//       other-thread branch performs once its compare-exchange confirms the copied name
//       replaced by `(void)path` -> "FAIL: a crash on a thread that is not applying leaves
//       no record" and "...leaves no file on disk", ctest Failed. Reverted.
void runOtherThreadChecks() {
    ScratchDir scratch("crashother");
    if (!scratch.ok()) {
        notVerified("S13.12", "no scratch state directory could be created");
        return;
    }

    bool ok = false;
    const ChildResult result = runChild(Mode::AbortElsewhere, scratch.path(), ok);
    if (!ok) return;

    if (kDeathSignalObservable) {
        check(diedOf(result, SIGABRT), "a crash on a thread that is not applying still ends the process");
    }
    check(!readCrashRecord(scratch.path(), kMonitorId).has_value(),
          "a crash on a thread that is not applying leaves no record");
    std::error_code ec;
    check(!std::filesystem::exists(crashRecordPath(scratch.path(), kMonitorId), ec),
          "a crash on a thread that is not applying leaves no file on disk");
}

// GOAL: S13.17 -- a process ended with no handler running (SIGKILL: the out-of-memory killer,
//       a teardown alarm, a power cut) while applying leaves an UNSTAMPED record, and the
//       reason a client reads says it was killed rather than naming a signal. False if that
//       case produces no record, since being killed on a huge image is the loop the breaker
//       exists to break.
// WHY-THIS-SHAPE: the kill is sent when the child says the record is on disk, over a pipe.
//       Timing it with a sleep would either kill the child before the constructor ran -- no
//       record, and the check would call that a defect -- or after the destructor, on a
//       machine slower or faster than this one.
// CONTROL: in backend/daemon/crash_record.cpp, ApplyingRecord's constructor changed to return
//       before its write loop (`if (true) { ::close(fd); return; }`) -> "FAIL: a process
//       killed while applying leaves a record", ctest Failed. Reverted.
void runKilledChecks() {
    ScratchDir scratch("crashkilled");
    if (!scratch.ok()) {
        notVerified("S13.17", "no scratch state directory could be created");
        return;
    }

    bool ok = false;
    const ChildResult result = runChild(Mode::KilledMidApply, scratch.path(), ok);
    if (!ok) return;

    check(diedOf(result, SIGKILL), "a child killed mid-apply dies of SIGKILL");
    const std::optional<CrashRecord> record = readCrashRecord(scratch.path(), kMonitorId);
    check(record.has_value(), "a process killed while applying leaves a record");
    if (!record) return;

    check(!record->signal.has_value(),
          "the record a killed process leaves carries no signal -- no handler ran to stamp one");
    check(record->type == kType && record->path == kPath,
          "the record a killed process leaves names the wallpaper that was being applied");

    const std::string reason = describeCrash(*record);
    check(reason.find("killed") != std::string::npos,
          "the reason a client reads after a kill says the daemon was killed");
    check(reason.find("signal") == std::string::npos && reason.find("SIG") == std::string::npos,
          "the reason a client reads after a kill names no signal, because none was recorded");
    std::printf("crash_record_test: after a kill -- %s\n", reason.c_str());
}

// GOAL: the record exists only while the wallpaper is being applied -- a run that finishes
//       leaves nothing for the next start to refuse. False if a daemon that shut down cleanly
//       refuses the wallpaper it last displayed.
// CONTROL: in backend/daemon/crash_record.cpp, the `::unlink(m_file.c_str())` in
//       ~ApplyingRecord deleted -> "FAIL: a record destroyed the ordinary way leaves no file",
//       ctest Failed. Reverted.
void runCleanExitChecks() {
    ScratchDir scratch("crashclean");
    if (!scratch.ok()) {
        notVerified("S13.11 (the clean-exit direction)", "no scratch state directory could be created");
        return;
    }

    bool ok = false;
    const ChildResult result = runChild(Mode::CleanExit, scratch.path(), ok);
    if (!ok) return;

    check(result.reaped && WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
          "a child that destroys its record the ordinary way exits cleanly");
    if (!(result.reaped && WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0)) {
        std::fprintf(stderr, "  child died: %s\n", howItDied(result).c_str());
    }
    std::error_code ec;
    check(!std::filesystem::exists(crashRecordPath(scratch.path(), kMonitorId), ec),
          "a record destroyed the ordinary way leaves no file");
    check(!readCrashRecord(scratch.path(), kMonitorId).has_value(),
          "a record destroyed the ordinary way leaves nothing for the next start to read");
}

// GOAL: S13.18 -- the handler hands the signal to whatever handler was installed before it.
//       False if installing this breaker silences a sanitizer's report, or a crash handler a
//       distribution installs, for every fatal signal in the daemon.
// WHY-THIS-SHAPE: the earlier handler reports through a pipe rather than by writing a file,
//       because it runs inside a signal handler where only async-signal-safe calls are legal;
//       and the pipe is drained after the reap, so "no byte" cannot mean "not yet".
// CONTROL: in backend/daemon/crash_record.cpp, the whole `if (sig >= 0 && sig < kHandlerSlots)`
//       block in onFatalSignal() deleted -> "FAIL: the handler installed before this one still
//       runs", ctest Failed. Reverted.
void runChainingChecks() {
    ScratchDir scratch("crashchain");
    if (!scratch.ok()) {
        notVerified("S13.18", "no scratch state directory could be created");
        return;
    }

    bool ok = false;
    const ChildResult result = runChild(Mode::ChainPrevious, scratch.path(), ok);
    if (!ok) return;

    check(result.previousHandlerRan, "the handler installed before this one still runs");
    if (kDeathSignalObservable) {
        check(diedOf(result, SIGABRT),
              "chaining to the previous handler does not stop the process dying of the signal");
    }
    const std::optional<CrashRecord> record = readCrashRecord(scratch.path(), kMonitorId);
    check(record && record->signal && *record->signal == SIGABRT,
          "chaining to the previous handler does not cost the record its stamp");
}

void writeText(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

// GOAL: a file in the state directory is only read as a record of THIS monitor's last apply.
//       Anything else -- another monitor's record under this name, a symlink, a directory, a
//       file too large to be one of ours, a torn or foreign line -- reads as no record. False
//       if any of them refuses a wallpaper, because a refusal is only ever justified by
//       evidence that this daemon wrote.
// WHY-THIS-SHAPE: each case is a file the harness plants, because none of them is a state a
//       correct daemon ever produces. The alternative -- grading only what ApplyingRecord
//       writes -- cannot reach the parser's guards at all, and those guards are what stand
//       between a stray file and a wallpaper the user cannot set.
// CONTROL: in backend/daemon/crash_record.cpp, `root.at(kKeyMonitorId) != monitorId` dropped
//       from readCrashRecord's rejection list -> "FAIL: a record naming another monitor is not
//       read as this one's"; O_NOFOLLOW dropped from the same function's open -> "FAIL: a
//       symlink in place of a record is not followed". Both reverted.
void runOnDiskChecks() {
    ScratchDir scratch("crashdisk");
    if (!scratch.ok()) {
        notVerified("the on-disk record's own rules", "no scratch state directory could be created");
        return;
    }
    const std::filesystem::path dir = scratch.path();

    try {
        {
            ApplyingRecord record(dir, 4, kType, kPath);
            std::error_code ec;
            std::filesystem::copy_file(crashRecordPath(dir, 4), crashRecordPath(dir, 5), ec);
            check(!ec, "the harness could copy one monitor's record under another's name");
        }
        check(!readCrashRecord(dir, 5).has_value(), "a record naming another monitor is not read as this one's");
        check(!readCrashRecord(dir, 4).has_value(), "a destroyed record is gone even though a copy of it is not");

        // dump() throws on invalid UTF-8, and this constructor runs on the render thread inside
        // the block whose exceptions are graded as the wallpaper failing to draw.
        { ApplyingRecord record(dir, 6, kType, "/pictures/\xff\xfe.png"); }
        std::error_code ec;
        check(!std::filesystem::exists(crashRecordPath(dir, 6), ec),
              "a path that is not valid UTF-8 writes no record rather than a half-written one");

        { ApplyingRecord record({}, 0, kType, kPath); }
        check(crashRecordPath({}, 0).empty(), "with no state directory there is no record path");
        check(!readCrashRecord({}, 0).has_value(), "with no state directory there is nothing to read");
    } catch (const std::exception& e) {
        check(false, (std::string("writing a record threw, and the render thread has no way to report it: ") +
                      e.what())
                         .c_str());
    }

    const std::string valid = "{\"monitor_id\":7,\"type\":\"static\",\"path\":\"/p.png\"}";
    writeText(crashRecordPath(dir, 7), valid + "\n");
    check(readCrashRecord(dir, 7).has_value(), "a well-formed planted record is read");

    writeText(crashRecordPath(dir, 7), valid);  // no newline: a write that was cut short
    check(!readCrashRecord(dir, 7).has_value(), "a record with no line terminator is not read as complete");

    writeText(crashRecordPath(dir, 7), "not json at all\n");
    check(!readCrashRecord(dir, 7).has_value(), "a file that is not one of ours is read as no record");

    writeText(crashRecordPath(dir, 7), "{\"monitor_id\":7,\"type\":\"static\"}\n");
    check(!readCrashRecord(dir, 7).has_value(), "a record with no path is not read as a record");

    writeText(crashRecordPath(dir, 7), valid + "\n" + std::string(128 * 1024, 'x'));
    check(!readCrashRecord(dir, 7).has_value(), "a file far larger than any record we write is not read as one");

    writeText(crashRecordPath(dir, 7), valid + "\nsignal 99\n");
    const std::optional<CrashRecord> unknown = readCrashRecord(dir, 7);
    check(unknown && unknown->signal && *unknown->signal == 99, "a signal number is read back as written");
    if (unknown) {
        check(describeCrash(*unknown).find("signal 99") != std::string::npos,
              "a signal this platform has no name for is reported by number");
    }
    removeCrashRecord(dir, 7);
    std::error_code ec;
    check(!std::filesystem::exists(crashRecordPath(dir, 7), ec), "removing a record removes its file");

    writeText(dir / "record-target.json", valid + "\n");
    std::filesystem::create_symlink(dir / "record-target.json", crashRecordPath(dir, 8), ec);
    if (ec) {
        notVerified("the symlink guard", "this filesystem would not create a symlink");
    } else {
        // The planted target names monitor 7, so a followed symlink is asked for 7 -- the id
        // check must not be what rejects it.
        writeText(dir / "record-target.json", "{\"monitor_id\":8,\"type\":\"static\",\"path\":\"/p.png\"}\n");
        check(!readCrashRecord(dir, 8).has_value(), "a symlink in place of a record is not followed");
    }
    // No check that a DIRECTORY in place of a record reads as none. readCrashRecord's S_ISREG
    // guard was removed as a control and the case still passed: the kernel refuses to read a
    // directory whatever the caller thinks it opened, so no production change makes such a
    // check go red, and one that cannot fail is worse than the gap it hides.
}

// ---------------------------------------------------------------------------- //
// The playing record: which video a death names (Step 13 Stage E5)
// ---------------------------------------------------------------------------- //

void check(bool condition, const std::string& what) { wallpaperx::testing::check(condition, what.c_str()); }

constexpr int kPlayingMonitor = 2;
constexpr const char* kPlayingPath = "/videos/the-one-that-crashes.mp4";
constexpr int kOtherPlayingMonitor = 6;
constexpr const char* kOtherPlayingPath = "/videos/a-good-one.mp4";

enum class PlayingMode {
    FaultNamed,       // a memory fault on the thread whose guard names a video
    FaultElsewhere,   // the guard is live on one thread, the fault lands on another
    FaultAfterGuard,  // the guard has been destroyed; the same thread then faults
    FaultAfterEnd,    // the guard is live but the record was ended (an orderly stop began)
    KilledNamed,      // SIGKILL while the guard is live: no handler runs
};

[[noreturn]] void runPlayingChildBody(PlayingMode mode, const std::filesystem::path& dir, int readyFd) {
    installFatalSignalHandlers();
    wallpaperx::PlayingRecord record(dir);
    record.start({{kPlayingMonitor, kPlayingPath, 1}, {kOtherPlayingMonitor, kOtherPlayingPath, 0}});
    std::error_code ec;
    if (!std::filesystem::exists(wallpaperx::playingRecordPath(dir), ec)) _exit(kChildRecordFailedExit);
    const std::string line = wallpaperx::playingBlameLine(kPlayingMonitor, kPlayingPath);

    std::promise<void> guardLive;
    std::future<void> live = guardLive.get_future();
    std::thread worker([&] {
        switch (mode) {
            case PlayingMode::FaultNamed: {
                const wallpaperx::PlayingBlame blame(line);
                faultNow();
            }
            case PlayingMode::FaultAfterGuard: {
                { const wallpaperx::PlayingBlame blame(line); }
                faultNow();
            }
            case PlayingMode::FaultAfterEnd: {
                const wallpaperx::PlayingBlame blame(line);
                record.end();
                faultNow();
            }
            case PlayingMode::FaultElsewhere:
            case PlayingMode::KilledNamed: {
                const wallpaperx::PlayingBlame blame(line);
                guardLive.set_value();
                sleepForever();
            }
        }
    });
    if (mode == PlayingMode::FaultElsewhere || mode == PlayingMode::KilledNamed) {
        live.wait();
        if (mode == PlayingMode::FaultElsewhere) faultNow();
        const char byte = 'r';
        const ssize_t ignored = ::write(readyFd, &byte, 1);
        (void)ignored;
    }
    sleepForever();
}

// A forked child in `mode`, reaped. `ok` is false when the harness could not reach the state being
// graded, for runChild()'s reason.
ChildResult runPlayingChild(PlayingMode mode, const std::filesystem::path& dir, bool& ok) {
    ok = false;
    ChildResult out;
    int ready[2] = {-1, -1};
    if (::pipe2(ready, O_CLOEXEC) != 0) {
        harnessFailure("could not create the readiness pipe for a playing-record child");
        return out;
    }
    const pid_t parentBeforeFork = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(ready[0]);
        ::close(ready[1]);
        harnessFailure("fork failed");
        return out;
    }
    if (pid == 0) {
        ::close(ready[0]);
        if (!wallpaperx::testing::armAgainstParentDeath(parentBeforeFork)) {
            _exit(wallpaperx::testing::kChildArmFailedExit);
        }
        wallpaperx::testing::suppressCoreDump();
        runPlayingChildBody(mode, dir, ready[1]);
    }
    ::close(ready[1]);
    bool killedOnCue = true;
    if (mode == PlayingMode::KilledNamed) {
        struct pollfd waitOn {
            ready[0], POLLIN, 0
        };
        if (::poll(&waitOn, 1, static_cast<int>(kChildTimeout.count()) * 1000) <= 0) {
            harnessFailure("a playing-record child never reported its guard live, so nothing was killed while it was");
            killedOnCue = false;
        }
        ::kill(pid, SIGKILL);
    }
    out.reaped = waitForExit(pid, out.status);
    if (!out.reaped) {
        ::kill(pid, SIGKILL);
        int discarded = 0;
        waitForExit(pid, discarded);
        harnessFailure("a playing-record child did not exit within the harness's bound");
    }
    ::close(ready[0]);
    if (out.reaped && WIFEXITED(out.status)) {
        if (const char* fault = wallpaperx::testing::forkPreambleFault(WEXITSTATUS(out.status)); fault != nullptr) {
            harnessFailure(fault);
            return out;
        }
        if (WEXITSTATUS(out.status) == kChildRecordFailedExit) {
            harnessFailure("a playing-record child could not put its record on disk, so its outcome grades nothing");
            return out;
        }
    }
    ok = out.reaped && killedOnCue;
    return out;
}

std::string describeDied(const std::optional<wallpaperx::PlayingHistory>& history) {
    if (!history) return "no readable record";
    if (!history->died) return "names no video";
    return "names monitor " + std::to_string(history->died->monitorId) + " '" + history->died->path + "'";
}

// GOAL: S13.184, at the record -- a fatal signal on the thread whose guard names a video writes that
//       video (its monitor and path) and the signal into the playing record, beside the counts the
//       run started with, and the process still dies of the signal. False if the record names
//       nothing (the crash loop goes on), or names it without the signal.
// WHY-THIS-SHAPE: a real store to a PROT_NONE page on the guarded thread, for runStampedChecks'
//       reason; and the counts are read back too, because the handler appends to the file the
//       counts live in and a write at the wrong offset would destroy them.
// CONTROL: in backend/daemon/crash_record.cpp, the `::write(playingFd, blame->data(), ...)` in
//       onFatalSignal() deleted -> "FAIL: a fault on the thread working on a video names that video
//       in the playing record", printing "the record names no video", and "FAIL: and the signal
//       that killed it" (a signal is read only beside a name); the replacement check below then
//       reports a harness fault, having no named death to replace. Observed 2026-09-21; reverted.
void runPlayingBlameChecks() {
    ScratchDir scratch("playingnamed");
    if (!scratch.ok()) {
        notVerified("S13.184 (at the record)", "no scratch state directory could be created");
        return;
    }
    bool ok = false;
    const ChildResult result = runPlayingChild(PlayingMode::FaultNamed, scratch.path(), ok);
    if (!ok) return;
    if (kDeathSignalObservable) {
        check(diedOf(result, SIGSEGV), "a fault on the thread working on a video still ends the process with SIGSEGV");
    }
    const std::optional<wallpaperx::PlayingHistory> history = wallpaperx::readPlayingRecord(scratch.path());
    const bool named = history && history->died && history->died->monitorId == kPlayingMonitor &&
                       history->died->path == kPlayingPath;
    check(named, "a fault on the thread working on a video names that video in the playing record");
    if (!named) std::fprintf(stderr, "  the record %s\n", describeDied(history).c_str());
    check(history && history->signal && *history->signal == SIGSEGV, "and the signal that killed it");
    const bool countsKept = history && history->videos.size() == 2 && history->videos[0].monitorId == kPlayingMonitor &&
                            history->videos[0].deaths == 1 && history->videos[1].monitorId == kOtherPlayingMonitor;
    check(countsKept, "and the counts the run started with are still read back beside it");
}

// GOAL: S13.184's other half, at the record -- a death that is not the named video's names
//       nothing: a fault on another thread while the guard is live, a fault on the same thread
//       after its guard has gone (nothing was being worked on), a fault after the record was ended
//       (an orderly stop had begun), and SIGKILL, which runs no handler at all. False if any of
//       them names the video, since each would then count a death against a file that did not
//       cause it.
// WHY-THIS-SHAPE: four children, each one change away from runPlayingBlameChecks' child, which is
//       the positive reading that proves a name CAN be written in this directory layout. The kill
//       is sent once the child says its guard is live, over a pipe, for runKilledChecks' reason.
// CONTROL: in backend/daemon/crash_record.cpp, (a) the `::gettid() == g_blameTid.load(...)` term
//       deleted from onFatalSignal() -> "FAIL: a fault on another thread while a video is being
//       worked on names nothing", printing "names monitor 2"; (b) `g_blame.store(nullptr, ...)`
//       and the restore of m_previous deleted from ~PlayingBlame -> "FAIL: a fault after the guard
//       has gone names nothing"; (c) PlayingRecord::end() made a no-op -> "FAIL: a fault after the
//       record was ended names nothing"; (d) PlayingBlame's constructor made to write `line` into
//       the record at once, as a record-per-picture design would -> "FAIL: SIGKILL while a video is
//       being worked on names nothing" and the three cases above with it. Each of (a)-(c) reddened
//       its own case alone. All observed 2026-09-21; reverted.
void runPlayingNamesNothingChecks() {
    struct Case {
        PlayingMode mode;
        const char* what;
        int signal;
    };
    const Case cases[] = {
        {PlayingMode::FaultElsewhere, "a fault on another thread while a video is being worked on names nothing",
         SIGSEGV},
        {PlayingMode::FaultAfterGuard, "a fault after the guard has gone names nothing", SIGSEGV},
        {PlayingMode::FaultAfterEnd, "a fault after the record was ended names nothing", SIGSEGV},
        {PlayingMode::KilledNamed, "SIGKILL while a video is being worked on names nothing", SIGKILL},
    };
    for (const Case& c : cases) {
        ScratchDir scratch("playingnothing");
        if (!scratch.ok()) {
            notVerified("S13.184 (a death that names nothing)", "no scratch state directory could be created");
            return;
        }
        bool ok = false;
        const ChildResult result = runPlayingChild(c.mode, scratch.path(), ok);
        if (!ok) continue;
        if (kDeathSignalObservable || c.signal == SIGKILL) {
            check(diedOf(result, c.signal), std::string(c.what) + " -- the child died of the expected signal");
        }
        const std::optional<wallpaperx::PlayingHistory> history = wallpaperx::readPlayingRecord(scratch.path());
        check(history.has_value(), std::string(c.what) + " -- the record itself is still readable");
        const bool nothing = history && !history->died && !history->signal;
        check(nothing, c.what);
        if (!nothing) std::fprintf(stderr, "  the record %s\n", describeDied(history).c_str());
    }
}

// GOAL: the playing record's own rules. start() replaces whatever the last run left, blame and
//       all, so one death is never counted twice; clear() zeroes a monitor's count for the next
//       read and a clear written by another run is ignored; a monitor the run did not count
//       writes no clear; and only one record per process can start, so a second instance of the
//       daemon cannot overwrite the running one's.
// WHY-THIS-SHAPE: the record replaced is one a real death left (runPlayingChild), not a planted
//       file, so "the blame is gone" is read against a blame line the handler wrote. The
//       other-run case plants a clear line on purpose: no correct run writes one naming a
//       different run, and that is exactly the leftover the run id exists to ignore.
// CONTROL: in backend/daemon/crash_record.cpp, (a) createRecordFile()'s O_TRUNC made O_APPEND, so
//       a new record lands after the old one -> "FAIL: a new run's record replaces the last run's,
//       blame and all", printing "names monitor 2", beside the cleared-count check. Removing O_TRUNC
//       alone is NOT a control: the shorter new line leaves old bytes ahead of the blame line, the
//       reader then finds no blame, and the check stays green. (b) the `!line.starts_with(run)` term
//       deleted from readCleared() -> "FAIL: a clear written by another run is ignored"; (c) the
//       compare-exchange in PlayingRecord::start() replaced by a plain store -> "FAIL: a second
//       record in the process writes nothing while the first is live"; (d) clear()'s
//       `m_counted.count(monitorId) == 0` return deleted -> "FAIL: a monitor the run did not count
//       writes no clear"; (e) clear()'s writeAll deleted -> "FAIL: a cleared monitor carries no count
//       at the next read". All observed 2026-09-21; reverted.
void runPlayingRecordRules() {
    ScratchDir scratch("playingrules");
    if (!scratch.ok()) {
        notVerified("the playing record's own rules", "no scratch state directory could be created");
        return;
    }
    const std::filesystem::path dir = scratch.path();
    bool ok = false;
    runPlayingChild(PlayingMode::FaultNamed, dir, ok);
    if (!ok) return;
    const std::optional<wallpaperx::PlayingHistory> died = wallpaperx::readPlayingRecord(dir);
    if (!died || !died->died) {
        harnessFailure("the dying child left no named death to be replaced, so the replacement check grades nothing");
        return;
    }
    try {
        {
            wallpaperx::PlayingRecord record(dir);
            record.start({{kOtherPlayingMonitor, kOtherPlayingPath, 1}});
            const std::optional<wallpaperx::PlayingHistory> fresh = wallpaperx::readPlayingRecord(dir);
            check(fresh && !fresh->died && fresh->videos.size() == 1 && fresh->videos[0].monitorId == kOtherPlayingMonitor,
                  "a new run's record replaces the last run's, blame and all");
            if (fresh && fresh->died) std::fprintf(stderr, "  the record %s\n", describeDied(fresh).c_str());

            record.clear(kPlayingMonitor);  // not counted by this run
            std::error_code ec;
            check(!std::filesystem::exists(dir / "playing-cleared", ec), "a monitor the run did not count writes no clear");

            record.clear(kOtherPlayingMonitor);
            const std::optional<wallpaperx::PlayingHistory> cleared = wallpaperx::readPlayingRecord(dir);
            check(cleared && cleared->videos.empty(), "a cleared monitor carries no count at the next read");

            ScratchDir second("playingsecond");
            wallpaperx::PlayingRecord other(second.path());
            other.start({{kPlayingMonitor, kPlayingPath, 1}});
            check(!std::filesystem::exists(wallpaperx::playingRecordPath(second.path()), ec),
                  "a second record in the process writes nothing while the first is live");
        }
        {
            wallpaperx::PlayingRecord record(dir);
            record.start({{kPlayingMonitor, kPlayingPath, 1}, {kOtherPlayingMonitor, kOtherPlayingPath, 1}});
        }
        // Another run's id of the same shape as this one's -- the same length, one digit different
        // -- so the line is rejected for naming another run and not for being malformed.
        std::string run;
        {
            std::ifstream in(wallpaperx::playingRecordPath(dir));
            std::string first;
            std::getline(in, first);
            const nlohmann::json root = nlohmann::json::parse(first, nullptr, false);
            if (root.is_object() && root.contains("run") && root["run"].is_string()) run = root["run"].get<std::string>();
        }
        if (run.empty()) {
            harnessFailure("the playing record names no run, so a clear from another run cannot be planted");
            return;
        }
        std::string otherRun = run;
        otherRun.back() = otherRun.back() == '0' ? '1' : '0';
        writeText(dir / "playing-cleared", otherRun + " " + std::to_string(kPlayingMonitor) + "\n");
        const std::optional<wallpaperx::PlayingHistory> foreign = wallpaperx::readPlayingRecord(dir);
        check(foreign && foreign->videos.size() == 2, "a clear written by another run is ignored");
        writeText(dir / "playing-cleared", run + " " + std::to_string(kPlayingMonitor) + "\n");
        const std::optional<wallpaperx::PlayingHistory> own = wallpaperx::readPlayingRecord(dir);
        check(own && own->videos.size() == 1, "while the same line naming this run's own id does clear it");
    } catch (const std::exception& e) {
        check(false, std::string("the playing record threw: ") + e.what());
    }
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "crash_record_test";

    // Announced once, here, rather than at each of the four sites it disables: a run that
    // silently stopped checking how the child died would look identical to one that checked.
    // The record checks are unaffected -- the handler writes its stamp before it chains to
    // whatever handler was there, and under a sanitizer that is the sanitizer's own.
    if (!kDeathSignalObservable) {
        notVerified("the death-signal half of S13.11, S13.12 and S13.18",
                    "this build is sanitized, and the sanitizer's own handler chooses the process's exit "
                    "status, so how a child died says nothing about the daemon's handler");
    }

    try {
        runStampedChecks(SIGSEGV, Mode::FaultOnApplying, "a memory fault");
        runStampedChecks(SIGABRT, Mode::AbortOnApplying, "an abort");
        runOtherThreadChecks();
        runKilledChecks();
        runCleanExitChecks();
        runChainingChecks();
        runOnDiskChecks();
        runPlayingBlameChecks();
        runPlayingNamesNothingChecks();
        runPlayingRecordRules();
    } catch (const std::exception& e) {
        harnessFailure(std::string("the run ended in an unhandled exception: ") + e.what());
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
