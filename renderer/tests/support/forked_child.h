#pragma once

// Fork-side scaffolding shared by the two test binaries in this repo that fork children:
//
//   renderer/tests/device_affinity_probe.cpp   -- one child per guarded call site
//   backend/tests/daemon_system_test.cpp       -- the daemon binary, and a self-probe
//
// It lives in the renderer tree because PLAN.md's dependency direction is one-way
// (wallpaperx-renderer is linked into wallpaperx-backend, never the reverse): a renderer
// probe may not include a backend header, while backend/tests already has renderer/tests
// on its include path. Same reasoning as support/validation_grade.h next door.
//
// Nothing here knows what a daemon, a Device or a bus is. A helper that needed any of
// them could not be shared, and would drag the dependency the wrong way.

#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace wallpaperx::testing {

// Called in the child immediately after fork(): asks the kernel to SIGKILL this process
// when the thread that forked it dies. Both calls are async-signal-safe, which is all
// that may run between fork() and exec().
//
// The backstop destructors cannot be (GOALS.md X.8): a parent killed by SIGABRT -- an
// assertion inside a library, a sanitizer report, std::terminate -- unwinds nothing, so
// no destructor reaps anything and the child is inherited by init still holding whatever
// it holds (a bus name, a port, a GPU device).
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
// CONTROL (the race guard; the window is microseconds wide, so no in-suite check reaches
//       it): a 200 ms sleep above the prctl plus a probe that aborts as soon as fork()
//       returns -- with the getppid line deleted the daemon survived with ppid 1 holding
//       the config port, with it in place the child died. Reverted.
inline bool armAgainstParentDeath(pid_t parentBeforeFork) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) return false;
    return getppid() == parentBeforeFork;
}

// Stops a deliberate abort in this child from reaching the host's core collector.
//
// PR_SET_DUMPABLE, not setrlimit(RLIMIT_CORE, 0): measured on a host whose
// /proc/sys/kernel/core_pattern pipes to systemd-coredump, the rlimit stores no core but
// the kernel still invokes the pipe handler, so every abort still lands in the journal and
// still raises a desktop crash notification (818 -> 823 entries for one run). Dumpable 0
// leaves it at 813 -> 813. The rlimit fixes disk usage; only this fixes the noise.
//
// It suppresses the dump only: the child still raises SIGABRT, waitpid still reports
// WIFSIGNALED with WTERMSIG == SIGABRT, and a stderr redirected before this call still
// reaches the parent's pipe. execve resets the flag, so a child that goes on to exec some
// other program does not inherit it.
inline void suppressCoreDump() { prctl(PR_SET_DUMPABLE, 0, 0, 0, 0); }

// Arms the child and execs `binary`, or exits with a code no real outcome uses.
[[noreturn]] inline void execChildOrDie(pid_t parentBeforeFork, const char* binary, char* const argv[],
                                        char* const envp[]) {
    if (!armAgainstParentDeath(parentBeforeFork)) _exit(125);
    if (envp != nullptr) {
        execve(binary, argv, envp);
    } else {
        execv(binary, argv);
    }
    _exit(127);  // only reached if the exec failed
}

