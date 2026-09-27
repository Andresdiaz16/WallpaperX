// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The daemon under test, as a child process, shared by every test that needs a real one.
//
// One definition rather than one per test binary: the fork preamble, the capture-pipe reader and
// the shutdown/reap sequence are where a copy diverges silently, and a divergent copy still
// compiles, still runs, and still reads as coverage.

#include "support/forked_child.h"
#include "tests/client/scratch_dir.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wallpaperx::testing {

using Clock = std::chrono::steady_clock;

// How long a daemon gets to exit before it is considered wedged.
constexpr auto kProcessTimeout = std::chrono::seconds(30);

// This process's environment with `extraEnv` ("KEY=VALUE") added, each replacing any inherited
// entry for its key. Call before fork(): it allocates, which a forked child of a threaded
// process must not.
inline std::vector<std::string> mergedEnvironment(const std::vector<std::string>& extraEnv) {
    std::vector<std::string> env;
    for (char** e = environ; *e != nullptr; ++e) {
        const std::string entry(*e);
        const size_t eq = entry.find('=');
        const std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
        bool overridden = false;
        for (const std::string& extra : extraEnv) {
            if (extra.rfind(key + "=", 0) == 0) overridden = true;
        }
        if (!overridden) env.push_back(entry);
    }
    env.insert(env.end(), extraEnv.begin(), extraEnv.end());
    return env;
}

// True when a daemon's captured output holds a sanitizer's report. It then exits with the
// sanitizer's status (66 for ThreadSanitizer), so an exit-status check would grade the sanitizer,
// not the path it was written for; callers grade the report instead. The one place this rule lives.
// CONTROL: by hand 2026-09-24, reverted: made to return true -> daemon_system's 24 sanitizer-
//       attribution lines, from all twelve of its call sites, and no other FAIL.
inline bool sanitizerReportedIn(std::string_view output) {
    return output.find("Sanitizer: reported") != std::string_view::npos;
}

// The system call thread `tid` of `pid` is blocked in; -1 when running, nothing when /proc will not
// say (it needs ptrace access, which a test has over the daemon it forked).
inline std::optional<long> blockedIn(pid_t pid, pid_t tid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/syscall");
    std::string number;
    if (!(in >> number)) return std::nullopt;
    if (number == "running") return -1;
    return std::strtol(number.c_str(), nullptr, 10);
}

// Whether `pid` has a handler installed for `signo`, read from /proc's SigCgt; nothing when that
// cannot be read.
inline std::optional<bool> catchesSignal(pid_t pid, int signo) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("SigCgt:", 0) != 0) continue;
        const unsigned long long mask = std::strtoull(line.c_str() + 7, nullptr, 16);
        return (mask >> (signo - 1) & 1ull) != 0;
    }
    return std::nullopt;
}

// A stream the daemon writes to, made into a reader that stopped reading: filled from a writer of
// this process's own until not one more byte fits. The filler is newlines, so a reader that
// resumes sees only empty lines ahead of what the daemon wrote. A terminal is not filled but
// suspended, as Ctrl-S does (tcflow TCOOFF), after which it takes nothing.
class StreamStall {
public:
    enum class Kind { Pipe, Socket, Terminal };

    // Takes ownership of `writer`: a non-blocking description of the pipe (never the daemon's
    // own, which must stay blocking), a descriptor of the daemon's end of the socket (written
    // with MSG_DONTWAIT), or the terminal opened with O_NONBLOCK.
    StreamStall(int writer, Kind kind) : m_fd(writer), m_kind(kind) {
        if (m_fd < 0) return;
        if (m_kind == Kind::Terminal && tcflow(m_fd, TCOOFF) != 0) return;
        // Page-sized first, then single bytes: a pipe refuses a 4096-byte write with up to 4095 free.
        for (const size_t size : {size_t{4096}, size_t{1}}) {
            if (!fill(size)) return;
        }
        m_full = true;
    }
    ~StreamStall() { resume(); }
    StreamStall(const StreamStall&) = delete;
    StreamStall& operator=(const StreamStall&) = delete;

    bool full() const { return m_full; }
    size_t written() const { return m_written; }

    // False if even one byte now fits, i.e. something drained or resumed the stream since.
    bool stillFull() const {
        if (m_fd < 0) return false;
        const char byte = '\n';
        ssize_t n = -1;
        do n = put(&byte, 1);
        while (n < 0 && errno == EINTR);
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }

