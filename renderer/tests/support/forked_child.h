// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Fork-side scaffolding shared by the two test binaries in this repo that fork children:
//
//   renderer/tests/device_affinity_probe.cpp   -- one child per guarded call site
//   backend/tests/daemon_system_test.cpp       -- the daemon binary, and a nested bus
//
// It lives in the renderer tree because PLAN.md's dependency direction is one-way
// (wallpaperx-renderer is linked into wallpaperx-backend, never the reverse): a renderer
// probe may not include a backend header, while backend/tests already has renderer/tests
// on its include path. Same reasoning as support/validation_grade.h next door.
//
// Nothing here knows what a daemon, a Device or a bus is. A helper that needed any of
// them could not be shared, and would drag the dependency the wrong way.

#include <fstream>
#include <optional>
#include <string>

#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace wallpaperx::testing {

// Called in the child immediately after fork(): asks the kernel to SIGKILL this process
// when the thread that forked it dies. Both calls are async-signal-safe, which is all
// that may run between fork() and exec().
//
// The backstop destructors cannot be: a parent killed by SIGABRT -- an assertion inside a
// library, a sanitizer report, std::terminate -- unwinds nothing, so no destructor reaps
// anything and the child is inherited by init still holding whatever it holds (a bus name,
// a port, a GPU device).
//
// PR_SET_PDEATHSIG is per *thread*: the signal arrives when the forking thread exits, not
// when its process does. Every fork in this repo happens on the main thread, whose death
// ends the process, so the two coincide. A fork moved onto a worker thread would silently
// weaken this to "when that worker finishes".
//
// `parentBeforeFork` closes the race the prctl alone leaves: the parent may already have
// died between fork() and here, in which case the signal is never sent because the death
// preceded the request. A changed parent id is that case, and the child must not survive
// it either.
//
// Both halves are verified out of suite, because a check that runs inside a process cannot
// report on what outlived it, and the in-suite apparatus that tried was retired for grading
// the property green with nothing ever orphaned.
// CONTROL (the prctl): a scratch parent forks a child that arms itself, signals through a
//       pipe that it is armed, and sleeps; the parent then aborts. With the prctl deleted
//       the child was still running a second later with ppid 1197; with it in place there
//       was no such process. The pipe handshake is load-bearing -- without it the parent's
//       abort wins the race and the getppid line below exits the child, which looks exactly
//       like the prctl working. Reverted.
// CONTROL (the getppid recheck; the window is microseconds wide, so no check of any kind
//       reaches it unaided): a 200 ms sleep injected above the prctl and no handshake, so
//       the parent is always dead first. With the recheck deleted the child survived with
//       ppid 1197; with it in place there was no survivor. Reverted.
inline bool armAgainstParentDeath(pid_t parentBeforeFork) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) return false;
    return getppid() == parentBeforeFork;
}

// Stops a deliberate abort in this child from reaching the host's core collector.
//
// Both mechanisms, because each covers a child the other cannot. execve RESETS
// PR_SET_DUMPABLE and does NOT reset RLIMIT_CORE, so a child that goes on to exec is
// covered by the rlimit alone. And the rlimit alone is not enough for a child that aborts
// where it stands: measured on a host whose /proc/sys/kernel/core_pattern pipes to
// systemd-coredump, it stores no core but the kernel still invokes the pipe handler, so
// every abort lands in the journal and raises a desktop crash notification (818 -> 823
// entries for one run), where dumpable 0 leaves it at 813 -> 813.
//
// It suppresses the dump only: the child still raises SIGABRT, waitpid still reports
// WIFSIGNALED with WTERMSIG == SIGABRT, and a stderr redirected before this call still
// reaches the parent's pipe.
//
// Every test process that crashes on purpose calls this first, not only forked children: the
// daemon the wedge shim faults (backend/tests/wedge_shim.cpp) and the app's namespace children
// (app/tests/wpx_fixtures.h) were exec'd, so only a call made after their exec counts.
// Async-signal-safe: two system calls.
inline void suppressCoreDump() {
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
}

// Whether this host's kernel hands a core to a program (core_pattern starts with '|') rather than
// writing a file; nullopt when /proc/sys/kernel/core_pattern cannot be read. Only on a pipe does
// WCOREDUMP tell a non-dumpable crash from a dumpable one: with a file pattern, an inherited
// RLIMIT_CORE of 0 alone already stops the dump, so the bit is clear either way.
inline std::optional<bool> coreGoesToAProgram() {
    std::ifstream pattern("/proc/sys/kernel/core_pattern");
    std::string text;
    if (!std::getline(pattern, text)) return std::nullopt;
    return !text.empty() && text.front() == '|';
}

// The codes a child exits with when it dies before the program it was meant to run has
// started. 126 and 127 are the shell's own meanings ("found but could not be executed",
// "not found"); 125 sits below them and no program this harness execs uses any of the
// three.
//
// 126 is not produced here: the dup2 that earns it has to happen at the fork site, before
// this call. It is named here anyway so both halves of the preamble are one list -- a
// grading site that handles one code and not the others reads the unhandled one as a
// verdict on a program that never ran.
//
// Only for children that exec. A fork site whose child runs in-process is free to choose
// its own codes, and 125 already means something else in one of them.
// 123, not 124: renderer/tests/device_affinity_probe.cpp already spends 124 on a meaning of
// its own, and one number with two meanings across the two harnesses is how a grading site
// comes to name the wrong cause. 122 sits below it for the same reason.
constexpr int kChildRlimitFailedExit = 122;
constexpr int kChildChdirFailedExit = 123;
constexpr int kChildArmFailedExit = 125;
constexpr int kChildDup2FailedExit = 126;
constexpr int kChildExecFailedExit = 127;

// Which of the three `exitCode` is, or nullptr when the exec'd program chose it itself.
//
// Says nothing about what the child was going to run: this header is shared with probes
// that know nothing of a daemon or a bus, and a sentence naming either could not live here.
inline const char* forkPreambleFault(int exitCode) {
    switch (exitCode) {
        case kChildRlimitFailedExit:
            return "the forked child could not apply the resource limit it was given (setrlimit failed)";
        case kChildChdirFailedExit:
            return "the forked child could not enter the working directory it was given (chdir failed)";
        case kChildArmFailedExit:
            return "the forked child could not arm itself against its parent's death (PR_SET_PDEATHSIG refused, or "
                   "the parent was already gone)";
        case kChildDup2FailedExit:
            return "the forked child could not redirect its output onto this harness's capture pipe (dup2 failed)";
        case kChildExecFailedExit:
            return "the forked child could not execute the binary it was given (execve failed, or its loader did)";
        default:
            return nullptr;
    }
}

// Arms the child and execs `binary`, or exits with a code no real outcome uses.
//
// The suppression is here rather than at each fork site, so no site can forget it: every
// process this harness execs is one it kills, or breaks on purpose in a control run, and
// those cores are the host's journal noise rather than evidence. Only the rlimit half
// survives into the exec'd program, which is why suppressCoreDump() sets both.
[[noreturn]] inline void execChildOrDie(pid_t parentBeforeFork, const char* binary, char* const argv[],
                                        char* const envp[]) {
    if (!armAgainstParentDeath(parentBeforeFork)) _exit(kChildArmFailedExit);
    suppressCoreDump();
    if (envp != nullptr) {
        execve(binary, argv, envp);
    } else {
        execv(binary, argv);
    }
    _exit(kChildExecFailedExit);  // only reached if the exec failed
}

}  // namespace wallpaperx::testing