// True while `pid` is a *running* process whose cmdline mentions `cmdlineNeedle`.
//
// Zombies say no: the process is dead and only its exit status is outstanding, which is
// exactly the state a killed orphan passes through on its way to being reaped by init.
// The cmdline comparison covers the other direction -- a pid recycled onto an unrelated
// process between the kill and this look.
inline bool runningProcessMatches(pid_t pid, std::string_view cmdlineNeedle) {
    const std::string dir = "/proc/" + std::to_string(static_cast<int>(pid));
    std::ifstream stat(dir + "/stat");
    std::string statLine;
    if (!std::getline(stat, statLine)) return false;
    // State is the field after the parenthesised comm, which may itself contain spaces.
    const size_t commEnd = statLine.rfind(')');
    if (commEnd == std::string::npos || commEnd + 2 >= statLine.size()) return false;
    if (statLine[commEnd + 2] == 'Z') return false;

    std::ifstream cmdline(dir + "/cmdline", std::ios::binary);
    std::string args((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
    return args.find(cmdlineNeedle) != std::string::npos;
}

// How a self-probe run ended. `timedOut` is deliberately separate from every other field:
// a probe that never finished has produced no evidence at all, and a caller that graded
// `termSignal != SIGABRT` on a killed probe would be reporting our own SIGKILL as the
// probe's verdict.
struct SelfProbeRun {
    bool started = false;
    std::string harnessError;  // set for a pre-fork failure AND for a failed reap
    std::string output;
    bool exitedNormally = false;
    int exitCode = -1;
    int termSignal = 0;
    bool timedOut = false;
};

// Runs this binary again with `flag`, captures what it prints, and reports how it died.
//
// GOAL: X.1 -- a probe that never reaches its own exit cannot hang the suite. Both waits
//       here are bounded and the expiry is reported as `timedOut`, never as data.
// WHY-THIS-SHAPE: the obvious version is a blocking read-to-EOF followed by a blocking
//       waitpid. Both are unbounded, and a probe that wedges before printing anything then
//       produces a ctest timeout with an empty log -- worse than a wrong answer, because
//       there is nothing to read. Draining still comes before reaping, because a probe that
//       writes more than a pipe buffer would otherwise deadlock against a wait-first order.
// CONTROL: the probe's entry point made to sleep past the timeout instead of aborting ->
//       the caller's "the probe did not finish within N s" skip, and the run continues.
inline SelfProbeRun runSelfProbe(const char* flag, std::chrono::milliseconds timeout) {
    using Clock = std::chrono::steady_clock;
    // Grace after a SIGKILL: long enough that a loaded host still reaps, short enough that
    // a child stuck unkillably in D state does not become a second unbounded wait.
    constexpr auto kAfterKillGrace = std::chrono::seconds(5);

    SelfProbeRun run;

    char exePath[4096];
    const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) {
        run.harnessError = "could not read /proc/self/exe";
        return run;
    }
    exePath[len] = '\0';

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        run.harnessError = std::string("pipe2 failed: ") + strerror(errno);
        return run;
    }

    std::string binary = exePath;
    std::string arg = flag;
    char* argv[] = {binary.data(), arg.data(), nullptr};

    const pid_t parentBeforeFork = getpid();
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        run.harnessError = std::string("fork failed: ") + strerror(errno);
        return run;
    }
    if (pid == 0) {
        if (dup2(fds[1], STDOUT_FILENO) == -1 || dup2(fds[1], STDERR_FILENO) == -1) _exit(126);
        execChildOrDie(parentBeforeFork, binary.c_str(), argv, nullptr);
    }
    close(fds[1]);
    run.started = true;

    bool killed = false;
    auto deadline = Clock::now() + timeout;
    char buf[4096];
    while (true) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) {
            if (killed) break;
            run.timedOut = true;
            kill(pid, SIGKILL);
            killed = true;
            deadline = Clock::now() + kAfterKillGrace;
            continue;
        }
        pollfd pfd{fds[0], POLLIN, 0};
        const int ready = poll(&pfd, 1, static_cast<int>(remaining));
        if (ready < 0) {
            if (errno == EINTR) continue;
            run.harnessError = std::string("poll on the probe's pipe failed: ") + strerror(errno);
            break;
        }
        if (ready == 0) continue;  // deadline handled at the top
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        run.output.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);

    int status = 0;
    auto reapDeadline = Clock::now() + kAfterKillGrace;
    while (true) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0) {
            if (errno == EINTR) continue;
            run.harnessError = std::string("waitpid on the probe failed: ") + strerror(errno);
            return run;
        }
        if (Clock::now() >= reapDeadline) {
            if (killed) {
                run.harnessError = "the probe did not exit even after SIGKILL";
                return run;
            }
            run.timedOut = true;
            kill(pid, SIGKILL);
            killed = true;
            reapDeadline = Clock::now() + kAfterKillGrace;
        }
        poll(nullptr, 0, 5);
    }
    run.exitedNormally = WIFEXITED(status);
    run.exitCode = run.exitedNormally ? WEXITSTATUS(status) : -1;
    if (WIFSIGNALED(status)) run.termSignal = WTERMSIG(status);
    return run;
}

}  // namespace wallpaperx::testing