    // Lets the stream flow again (a terminal) and gives up this writer, so the reader can reach
    // end-of-file once the daemon has gone. Idempotent.
    void resume() {
        if (m_fd < 0) return;
        if (m_kind == Kind::Terminal) (void)tcflow(m_fd, TCOON);
        close(m_fd);
        m_fd = -1;
    }

private:
    ssize_t put(const char* data, size_t size) const {
        return m_kind == Kind::Socket ? send(m_fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL) : write(m_fd, data, size);
    }

    // Until a write of `size` is refused as full. False on any other error, or at a cap far past
    // any pipe's or socket's capacity, which a reader still draining it would reach.
    bool fill(size_t size) {
        constexpr size_t kCap = size_t{16} << 20;
        const std::string chunk(size, '\n');
        while (m_written < kCap) {
            const ssize_t n = put(chunk.data(), size);
            if (n > 0) {
                m_written += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
        return false;
    }

    int m_fd = -1;
    Kind m_kind;
    bool m_full = false;
    size_t m_written = 0;
};

// ---------------------------------------------------------------------------- //
// The daemon under test, as a child process.
// ---------------------------------------------------------------------------- //

// Spawns the real wallpaperx-daemon binary with its stdout and stderr merged into one
// pipe, and reads that pipe on a thread for the process's lifetime.
//
// The output is captured rather than inherited for two reasons: the shutdown assertions
// are *about* what the daemon printed ("stopped cleanly" is the only externally visible
// evidence that the destructor chain ran), and a Vulkan validation warning in the child
// reaches this process no other way. It is echoed as it arrives, prefixed, so a ctest
// failure still shows the daemon's own log next to the FAIL lines.
class SpawnedDaemon {
public:
    enum class Stdout {
        // stdout and stderr share the capture pipe, drained by the reader thread.
        Captured,
        // stdout gets a pipe of its own that nothing ever reads, whose read end the test
        // closes on demand (breakStdout()). That is `wallpaperx-daemon | head` after head
        // has exited: every later write to stdout fails with EPIPE. stderr is still
        // captured, so the daemon can still be heard complaining.
        Breakable,
        // As Captured, but on a stream socket rather than a pipe: the layout journald gives a
        // unit. A socket cannot be reopened through /proc, which is what a daemon's non-blocking
        // log path has to cope with.
        CapturedSocket,
        // As Captured, but on a pseudo-terminal: the daemon run from a shell. The terminal
        // translates each "\n" to "\r\n" in output().
        CapturedTerminal,
        // stdout closed at launch (`>&-`), stderr captured.
        StdoutClosed,
        // stderr closed at launch, stdout captured.
        StderrClosed,
        // Both closed at launch; output() stays empty.
        BothClosed,
    };

    // `extraEnv` entries are "KEY=VALUE"; each replaces any inherited entry for that key.
    // A non-empty `workingDir` is the directory the daemon is started FROM -- the only way
    // to ask what a daemon with no usable HOME writes into its own working directory,
    // since this process's own is the build tree. A non-zero `fileSizeLimit` is an
    // RLIMIT_FSIZE applied to the child, which is how a config write can be made to fail
    // the way a systemd unit's LimitFSIZE= makes it fail on a user's machine. `args` follow
    // the binary on its command line. `stalledFromStart` (Captured, CapturedSocket and
    // CapturedTerminal only) fills the capture stream and stops reading it before the daemon is
    // started, as stallCapture() does later; stalledFromStart() says whether that worked.
    explicit SpawnedDaemon(const std::vector<std::string>& extraEnvIn = {},
                           Stdout stdoutMode = Stdout::Captured, const std::string& workingDir = {},
                           rlim_t fileSizeLimit = 0, const std::vector<std::string>& args = {},
                           bool stalledFromStart = false)
        : m_mode(stdoutMode) {
        // The daemon persists wallpapers under $XDG_CONFIG_HOME. Unset, it would read and
        // rewrite the developer's own config -- restoring test wallpapers into their
        // desktop, and carrying one test's 28 monitors into the next. A fresh directory
        // per spawn; a test that wants state to survive a restart passes its own.
        std::vector<std::string> extraEnv = extraEnvIn;
        bool configHomeSet = false;
        bool stateHomeSet = false;
        for (const std::string& entry : extraEnv) {
            if (entry.rfind("XDG_CONFIG_HOME=", 0) == 0) configHomeSet = true;
            if (entry.rfind("XDG_STATE_HOME=", 0) == 0) stateHomeSet = true;
        }
        if (!configHomeSet) {
            // A member that owns itself, rather than a path cleaned up in ~SpawnedDaemon: a
            // pipe2 or fork failure below throws before this object exists, so the enclosing
            // destructor never runs and the directory would be left in /tmp for good.
            m_configHome.emplace("spawn");
            if (!m_configHome->ok()) throw std::runtime_error("scratch XDG_CONFIG_HOME could not be created");
            extraEnv.push_back(m_configHome->env());
        }
        // The same for the copy the daemon takes of a config it rejects: unset, it would land
        // in the developer's own $XDG_STATE_HOME. Kept beside whichever config home is in use
        // when this owns one, so the two are removed together.
        if (!stateHomeSet) {
            if (!m_configHome.has_value()) {
                m_stateHome.emplace("spawnstate");
                if (!m_stateHome->ok()) throw std::runtime_error("scratch XDG_STATE_HOME could not be created");
            }
            extraEnv.push_back(m_configHome.has_value() ? m_configHome->stateEnv() : m_stateHome->stateEnv());
        }

        int fds[2];
        if (!openCaptureStream(fds)) throw std::runtime_error("the capture stream could not be created");
        if (m_mode == Stdout::CapturedSocket) {
            struct stat st {};
            if (fstat(fds[1], &st) == 0) m_captureInode = st.st_ino;
        }

        int outFds[2] = {-1, -1};
        if (stdoutMode == Stdout::Breakable && pipe2(outFds, O_CLOEXEC) != 0) {
            close(fds[0]);
            close(fds[1]);
            throw std::runtime_error("pipe2 failed");
        }

        // Built before fork(). Between fork() and exec() only async-signal-safe calls
        // are legal, and this process has sdbus dispatch threads running -- setenv() in
        // the child could deadlock on a lock some other thread held at the instant of
        // the fork. Assembling the environment here and handing it to execve() sidesteps
        // that entirely.
        std::vector<std::string> env = mergedEnvironment(extraEnv);

        std::vector<char*> envp;
        envp.reserve(env.size() + 1);
        for (std::string& entry : env) envp.push_back(entry.data());
        envp.push_back(nullptr);

        // Before the fork, while this process still holds both ends; the reader starts held.
        if (stalledFromStart) {
            m_stall.emplace(captureWriter(fds), stallKind());
            m_stalledFromStart = m_stall->full();
            m_holdCapture.store(true, std::memory_order_release);
        }

        std::string binary = WALLPAPERX_DAEMON_BINARY;
        std::vector<std::string> argStorage = args;
        std::vector<char*> argvList{binary.data()};
        for (std::string& arg : argStorage) argvList.push_back(arg.data());
        argvList.push_back(nullptr);
        char** argv = argvList.data();

        const pid_t parentBeforeFork = getpid();
        m_pid = fork();
        if (m_pid < 0) {
            close(fds[0]);
            close(fds[1]);
            if (outFds[0] >= 0) {
                close(outFds[0]);
                close(outFds[1]);
            }
            throw std::runtime_error("fork failed");
        }
        if (m_pid == 0) {
            // dup2 clears FD_CLOEXEC on the new descriptor, which is why the pipe can be
            // opened O_CLOEXEC and still survive into the exec'd program on 1 and 2.
            //
            // Checked, because a silent failure here is worse than the failure itself: the
            // daemon would run correctly with its original stdout, the capture pipe would
            // stay empty, and the parent would report "the daemon runs its destructor
            // chain" and the validation scan as failures of the daemon.
            const int childStdout = outFds[1] >= 0 ? outFds[1] : fds[1];
            if (dup2(childStdout, STDOUT_FILENO) == -1 || dup2(fds[1], STDERR_FILENO) == -1) {
                _exit(wallpaperx::testing::kChildDup2FailedExit);
            }
            // close is async-signal-safe. Closed after the dup2s, so the number stays free for
            // the daemon to find closed rather than being reused by anything here.
            if (m_mode == Stdout::StdoutClosed || m_mode == Stdout::BothClosed) close(STDOUT_FILENO);
            if (m_mode == Stdout::StderrClosed || m_mode == Stdout::BothClosed) close(STDERR_FILENO);
            // chdir is async-signal-safe, which is the whole constraint between fork() and
            // exec(). Its code is in forked_child.h's preamble map with the other three, so
            // every site that grades a startup already names a refused chdir correctly
            // rather than reporting "the daemon never acquired the bus name".
            if (!workingDir.empty() && chdir(workingDir.c_str()) != 0) {
                _exit(wallpaperx::testing::kChildChdirFailedExit);
            }
            // setrlimit is a bare syscall, so it is safe here too. Applied to both the soft
            // and the hard limit, since a raised soft limit would let the daemon write past
            // it and the phase would establish nothing.
            if (fileSizeLimit > 0) {
                struct rlimit limit {
                    fileSizeLimit, fileSizeLimit
                };
                if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(wallpaperx::testing::kChildRlimitFailedExit);
            }
            execChildOrDie(parentBeforeFork, binary.c_str(), argv, envp.data());
        }

        close(fds[1]);
        m_readFd = fds[0];
        if (outFds[1] >= 0) {
            // The parent must not keep a write end open, or closing the read end would
            // still leave one and the daemon's writes would block instead of failing --
            // turning the closed-stdout phase into a hang rather than a check.
            close(outFds[1]);
            m_stdoutFd = outFds[0];
        }

        // Past fork(), so the child exists but this object does not yet -- a throw here
        // would skip ~SpawnedDaemon entirely, leaving a daemon running and holding the
        // bus name with nothing tracking it, and every later phase talking to a process
        // nobody can shut down.
        try {
            m_reader = std::thread([this] { readLoop(); });
        } catch (...) {
            kill(m_pid, SIGKILL);
            int status = 0;
            waitpid(m_pid, &status, 0);
            m_pid = -1;
            close(m_readFd);
            m_readFd = -1;
            breakStdout();
            throw;
        }
    }

    ~SpawnedDaemon() {
        // A test that failed part way through must not leave a daemon holding the bus
        // name; the next phase would then be talking to a process nobody is managing.
        if (m_pid > 0) {
            kill(m_pid, SIGKILL);
            reap();
        }
        breakStdout();
    }

    SpawnedDaemon(const SpawnedDaemon&) = delete;
    SpawnedDaemon& operator=(const SpawnedDaemon&) = delete;

    pid_t pid() const { return m_pid; }

    // True when the process had to be SIGKILLed because it did not exit in time. Kept
    // separate from the exit code because reap() reports both "killed by a signal" and
    // "timed out" as -1, and a check written as `exitCode != 0` is satisfied by the
    // timeout -- which turns "this process never exited" into evidence that it exited
    // for the right reason.
    bool timedOut() const { return m_timedOut; }

    // True only when the process ran to a normal exit(). A daemon that died on SIGABRT
    // also reports exit code -1, so a check written as "exit code is non-zero" is
    // satisfied by a crash -- which is how "the second instance was refused" would be
    // reported for an instance that in fact aborted while failing its name request.
    bool exitedNormally() const { return m_exitedNormally; }
    int termSignal() const { return m_termSignal; }

    // True once the capture pipe has stopped delivering -- EOF, or a read error. A cheap
    // gate for "the daemon may be gone", not an answer to it: readFailed() below separates
    // the two causes, and departure() is the only thing that establishes what the process
    // actually did.
    bool exited() const { return m_readerDone.load(std::memory_order_acquire); }

    // True when the reader stopped because read() failed rather than because the pipe
    // reached EOF. Kept apart because only the second says anything about the child: a
    // failed read is this harness losing its capture, and grading it as the daemon having
    // died is a verdict on the wrong process.
    bool readFailed() const { return m_readFailed.load(std::memory_order_acquire); }

    // What the process did, for a caller that must tell a daemon which quit of its own
    // accord (the daemon's defect) from one the machine killed (not the daemon's defect).
    // exited() cannot: the capture pipe's end-of-file carries no exit status.
    //
    // waitid with WNOWAIT, as startupFault(): WNOHANG *consumes* the status, so reap()
    // would then hit ECHILD and invent a second fault on top of the real one. WNOWAIT leaves
    // the child reapable, so shutdown()/waitForExit() are unaffected.
    //
    // Polled, briefly: the child's descriptors close a fraction before its status becomes
    // publishable, so a single sample taken the instant the pipe hit EOF would decide the
    // verdict on that gap. Callers gate on exited() first, so the poll only ever runs on a
    // path that is already going to report something.
    enum class Departure { StillRunning, QuitOnItsOwn, KilledFromOutside, Unknown };

    Departure departure(int& signo) const {
        signo = 0;
        if (m_pid <= 0) return Departure::Unknown;
        const auto deadline = Clock::now() + std::chrono::milliseconds(500);
        while (true) {
            siginfo_t info{};
            info.si_pid = 0;  // waitid leaves it untouched when WNOHANG finds nothing
            if (waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
                return Departure::Unknown;
            }
            if (info.si_pid != 0) {
                if (info.si_code != CLD_KILLED && info.si_code != CLD_DUMPED) return Departure::QuitOnItsOwn;
                signo = info.si_status;
                // A fault signal is the daemon dying of its own defect and stays its own;
                // these four are what an OOM killer, a stray script or a session teardown
                // sends, and no phase sends any of them before asking.
                switch (signo) {
                    case SIGKILL:
                    case SIGTERM:
                    case SIGINT:
                    case SIGHUP:
                        return Departure::KilledFromOutside;
                    default:
                        return Departure::QuitOnItsOwn;
                }
            }
            // waitid found the child but had nothing to report, so it has not terminated.
            if (Clock::now() >= deadline) return Departure::StillRunning;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // GOAL: X.3 -- a child that died in its fork preamble (PR_SET_PDEATHSIG refused, dup2
    //       failed, execve failed) never became the daemon, so its absence must be reported
    //       as this harness failing to set the phase up, never as the daemon failing to
    //       start. Non-null names which of the three happened.
    // WHY-THIS-SHAPE: waitid with WNOWAIT, not waitpid(WNOHANG). WNOHANG *consumes* the
    //       status, so reap() would then hit ECHILD and record m_waitFailed -- an invented
    //       second fault on top of the real one -- and shutdown()/waitForExit() would stop
    //       reporting the code at all. WNOWAIT reads the status and leaves the child
    //       reapable, so every caller below is unaffected.
    //       It never waits: a preamble death happens within microseconds of the fork, and
    //       every caller asks only after a wait for the daemon to appear has already timed
    //       out, by which point that child has been dead for the whole of that wait. A
    //       version that blocked here would put a fresh timeout on the healthy path.
    //       The post-reap arm answers for a child already collected -- the second-instance
    //       phase grades an exit code, so it has to ask after waitForExit().
    // CONTROL: three, all reverted. armAgainstParentDeath() forced false -> 8 HARNESS FAULT
    //       lines naming PR_SET_PDEATHSIG and 0 FAILs, where the same mutation before this
    //       existed produced 4 FAILs against the daemon. The dup2 above forced to fail -> the
    //       same 8, naming dup2; with only forkPreambleFault's 126 arm removed, those 4 FAILs
    //       came straight back, so that arm is load-bearing rather than decorative. The
    //       binary path suffixed so execve fails -> 3, naming execve.
    const char* startupFault() const {
        if (m_pid > 0) {
            siginfo_t info{};
            info.si_pid = 0;  // waitid leaves it untouched when WNOHANG finds nothing
            if (waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0) return nullptr;
            if (info.si_pid == 0 || info.si_code != CLD_EXITED) return nullptr;
            return wallpaperx::testing::forkPreambleFault(info.si_status);
        }
        if (!m_exitedNormally) return nullptr;
        return wallpaperx::testing::forkPreambleFault(m_exitCode);
    }


    // Signals the daemon and waits for it to exit. Returns the exit code, or -1 if it
    // was killed by a signal or did not exit within kProcessTimeout (in which case it is
    // SIGKILLed so the test can continue -- see timedOut()).
    int shutdown(int signo) {
        if (m_pid <= 0) return m_exitCode;
        kill(m_pid, signo);
        return reap();
    }

    // Waits for a daemon expected to exit on its own -- the second-instance case, where
    // the name request fails and main() returns 1.
    int waitForExit() {
        if (m_pid <= 0) return m_exitCode;
        return reap();
    }

    // sanitizerReportedIn() on this daemon's output. Meaningful only after exit.
    bool sanitizerReported() const { return sanitizerReportedIn(output()); }

    // Everything the daemon has written so far. Complete only after it has exited: the
    // reader thread is still draining the pipe until then, so any assertion about the
    // daemon's output belongs after a shutdown()/waitForExit().
    //
    // In Stdout::Breakable mode this is stderr only -- stdout went to the pipe that gets
    // closed, which is the entire point of that mode.
    std::string output() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_output;
    }

    // Closes the read end of a Breakable stdout, so every subsequent write the daemon
    // makes to fd 1 fails. Idempotent: the destructor calls it too, and the constructor's
    // failure path calls it on a daemon that may never have had a Breakable stdout at
    // all. The bool exists for those callers; for a Breakable daemon the constructor has
    // already guaranteed the descriptor, so a phase must not grade this return -- there
    // is no reachable false.
    bool breakStdout() {
        if (m_stdoutFd < 0) return false;
        close(m_stdoutFd);
        m_stdoutFd = -1;
        return true;
    }

    // Reads the daemon's *stdout* -- fd 1 specifically, not the merged capture -- until
    // `needle` appears. Breakable mode is the only mode that has a separate stdout pipe,
    // so this is the only way to observe what went to fd 1 rather than to fd 2.
    //
    // Returns false on timeout, on EOF (the daemon exited), and on read error. There is
    // no path here that reports "not seen" as "seen", and a false must fail the caller's
    // check rather than fall through into it: a daemon that prints nothing would
    // otherwise turn the phase into a stall that is graded as data.
    bool waitForStdout(std::string_view needle, Clock::duration timeout) {
        if (m_stdoutFd < 0) return false;
        const auto deadline = Clock::now() + timeout;
        while (true) {
            if (m_stdoutText.find(needle) != std::string::npos) return true;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) return false;
            pollfd pfd{m_stdoutFd, POLLIN, 0};
            const int ready = poll(&pfd, 1, static_cast<int>(remaining));
            if (ready < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (ready == 0) return false;
            char buf[4096];
            const ssize_t n = read(m_stdoutFd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            // n == 0 is EOF: every write end is closed, i.e. the daemon is gone, so the
            // needle can never arrive and waiting out the deadline would only convert a
            // dead child into a slow one.
            if (n <= 0) return m_stdoutText.find(needle) != std::string::npos;
            m_stdoutText.append(buf, static_cast<size_t>(n));
        }
    }

    // Waits for `needle` to appear in the captured output. Returns false on timeout AND
    // on the daemon exiting without ever printing it -- there is no path here that
    // reports "not seen" as "seen", and no caller may treat the false as data: it means
    // the phase could not be set up.
    bool waitForOutput(std::string_view needle, Clock::duration timeout) const {
        const auto deadline = Clock::now() + timeout;
        while (true) {
            {
                // Searched under the lock rather than through output(), which copies the whole
                // captured log on every turn of this wait.
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_output.find(needle) != std::string::npos) return true;
            }
            // EOF on the capture pipe means every write end is closed, i.e. the child is
            // gone. Without this the wait would burn the whole timeout on a daemon that
            // died at startup -- and a 30-second stall is exactly how a setup failure
            // gets mistaken for a slow one.
            if (m_readerDone.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_output.find(needle) != std::string::npos;
            }
            if (Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Stops draining the capture stream until resumeCapture(), so it can be filled and the daemon's
    // output made a reader that stopped reading. Returns once the reader thread has read for the
    // last time; false if it did not confirm within 2 s. While held, output is not captured and
    // exited() stays false until shutdown()/waitForExit() reaps the process.
    bool holdCapture() {
        m_holdCapture.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_holdChanged.wait_for(lock, std::chrono::seconds(2), [this] { return m_captureHeld; });
    }

    // holdCapture(), then the capture stream filled from this process's end (StreamStall), so the
    // daemon's stdout and stderr lead to a reader that stopped reading and has no room left.
    // Captured, CapturedSocket and CapturedTerminal. False, with `why` saying so, if either step
    // failed; the stream then must not be taken as stalled. Output from here on is in output()
    // past stalledAt(), once resumeCapture() has let it through.
    bool stallCapture(std::string& why) {
        if (!holdCapture()) {
            why = "the capture reader did not stop reading";
            return false;
        }
        m_stalledAt = output().size();
        m_stall.emplace(captureWriter(nullptr), stallKind());
        if (!m_stall->full()) {
            why = std::string("the capture stream could not be filled from this side (") + std::strerror(errno) + ")";
            return false;
        }
        return true;
    }

    // Whether stalledFromStart filled the stream before the daemon started.
    bool stalledFromStart() const { return m_stalledFromStart; }
    // Whether the stall still holds: false once anything drained or resumed the capture stream.
    // Ask before the daemon is reaped.
    bool captureStillStalled() const { return m_stall.has_value() && m_stall->stillFull(); }
    size_t stallBytes() const { return m_stall.has_value() ? m_stall->written() : 0; }
    size_t stalledAt() const { return m_stalledAt; }

    // Undoes stallCapture(): the stream flows and the reader reads again, filler first.
    void resumeCapture() {
        if (m_stall.has_value()) m_stall->resume();
        m_holdCapture.store(false, std::memory_order_release);
    }

    // Breakable mode: fills the stdout pipe that nothing reads, from a writer of this process's
    // own, so stdout alone is a reader that stopped reading. False if it could not be filled.
    bool stallStdout() {
        if (m_stdoutFd < 0) return false;
        const std::string path = "/proc/self/fd/" + std::to_string(m_stdoutFd);
        m_stdoutStall.emplace(open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC), StreamStall::Kind::Pipe);
        return m_stdoutStall->full();
    }
    bool stdoutStillStalled() const { return m_stdoutStall.has_value() && m_stdoutStall->stillFull(); }
    size_t stdoutStallBytes() const { return m_stdoutStall.has_value() ? m_stdoutStall->written() : 0; }

    // Whether each line read is echoed as "[daemon] ..." to this process's stdout. Off for a phase
    // that makes the daemon write hundreds of kilobytes.
    void echo(bool on) { m_echo.store(on, std::memory_order_relaxed); }

private:
    StreamStall::Kind stallKind() const {
        return m_mode == Stdout::CapturedSocket     ? StreamStall::Kind::Socket
               : m_mode == Stdout::CapturedTerminal ? StreamStall::Kind::Terminal
                                                    : StreamStall::Kind::Pipe;
    }

    bool openCaptureStream(int fds[2]) {
        if (m_mode == Stdout::CapturedSocket) return socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0;
        if (m_mode != Stdout::CapturedTerminal) return pipe2(fds, O_CLOEXEC) == 0;
        const int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0) return false;
        char name[128];
        if (grantpt(master) != 0 || unlockpt(master) != 0 || ptsname_r(master, name, sizeof(name)) != 0) {
            close(master);
            return false;
        }
        const int slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (slave < 0) {
            close(master);
            return false;
        }
        m_ptyName = name;
        fds[0] = master;
        fds[1] = slave;
        return true;
    }

    // A writer onto the capture stream in the daemon's direction, owned by the caller; -1 if none
    // could be had. `fds` is the pair before the fork, when this process still holds both ends.
    // After it, a socket's daemon end is taken from the daemon (pidfd_getfd), found by its inode.
    int captureWriter(const int* fds) const {
        switch (stallKind()) {
            case StreamStall::Kind::Terminal:
                return open(m_ptyName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
            case StreamStall::Kind::Pipe: {
                // Opening a pipe's /proc entry for writing yields a new write description of it.
                const std::string path = "/proc/self/fd/" + std::to_string(fds != nullptr ? fds[0] : m_readFd);
                return open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            }
            case StreamStall::Kind::Socket:
                break;
        }
        if (fds != nullptr) return fcntl(fds[1], F_DUPFD_CLOEXEC, 0);
        if (m_pid <= 0 || m_captureInode == 0) return -1;
        const std::string want = "socket:[" + std::to_string(m_captureInode) + "]";
        const std::string dir = "/proc/" + std::to_string(m_pid) + "/fd/";
        for (int fd = 0; fd < 1024; ++fd) {
            char target[64];
            const ssize_t n = readlink((dir + std::to_string(fd)).c_str(), target, sizeof(target) - 1);
            if (n <= 0 || std::string_view(target, static_cast<size_t>(n)) != want) continue;
            const int pidfd = static_cast<int>(syscall(SYS_pidfd_open, m_pid, 0));
            if (pidfd < 0) return -1;
            const int copy = static_cast<int>(syscall(SYS_pidfd_getfd, pidfd, fd, 0));
            close(pidfd);
            return copy;
        }
        errno = ENOENT;
        return -1;
    }

    void readLoop() {
        char buf[4096];
        std::string pending;
        while (true) {
            if (m_holdCapture.load(std::memory_order_acquire)) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_captureHeld = true;
                }
                m_holdChanged.notify_all();
                while (m_holdCapture.load(std::memory_order_acquire) && !m_stopReader.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (m_holdCapture.load(std::memory_order_acquire)) break;  // reaped while held
                std::lock_guard<std::mutex> lock(m_mutex);
                m_captureHeld = false;
                continue;
            }
            // Polled rather than blocked in read(): a child that forks something inheriting this
            // pipe leaves it open after the daemon exits, and a blocking read would then never
            // return and the join below would hang instead of reporting.
            pollfd pfd{m_readFd, POLLIN, 0};
            const int ready = poll(&pfd, 1, 200);
            if (ready == 0) {
                if (m_stopReader.load(std::memory_order_acquire)) break;
                continue;
            }
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) {
                m_readFailed.store(true, std::memory_order_release);
                break;
            }
            const ssize_t n = read(m_readFd, buf, sizeof(buf));
            // EINTR is not EOF. Treating it as one truncates the capture, and every
            // assertion about the daemon's output below -- "stopped cleanly", the signal
            // line, the validation scan -- then reads a short buffer and fails, or passes,
            // for a reason that has nothing to do with the daemon.
            if (n < 0 && errno == EINTR) continue;
            // A terminal's master reads EIO once every slave descriptor is closed: its EOF.
            if (n < 0 && errno == EIO && m_mode == Stdout::CapturedTerminal) break;
            if (n < 0) {
                m_readFailed.store(true, std::memory_order_release);
                break;
            }
            if (n == 0) break;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_output.append(buf, static_cast<size_t>(n));
            }
            pending.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                // Empty lines are a stall's filler.
                if (!line.empty() && m_echo.load(std::memory_order_relaxed)) std::printf("  [daemon] %s\n", line.c_str());
            }
        }
        if (!pending.empty() && m_echo.load(std::memory_order_relaxed)) std::printf("  [daemon] %s\n", pending.c_str());
        std::fflush(stdout);
        m_readerDone.store(true, std::memory_order_release);
    }

    int reap() {
        const auto deadline = Clock::now() + kProcessTimeout;
        int status = 0;
        bool exited = false;
        while (Clock::now() < deadline) {
            const pid_t r = waitpid(m_pid, &status, WNOHANG);
            if (r == m_pid) {
                exited = true;
                break;
            }
            if (r < 0) {
                // waitpid itself failed (ECHILD, EINVAL). Not a timeout and not an exit:
                // reporting it as either would accuse the daemon of something it did not
                // do, so it is recorded on its own.
                m_waitFailed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!exited) {
            kill(m_pid, SIGKILL);
            waitpid(m_pid, &status, 0);
            m_exitCode = -1;
            m_timedOut = !m_waitFailed;
        } else {
            m_exitedNormally = WIFEXITED(status);
            m_exitCode = m_exitedNormally ? WEXITSTATUS(status) : -1;
            if (WIFSIGNALED(status)) m_termSignal = WTERMSIG(status);
        }
        m_pid = -1;

        // Joined only after the child is gone: the reader ends at EOF, and EOF arrives
        // only once every write end of the pipe is closed, which the child's exit does.
        m_stopReader.store(true, std::memory_order_release);
        if (m_reader.joinable()) m_reader.join();
        close(m_readFd);
        m_readFd = -1;
        return m_exitCode;
    }

    // Only engaged when this spawn created the directory, so an explicit override is never
    // deleted by us. Declared before every other member so it outlives them. m_stateHome is
    // engaged only when the caller brought its own config home and no state home, which is the
    // one case the config home cannot hold the copies.
    std::optional<wallpaperx::testing::ScratchDir> m_configHome;
    std::optional<wallpaperx::testing::ScratchDir> m_stateHome;

    Stdout m_mode;
    ino_t m_captureInode = 0;  // CapturedSocket: the daemon's end, to find it in /proc/<pid>/fd
    std::string m_ptyName;     // CapturedTerminal: the terminal's path, to stall and resume it
    std::optional<StreamStall> m_stall;
    std::optional<StreamStall> m_stdoutStall;
    bool m_stalledFromStart = false;
    size_t m_stalledAt = 0;
    std::atomic<bool> m_echo{true};

    pid_t m_pid = -1;
    int m_readFd = -1;
    int m_stdoutFd = -1;
    // Only ever touched from the test thread through waitForStdout(); the reader thread
    // drains the *capture* pipe and never sees this one.
    std::string m_stdoutText;
    int m_exitCode = -1;
    bool m_timedOut = false;
    bool m_waitFailed = false;
    bool m_exitedNormally = false;
    int m_termSignal = 0;
    std::thread m_reader;
    std::atomic<bool> m_readerDone{false};
    std::atomic<bool> m_stopReader{false};
    std::atomic<bool> m_readFailed{false};
    std::atomic<bool> m_holdCapture{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_holdChanged;
    bool m_captureHeld = false;  // under m_mutex
    std::string m_output;
};

}  // namespace wallpaperx::testing
