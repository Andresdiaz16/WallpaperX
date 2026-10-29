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
#include "daemon/log_pump.h"
#include "daemon/resume_record.h"

#include "support/forked_child.h"
#include "tests/client/scratch_dir.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
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
#include <new>
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

// In log_writer_fault.cpp: the next memrchr call, which only the log writer makes, faults.
void wallpaperxTestFaultInLogWriter();

#if defined(WALLPAPERX_FAKE_SANITIZER_DEATH)
// In sanitizer_death_fake.cpp: the death callback the log writer registered, as a sanitizer holds it.
void (*wallpaperxTestDeathCallback())();
#endif

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
// The child could not set up the daemon's log writer, or the stalled stream it writes to.
constexpr int kChildLogWriterFailedExit = 119;

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
    // With the daemon's log writer running: a memory fault on the writer's own thread.
    FaultOnLogWriter,
    // With the log writer running and its destination full, so the handler's flush waits: SIGABRT
    // on another thread, and the applying thread starts a new record once the handler has begun.
    AbortElsewhereWhileStalled,
};

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::FaultOnApplying: return "a memory fault on the applying thread";
        case Mode::AbortOnApplying: return "an abort on the applying thread";
        case Mode::AbortElsewhere: return "an abort on another thread";
        case Mode::KilledMidApply: return "killed mid-apply";
        case Mode::CleanExit: return "a clean exit";
        case Mode::ChainPrevious: return "an abort chained to a previous handler";
        case Mode::FaultOnLogWriter: return "a fault on the log writer's thread";
        case Mode::AbortElsewhereWhileStalled: return "a crash elsewhere with the output stalled";
    }
    return "an unnamed mode";
}

// Descriptors 1 and 2 on a pipe that is full and never read, the reader kept open so a write
// meets a full pipe rather than a closed one. False if it could not be filled.
bool stallOwnOutput() {
    int ends[2];
    if (::pipe2(ends, O_CLOEXEC) != 0) return false;
    if (::fcntl(ends[1], F_SETFL, O_NONBLOCK) != 0) return false;
    const char filler[4096] = {};
    for (const size_t size : {sizeof(filler), size_t{1}}) {
        for (;;) {
            const ssize_t n = ::write(ends[1], filler, size);
            if (n > 0) continue;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) break;
            return false;
        }
    }
    if (::fcntl(ends[1], F_SETFL, 0) != 0) return false;
    return ::dup2(ends[1], STDOUT_FILENO) >= 0 && ::dup2(ends[1], STDERR_FILENO) >= 0;
}

bool outputToDevNull() {
    const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    return null >= 0 && ::dup2(null, STDOUT_FILENO) >= 0 && ::dup2(null, STDERR_FILENO) >= 0;
}

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

