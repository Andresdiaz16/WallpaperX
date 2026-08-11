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
inline void suppressCoreDump() {
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
}

// Arms the child and execs `binary`, or exits with a code no real outcome uses.
//
// The suppression is here rather than at each fork site, so no site can forget it: every
// process this harness execs is one it kills, or breaks on purpose in a control run, and
// those cores are the host's journal noise rather than evidence. Only the rlimit half
// survives into the exec'd program, which is why suppressCoreDump() sets both.
[[noreturn]] inline void execChildOrDie(pid_t parentBeforeFork, const char* binary, char* const argv[],
                                        char* const envp[]) {
    if (!armAgainstParentDeath(parentBeforeFork)) _exit(125);
    suppressCoreDump();
    if (envp != nullptr) {
        execve(binary, argv, envp);
    } else {
        execv(binary, argv);
    }
    _exit(127);  // only reached if the exec failed
}

}  // namespace wallpaperx::testing