// Shared with the forked child: set once its own setup is done and the handlers are installed, so
// that from then on only the code under test -- the handler, the record -- decides when it ends.
// A child that never ends before this is the harness's fault; after it, the handler's.
class StagedFlag {
public:
    StagedFlag() {
        void* page = ::mmap(nullptr, sizeof(std::atomic<int>), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (page != MAP_FAILED) m_flag = new (page) std::atomic<int>(0);
    }
    ~StagedFlag() {
        if (m_flag != nullptr) ::munmap(m_flag, sizeof(std::atomic<int>));
    }
    StagedFlag(const StagedFlag&) = delete;
    StagedFlag& operator=(const StagedFlag&) = delete;
    bool ok() const { return m_flag != nullptr; }
    std::atomic<int>* get() const { return m_flag; }
    bool isSet() const { return m_flag != nullptr && m_flag->load() == 1; }

private:
    std::atomic<int>* m_flag = nullptr;
};

// The running child's flag, set by the parent just before fork() so the child inherits it.
std::atomic<int>* g_staged = nullptr;

void markStaged() {
    if (g_staged != nullptr) g_staged->store(1);
}

// Why a child that had to be killed was: before markStaged(), the harness's own setup stalled; after
// it, the code under test kept the process alive.
void reportNeverEnded(bool staged, const std::string& what) {
    if (!staged) {
        harnessFailure(what + " stalled in this harness's own setup, before its handlers were installed");
        return;
    }
    check(false, (what + " ended within 20 s of its handlers taking over: it never did, so the crash handler (or the "
                         "record it held) kept the process alive")
                     .c_str());
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
    // As the daemon's main() does: the log writer first, then the handlers.
    if (mode == Mode::FaultOnLogWriter || mode == Mode::AbortElsewhereWhileStalled) {
        const bool routed = mode == Mode::FaultOnLogWriter ? outputToDevNull() : stallOwnOutput();
        if (!routed || !wallpaperx::logpump::install()) _exit(kChildLogWriterFailedExit);
    }
    installFatalSignalHandlers();
    markStaged();

    std::promise<void> recordLive;
    std::future<void> live = recordLive.get_future();

    std::thread applying([&] {
        std::optional<ApplyingRecord> record(std::in_place, dir, kMonitorId, kType, kPath);
        // Nothing below says anything unless the record actually reached the disk. Exiting
        // with a code of its own keeps "the harness could not set this up" apart from "the
        // handler did the wrong thing", which is otherwise the same missing file.
        std::error_code ec;
        const std::filesystem::path file = crashRecordPath(dir, kMonitorId);
        if (!std::filesystem::exists(file, ec)) _exit(kChildRecordFailedExit);
        recordLive.set_value();

        if (mode == Mode::AbortElsewhereWhileStalled) {
            // The handler removing this record is the sign it has begun; the next record is then
            // started while its flush holds the process open. No deadline of its own: the parent's
            // kill bounds it, so 'b' only ever means the record was removed first.
            while (std::filesystem::exists(file, ec)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            record.reset();
            record.emplace(dir, kMonitorId, kType, kPath);
            const char byte = 'b';
            const ssize_t ignored = ::write(readyFd, &byte, 1);
            (void)ignored;
            sleepForever();
        }

        switch (mode) {
            case Mode::FaultOnApplying:
                faultNow();
            case Mode::AbortOnApplying:
            case Mode::ChainPrevious:
                std::abort();
            case Mode::AbortElsewhere:
            case Mode::KilledMidApply:
            case Mode::FaultOnLogWriter:
            case Mode::AbortElsewhereWhileStalled:
                sleepForever();  // holds the record open for whatever kills this process
            case Mode::CleanExit:
                break;
        }
    });

    live.wait();
    switch (mode) {
        case Mode::AbortElsewhereWhileStalled:
            // Queued behind the full destination, so the handler's flush has a line it cannot deliver.
            std::printf("a line the destination will not take\n");
            std::fflush(stdout);
            [[fallthrough]];
        case Mode::AbortElsewhere:
            std::abort();  // the main thread dies; the applying thread is still holding a record
        case Mode::FaultOnLogWriter:
            wallpaperxTestFaultInLogWriter();
            std::printf("a line for the log writer to write\n");
            std::fflush(stdout);
            sleepForever();
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
    bool laterRecordAttempted = false;  // AbortElsewhereWhileStalled: a record was begun inside the handler
};

// Runs one child to its end and returns how it died. `ok` is false when there is nothing left to
// grade: the harness could not get the child to the point being graded, or the child never ended,
// which is already reported (as a verdict once its handlers were in charge).
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

    StagedFlag staged;
    if (!staged.ok()) {
        ::close(ready[0]);
        ::close(ready[1]);
        ::close(chain[0]);
        ::close(chain[1]);
        harnessFailure("could not map the flag a forked child reports its setup through");
        return out;
    }
    g_staged = staged.get();

    // A child inherits unflushed stdio, and one whose output is a full pipe would block writing it.
    std::fflush(nullptr);
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

    // GOAL: X.3 -- a child whose crash handler never finishes is the handler's failure, not this
    //       harness's: every mode but KilledMidApply leaves the ending to the handler (or, for
    //       CleanExit, to the record's destructor).
    // WHY-THIS-SHAPE: the staged flag, not the mode alone, decides the blame: a child stuck in
    //       its own setup before the handlers exist says nothing about them. KilledMidApply was
    //       SIGKILLed by this harness, so one that outlives that is the host's, never the handler's.
    // CONTROL: in backend/daemon/crash_record.cpp via control_run.sh 2026-09-27, reverted:
    //       onFatalSignal's flush given an hour -> "a forked child (a crash elsewhere with the
    //       output stalled) ended within 20 s of its handlers taking over" red, alone. By hand,
    //       reverted: an endless sleep before the stalled mode's handlers are installed ->
    //       "FAIL (harness): ... stalled in this harness's own setup", no FAIL.
    out.reaped = waitForExit(pid, out.status);
    if (!out.reaped) {
        ::kill(pid, SIGKILL);
        int discarded = 0;
        waitForExit(pid, discarded);
        const std::string what = std::string("a forked child (") + modeName(mode) + ")";
        if (mode == Mode::KilledMidApply) {
            harnessFailure(what + " outlived the SIGKILL this harness sent it");
        } else {
            reportNeverEnded(staged.isSet(), what);
        }
    }
    g_staged = nullptr;

    // After the reap, so the byte is either in the pipe or was never written.
    char byte = 0;
    int flags = ::fcntl(chain[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(chain[0], F_SETFL, flags | O_NONBLOCK);
    out.previousHandlerRan = ::read(chain[0], &byte, 1) == 1;
    if (mode == Mode::AbortElsewhereWhileStalled) {
        flags = ::fcntl(ready[0], F_GETFL, 0);
        if (flags >= 0) ::fcntl(ready[0], F_SETFL, flags | O_NONBLOCK);
        out.laterRecordAttempted = ::read(ready[0], &byte, 1) == 1 && byte == 'b';
    }

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
        if (WEXITSTATUS(out.status) == kChildLogWriterFailedExit) {
            harnessFailure("a forked child could not route its output or start the log writer");
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
// CONTROL: in backend/daemon/crash_record.cpp, the `::write(fd, line, n)` in writeSignalLine()
//       deleted -> "the record left by a memory fault on the applying thread is stamped with it"
//       and "the reason a client reads ... names that signal", each for the fault and the abort,
//       with the chaining phase's stamp line and the playing record's "and the signal that killed
//       it": six. Observed 2026-09-25 via control_run.sh; reverted.
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
// CONTROL: in backend/daemon/crash_record.cpp, the `::unlink(path)` that unlinkPublished()
//       performs once its compare-exchange confirms the copied name replaced by `(void)path` ->
//       "a crash on a thread that is not applying leaves no record" and "...leaves no file on
//       disk" red, with the log-writer fault phase's line; the stalled phase below is NOT VERIFIED
//       under it, since the record it waits to see removed never is. Observed 2026-09-25 via
//       control_run.sh; reverted.
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

// GOAL: S14.33 -- a memory fault on the log writer's own thread (backend/daemon/log_pump.cpp)
//       while a wallpaper is being applied leaves no applying record. False if that thread keeps
//       the fault signals blocked: the kernel then kills the process with no handler run, and the
//       next start refuses a wallpaper that did nothing.
// WHY-THIS-SHAPE: the fault is a real store to a PROT_NONE page inside memrchr, which only the log
//       writer calls here (log_writer_fault.cpp takes libc's place). tgkill(SIGSEGV) at that thread
//       would not do: a blocked signal sent from outside stays pending rather than killing.
// CONTROL: in backend/daemon/log_pump.cpp, SIGSEGV left in the writer thread's blocked set (dropped
//       from install()'s sigdelset list) -> "a fault on the log writer's thread while a wallpaper
//       is applied leaves no record" red, alone. Observed 2026-09-25 via control_run.sh; reverted.
void runLogWriterFaultChecks() {
    ScratchDir scratch("crashwriter");
    if (!scratch.ok()) {
        notVerified("S14.33", "no scratch state directory could be created");
        return;
    }
    bool ok = false;
    const ChildResult result = runChild(Mode::FaultOnLogWriter, scratch.path(), ok);
    if (!ok) return;
    if (kDeathSignalObservable) {
        check(diedOf(result, SIGSEGV), "a fault on the log writer's thread still ends the process with SIGSEGV");
        if (!diedOf(result, SIGSEGV)) std::fprintf(stderr, "  child died: %s\n", howItDied(result).c_str());
    }
    std::error_code ec;
    check(!std::filesystem::exists(crashRecordPath(scratch.path(), kMonitorId), ec),
          "a fault on the log writer's thread while a wallpaper is applied leaves no record");
}

// GOAL: S14.34 -- a fatal signal on a thread that is not applying, with the output stalled so the
//       handler's 250 ms flush runs its full length, leaves no applying record -- neither the one
//       live at the signal nor one the applying thread starts while the handler flushes. False if
//       the handler only removes the live record: the render thread goes on applying, and the
//       process dies holding the next one.
// WHY-THIS-SHAPE: the applying thread starts its next record when it sees the live one removed,
//       which is the handler's first act, so the new record provably begins inside the flush and
//       not before the signal or after the death; a loop of records timed against the 250 ms
//       would be a bet on the scheduler. It reports the attempt through a pipe; a child that
//       never made it is NOT VERIFIED, not a pass.
// CONTROL: in backend/daemon/crash_record.cpp, onFatalSignal's other-thread branch made a bare
//       `unlinkPublished(generation)` instead of stopBlamingWallpapers() -> this phase's "leaves no
//       record, even one begun during the handler's flush" red, alone; S13.12's phase above stays
//       green, since the live record is still removed. Observed 2026-09-27 via control_run.sh;
//       reverted. By hand, reverted: the child's 'b' byte made 'x' -> NOT VERIFIED, no FAIL.
void runStalledOtherThreadChecks() {
    ScratchDir scratch("crashstalled");
    if (!scratch.ok()) {
        notVerified("S14.34", "no scratch state directory could be created");
        return;
    }
    bool ok = false;
    const ChildResult result = runChild(Mode::AbortElsewhereWhileStalled, scratch.path(), ok);
    if (!ok) return;
    if (!result.laterRecordAttempted) {
        notVerified("S14.34", "the applying thread never began a record inside the handler's flush, so nothing "
                              "was staged for the handler to refuse");
        return;
    }
    if (kDeathSignalObservable) {
        check(diedOf(result, SIGABRT), "a crash elsewhere with the output stalled still ends the process with SIGABRT");
        if (!diedOf(result, SIGABRT)) std::fprintf(stderr, "  child died: %s\n", howItDied(result).c_str());
    }
    std::error_code ec;
    check(!std::filesystem::exists(crashRecordPath(scratch.path(), kMonitorId), ec),
          "a crash on a thread that is not applying, with the output stalled, leaves no record, even one begun "
          "during the handler's flush");
}

// ---------------------------------------------------------------------------- //
// A sanitizer's report on a stalled output: a handler installed before the daemon's that writes a
// report to descriptor 2, runs the death callback the log writer registered, and exits -- the
// order a sanitizer's Die() runs them in.
// ---------------------------------------------------------------------------- //

#if defined(WALLPAPERX_FAKE_SANITIZER_DEATH)

// More than a pipe holds (64 KiB) and less than the log writer's queue (256 KiB), in lines a pipe
// takes whole.
constexpr size_t kReportLines = 200;
constexpr size_t kReportLineSize = 1024;
constexpr int kFakeSanitizerExit = 42;
// The crash spends at most two 250 ms flushes (the daemon's handler's, then the death callback's);
// the rest is margin for a loaded runner. A handler that blocks on the destination never ends.
constexpr auto kReportCrashEnd = std::chrono::milliseconds(1250);
// After the report is written: late enough that a process skipping the death callback's flush has
// exited, early enough to be inside the 250 ms that flush waits.
constexpr auto kResumeAfterReport = std::chrono::milliseconds(50);
constexpr auto kResumeLatest = std::chrono::milliseconds(200);

char g_report[kReportLines * kReportLineSize];
int g_reportNotifyFd = -1;

std::string reportLine(size_t i) {
    char head[32];
    const int n = std::snprintf(head, sizeof(head), "==fakesan== %04zu ", i);
    return std::string(head, static_cast<size_t>(n)) + std::string(kReportLineSize - 1 - static_cast<size_t>(n), 'r');
}

void notifyParent(char byte) {
    const ssize_t ignored = ::write(g_reportNotifyFd, &byte, 1);
    (void)ignored;
}

void fakeSanitizerHandler(int) {
    for (size_t done = 0; done < sizeof(g_report);) {
        const ssize_t n = ::write(STDERR_FILENO, g_report + done, sizeof(g_report) - done);
        if (n > 0) done += static_cast<size_t>(n);
        else if (n < 0 && errno != EINTR) break;
    }
    notifyParent('w');
    if (void (*callback)() = wallpaperxTestDeathCallback(); callback != nullptr) callback();
    _exit(kFakeSanitizerExit);
}

[[noreturn]] void runReportChildBody(int destination, int notifyFd) {
    for (size_t i = 0; i < kReportLines; ++i) {
        const std::string line = reportLine(i) + "\n";
        std::memcpy(g_report + i * kReportLineSize, line.data(), kReportLineSize);
    }
    g_reportNotifyFd = notifyFd;
    // Blocking, as a destination a process is started with usually is: what release() hands back.
    if (::dup2(destination, STDOUT_FILENO) < 0 || ::dup2(destination, STDERR_FILENO) < 0) {
        _exit(kChildLogWriterFailedExit);
    }
    // As a sanitizer's runtime does before main(): its handler is in place before the daemon's.
    struct sigaction previous {};
    previous.sa_handler = fakeSanitizerHandler;
    sigemptyset(&previous.sa_mask);
    ::sigaction(SIGSEGV, &previous, nullptr);
    if (!wallpaperx::logpump::install()) _exit(kChildLogWriterFailedExit);
    installFatalSignalHandlers();
    markStaged();
    notifyParent('f');
    faultNow();
}

struct ReportRun {
    bool ok = false;  // false: already reported
    std::optional<std::chrono::steady_clock::duration> endedAfterFault;
    int status = 0;
    std::optional<std::chrono::steady_clock::duration> resumedAfterReport;  // nothing: never written
    std::string delivered;
};

bool awaitByte(int fd, char expected, std::chrono::milliseconds limit) {
    struct pollfd waitOn {
        fd, POLLIN, 0
    };
    char byte = 0;
    return ::poll(&waitOn, 1, static_cast<int>(limit.count())) == 1 && ::read(fd, &byte, 1) == 1 && byte == expected;
}

// One crash with the destination -- a pipe this process holds the reader of -- full before the
// child starts. Resumed `kResumeAfterReport` after the report is written when `resume`; never read
// otherwise.
ReportRun runReportChild(bool resume) {
    using std::chrono::steady_clock;
    ReportRun out;
    int dest[2] = {-1, -1};
    int notify[2] = {-1, -1};
    if (::pipe2(dest, O_CLOEXEC) != 0 || ::pipe2(notify, O_CLOEXEC) != 0) {
        harnessFailure("could not create the destination or notify pipe for a report child");
        return out;
    }
    const auto closeAll = [&] {
        for (int* fd : {&dest[0], &dest[1], &notify[0], &notify[1]}) {
            if (*fd >= 0) ::close(*fd);
            *fd = -1;
        }
    };
    // Filled through a description of its own, so the one the child inherits stays blocking.
    {
        const std::string self = "/proc/self/fd/" + std::to_string(dest[1]);
        const int filler = ::open(self.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        const std::string lines(4096, '\n');
        bool full = false;
        for (size_t size : {lines.size(), size_t{1}}) {
            for (;;) {
                const ssize_t n = filler < 0 ? -1 : ::write(filler, lines.data(), size);
                if (n > 0) continue;
                full = n < 0 && errno == EAGAIN;
                break;
            }
        }
        if (filler >= 0) ::close(filler);
        if (!full) {
            closeAll();
            harnessFailure("the report child's destination pipe could not be filled");
            return out;
        }
    }
    StagedFlag staged;
    if (!staged.ok()) {
        closeAll();
        harnessFailure("could not map the flag a report child reports its setup through");
        return out;
    }
    g_staged = staged.get();
    std::fflush(nullptr);
    const pid_t parentBeforeFork = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        closeAll();
        harnessFailure("fork failed");
        return out;
    }
    if (pid == 0) {
        ::close(dest[0]);
        ::close(notify[0]);
        if (!wallpaperx::testing::armAgainstParentDeath(parentBeforeFork)) {
            _exit(wallpaperx::testing::kChildArmFailedExit);
        }
        wallpaperx::testing::suppressCoreDump();
        runReportChildBody(dest[1], notify[1]);
    }
    ::close(dest[1]);
    ::close(notify[1]);
    dest[1] = notify[1] = -1;
    g_staged = nullptr;

    const auto killAndReap = [&] {
        ::kill(pid, SIGKILL);
        int discarded = 0;
        waitForExit(pid, discarded);
    };
    if (!awaitByte(notify[0], 'f', std::chrono::duration_cast<std::chrono::milliseconds>(kChildTimeout))) {
        killAndReap();
        closeAll();
        harnessFailure("a report child never reached its fault: it could not set up, or stalled doing so");
        return out;
    }
    const auto faultAt = steady_clock::now();
    const auto giveUpAt = faultAt + std::chrono::seconds(5);

    if (resume) {
        if (awaitByte(notify[0], 'w', std::chrono::seconds(5))) {
            const auto writtenAt = steady_clock::now();
            std::this_thread::sleep_for(kResumeAfterReport);
            out.resumedAfterReport = steady_clock::now() - writtenAt;
            char buffer[65536];
            for (;;) {
                struct pollfd readable {
                    dest[0], POLLIN, 0
                };
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(giveUpAt - steady_clock::now());
                if (left.count() <= 0 || ::poll(&readable, 1, static_cast<int>(left.count())) != 1) break;
                const ssize_t n = ::read(dest[0], buffer, sizeof(buffer));
                if (n <= 0) break;  // end of file: every writer, the child included, is gone
                out.delivered.append(buffer, static_cast<size_t>(n));
            }
        }
    }
    for (;;) {
        const pid_t done = ::waitpid(pid, &out.status, WNOHANG);
        if (done == pid) {
            out.endedAfterFault = steady_clock::now() - faultAt;
            break;
        }
        if (done < 0 && errno != EINTR) break;
        if (steady_clock::now() >= giveUpAt) {
            killAndReap();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    closeAll();
    out.ok = true;
    return out;
}

// GOAL: S14.41 -- a crash with the output stalled throughout, whose previous handler (a sanitizer's)
//       writes a report larger than a pipe holds to descriptor 2, still ends within 1250 ms of the
//       fault, as that handler chose. False if the daemon's handler hands the process's blocking
//       descriptors back before chaining: the report's write then blocks on the full destination.
// WHY-THIS-SHAPE: a stand-in handler, not a sanitizer: only a sanitized build has one, and this
//       project has no AddressSanitizer tree to register the check in. It does what a sanitizer's
//       Die() does, in that order: report to descriptor 2, the registered death callback, _exit.
//       Timed from the child's own mark just before the fault, never from the parent's fork.
// CONTROL: in backend/daemon/crash_record.cpp via control_run.sh 2026-09-27, reverted:
//       onFatalSignal's `logpump::flush(logpump::kExitFlush)` made `logpump::release()` ->
//       "ends within 1250 ms of the fault" and "as that handler chose" red; the resumed phase
//       below NOT VERIFIED (its report is never written).
// COVERAGE-ASSERT: the exit status 42 is the stand-in's own, so a green run proves the chain
//       reached it and it ran to its end.
void runReportStalledThroughoutChecks() {
    const ReportRun run = runReportChild(false);
    if (!run.ok) return;
    const bool inTime = run.endedAfterFault && *run.endedAfterFault <= kReportCrashEnd;
    check(inTime, "a crash whose previous handler writes a 200 KiB report, with the output stalled throughout, "
                  "ends within 1250 ms of the fault");
    const bool asChosen = run.endedAfterFault && WIFEXITED(run.status) && WEXITSTATUS(run.status) == kFakeSanitizerExit;
    check(asChosen, "and ends as that handler chose (its exit 42)");
    std::printf("crash_record_test: report on a stalled output: %s\n",
                run.endedAfterFault
                    ? ("ended " + std::to_string(std::chrono::duration<double>(*run.endedAfterFault).count()) +
                       " s after the fault")
                          .c_str()
                    : "still running 5 s after the fault, killed");
}

// GOAL: S14.42 -- a destination that resumes inside the death callback's 250 ms flush receives the
//       whole report the previous handler wrote after the daemon's handler had given up waiting.
//       False if the death callback does not flush: the process exits with the report still queued.
// WHY-THIS-SHAPE: resumed 50 ms after the stand-in says its report is written, not at a fixed time
//       after the fault: the daemon's handler has flushed by then, so only the death callback's
//       flush can deliver it, and a process without one has already exited. A resume later than
//       200 ms after the report is NOT VERIFIED rather than a verdict.
// CONTROL: in backend/daemon/log_pump.cpp via control_run.sh 2026-09-27, reverted: the callback
//       install() registers made `[] {}` -> "receives all 200 lines of the report" red, alone.
void runReportResumedChecks() {
    const ReportRun run = runReportChild(true);
    if (!run.ok) return;
    if (!run.resumedAfterReport) {
        notVerified("S14.42", "the stand-in handler never wrote its report within 5 s of the fault (S14.41 grades that)");
        return;
    }
    if (*run.resumedAfterReport > kResumeLatest) {
        notVerified("S14.42", "this test resumed the destination " +
                                  std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     *run.resumedAfterReport)
                                                     .count()) +
                                  " ms after the report, too late to be sure it was inside the 250 ms flush");
        return;
    }
    std::vector<std::string> seen;
    for (size_t at = 0; at < run.delivered.size();) {
        size_t end = run.delivered.find('\n', at);
        if (end == std::string::npos) end = run.delivered.size();
        std::string line = run.delivered.substr(at, end - at);
        if (line.rfind("==fakesan== ", 0) == 0) seen.push_back(std::move(line));
        at = end + 1;
    }
    bool whole = seen.size() == kReportLines;
    for (size_t i = 0; whole && i < kReportLines; ++i) whole = seen[i] == reportLine(i);
    check(whole, "a destination resumed inside the death callback's flush receives all 200 lines of the report, "
                 "whole and in order");
    if (!whole) std::fprintf(stderr, "  %zu report line(s) arrived\n", seen.size());
}

#endif  // WALLPAPERX_FAKE_SANITIZER_DEATH

void runReportChecks() {
#if defined(WALLPAPERX_FAKE_SANITIZER_DEATH)
    runReportStalledThroughoutChecks();
    runReportResumedChecks();
#else
    notVerified("S14.41, S14.42", "this build is sanitized: its runtime's own death callback would clash with the "
                                  "stand-in these checks register");
#endif
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
//       before its write loop (`if (true) { ::close(fd); return; }`) -> "a process killed while
//       applying leaves a record" red, with the six other lines that read an applying record back
//       (stampedFault, stampedAbort, otherThread, logWriterFault, cleanExit, chaining); the
//       playing-record phases, playingBothBlamed among them, stay green. Re-run 2026-10-11 via
//       control_run.sh; reverted.
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

// GOAL: S13.16, at the record -- it exists only while the wallpaper is being applied: a run that finishes
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
//       block in onFatalSignal() made `if (false)` -> "FAIL: the handler installed before this one
//       still runs", alone. Observed 2026-09-25 via control_run.sh; reverted.
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

// GOAL: S13.19 -- a file in the state directory is only read as a record of THIS monitor's last apply.
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
//       read as this one's"; O_NOFOLLOW dropped from the open in readRecordFile
//       (backend/daemon/record_file.cpp, shared with the resume record's reader) -> "FAIL: a
//       symlink in place of a record is not followed", alone; re-run 2026-10-24. Both reverted. Via control_batch.sh
//       2026-10-14, each alone in readCrashRecord unless named, reverted: the no-newline return
//       deleted -> "no line terminator"; the path term made always-true -> "a record with no path";
//       kMaxRecordBytes 64 KiB -> 1 MiB -> "far larger"; crashRecordPath's empty-directory return
//       deleted -> "no state directory there is no record path"; ApplyingRecord's isValidUtf8 guard
//       deleted -> "writing a record threw".
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

        // dump() throws on invalid UTF-8, and this constructor runs on the render thread inside
        // the block whose exceptions are graded as the wallpaper failing to draw.
        // GOAL: S13.227 -- a wallpaper whose path is not valid UTF-8 is applied with no record on disk.
        // WHY-THIS-SHAPE: read while the record is alive: its destructor unlinks the file, so a check
        //       after the scope passes even for a record written with the bytes replaced.
        // CONTROL: in backend/daemon/crash_record.cpp, ApplyingRecord's isValidUtf8 guard deleted and its
        //       dump() given error_handler_t::replace -> "FAIL: a path that is not valid UTF-8 writes no
        //       record rather than a half-written one"; control_batch.sh 2026-10-15, reverted.
        {
            ApplyingRecord record(dir, 6, kType, "/pictures/\xff\xfe.png");
            std::error_code ec;
            check(!std::filesystem::exists(crashRecordPath(dir, 6), ec),
                  "a path that is not valid UTF-8 writes no record rather than a half-written one");
        }

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

    writeText(crashRecordPath(dir, 7), "{\"monitor_id\":7,\"type\":\"static\"}\n");
    check(!readCrashRecord(dir, 7).has_value(), "a record with no path is not read as a record");

    writeText(crashRecordPath(dir, 7), valid + "\n" + std::string(128 * 1024, 'x'));
    check(!readCrashRecord(dir, 7).has_value(), "a file far larger than any record we write is not read as one");

    std::error_code ec;
    writeText(dir / "record-target.json", valid + "\n");
    std::filesystem::create_symlink(dir / "record-target.json", crashRecordPath(dir, 8), ec);
    if (ec) {
        notVerified("the symlink guard", "this filesystem would not create a symlink");
    } else {
        // The target is rewritten to name monitor 8, the id asked for, so a followed symlink would
        // pass the id check -- the id check must not be what rejects it.
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
    // Two threads each hold a guard -- as the render and sound threads do -- and the one whose guard
    // was made FIRST faults: monitor 2's (BothFirstFaults) or monitor 6's (BothOtherFirstFaults).
    BothFirstFaults,
    BothOtherFirstFaults,
};

const char* playingModeName(PlayingMode mode) {
    switch (mode) {
        case PlayingMode::FaultNamed: return "a fault on the guarded thread";
        case PlayingMode::FaultElsewhere: return "a fault beside the guarded thread";
        case PlayingMode::FaultAfterGuard: return "a fault after the guard ended";
        case PlayingMode::FaultAfterEnd: return "a fault after the record ended";
        case PlayingMode::KilledNamed: return "killed while guarded";
        case PlayingMode::BothFirstFaults: return "a fault on the first of two guarded threads";
        case PlayingMode::BothOtherFirstFaults: return "a fault on the first of two guarded threads, the other way round";
    }
    return "an unnamed mode";
}

[[noreturn]] void runPlayingChildBody(PlayingMode mode, const std::filesystem::path& dir, int readyFd) {
    installFatalSignalHandlers();
    markStaged();
    wallpaperx::PlayingRecord record(dir);
    record.start({{kPlayingMonitor, kPlayingPath, 1}, {kOtherPlayingMonitor, kOtherPlayingPath, 0}});
    std::error_code ec;
    if (!std::filesystem::exists(wallpaperx::playingRecordPath(dir), ec)) _exit(kChildRecordFailedExit);
    const std::string line = wallpaperx::playingBlameLine(kPlayingMonitor, kPlayingPath);

    if (mode == PlayingMode::BothFirstFaults || mode == PlayingMode::BothOtherFirstFaults) {
        const std::string other = wallpaperx::playingBlameLine(kOtherPlayingMonitor, kOtherPlayingPath);
        const std::string& first = mode == PlayingMode::BothFirstFaults ? line : other;
        const std::string& second = mode == PlayingMode::BothFirstFaults ? other : line;
        std::promise<void> firstLive, secondLive;
        std::future<void> firstUp = firstLive.get_future();
        std::shared_future<void> secondUp = secondLive.get_future().share();
        std::thread faulting([&] {
            const wallpaperx::PlayingBlame blame(first);
            firstLive.set_value();
            secondUp.wait();
            faultNow();
        });
        firstUp.wait();
        std::thread holding([&] {
            const wallpaperx::PlayingBlame blame(second);
            secondLive.set_value();
            sleepForever();
        });
        sleepForever();
    }

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
            case PlayingMode::BothFirstFaults:
            case PlayingMode::BothOtherFirstFaults: sleepForever();  // handled above
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
    StagedFlag staged;
    if (!staged.ok()) {
        ::close(ready[0]);
        ::close(ready[1]);
        harnessFailure("could not map the flag a playing-record child reports its setup through");
        return out;
    }
    g_staged = staged.get();
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
    // runChild's attribution. CONTROL: in backend/daemon/crash_record.cpp via control_run.sh
    //       2026-10-11, reverted: onFatalSignal made to pause for good after writing the blame line
    //       -> "a playing-record child (a fault on the guarded thread) ended within 20 s of its
    //       handlers taking over" red in phases playingBlame and playingRecordRules, and "(a fault on
    //       the first of two guarded threads)" and "(..., the other way round)" in playingBothBlamed.
    out.reaped = waitForExit(pid, out.status);
    if (!out.reaped) {
        ::kill(pid, SIGKILL);
        int discarded = 0;
        waitForExit(pid, discarded);
        const std::string what = std::string("a playing-record child (") + playingModeName(mode) + ")";
        if (mode == PlayingMode::KilledNamed) {
            harnessFailure(what + " outlived the SIGKILL this harness sent it");
        } else {
            reportNeverEnded(staged.isSet(), what);
        }
    }
    g_staged = nullptr;
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
//       reports a harness fault, having no named death to replace. Observed 2026-09-25 via
//       control_run.sh, again via control_batch.sh 2026-10-15; reverted.
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
// CONTROL: in backend/daemon/crash_record.cpp, (a) `thread_local` dropped from t_blame (one slot for
//       the process) -> "FAIL: a fault on another thread while a video is being worked on names
//       nothing", printing "names monitor 2"; (b) ~PlayingBlame no longer restoring t_blame ->
//       "FAIL: a fault after the guard has gone names nothing"; (c) PlayingRecord::end() made a
//       no-op -> "FAIL: a fault after the record was ended names nothing"; (d) PlayingBlame's
//       constructor made to write `line` into the record at once, as a record-per-picture design
//       would -> "FAIL: SIGKILL while a video is being worked on names nothing" and the three cases
//       above with it. Each of (a)-(c) reddened its own case alone, (a)-(c) again 2026-10-15. (a) and (b) observed 2026-10-11
//       via control_run.sh against the per-thread t_blame, (c) and (d) 2026-09-21; reverted.
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

// GOAL: S15.514 -- two threads working on two videos at once (the render thread and the sound thread)
//       each name their own: a fatal signal on either writes the video ITS guard names, whichever guard
//       was made last. False for one blame slot shared by the process, where the later guard's video is
//       named for a death on the other thread.
// WHY-THIS-SHAPE: the thread that faults is always the one whose guard was made FIRST, both ways
//       round. A shared slot holds the last guard made, so a fault on the last-guarded thread would name
//       the right video under that defect too and grade nothing. Plain threads, not the daemon's: the
//       property is the record's, and audio_playback's sound-fault phases grade the daemon's use of it.
// CONTROL: control_run.sh 2026-10-11, reverted: `thread_local` (and its tls_model) dropped from t_blame in
//       backend/daemon/crash_record.cpp -> "FAIL: with two threads guarded, a fault on the one guarded first
//       names its own video" (names monitor 6) and "FAIL: and the same with the two videos the other way
//       round" (names monitor 2).
void runPlayingBothBlamedChecks() {
    struct Case {
        PlayingMode mode;
        int monitor;
        const char* path;
        const char* what;
    };
    const Case cases[] = {
        {PlayingMode::BothFirstFaults, kPlayingMonitor, kPlayingPath,
         "with two threads guarded, a fault on the one guarded first names its own video"},
        {PlayingMode::BothOtherFirstFaults, kOtherPlayingMonitor, kOtherPlayingPath,
         "and the same with the two videos the other way round"},
    };
    for (const Case& c : cases) {
        ScratchDir scratch("playingboth");
        if (!scratch.ok()) {
            notVerified("S15.514", "no scratch state directory could be created");
            return;
        }
        bool ok = false;
        const ChildResult result = runPlayingChild(c.mode, scratch.path(), ok);
        if (!ok) continue;
        if (kDeathSignalObservable) {
            check(diedOf(result, SIGSEGV), std::string(c.what) + " -- the child died of SIGSEGV");
        }
        const std::optional<wallpaperx::PlayingHistory> history = wallpaperx::readPlayingRecord(scratch.path());
        const bool own = history && history->died && history->died->monitorId == c.monitor && history->died->path == c.path;
        check(own, c.what);
        if (!own) std::fprintf(stderr, "  the record %s\n", describeDied(history).c_str());
    }
}

// GOAL: S13.226, and S13.188 at the record -- the playing record's own rules. start() replaces whatever the last run left, blame and
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
//       at the next read". All observed 2026-09-21, and again via control_batch.sh 2026-10-15; reverted.
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

// GOAL: S15.947 -- the resume-record reader drops an entry holding any number its field cannot hold
//       (a monitor_id outside 0..INT_MAX, a negative device, a size above INT64_MAX) and keeps the valid
//       entries beside it, with their values.
// WHY-THIS-SHAPE: one planted file, each entry under its own path, so which entries came back names which
//       guard held. Each bad value is one that a plain get<T>() narrows to something valid (4294967297 and
//       -4294967295 to monitor 1), so a missing guard shows as an entry kept, not as garbage.
// CONTROL: per check below, in readNumber (backend/daemon/record_file.h, shared with the scan record) or
//       readResumeRecord (backend/daemon/resume_record.cpp), via control_batch.sh 2026-10-23, each alone,
//       reverted; the three readNumber breaks re-run 2026-10-24 (G5) in record_file.h, the same lines.
// COVERAGE-ASSERT: the valid entry must come back with every value, so a reader that drops everything fails.
void runResumeRecordRanges() {
    constexpr const char* kGoals = "S15.947 (the resume-record reader's ranges)";
    ScratchDir scratch("resumeranges");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch state directory could be created");
        return;
    }
    const auto entry = [](const std::string& path, nlohmann::json monitor, nlohmann::json device, nlohmann::json size) {
        return nlohmann::json{{"monitor_id", std::move(monitor)}, {"path", path},       {"device", std::move(device)},
                              {"inode", 7},                        {"size", std::move(size)}, {"modified_ns", 9},
                              {"position", 11}};
    };
    nlohmann::json points = nlohmann::json::array();
    points.push_back(entry("/valid", 2, 5, 13));
    points.push_back(entry("/monitor-wraps-up", uint64_t{4294967297}, 5, 13));
    points.push_back(entry("/monitor-wraps-down", int64_t{-4294967295}, 5, 13));
    points.push_back(entry("/monitor-negative", -1, 5, 13));
    points.push_back(entry("/device-negative", 2, -1, 13));
    points.push_back(entry("/size-past-int64", 2, 5, uint64_t{18446744073709551615u}));
    writeText(wallpaperx::resumeRecordPath(scratch.path()), nlohmann::json{{"points", points}}.dump());

    const std::vector<wallpaperx::ResumePoint> read = wallpaperx::readResumeRecord(scratch.path());
    const auto kept = [&read](const char* path) {
        return std::any_of(read.begin(), read.end(), [path](const wallpaperx::ResumePoint& p) { return p.path == path; });
    };
    const auto valid = std::find_if(read.begin(), read.end(), [](const wallpaperx::ResumePoint& p) { return p.path == "/valid"; });
    // CONTROL: `point.monitorId < 0` made `point.monitorId >= 0`, which drops every valid entry -> this
    //       line red, with the negative-monitor line (that break keeps -1).
    check(valid != read.end() && valid->monitorId == 2 && valid->device == 5 && valid->inode == 7 && valid->size == 13 &&
              valid->modifiedNs == 9 && valid->position == 11,
          "resume record: a valid entry beside bad ones is kept, with its values");
    // CONTROL: the unsigned branch's `value > max` test deleted -> this line red, with the size line.
    check(!kept("/monitor-wraps-up"), "resume record: monitor_id 4294967297 is not read as another monitor");
    // CONTROL: the signed branch's `value < min || value > max` test deleted -> this line red.
    check(!kept("/monitor-wraps-down"), "resume record: monitor_id -4294967295 is not read as another monitor");
    // CONTROL: `|| point.monitorId < 0` deleted from readResumeRecord -> this line red, alone.
    check(!kept("/monitor-negative"), "resume record: a negative monitor_id is dropped");
    // CONTROL: the `if constexpr (std::is_unsigned_v<T>)` branch's `return false` made to read get<int64_t>()
    //       narrowed -> this line red, alone.
    check(!kept("/device-negative"), "resume record: a negative device is dropped");
    // CONTROL: the unsigned-max break above -> this line red.
    check(!kept("/size-past-int64"), "resume record: a size above INT64_MAX is dropped");
}

// GOAL: S15.1150 (D13-b) -- resume.json carries pass_first_ns for a point that knew its pass start and
//       omits it for one that did not; read back, each has exactly what was written. A record without the
//       field, or with one that is not an int64, still loads the point with its position.
// WHY-THIS-SHAPE: the omission is graded on the file's text, since a reader that ignored a written 0 would
//       make "omitted" and "written as 0" read back alike. The bad values are planted beside a good one
//       so a reader that drops the whole entry (rather than only the field) shows as a missing position.
// CONTROL: backend/daemon/resume_record.cpp, control_batch.sh 2026-10-29, each alone, reverted: (1) the
//       writer's `if (point.passFirst)` line made `entry[kKeyPassFirst] = point.passFirst.value_or(0);` ->
//       "FAIL: resume record: a point that knew no pass start is written without pass_first_ns" (with the
//       read-back line); (2) the reader's pass_first_ns line deleted -> "FAIL: resume record: pass_first_ns
//       reads back as written", alone; (3) the reader's optional read made a requirement (`else continue;`)
//       -> "FAIL: resume record: a record without pass_first_ns, or with a bad one, still loads each point's
//       position" (with the read-back line); (4) the writer's `if (point.passFirst)` line deleted -> "FAIL:
//       resume record: a point that knew its pass start is written with pass_first_ns" (with the read-back line).
void runResumeRecordPassFirst() {
    constexpr const char* kGoals = "S15.1150 (resume.json's pass_first_ns)";
    ScratchDir scratch("resumepassfirst");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch state directory could be created");
        return;
    }
    wallpaperx::ResumePoint knew;
    knew.monitorId = 1;
    knew.path = "/knew";
    knew.position = 11;
    knew.passFirst = int64_t{1234567};
    wallpaperx::ResumePoint unknown = knew;
    unknown.monitorId = 2;
    unknown.path = "/unknown";
    unknown.passFirst.reset();
    const std::string error = wallpaperx::writeResumeRecord(scratch.path(), {knew, unknown});
    if (!error.empty()) {
        notVerified(kGoals, "the record could not be written: " + error);
        return;
    }
    std::ifstream in(wallpaperx::resumeRecordPath(scratch.path()));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const nlohmann::json written = nlohmann::json::parse(text, nullptr, false);
    const auto entryFor = [&written](const char* path) -> const nlohmann::json* {
        if (!written.is_object() || !written.contains("points") || !written["points"].is_array()) return nullptr;
        for (const nlohmann::json& e : written["points"]) {
            if (e.is_object() && e.value("path", "") == path) return &e;
        }
        return nullptr;
    };
    const nlohmann::json* knewEntry = entryFor("/knew");
    const nlohmann::json* unknownEntry = entryFor("/unknown");
    check(knewEntry && knewEntry->contains("pass_first_ns"), "resume record: a point that knew its pass start is written with pass_first_ns");
    check(unknownEntry && !unknownEntry->contains("pass_first_ns"),
          "resume record: a point that knew no pass start is written without pass_first_ns");

    const std::vector<wallpaperx::ResumePoint> round = wallpaperx::readResumeRecord(scratch.path());
    const auto find = [](const std::vector<wallpaperx::ResumePoint>& in, const char* path) {
        const auto it = std::find_if(in.begin(), in.end(), [path](const wallpaperx::ResumePoint& p) { return p.path == path; });
        return it == in.end() ? std::optional<wallpaperx::ResumePoint>() : std::optional(*it);
    };
    const auto knewBack = find(round, "/knew");
    const auto unknownBack = find(round, "/unknown");
    check(knewBack && knewBack->passFirst == int64_t{1234567} && unknownBack && !unknownBack->passFirst,
          "resume record: pass_first_ns reads back as written");

    const auto planted = [](const char* path, std::optional<nlohmann::json> passFirst) {
        nlohmann::json e{{"monitor_id", 3}, {"path", path}, {"device", 5}, {"inode", 7}, {"size", 13}, {"modified_ns", 9}, {"position", 42}};
        if (passFirst) e["pass_first_ns"] = std::move(*passFirst);
        return e;
    };
    nlohmann::json points = nlohmann::json::array();
    points.push_back(planted("/old", std::nullopt));
    points.push_back(planted("/text", nlohmann::json("soon")));
    points.push_back(planted("/past-int64", nlohmann::json(uint64_t{18446744073709551615u})));
    points.push_back(planted("/fraction", nlohmann::json(1.5)));
    writeText(wallpaperx::resumeRecordPath(scratch.path()), nlohmann::json{{"points", points}}.dump());
    const std::vector<wallpaperx::ResumePoint> old = wallpaperx::readResumeRecord(scratch.path());
    int loaded = 0;
    for (const char* path : {"/old", "/text", "/past-int64", "/fraction"}) {
        const auto p = find(old, path);
        if (p && p->position == 42 && !p->passFirst) ++loaded;
        else std::fprintf(stderr, "  %s: %s\n", path, p ? "loaded, but with a pass start or another position" : "dropped");
    }
    // COVERAGE-ASSERT: a literal 4, one per planted entry.
    check(loaded == 4, "resume record: a record without pass_first_ns, or with a bad one, still loads each point's position");
}

// GOAL: S15.947's neighbour, INFERRED from S13.19's symlink case -- the resume-record reader does not
//       follow a symlink planted at resume.json: it reads no entries, even when the target is a valid record.
// WHY-THIS-SHAPE: the same bytes are read first as a regular file at that path, so an empty read under
//       the symlink is the link being refused, not content the reader would have dropped anyway.
// CONTROL: O_NOFOLLOW dropped from readRecordFile's open (backend/daemon/record_file.cpp) -> "FAIL: resume
//       record: a symlink in place of the record is not followed", alone; control_batch.sh 2026-10-24, reverted.
// COVERAGE-ASSERT: the premise line is red if the planted record does not read as one entry
//       (test-side, 2026-10-24: the entry's monitor_id made -1 -> the premise line red, and the phase stops).
void runResumeRecordSymlink() {
    constexpr const char* kGoals = "the resume-record reader's symlink guard";
    ScratchDir scratch("resumesymlink");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch state directory could be created");
        return;
    }
    const std::filesystem::path record = wallpaperx::resumeRecordPath(scratch.path());
    const std::filesystem::path target = scratch.path() / "resume-target.json";
    const nlohmann::json point{{"monitor_id", 2}, {"path", "/valid"}, {"device", 5},    {"inode", 7},
                               {"size", 13},      {"modified_ns", 9}, {"position", 11}};
    const std::string text = nlohmann::json{{"points", nlohmann::json::array({point})}}.dump();
    writeText(record, text);
    const bool readsAsFile = wallpaperx::readResumeRecord(scratch.path()).size() == 1;
    check(readsAsFile, "resume record: the planted record, as a regular file, is read as one entry (the premise)");
    if (!readsAsFile) return;
    std::error_code ec;
    std::filesystem::remove(record, ec);
    writeText(target, text);
    if (!ec) std::filesystem::create_symlink(target, record, ec);
    if (ec) {
        notVerified(kGoals, "the planted record could not be replaced by a symlink: " + ec.message());
        return;
    }
    check(wallpaperx::readResumeRecord(scratch.path()).empty(),
          "resume record: a symlink in place of the record is not followed");
}

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
        using wallpaperx::testing::phaseSelected;
        if (phaseSelected("stampedFault")) runStampedChecks(SIGSEGV, Mode::FaultOnApplying, "a memory fault");
        if (phaseSelected("stampedAbort")) runStampedChecks(SIGABRT, Mode::AbortOnApplying, "an abort");
        if (phaseSelected("otherThread")) runOtherThreadChecks();
        if (phaseSelected("logWriterFault")) runLogWriterFaultChecks();
        if (phaseSelected("stalledOtherThread")) runStalledOtherThreadChecks();
        if (phaseSelected("report")) runReportChecks();
        if (phaseSelected("killed")) runKilledChecks();
        if (phaseSelected("cleanExit")) runCleanExitChecks();
        if (phaseSelected("chaining")) runChainingChecks();
        if (phaseSelected("onDisk")) runOnDiskChecks();
        if (phaseSelected("playingBlame")) runPlayingBlameChecks();
        if (phaseSelected("playingNamesNothing")) runPlayingNamesNothingChecks();
        if (phaseSelected("playingBothBlamed")) runPlayingBothBlamedChecks();
        if (phaseSelected("playingRecordRules")) runPlayingRecordRules();
        if (phaseSelected("resumeRecordRanges")) runResumeRecordRanges();
        if (phaseSelected("resumeRecordSymlink")) runResumeRecordSymlink();
        if (phaseSelected("resumeRecordPassFirst")) runResumeRecordPassFirst();
    } catch (const std::exception& e) {
        harnessFailure(std::string("the run ended in an unhandled exception: ") + e.what());
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
