// System-level test for Step 6 task 4: drives the **real daemon binary, in its own
// process**, over a private session bus.
//
// Everything else in this suite links the daemon's code into the test and calls it.
// That covers the logic but not the program: `main.cpp` -- bus-name acquisition, the
// signal mask, the sigwait thread, the destructor chain on the way out -- has no
// coverage at all from an in-process test, because an in-process test never runs it.
// Three things are only observable from out here:
//
//   1. The frame crosses an address-space boundary. In-process, "the client mapped the
//      exported fd" proves less than it appears to: the pages were already mapped into
//      the same process by the exporter. Here the mapping is made by a process that
//      never allocated the image, which is what a compositor actually does.
//   2. The daemon is a *program*. It must acquire its name, refuse a second instance,
//      answer while its GPU thread is busy, and exit 0 on a signal having run its
//      destructors. Every one of those was previously verified by hand, once, and then
//      believed.
//   3. The shipped default geometry (1920x1080) is exercised. Every other test
//      overrides WallpaperManager::Config to keep software-rasterised runs fast, so the
//      size the daemon actually ships with is the one size nothing rendered.
//
// Run under a private bus: `dbus-run-session -- ./wallpaperx-daemon-system-test`.
//
// Rules this file follows, inherited from dbus_service_test:
//
//  - Nothing waits on a sleep for a *render*. SetWallpaper returns before the render
//    happens, so "the frame is ready" is knowable only from FrameReady; negative cases
//    are settled by posting a later request that does signal, since the daemon has one
//    FIFO render thread. Process startup is the one exception -- a process appearing on
//    the bus has its own signal (NameOwnerChanged) and that is what is waited on, not a
//    poll loop.
//  - A check that cannot be conclusive on this host says so and is not counted. A
//    skipped check that silently reads as a pass is the failure mode this suite has
//    been bitten by most.

#include "daemon/dbus_service.h"
// For HttpServer::kDefaultPort only -- the shipped port number, taken from the header the
// daemon compiles against rather than written as 18462 here, so a change of port fails
// this file at build time instead of leaving it probing a port nothing serves.
#include "daemon/http_server.h"
#include "tests/client/daemon_client.h"
#include "tests/client/http_probe.h"

#include "support/exported_frame_map.h"
// Fork-side scaffolding, shared with renderer/tests/device_affinity_probe.cpp. It lives in
// the renderer tree because the dependency direction is one-way: a renderer probe may not
// include a backend header, and backend/tests already has renderer/tests on its path.
#include "support/forked_child.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <vulkan/vulkan.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using wallpaperx::DbusService;
using wallpaperx::testing::Check;
using wallpaperx::testing::validationWaived;
using wallpaperx::testing::Frame;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::getFrameFd;
using wallpaperx::testing::getStatus;
using wallpaperx::testing::execChildOrDie;
using wallpaperx::testing::getStatusOrExplain;
using wallpaperx::testing::openFdCount;
using wallpaperx::testing::runningProcessMatches;
using wallpaperx::testing::runSelfProbe;
using wallpaperx::testing::SelfProbeRun;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::StatusReply;
using wallpaperx::testing::suppressCoreDump;
using wallpaperx::testing::wallpaperStatus;

namespace {

// The daemon's shipped default, from WallpaperManager::Config. Restated rather than
// included so that a change to the default fails here loudly instead of silently
// re-pointing this test at whatever the new value is -- the point of this file is that
// something renders at the size users get.
constexpr uint32_t kDaemonWidth = 1920;
constexpr uint32_t kDaemonHeight = 1080;

// Monitor ids are partitioned per phase and never reused across them: an assertion
// about "a monitor with no frame" silently changes meaning the moment another phase has
// touched that id.
constexpr int32_t kMonitorFirstFrame = 0;
constexpr int32_t kMonitorTiming = 1;
constexpr int32_t kMonitorLoadBase = 2;    // .. kMonitorLoadBase + loadCount - 1
constexpr int32_t kMonitorStormBase = 20;  // .. kMonitorStormBase + kStormMonitors - 1
constexpr int32_t kStormMonitors = 4;
constexpr int32_t kMonitorBarrierPre = 30;
constexpr int32_t kMonitorBarrierPost = 31;
// Inside the daemon's 0..31 monitor range, and above every id the phases before it use --
// the responsiveness burst runs 2..17 and the storm 20..23, so 24 and 25 are clear of
// both. (They are not "in the gap between" them, as this comment used to say; the only gap
// there is 18..19 and neither is in it. The ids are fine, the reasoning was not.) Two ids,
// because the closed-stdout phase renders once with its stdout still readable -- to
// establish that the handler does write to fd 1 -- and once after breaking it.
constexpr int32_t kMonitorClosedStdoutPre = 25;
constexpr int32_t kMonitorClosedStdout = 24;
// The port-conflict phase's own id: it renders once, to show the daemon is still doing its
// job while reporting that it lost its config port. Ids still unclaimed: 18..19 and 28..29.
constexpr int32_t kMonitorLostPort = 27;

constexpr int kStormThreads = 4;

// Enough to keep a reply in flight across the device-creation window without the
// polling itself becoming the bottleneck the first render is waiting on.
constexpr int kGpuInitWatchers = 3;
// Sized so each client observes each monitor several times. Monotonicity is a property of
// one observer's sequence, so a client's *first* look at a monitor can never register a
// regression -- with a wide monitor pool and few iterations, nearly every observation was
// a first look and the check had almost nothing to compare. Narrower pool, more
// iterations: most observations become real comparisons. The count is asserted and
// printed, so an edit that makes it vacuous again is visible rather than silent.
constexpr int kStormIterations = 24;

// Upper bound on how many monitors the responsiveness phase queues at once. Each costs
// a full-size exportable image in the daemon (~8 MB at 1080p) that lives until it
// exits, so this is a memory bound as much as a time one.
constexpr int kMaxLoadMonitors = 16;

// How long the queue must take to drain for the responsiveness checks to mean
// anything. Both compare an interval against that drain time, so the drain has to be
// large next to the bare DBus overhead of issuing the burst -- around 0.05 ms per round
// trip, so under 1 ms for the whole of it. Twenty gives that a wide margin while still
// being reached on a 3080 Ti, where a 1080p static render costs under 2 ms.
constexpr auto kMinConclusiveDrain = std::chrono::milliseconds(20);

constexpr auto kProcessTimeout = std::chrono::seconds(30);

// The tail of the line main.cpp prints when a signal really did arrive. One constant
// rather than the literal spelled out at each site, because two of those sites assert its
// PRESENCE (SIGTERM, SIGHUP) and one asserts its ABSENCE (the connection-loss phase), and
// the absence check is only able to fail while the needle is still a string this daemon
// ever prints. Sharing it means a reworded message cannot quietly hollow that check out:
// it turns the presence checks red first, which points at the reword instead of leaving a
// silent green behind.
constexpr const char* kSignalShutdownLine = "received, shutting down";

std::string testAssetDir() { return std::string(WALLPAPERX_TEST_IMAGE_DIR); }

// Goals this run could not exercise at all, because the host or the harness could not
// build the precondition -- as opposed to goals it exercised and that held.
//
// The two were previously indistinguishable from outside: the exit status was a pure
// function of the failure count, so a phase that returned early after printing a reason
// left ctest reporting PASS for coverage that never ran. That is the vacuous-green trap,
// and it sat on the phase written for the worst crash the QA pass found. Reported the way
// the renderer probes report it (modifier_roundtrip_probe's g_notVerified /
// kNotVerifiedExitCode): exit 77, which ctest turns into a SKIP through the
// SKIP_RETURN_CODE property set in backend/CMakeLists.txt. A skip is not a pass, and no
// reader can mistake it for one.
//
// Not a check() failure, deliberately: X.3 forbids reporting a fault of the host or the
// harness as a fault of the daemon, and every caller here names a cause on this side of
// the boundary.
int g_notVerified = 0;
constexpr int kNotVerifiedExitCode = 77;

void notVerified(const char* goals, const std::string& reason) {
    std::printf("daemon_system_test: NOT VERIFIED -- %s could not be exercised: %s. This run establishes "
                "nothing about them, and is reported to ctest as a skip rather than as a pass.\n",
                goals, reason.c_str());
    std::fflush(stdout);
    ++g_notVerified;
}

using Clock = std::chrono::steady_clock;

double millis(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(d).count();
}

// A solid RGBA PNG. The daemon scales it to the full target, so a solid source makes
// every pixel of the exported frame a known value -- which is what lets the content
// check below be a straight comparison rather than a resampling model.
std::string writeSolidImage(const char* name, unsigned char r, unsigned char g, unsigned char b) {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/" + name;
    constexpr int kSide = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(kSide) * kSide * 4);
    for (int i = 0; i < kSide * kSide; ++i) {
        pixels[i * 4 + 0] = r;
        pixels[i * 4 + 1] = g;
        pixels[i * 4 + 2] = b;
        pixels[i * 4 + 3] = 255;
    }
    if (stbi_write_png(path.c_str(), kSide, kSide, 4, pixels.data(), kSide * 4) == 0) {
        throw std::runtime_error("could not write " + path);
    }
    return path;
}

// ---------------------------------------------------------------------------- //
// The daemon under test, as a child process.
// ---------------------------------------------------------------------------- //

// execChildOrDie -- the PR_SET_PDEATHSIG arming every fork in this file goes through -- is
// declared in support/forked_child.h, included above, and is deliberately not restated
// here. Shared rather than copied because X.8 is a property of *every* forked child in the
// repo, and a file-local copy left the renderer's forking probe uncovered by construction.

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
    };

    // `extraEnv` entries are "KEY=VALUE"; each replaces any inherited entry for that key.
    explicit SpawnedDaemon(const std::vector<std::string>& extraEnv = {}, Stdout stdoutMode = Stdout::Captured) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) throw std::runtime_error("pipe2 failed");

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

        std::vector<char*> envp;
        envp.reserve(env.size() + 1);
        for (std::string& entry : env) envp.push_back(entry.data());
        envp.push_back(nullptr);

        std::string binary = WALLPAPERX_DAEMON_BINARY;
        char* argv[] = {binary.data(), nullptr};

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
                _exit(126);
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
            if (output().find(needle) != std::string::npos) return true;
            // EOF on the capture pipe means every write end is closed, i.e. the child is
            // gone. Without this the wait would burn the whole timeout on a daemon that
            // died at startup -- and a 30-second stall is exactly how a setup failure
            // gets mistaken for a slow one.
            if (m_readerDone.load(std::memory_order_acquire)) {
                return output().find(needle) != std::string::npos;
            }
            if (Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

private:
    void readLoop() {
        char buf[4096];
        std::string pending;
        while (true) {
            const ssize_t n = read(m_readFd, buf, sizeof(buf));
            // EINTR is not EOF. Treating it as one truncates the capture, and every
            // assertion about the daemon's output below -- "stopped cleanly", the signal
            // line, the validation scan -- then reads a short buffer and fails, or passes,
            // for a reason that has nothing to do with the daemon.
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_output.append(buf, static_cast<size_t>(n));
            }
            pending.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::printf("  [daemon] %s\n", pending.substr(0, nl).c_str());
                pending.erase(0, nl + 1);
            }
        }
        if (!pending.empty()) std::printf("  [daemon] %s\n", pending.c_str());
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
        if (m_reader.joinable()) m_reader.join();
        close(m_readFd);
        m_readFd = -1;
        return m_exitCode;
    }

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
    mutable std::mutex m_mutex;
    std::string m_output;
};

// The line the orphan probe prints so the parent can look for the daemon afterwards. One
// constant, because the probe writes it and the phase parses it in two different
// processes: a reworded literal on one side alone would make the phase report a probe
// that never started rather than a daemon that never died.
constexpr const char* kOrphanPidPrefix = "orphan-probe: daemon pid ";

// The child half of the aborted-parent phase, running as a fresh exec of this binary with
// --orphan-probe. Spawns a daemon through the ordinary SpawnedDaemon path, says which pid
// it got, and then dies the way a failed assertion inside a library kills a test: SIGABRT,
// so no destructor of any kind runs and ~SpawnedDaemon's kill/waitpid never happens.
//
// Its own process rather than a fork() of the test: a fork of a process that already has
// sdbus dispatch threads may only call async-signal-safe functions until it execs, and
// SpawnedDaemon's constructor allocates throughout.
int runOrphanProbe() {
    // The abort below is deliberate, so its core -- and, on a host whose core_pattern pipes
    // to a collector, its journal entry and desktop crash notification -- is pure noise.
    // PR_SET_DUMPABLE and not setrlimit(RLIMIT_CORE, 0); the measurement that separates
    // them is on suppressCoreDump(). execve resets the flag, so the daemon spawned below
    // does not inherit it and its /proc entries stay readable to the phase that polls them.
    suppressCoreDump();

    SpawnedDaemon daemon;
    if (!daemon.waitForOutput(std::string("serving ") + DbusService::kBusName, std::chrono::seconds(30))) {
        std::printf("orphan-probe: the daemon never announced itself, so there is nothing to orphan\n");
        std::fflush(stdout);
        return 1;
    }
    std::printf("%s%d\n", kOrphanPidPrefix, static_cast<int>(daemon.pid()));
    std::fflush(stdout);
    std::abort();
}

// runSelfProbe() and its SelfProbeRun live in support/forked_child.h: the renderer's own
// forking probe needs the same self-exec-and-classify apparatus for X.8, and both need the
// bounded drain and bounded reap it carries. SpawnedDaemon is not reused for it because
// that class is about the daemon binary specifically and needs a reader thread this does
// not.

// How long the orphan probe gets to spawn a daemon, wait for its startup line and abort.
// Comfortably above the 30 s the probe itself allows for the announcement, so a probe that
// merely lost the announcement still gets to print its own reason before this expires.
constexpr auto kSelfProbeTimeout = std::chrono::seconds(60);

bool daemonRunning(pid_t pid) { return runningProcessMatches(pid, WALLPAPERX_DAEMON_BINARY); }

// A session bus of this test's own, started as a child process so it can be killed.
//
// Exists for one reason: to be destroyed under a running daemon. ctest's bus comes from
// dbus-run-session and is shared with every other phase in this file, so killing it would
// take the test's own connection with it.
class NestedBus {
public:
    NestedBus() {
        const std::string binaryPath = WALLPAPERX_DBUS_DAEMON_BINARY;
        if (binaryPath.empty()) {
            m_reason = "no dbus-daemon binary was found at configure time";
            return;
        }

        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) {
            m_reason = "pipe2 failed";
            return;
        }

        // Assembled before fork() for the same reason SpawnedDaemon assembles its
        // environment there: this process has sdbus dispatch threads, and allocating
        // between fork() and exec() can deadlock on a lock one of them held.
        std::string binary = binaryPath;
        std::string session = "--session";
        std::string nofork = "--nofork";
        std::string printAddress = "--print-address";
        char* argv[] = {binary.data(), session.data(), nofork.data(), printAddress.data(), nullptr};

        const pid_t parentBeforeFork = getpid();
        m_pid = fork();
        if (m_pid < 0) {
            close(fds[0]);
            close(fds[1]);
            m_reason = "fork failed";
            return;
        }
        if (m_pid == 0) {
            if (dup2(fds[1], STDOUT_FILENO) == -1) _exit(126);
            execChildOrDie(parentBeforeFork, binary.c_str(), argv, nullptr);
        }
        close(fds[1]);

        // Bounded read of the address line. A bus that fails to start closes the pipe,
        // which ends this at EOF rather than at the deadline; either way the failure is
        // reported as a setup failure and never as a property of the daemon.
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        std::string line;
        while (line.find('\n') == std::string::npos) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) {
                m_reason = "dbus-daemon printed no address within 10s";
                break;
            }
            pollfd pfd{fds[0], POLLIN, 0};
            const int ready = poll(&pfd, 1, static_cast<int>(remaining));
            if (ready < 0) {
                if (errno == EINTR) continue;
                m_reason = "poll on the nested bus address pipe failed";
                break;
            }
            if (ready == 0) {
                m_reason = "dbus-daemon printed no address within 10s";
                break;
            }
            char buf[512];
            const ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                m_reason = "dbus-daemon exited before printing an address";
                break;
            }
            line.append(buf, static_cast<size_t>(n));
        }

        const size_t newline = line.find('\n');
        if (newline != std::string::npos) m_address = line.substr(0, newline);
        if (m_address.empty() && m_reason.empty()) m_reason = "dbus-daemon printed an empty address";

        // The read end is kept and drained for the child's whole lifetime, not closed
        // here. dbus-daemon's fd 1 stays pointed at this pipe long after the address
        // line, so closing the only reader would leave it writing into a readerless pipe:
        // one --session config warning at SIGPIPE's default disposition and the nested bus
        // dies of the test's own doing, either before the daemon comes up (reported as a
        // daemon that never started) or after (reported as a connection loss the test did
        // not cause). Leaving it open and unread is not enough either -- it trades SIGPIPE
        // for a blocked write once the 64K buffer fills, which stalls the bus instead of
        // killing it. Same m_readFd/drain-thread pair SpawnedDaemon already uses.
        //
        // CONTROL: not expressible as a check here -- making the real dbus-daemon write to
        // stdout after its address line is host- and version-dependent, so a check built on
        // it would be the flaky kind. Controlled outside the suite instead, on the
        // mechanism: a scratch program that forks a child printing one line, then a second
        // line after a delay, reports "child died on signal 13" when the parent closes the
        // read end after the first line and "child exited 0" when it drains instead. That
        // is exactly the difference this member makes, measured rather than argued.
        m_readFd = fds[0];
        try {
            m_drain = std::thread([this] { drainLoop(); });
        } catch (...) {
            // Past fork() with no drain running: kill the child now rather than leave it
            // writing into a pipe nobody reads for the rest of the phase.
            kill();
            close(m_readFd);
            m_readFd = -1;
            m_address.clear();
            m_reason = "could not start the drain thread for the nested bus";
        }
    }

    ~NestedBus() { kill(); }

    NestedBus(const NestedBus&) = delete;
    NestedBus& operator=(const NestedBus&) = delete;

    bool ok() const { return !m_address.empty(); }
    const std::string& reason() const { return m_reason; }
    const std::string& address() const { return m_address; }

    // Destroys the bus out from under whatever is connected to it. SIGKILL, not SIGTERM:
    // an orderly bus shutdown is a different event from a bus that vanishes, and the
    // second is the one a client has no chance to prepare for.
    void kill() {
        if (m_pid > 0) {
            ::kill(m_pid, SIGKILL);
            int status = 0;
            waitpid(m_pid, &status, 0);
            m_pid = -1;
        }
        // After the reap, for the same reason SpawnedDaemon joins after its own: the
        // drain ends at EOF, and EOF arrives only once the child's write end is gone.
        // Joining here rather than in the destructor keeps the join on the one path every
        // exit goes through -- the destructor calls this too.
        if (m_drain.joinable()) m_drain.join();
        if (m_readFd >= 0) {
            close(m_readFd);
            m_readFd = -1;
        }
    }

private:
    // Discards whatever dbus-daemon says on stdout after the address line. The bytes are
    // of no interest; keeping a reader on the pipe is the entire point.
    void drainLoop() {
        char buf[4096];
        while (true) {
            const ssize_t n = read(m_readFd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
        }
    }

    pid_t m_pid = -1;
    int m_readFd = -1;
    std::thread m_drain;
    std::string m_address;
    std::string m_reason;
};

// Watches org.freedesktop.DBus for the daemon's name appearing and disappearing.
//
// Subscribed before the daemon is spawned, so there is no window in which the name
// could be acquired unobserved -- which is what makes this a signal wait rather than a
// poll loop with a sleep in it.
class NameWatcher {
public:
    NameWatcher()
        : m_proxy(sdbus::createProxy(sdbus::ServiceName{"org.freedesktop.DBus"},
                                     sdbus::ObjectPath{"/org/freedesktop/DBus"})) {
        m_proxy->uponSignal("NameOwnerChanged")
            .onInterface("org.freedesktop.DBus")
            .call([this](const std::string& name, const std::string& oldOwner, const std::string& newOwner) {
                if (name != DbusService::kBusName) return;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_events;
                }
                (void)oldOwner;
                (void)newOwner;
                m_changed.notify_all();
            });
    }

    // True if the daemon currently holds the name, asked of the bus itself.
    bool ownedNow() {
        bool has = false;
        m_proxy->callMethod("NameHasOwner")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string(DbusService::kBusName))
            .storeResultsTo(has);
        return has;
    }

    // Waits until the name is (or is not) owned. Returns false on timeout.
    //
    // Deliberately not "cache a bool in the signal handler and wait on it". Two attempts
    // at that were both wrong and in opposite directions: caching only in the handler
    // left the flag holding the *previous* daemon's value after an early return, so a
    // later wait was satisfied before the next daemon existed; writing the bus's answer
    // back into the flag then let a NameOwnerChanged delivered between the reply and the
    // lock be overwritten, losing the very transition being waited for.
    //
    // The signal carries no state here at all -- it only bumps a counter, which cannot be
    // lost or clobbered. Ownership itself is always read from the bus, which is the one
    // authority that cannot go stale. Snapshotting the counter *before* asking means any
    // transition racing the query bumps it and wakes the wait, so nothing is missed.
    bool waitFor(bool owned) {
        const auto deadline = std::chrono::steady_clock::now() + kProcessTimeout;
        while (true) {
            uint64_t seen = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                seen = m_events;
            }
            if (ownedNow() == owned) return true;

            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_changed.wait_until(lock, deadline, [&] { return m_events != seen; })) return false;
        }
    }

private:
    // GOAL: X.4 -- no test aborts or corrupts memory instead of reporting.
    // WHY-THIS-SHAPE: m_proxy is declared *last*, which reads backwards next to the
    //       constructor that initialises it first, and it has to be. Members are
    //       destroyed in reverse declaration order, and the signal callback registered
    //       on the proxy touches m_mutex, m_events and m_changed. With the proxy
    //       declared first it was destroyed last, so a NameOwnerChanged arriving during
    //       teardown ran notify_all() on an already-destroyed condition_variable.
    //       ThreadSanitizer caught it on a real run: "data race ...
    //       std::condition_variable::~condition_variable()" with the previous access
    //       inside NameWatcher's own lambda. Declaring the proxy last makes it the
    //       first thing destroyed, and sdbus unregisters the handler there.
    // CONTROL: none that reproduces, and saying so is the point. Putting the
    //       declaration back above m_mutex and running daemon_system_lavapipe six times
    //       under the TSan tier produced zero ThreadSanitizer reports -- the racy layout
    //       is silent here on demand. An earlier version of this comment claimed the
    //       opposite ("six runs ... each reported the race twice"); that figure did not
    //       hold up and is withdrawn. The race needs a NameOwnerChanged still in flight
    //       at the instant the watcher is destroyed, and this test's shutdown almost
    //       never produces that window; the one time TSan did catch it was on an
    //       unrelated run whose timing happened to hit it.
    //       So this ordering is justified by the language rule rather than by a
    //       control: members are destroyed in reverse declaration order, and the
    //       callback registered on m_proxy touches all three members below it. That is
    //       checkable by reading it and does not depend on a scheduler. Do not read the
    //       silence above as evidence the ordering is optional -- an unreproducible race
    //       is still a race, and a control that cannot make this go red means this
    //       comment is a claim about C++ semantics, not an observation.
    std::mutex m_mutex;
    std::condition_variable m_changed;
    uint64_t m_events = 0;
    std::unique_ptr<sdbus::IProxy> m_proxy;
};

// Whether VK_LAYER_KHRONOS_validation can be loaded on this host. Asked through the
// loader, from this process, with the same environment the daemon child inherits -- so
// the answer is the one the child will get. Without it, "the daemon printed no
// [vulkan] warnings" is equally true of a daemon that had no validator listening, and
// the only GPU-side oracle this test has would be silently vacuous.
bool validationLayerAvailable() {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) return true;
    }
    return false;
}

// Lines the daemon printed that came from the validation layer, excluding the one
// notice Device emits when the layer is present but VK_EXT_validation_features is not
// -- that is a report about the layer build, not a finding about our Vulkan usage.
std::vector<std::string> validationLines(const std::string& output) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < output.size()) {
        const size_t end = output.find('\n', pos);
        const std::string line = output.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (line.rfind("[vulkan] ", 0) == 0 &&
            line.find("VK_EXT_validation_features is not") == std::string::npos) {
            lines.push_back(line);
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return lines;
}

// ---------------------------------------------------------------------------- //
// Phase 2 -- one frame, end to end, into a process that did not allocate it.
// ---------------------------------------------------------------------------- //
void checkFirstFrame(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& bluePath, const Check& check) {
    setWallpaper(proxy, "static", bluePath, kMonitorFirstFrame);
    const bool arrived = signals.wait(kMonitorFirstFrame, 1);
    check(arrived, "FrameReady(monitor, 1) arrives for the first static wallpaper");
    // Everything below reads the frame that signal announced; without it there is
    // nothing to export and GetFrameFd would fail for a reason already reported.
    if (!arrived) return;

    const Frame frame = getFrameFd(proxy, kMonitorFirstFrame);

    check(fcntl(frame.fd.get(), F_GETFD) != -1, "the fd received from another process is open in this one");
    check(frame.width == kDaemonWidth && frame.height == kDaemonHeight,
          "the frame is the daemon's shipped default size (1920x1080), which no other test renders");
    check(frame.generation == 1, "the first frame's generation is 1");

    const uint64_t minimumBytes = static_cast<uint64_t>(frame.width) * frame.height * 4;
    check(frame.memorySize >= minimumBytes, "memory_size covers at least width*height*4 bytes");

    // Diagnostic here, a precondition further down. An OPAQUE_FD is a handle to a
    // driver object -- on NVIDIA a /dev/nvidiactl fd -- with no size and no seek, so
    // lseek returning -1 is the correct answer for it and asserting on it would fail a
    // working export. It becomes an assertion only on the dma_buf path, where the fd
    // does name a sized object and where a published memory_size larger than that
    // object SIGBUSes the mmap below instead of failing by name.
    const off_t fdSize = lseek(frame.fd.get(), 0, SEEK_END);
    // Captured here, not where it is printed: printf is allowed to set errno even when it
    // succeeds, and three of them run in between -- so the skip message below was liable
    // to name an unrelated error, or "Success", on the one path where that message is the
    // entire diagnostic.
    const int lseekErrno = errno;

    std::printf("daemon_system_test: monitor %d -> %ux%u, memory_size=%llu, handle_type=%d, modifier=0x%llx, "
                "fd is %s (%lld bytes)\n",
                kMonitorFirstFrame, frame.width, frame.height,
                static_cast<unsigned long long>(frame.memorySize), frame.handleType,
                static_cast<unsigned long long>(frame.drmModifier), wallpaperx::testing::fdDescription(frame.fd.get()).c_str(),
                static_cast<long long>(fdSize));

    // Everything above holds on both handle types. The pixel comparison does not: only
    // a LINEAR dma_buf is CPU-addressable at a constant stride. OPAQUE_FD (this
    // machine's NVIDIA policy) carries no layout at all, and a tiled modifier's
    // rowPitch is not a row length -- mapping either and comparing bytes would fail a
    // perfectly correct export.
    if (frame.handleType != 1) {
        std::printf("daemon_system_test: cross-process content check skipped -- handle_type=%d (OPAQUE_FD); "
                    "the fd is not a dma_buf and cannot be mapped. Run the lavapipe twin for this half.\n",
                    frame.handleType);
        return;
    }
    if (frame.drmModifier != wallpaperx::testing::kDrmModifierLinear || frame.planes.size() != 1) {
        std::printf("daemon_system_test: cross-process content check skipped -- modifier 0x%llx with %zu plane(s) "
                    "is not linear single-plane, so its rows are not CPU-addressable.\n",
                    static_cast<unsigned long long>(frame.drmModifier), frame.planes.size());
        return;
    }
    // "I cannot answer" and "the buffer is too small" are different results and only the
    // second is a defect. A dma_buf whose driver refuses SEEK_END returns -1, which the
    // sibling tier treats as a skip -- scoring it as a failure here would report a defect
    // in the export path for a property of the driver.
    if (fdSize < 0) {
        std::printf("daemon_system_test: cross-process content check skipped -- lseek(SEEK_END) cannot size this "
                    "fd (%s), so the mapping below could not be bounded safely.\n",
                    std::strerror(lseekErrno));
        return;
    }
    check(static_cast<uint64_t>(fdSize) >= frame.memorySize,
          "the dma_buf behind the fd is at least as large as the published memory_size");
    if (static_cast<uint64_t>(fdSize) < frame.memorySize) return;

    const wallpaperx::testing::MappedFrame mapping(frame.fd.get(), frame.memorySize);
    if (!mapping.valid()) {
        std::printf("daemon_system_test: cross-process content check skipped -- mmap failed (%s).\n",
                    mapping.error().c_str());
        return;
    }

    // The source is solid blue (R=0, G=0, B=255) and the frame is B8G8R8A8.
    constexpr int kTolerance = 6;
    const uint64_t offset = frame.planes[0].get<0>();
    const uint64_t pitch = frame.planes[0].get<1>();
    size_t badPixels = 0;
    unsigned char firstBad[4] = {0, 0, 0, 0};
    for (uint32_t y = 0; y < frame.height; ++y) {
        const unsigned char* row = mapping.row(offset, pitch, y, static_cast<size_t>(frame.width) * 4);
        if (row == nullptr) {
            badPixels += frame.width;
            continue;
        }
        for (uint32_t x = 0; x < frame.width; ++x) {
            const unsigned char* px = row + static_cast<size_t>(x) * 4;
            if (std::abs(int(px[0]) - 255) > kTolerance || std::abs(int(px[1]) - 0) > kTolerance ||
                std::abs(int(px[2]) - 0) > kTolerance) {
                if (badPixels == 0) std::memcpy(firstBad, px, 4);
                ++badPixels;
            }
        }
    }

    check(badPixels == 0,
          "a process that never allocated the image reads the expected pixels through the published layout");
    if (badPixels != 0) {
        std::fprintf(stderr, "  %zu of %llu pixels differ; expected B,G,R = 255,0,0, first mismatch was %d,%d,%d\n",
                     badPixels, static_cast<unsigned long long>(frame.width) * frame.height, firstBad[0], firstBad[1],
                     firstBad[2]);
    } else {
        std::printf("daemon_system_test: %ux%u pixels verified across the process boundary\n", frame.width,
                    frame.height);
    }
}

// Samples GetStatus from several clients across the daemon's first device creation, and
// grades every reply against the one cross-field invariant snapshot() promises.
//
// GOAL: S6.18 -- a GetStatus reply is internally consistent: it never reports an
//       uninitialised GPU beside a monitor that already has a frame.
// WHY-THIS-SHAPE: the invariant is only *falsifiable* while gpu.initialized is false.
//       statusSelfConsistent returns true at its first branch once that flag is set,
//       WallpaperManager::ensureGpu sets it exactly once and nothing clears it, so any
//       sampling after the first render grades nothing -- which is precisely how this
//       check spent its previous life in the concurrent-clients storm, pinned at zero
//       inconsistencies over ~29 replies that had all short-circuited. The window is the
//       first SetWallpaper: the daemon has monitors in its map, is about to commit a
//       frame, and has not yet published a GPU. So the watchers start before
//       checkFirstFrame issues that request and stop after its frame has arrived, which
//       is the only interval in the whole suite where a two-lock snapshot() could be
//       caught. checkFirstFrame is called between them rather than duplicated, so the
//       phase it already owns keeps its own assertions and its own first-render
//       semantics.
// COVERAGE-ASSERT: the run is required to have observed the flag both false and true.
//       Without that, a watcher fleet that started too late would report zero
//       inconsistencies out of replies that were all post-initialisation -- exactly the
//       vacuum this move exists to escape -- and it would look identical to a pass.
// CONTROL: ensureGpu() changed to publish m_gpuStatus.initialized from a detached
//       thread 1200 ms late, so the first frame commits while the GPU still reads as
//       absent -> "FAIL: no GetStatus reply reports a frame before the GPU exists" over
//       8441 replies. The coverage assert went red in the same run (0 post-init), which
//       is the other half of it observed failing. Reverted.
// WHAT DID NOT REPRODUCE, recorded so nobody over-trusts this check: splitting
//       snapshot() into two acquisitions of m_stateMutex -- the regression that
//       function's own comment warns about -- produced 0 inconsistencies over ~25,000
//       replies across three runs. It cannot be caught that way. ensureGpu() publishes
//       the GPU *before* any render commits, so the two reads have to be separated by
//       the whole of device creation to disagree, and a lock released and immediately
//       retaken is nowhere near that wide. This check catches a snapshot whose halves
//       drift far apart; it does not catch a narrow split, and a future reader must not
//       read its green as proof that snapshot() is still single-locked.
// CONTROL (the readiness barrier): with the barrier removed, the straddle depends on
//       three fresh threads each completing a session-bus connect before the daemon
//       finishes ensureGpu -- an assertion on the scheduler. The barrier makes it a
//       property of the harness, and its own check ("every status watcher was live
//       before the first render was requested") is what would fail if a watcher never
//       reached the bus.
// CONTROL (the Stopper scope guard): checkFirstFrame patched to `throw
//       std::runtime_error(...)` on entry, with the watchers live -> with the guard,
//       exit 1 and "FAIL: unexpected exception: CONTROL: simulated sdbus failure
//       mid-phase"; with `Stopper` left uninstantiated, exit 134 and "terminate called"
//       -- the joinable-thread destructor taking the process down and replacing every
//       FAIL line with a bare abort. Both reverted.
struct GpuInitWatch {
    std::atomic<bool> stop{false};
    // Counts watchers that have completed one SUCCESSFUL reply. The first SetWallpaper
    // waits on this: the straddle must be a property of the harness, not of whether the
    // scheduler happened to run three fresh threads (each of which still has a
    // session-bus connect ahead of it) before the daemon finished creating its device.
    //
    // Successful, not merely attempted. Counting failed calls here would let the barrier
    // release with no watcher having ever been answered, and the phase would then report
    // "the sampling straddled the first device creation" as the failure -- a timing
    // story -- when the truth is that GetStatus never worked at all. The bounded wait at
    // the call site is what keeps a never-answering daemon from hanging the phase.
    std::atomic<int> ready{0};
    std::atomic<int> replies{0};
    std::atomic<int> preInit{0};
    std::atomic<int> postInit{0};
    std::atomic<int> inconsistent{0};
    std::atomic<int> errors{0};
};

void gpuInitWatchThread(GpuInitWatch& watch) {
    // Constructed inside the thread and inside the guard, for the reason stormThread
    // gives: an exception escaping a thread entry point is std::terminate, which would
    // replace this phase's FAIL lines with a bare abort.
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
        proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                   sdbus::ObjectPath{DbusService::kObjectPath});
    } catch (const std::exception&) {
        ++watch.errors;
        return;
    }

    bool announced = false;
    while (!watch.stop.load(std::memory_order_relaxed)) {
        // The catch stays even though getStatus() no longer throws: this is a thread entry
        // point, so anything that escapes it -- a bad_alloc out of the json, most plausibly
        // -- is std::terminate and takes the phase's FAIL lines with it.
        try {
            const StatusReply status = getStatus(*proxy);
            if (!status.ok()) {
                // A failed GetStatus is not a consistency violation. Counted, and reported
                // below only if it swallowed the whole sample. Deliberately does NOT mark
                // this watcher ready: readiness means "answered once", so that the pre-init
                // sample the phase depends on is guaranteed rather than raced for. A watcher
                // that only ever errors therefore never becomes ready, and the barrier below
                // falls through on its deadline rather than hanging -- the phase then reports
                // the zero readiness directly instead of blaming the timing.
                ++watch.errors;
            } else {
                ++watch.replies;
                if (!announced) {
                    announced = true;
                    ++watch.ready;
                }
                const nlohmann::json& json = status.json();
                const bool initialized =
                    json.contains("gpu") && json["gpu"].is_object() && json["gpu"].value("initialized", false);
                if (initialized) {
                    ++watch.postInit;
                } else {
                    ++watch.preInit;
                }
                if (!wallpaperx::testing::statusSelfConsistent(status)) ++watch.inconsistent;
            }
        } catch (const std::exception&) {
            ++watch.errors;
        }
        // Outside the try/catch, so the error path is paced too. Inside it, a daemon
        // that starts refusing calls (crashed mid-render, bus name dropped, an immediate
        // ServiceUnknown instead of a timeout) put three threads into a 100%-CPU spin for
        // the whole of checkFirstFrame's timeout -- starving the very render being
        // measured, so a daemon fault surfaced as a timing failure blamed on the render
        // path. A millisecond still yields hundreds of samples across a device creation
        // measured in tens to hundreds of milliseconds.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void checkFirstFrameUnderStatusLoad(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& bluePath,
                                    const Check& check) {
    GpuInitWatch watch;
    std::vector<std::thread> watchers;
    watchers.reserve(kGpuInitWatchers);

    // Stop and join on EVERY path out of here, exceptions included, and constructed
    // BEFORE the spawn loop rather than after it. checkFirstFrame calls setWallpaper()
    // and getFrameFd() without a try/catch and both throw sdbus::Error when the daemon
    // answers with an error or dies mid-phase; letting that unwind past a vector of
    // joinable threads is std::terminate -- a bare SIGABRT in place of this phase's FAIL
    // lines and main's own handler -- while the live watchers keep writing to a `watch`
    // whose storage is being destroyed. Declared after the loop it would still leave one
    // hole: std::thread's own constructor throws std::system_error on EAGAIN when the
    // process is at its thread limit, and this binary has already spawned the daemon and
    // several bus connections, so a throw on the second or third iteration would unwind
    // past the one or two threads already in the vector before any guard existed.
    struct Stopper {
        GpuInitWatch& watch;
        std::vector<std::thread>& watchers;
        void stopAndJoin() {
            watch.stop.store(true, std::memory_order_relaxed);
            for (std::thread& t : watchers) {
                if (t.joinable()) t.join();
            }
        }
        ~Stopper() { stopAndJoin(); }
    } stopper{watch, watchers};

    for (int i = 0; i < kGpuInitWatchers; ++i) {
        watchers.emplace_back([&watch] { gpuInitWatchThread(watch); });
    }

    // Every watcher has answered once before the first render is asked for, so the
    // pre-init sample below is guaranteed rather than raced for. Bounded: a watcher that
    // cannot reach the bus at all would otherwise stall the whole test here, and that
    // fault is reported by the reply/error counts further down.
    const auto barrierDeadline = std::chrono::steady_clock::now() + wallpaperx::testing::kSignalTimeout;
    while (watch.ready.load() < kGpuInitWatchers && std::chrono::steady_clock::now() < barrierDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const int readyAtStart = watch.ready.load();

    checkFirstFrame(proxy, signals, bluePath, check);

    stopper.stopAndJoin();

    const int replies = watch.replies.load();
    const int preInit = watch.preInit.load();
    const int postInit = watch.postInit.load();

    // The coverage assert comes first: if the sampling missed the transition, the
    // invariant below was applied to nothing and reporting it as green would be the
    // same false signal this phase was moved to escape.
    check(readyAtStart == kGpuInitWatchers,
          "every status watcher was live before the first render was requested");
    const bool straddled = preInit > 0 && postInit > 0;
    check(straddled, "the GetStatus sampling straddled the daemon's first device creation");
    if (!straddled) {
        std::fprintf(stderr,
                     "  %d replies: %d before the GPU was published, %d after -- the consistency check below "
                     "graded nothing, because statusSelfConsistent is only falsifiable while gpu.initialized "
                     "is false\n",
                     replies, preInit, postInit);
    }

    check(watch.inconsistent.load() == 0, "no GetStatus reply reports a frame before the GPU exists");

    if (replies == 0) {
        check(false, "the status watchers got at least one reply");
        std::fprintf(stderr, "  %d client error(s) and no replies at all\n", watch.errors.load());
    }

    std::printf("daemon_system_test: %d GetStatus replies across first device creation (%d pre-init, %d post-init, "
                "%d client error(s))\n",
                replies, preInit, postInit, watch.errors.load());
}

// ---------------------------------------------------------------------------- //
// Phase 3 -- the event loop stays responsive while the GPU thread is busy.
//
// This is the one mechanical check of the rule PLAN.md records as unenforced: no DBus
// handler touches the GPU. SetWallpaper records and posts; GetFrameFd is asynchronous
// and answered from the GPU thread. A handler that rendered inline would still be
// *correct* -- it would just serialise every client behind the render queue, which is
// precisely what cannot be seen from a single-threaded test that waits for each frame.
//
// Two properties, chosen because each actually separates the two designs. The first
// draft of this phase asserted that GetStatus answers quickly while renders are queued,
// which sounds like the property and is not one: with the handler rendering inline, the
// whole burst would already have finished by the time GetStatus was issued, so it would
// have answered quickly too and the check would have passed for the daemon it exists to
// catch. What does separate them:
//
//   - The time to *issue* the burst. Inline rendering makes issuing the burst take as
//     long as rendering it; posting makes it a rounding error next to the same drain.
//     Controlled: a daemon patched to render inside SetWallpaper takes 27.3 ms to issue
//     sixteen requests that then drain in 28.7 ms, against 0.4 ms and 28.0 ms for the
//     real one, and this check fails.
//
// The GetStatus/GetFrameFd ordering is *printed and not asserted*, which is worth
// stating plainly because the first draft did assert it. The reasoning was that a
// synchronous GetFrameFd would hold the event loop and GetStatus could not overtake it.
// That is true, and it still cannot fail: the configuration it describes is unreachable.
// GetFrameFd does GPU work, the Device aborts on any GPU call from a thread other than
// its creator, and the creator is the render worker -- so a GetFrameFd answered on the
// event loop kills the daemon before any ordering can be observed. Controlled, and that
// is exactly what happens: the daemon dies and the test reports a disconnect rather than
// the ordering.
//
// Which corrects something PLAN.md records: the "no DBus handler touches the GPU" rule
// is *not* unenforced. The Device's thread pinning enforces it, fatally and at the first
// violation. What has no mechanical enforcement is the weaker half -- a handler doing
// slow *non-GPU* work -- and the issue-time check above is what covers that.
// ---------------------------------------------------------------------------- //
void checkResponsivenessUnderLoad(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& bluePath,
                                  const Check& check) {
    // One warm render, to size the load. The GPU is already initialised by phase 2, so
    // this measures a render and not a device creation.
    const auto renderStart = Clock::now();
    setWallpaper(proxy, "static", bluePath, kMonitorTiming);
    if (!signals.wait(kMonitorTiming, 1)) {
        check(false, "the timing render completes (responsiveness phase cannot run without it)");
        return;
    }
    const auto singleRender = Clock::now() - renderStart;

    // Enough queued work that a GPU-blocking handler could not hide. Distinct monitors,
    // so nothing coalesces away: coalescing is what makes a burst at *one* monitor
    // finish in one render, and that would leave nothing to be busy with.
    const double perRenderMs = std::max(1.0, millis(singleRender));
    const int loadCount = std::clamp(static_cast<int>(400.0 / perRenderMs) + 1, 4, kMaxLoadMonitors);

    // The second client is built before the clock starts. Creating a connection costs
    // milliseconds, and those would land inside the interval being measured.
    auto observer =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

    const auto loadStart = Clock::now();
    for (int i = 0; i < loadCount; ++i) {
        setWallpaper(proxy, "static", bluePath, kMonitorLoadBase + i);
    }
    const auto issueTime = Clock::now() - loadStart;

    // Asks for the frame of the *last* monitor queued, so its export task sits behind
    // every render in the FIFO. Issued first; GetStatus below is issued after it and
    // must still finish first.
    // Plain variables, not atomics: join() below is the synchronisation point, and
    // everything the thread wrote happens-before everything read after it.
    Clock::time_point frameDoneAt{};
    bool frameFailed = false;
    // jthread, not thread: everything between here and the join() below is a blocking DBus
    // exchange with a daemon that may die or time out mid-burst -- which is exactly what a
    // daemon answering GetFrameFd on the event loop does. Unwinding past a still-joinable
    // std::thread is std::terminate, so the one regression this phase exists to catch would
    // have produced a bare SIGABRT with no FAIL lines instead of reaching main's handler.
    // The explicit join() below stays: it is the happens-before edge that makes the plain
    // frameDoneAt/frameFailed safe to read, and it leaves the destructor with nothing to do.
    std::jthread frameRequest([&] {
        try {
            const Frame frame = getFrameFd(*observer, kMonitorLoadBase + loadCount - 1);
            (void)frame;
        } catch (const std::exception&) {
            frameFailed = true;
        }
        frameDoneAt = Clock::now();
    });

    const StatusReply status = getStatusOrExplain(proxy);
    const auto statusDoneAt = Clock::now();
    check(status.ok(), "GetStatus still returns parseable JSON while renders are queued");

    frameRequest.join();
    check(!frameFailed, "the GetFrameFd issued while the queue was deep is answered rather than failing");

    bool round1Ok = true;
    for (int i = 0; i < loadCount; ++i) {
        if (!signals.wait(kMonitorLoadBase + i, 1)) round1Ok = false;
    }
    const auto drain = Clock::now() - loadStart;
    check(round1Ok, "every queued render completes and announces itself");

    const auto frameLatency = frameDoneAt - loadStart;
    std::printf("daemon_system_test: one render %.1f ms; issuing %d requests took %.1f ms, draining them %.1f ms; "
                "GetFrameFd behind the queue took %.1f ms and a GetStatus issued after it finished %.1f ms sooner\n",
                perRenderMs, loadCount, millis(issueTime), millis(drain), millis(frameLatency),
                millis(frameDoneAt - statusDoneAt));

    // Printed, never counted -- see the header comment above for why this one cannot
    // fail for any daemon that does not already abort.
    std::printf("daemon_system_test: GetStatus %s the GetFrameFd issued before it (corroborating, not asserted -- "
                "the Device's thread pinning is what makes a GPU-touching handler fatal)\n",
                statusDoneAt < frameDoneAt ? "overtook" : "did not overtake");

    // Round 2 is only meaningful, and only *possible*, if round 1 committed everywhere:
    // it waits for each monitor's generation 2, which a monitor left at generation 0 by a
    // lost round-1 render can never reach. Continuing would add a 30 s timeout per
    // monitor and then grade the resulting stall as a very slow drain -- which is to say
    // it would pass the assertion below on the strength of the hang.
    if (!round1Ok) {
        std::printf("daemon_system_test: responsiveness check abandoned -- the first burst did not drain, so no "
                    "issue/drain ratio from this run means anything.\n");
        return;
    }

    // A second, undisturbed round. issueTime is a handful of DBus round trips measured in
    // tens of microseconds, and each one makes the daemon printf and fflush a line into
    // the pipe this test owns -- so one scheduler preemption inside that loop inflates it
    // without inflating drain, and the check below would accuse production code of
    // rendering inline on a host that merely hiccuped. Two rounds and the better ratio
    // fixes that without weakening the check: a daemon that really does render inline
    // measures issueTime ~= drain in *both* rounds, because that is what it does rather
    // than what the scheduler did to it.
    const auto secondStart = Clock::now();
    for (int i = 0; i < loadCount; ++i) {
        setWallpaper(proxy, "static", bluePath, kMonitorLoadBase + i);
    }
    const auto issueTime2 = Clock::now() - secondStart;

    // Its own flag. Reusing round 1's meant a lost first-round signal was reported twice,
    // the second time as a failure of a burst that had in fact delivered everything.
    bool round2Ok = true;
    for (int i = 0; i < loadCount; ++i) {
        if (!signals.wait(kMonitorLoadBase + i, 2)) round2Ok = false;
    }
    const auto drain2 = Clock::now() - secondStart;
    check(round2Ok, "the second measuring burst also completes");

    // Ratios rather than raw intervals, since the two rounds may drain at different
    // speeds. A round is *eligible* only if it both drained and took long enough to be
    // conclusive: below kMinConclusiveDrain a daemon that rendered inline would have
    // answered just as fast, so a ratio from such a round is not evidence either way.
    //
    // Eligibility is per round, not applied to the winner after the fact. Grading the
    // better ratio and *then* testing that round's drain let a cheap second round -- round
    // 2 re-renders monitors whose images round 1 already allocated, so it is routinely
    // faster -- suppress the assertion even when round 1 was conclusive on its own.
    const double ratio1 = millis(drain) > 0 ? millis(issueTime) / millis(drain) : 1.0;
    const double ratio2 = millis(drain2) > 0 ? millis(issueTime2) / millis(drain2) : 1.0;
    const bool round1Eligible = round1Ok && drain >= kMinConclusiveDrain;
    const bool round2Eligible = round2Ok && drain2 >= kMinConclusiveDrain;

    std::printf("daemon_system_test: issue/drain ratio %.4f (%s) then %.4f (%s), threshold 0.25\n", ratio1,
                round1Eligible ? "eligible" : "too fast to be conclusive", ratio2,
                round2Eligible ? "eligible" : "too fast to be conclusive");

    if (!round1Eligible && !round2Eligible) {
        std::printf("daemon_system_test: responsiveness check INCONCLUSIVE on this host -- neither round's queue "
                    "took the %lld ms needed for a blocking handler to be distinguishable.\n",
                    static_cast<long long>(kMinConclusiveDrain.count()));
        return;
    }

    // The best ratio among the rounds that are actually evidence. A daemon rendering
    // inline scores ~1.0 in every round, so taking the minimum cannot rescue it.
    double bestRatio = 1.0;
    if (round1Eligible) bestRatio = std::min(bestRatio, ratio1);
    if (round2Eligible) bestRatio = std::min(bestRatio, ratio2);

    check(bestRatio * 4 < 1.0,
          "issuing a burst of requests costs a fraction of rendering it (SetWallpaper does not render inline)");
}

// ---------------------------------------------------------------------------- //
// Phase 4 -- several independent clients, concurrently.
//
// Discharges PLAN.md's task-4 obligation: the daemon is multi-threaded (DBus event loop
// plus one GPU thread) and nothing tested it under contention. Each thread gets its own
// connection and proxy, because separate clients is what actually happens -- a single
// proxy shared across threads would exercise sdbus's locking, not the daemon's.
//
// What can be asserted from out here is deliberately narrow. `requestSeq` supersede and
// `servicedSeq` idempotence are internal, and a test that guessed at their values would
// be asserting on the scheduler. The externally true statements are: nothing crashes,
// every reply is either well-formed or one of the two named errors that are legitimate
// mid-flight, and `generation` never goes backwards for a monitor -- which is the
// observable consequence of both paths being correct.
// ---------------------------------------------------------------------------- //
struct StormResult {
    std::atomic<int> unexpectedErrors{0};
    std::atomic<int> generationRegressions{0};
    std::atomic<int> generationComparisons{0};
    std::atomic<int> calls{0};
    std::mutex mutex;
    std::string firstUnexpected;
};

// Each client's own high-water mark, held on its stack -- deliberately NOT shared across
// the storm threads. Monotonicity is a property of one observer's sequence: two clients
// can read generations 5 and 6 in either real-time order and record them in either order,
// so a mark shared between them reports a perfectly monotonic daemon as regressing the
// moment the faster reader records first. Per client, the property is exactly what the
// daemon promises and nothing about the interleaving can fake a violation.
using GenerationMarks = std::map<int32_t, uint64_t>;

void observeGeneration(StormResult& result, GenerationMarks& marks, int32_t monitorId, uint64_t generation) {
    const auto it = marks.find(monitorId);
    if (it == marks.end()) {
        // A first look establishes the mark and compares against nothing. Excluded from
        // the count below so the assertion's actual content is measurable.
        marks.emplace(monitorId, generation);
        return;
    }
    ++result.generationComparisons;
    if (generation < it->second) {
        ++result.generationRegressions;
        return;
    }
    it->second = generation;
}

void recordUnexpected(StormResult& result, const std::string& what) {
    ++result.unexpectedErrors;
    std::lock_guard<std::mutex> lock(result.mutex);
    if (result.firstUnexpected.empty()) result.firstUnexpected = what;
}

void stormThread(int index, const std::string& bluePath, const std::string& redPath, StormResult& result) {
    // Inside the guard, not before it: an exception escaping a thread entry point is
    // std::terminate, and createProxy throws if the bus refuses another connection or the
    // daemon has already died -- turning a storm-phase failure into a bare abort with no
    // FAIL lines.
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
        proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                   sdbus::ObjectPath{DbusService::kObjectPath});
    } catch (const std::exception& e) {
        recordUnexpected(result, std::string("could not create a client connection: ") + e.what());
        return;
    }

    // Deterministic per thread, so a failure is reproducible. std::random_device would
    // make the interleaving that broke the daemon unrecoverable from the log.
    GenerationMarks marks;
    uint32_t rng = 0x9e3779b9u * static_cast<uint32_t>(index + 1);
    const auto next = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 16;
    };

    for (int i = 0; i < kStormIterations; ++i) {
        const int32_t monitorId = kMonitorStormBase + static_cast<int32_t>((next() + index) % kStormMonitors);
        try {
            switch (next() % 3) {
                case 0:
                    setWallpaper(*proxy, "static", (next() % 2) ? bluePath : redPath, monitorId);
                    break;
                case 1: {
                    const StatusReply status = getStatus(*proxy);
                    // Recorded here rather than left to the catch below: getStatus() hands
                    // a failed call back as a value, so without this a daemon that stopped
                    // answering mid-storm would read as an empty reply with no monitor in
                    // it -- no finding at all, where the throw used to be counted as an
                    // unexpected error because its name is neither of the two legitimate
                    // ones.
                    // CONTROL: DbusService::getStatus made to throw -> "FAIL: concurrent
                    //       clients see only the two named errors that are legitimate
                    //       mid-render", the first one naming the bus. Reverted.
                    if (!status.ok()) recordUnexpected(result, status.error());
                    // Called for the generation observation below. Snapshot consistency
                    // is deliberately NOT graded from these replies -- see the note at
                    // the end of this function.
                    const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
                    if (entry != nullptr) {
                        observeGeneration(result, marks, monitorId, entry->value("generation", uint64_t{0}));
                    }
                    break;
                }
                default: {
                    const Frame frame = getFrameFd(*proxy, monitorId);
                    if (fcntl(frame.fd.get(), F_GETFD) == -1) {
                        recordUnexpected(result, "GetFrameFd returned a closed fd");
                    }
                    observeGeneration(result, marks, monitorId, frame.generation);
                    break;
                }
            }
            ++result.calls;
        } catch (const sdbus::Error& e) {
            // Both are legitimate answers mid-storm: a monitor whose render has not
            // committed yet has no frame, and a monitor whose most recent request was
            // superseded before it rendered reports the previous failure. Anything else
            // -- a timeout, a disconnect, an unhandled exception escaping a handler --
            // is a defect.
            const std::string name = e.getName();
            if (name != "io.wallpaperx.Daemon.Error.NoFrame" && name != "io.wallpaperx.Daemon.Error.RenderFailed") {
                recordUnexpected(result, name + ": " + e.getMessage());
            }
            ++result.calls;
        } catch (const std::exception& e) {
            recordUnexpected(result, std::string("non-DBus exception: ") + e.what());
        }
    }
}

void checkConcurrentClients(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& bluePath,
                            const std::string& redPath, const std::string& corruptPath, const Check& check) {
    StormResult result;

    // jthread for the same reason as above: emplace_back can throw (thread exhaustion,
    // or a reallocation running out of memory) while the vector already holds joinable
    // threads, and destroying those is std::terminate.
    std::vector<std::jthread> threads;
    threads.reserve(kStormThreads);
    for (int i = 0; i < kStormThreads; ++i) {
        threads.emplace_back([&, i] { stormThread(i, bluePath, redPath, result); });
    }
    for (std::jthread& t : threads) t.join();

    check(result.unexpectedErrors == 0, "concurrent clients see only the two named errors that are legitimate "
                                        "mid-render");
    if (result.unexpectedErrors != 0) {
        std::lock_guard<std::mutex> lock(result.mutex);
        std::fprintf(stderr, "  %d unexpected failure(s); first was: %s\n", result.unexpectedErrors.load(),
                     result.firstUnexpected.c_str());
    }
    check(result.generationRegressions == 0, "a monitor's generation never goes backwards under concurrent clients");
    // GOAL: X.5 -- the monotonicity check asserts how much comparing it actually did.
    // WHY-THIS-SHAPE: a first look establishes a client's mark and compares against
    //       nothing, so a run with near-zero repeat observations has asserted
    //       near-nothing. The bound was kStormThreads (4), and the count turns out to
    //       be steeply non-linear in kStormIterations -- measured on this host, 8
    //       iterations yields 1 comparison, 14 yields 6, 16 yields 11 and the shipped
    //       24 yields 24-27 over eight runs across both ICDs. So the old bound left a
    //       wide band (5 through 23) in which the check was two-thirds hollow and
    //       still green. Three per client (12) is half the measured minimum: enough
    //       margin that a slow host cannot flake it, and enough bite that any
    //       meaningful reduction fails. Deliberately not tightened to the ~16 the
    //       GetStatus branch alone should produce -- how many GetFrameFd calls land
    //       on a monitor with no frame yet is a scheduling outcome, and a bound set at
    //       the margin would be asserting on the scheduler.
    // CONTROL: kStormIterations reduced from 24 to 14 -- chosen because the *old*
    //       bound of 4 still passes there, so the control separates the new check from
    //       the one it replaces. 6 comparisons, "FAIL: the monotonicity check made
    //       enough repeat observations to be worth asserting". Reverted.
    check(result.generationComparisons >= kStormThreads * 3,
          "the monotonicity check made enough repeat observations to be worth asserting");

    // The snapshot-consistency invariant is NOT graded here, and the two checks that
    // used to be are gone rather than relaxed. statusSelfConsistent returns true the
    // moment gpu.initialized is true (see tests/client/daemon_client.h), that flag is
    // set once in WallpaperManager::ensureGpu and never cleared, and this phase runs
    // long after the first render -- so every one of the ~29 replies short-circuited at
    // the first branch and inconsistentSnapshots was pinned at 0 by construction. The
    // companion "enough replies were sampled" bound made that worse than merely useless:
    // it printed a reassuring count and reported the invariant as covered under
    // concurrency when no reply had been graded at all. The window the invariant guards
    // is only open around the daemon's first device creation, so the check now lives
    // where it can fail -- see checkFirstFrameUnderStatusLoad below.
    std::printf("daemon_system_test: %d concurrent calls across %d clients completed, %d generation comparisons\n",
                result.calls.load(), kStormThreads, result.generationComparisons.load());

    // The daemon is still there and still correct afterwards. Quiescing each stormed
    // monitor to a known request and checking where it lands is what makes the storm an
    // assertion rather than a smoke test: a supersede bug leaves a monitor flagged as
    // holding a frame whose request was replaced, and a lost request leaves one holding
    // nothing at all.
    // Drain first, THEN read the generations. The order matters and the reverse is a
    // vacuous check: SetWallpaper is asynchronous, so storm renders are still queued when
    // the threads join. A threshold read at that moment is a *stale* generation, and the
    // waitAtLeast below would be satisfied by one of those leftover renders committing --
    // so "every monitor still renders after the storm" would pass for a daemon whose
    // render worker had wedged the instant the storm ended. One FIFO render thread means
    // this barrier's own signal proves every storm render has already been serviced.
    setWallpaper(proxy, "static", bluePath, kMonitorBarrierPre);
    const bool drained = signals.wait(kMonitorBarrierPre, 1);
    check(drained, "the storm's queued renders all drain (barrier render completes)");
    if (!drained) {
        // Checked *and acted on*. Without the return, the snapshot below would be taken
        // from exactly the non-quiesced state this barrier exists to rule out, and the
        // waitAtLeast built on it would then be satisfied by a leftover storm render --
        // reporting "every monitor still renders" for the wedged worker that caused the
        // barrier to time out in the first place.
        std::printf("daemon_system_test: post-storm state checks abandoned -- the render queue never drained, so "
                    "any generation read now would be a stale threshold.\n");
        return;
    }

    std::map<int32_t, uint64_t> before;
    {
        const StatusReply status = getStatusOrExplain(proxy);
        check(status.ok(), "the daemon still answers GetStatus after the storm");
        // Acted on for the same reason the drain barrier above is: every generation below
        // is read from this one reply, and an unanswered call leaves them all at 0 --
        // which is a threshold any leftover render satisfies, so "every monitor still
        // renders after the storm" would pass on the strength of the failure just graded.
        // CONTROL: DbusService::getStatus made to throw -> "FAIL: the daemon still answers
        //       GetStatus after the storm" and this abandonment line, with the phases after
        //       it still running. Reverted.
        if (!status.ok()) {
            std::printf("daemon_system_test: post-storm state checks abandoned -- GetStatus did not answer, so "
                        "every threshold below would be a fabricated zero.\n");
            return;
        }
        for (int i = 0; i < kStormMonitors; ++i) {
            const int32_t monitorId = kMonitorStormBase + i;
            const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
            before[monitorId] = entry == nullptr ? 0 : entry->value("generation", uint64_t{0});
        }
    }

    // Even ids end on a renderable image, odd ids on one that cannot decode, so the two
    // opposite outcomes are both asserted. A single direction would pass for a daemon
    // that had stopped rendering as readily as for one that works.
    for (int i = 0; i < kStormMonitors; ++i) {
        const int32_t monitorId = kMonitorStormBase + i;
        setWallpaper(proxy, "static", (i % 2 == 0) ? bluePath : corruptPath, monitorId);
    }

    // The renderable ones announce themselves. waitAtLeast, not wait: which generation a
    // monitor reaches depends on how many of the storm's requests coalesced, which is a
    // scheduling outcome and not something to assert.
    bool quiesced = true;
    for (int i = 0; i < kStormMonitors; i += 2) {
        const int32_t monitorId = kMonitorStormBase + i;
        if (!signals.waitAtLeast(monitorId, before[monitorId] + 1)) quiesced = false;
    }
    check(quiesced, "every monitor still renders after the storm");

    // The failing ones announce nothing, so their state is settled by a barrier instead:
    // one FIFO render thread means a later request's FrameReady proves the earlier ones
    // have already been serviced. No sleep, and no assumption about how long a render
    // takes.
    setWallpaper(proxy, "static", bluePath, kMonitorBarrierPost);
    check(signals.wait(kMonitorBarrierPost, 1), "the post-storm barrier render completes");

    // Graded, and acted on, for the same reason the `before` reading above is: an
    // unanswered call leaves an empty object, so every entry below reads as absent and the
    // two counts land at zero -- which prints as two monitor-state failures for what is in
    // fact the bus never answering.
    // CONTROL: DbusService::getStatus made to throw -> "FAIL: the daemon still answers
    //       GetStatus after the post-storm barrier" and this abandonment line, in place of
    //       the two monitor-state FAILs. Reverted.
    const StatusReply after = getStatusOrExplain(proxy);
    check(after.ok(), "the daemon still answers GetStatus after the post-storm barrier");
    if (!after.ok()) {
        std::printf("daemon_system_test: post-storm monitor states not graded -- GetStatus did not answer, so every "
                    "monitor below would read as absent.\n");
        return;
    }

    int goodMonitors = 0;
    int failedMonitors = 0;
    for (int i = 0; i < kStormMonitors; ++i) {
        const int32_t monitorId = kMonitorStormBase + i;
        const nlohmann::json* entry = wallpaperStatus(after, std::to_string(monitorId));
        if (entry == nullptr) continue;
        const bool hasFrame = entry->value("has_frame", false);
        const bool hasError = entry->contains("last_error");
        if (i % 2 == 0) {
            if (hasFrame && !hasError && entry->value("width", uint32_t{0}) == kDaemonWidth &&
                entry->value("height", uint32_t{0}) == kDaemonHeight) {
                ++goodMonitors;
            }
        } else if (!hasFrame && hasError) {
            ++failedMonitors;
        }
    }
    std::printf("daemon_system_test: after the storm, %d of %d monitors hold a frame and %d of %d report an error\n",
                goodMonitors, (kStormMonitors + 1) / 2, failedMonitors, kStormMonitors / 2);
    check(goodMonitors == (kStormMonitors + 1) / 2,
          "after the storm, every monitor left on a valid image reports a full-size frame and no error");
    check(failedMonitors == kStormMonitors / 2,
          "after the storm, every monitor left on an undecodable image reports no frame and an error");
}

// ---------------------------------------------------------------------------- //
// Signals and connection loss (S6.35, S6.36, S6.37)
// ---------------------------------------------------------------------------- //

// A daemon's signal masks, read from /proc. Every field is a hex bitmask in which signal
// N is bit N-1.
//
// `blocked` is the union over a caller-supplied set of threads, and `ignored` comes from
// the leader. That split is not tidiness: a signal's *disposition* is per-process, but its
// *blocked* mask is per-thread, and /proc/<pid>/status reports only the thread-group
// leader's. A daemon that blocked SIGPIPE on the render worker or an sdbus dispatch thread
// -- both of which printf, so either can take the failing write -- would leave the leader's
// mask clean and satisfy a leader-only read while doing exactly what S6.37 forbids.
struct SignalMasks {
    bool read = false;
    uint64_t blocked = 0;
    uint64_t ignored = 0;
    int threadsRead = 0;
};

constexpr uint64_t signalBit(int signo) { return 1ull << (signo - 1); }

// Parses one /proc/<pid>/task/<tid>/status. Returns false if either field was missing,
// which the caller must not treat as a clean mask.
bool readTaskMasks(const std::string& path, uint64_t& blocked, uint64_t& ignored) {
    std::ifstream status(path);
    if (!status) return false;
    std::string line;
    bool sawBlocked = false;
    bool sawIgnored = false;
    while (std::getline(status, line)) {
        const bool isBlocked = line.rfind("SigBlk:", 0) == 0;
        const bool isIgnored = line.rfind("SigIgn:", 0) == 0;
        if (!isBlocked && !isIgnored) continue;
        const size_t tab = line.find_first_of(" \t");
        if (tab == std::string::npos) continue;
        const uint64_t value = std::strtoull(line.c_str() + tab, nullptr, 16);
        if (isBlocked) {
            blocked = value;
            sawBlocked = true;
        } else {
            ignored = value;
            sawIgnored = true;
        }
    }
    return sawBlocked && sawIgnored;
}

// Every thread of `pid`, as tids. Taken before the daemon's first render, where the only
// threads in existence are the ones the daemon itself created -- see readSignalMasks.
std::vector<pid_t> daemonThreads(pid_t pid) {
    std::vector<pid_t> tids;
    std::error_code ec;
    const std::string taskDir = "/proc/" + std::to_string(pid) + "/task";
    for (auto it = std::filesystem::directory_iterator(taskDir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.find_first_not_of("0123456789") != std::string::npos) continue;
        tids.push_back(static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10)));
    }
    return tids;
}

// Unions SigBlk over exactly `tids`, and takes SigIgn from the leader.
//
// The thread set is passed in rather than scanned here, and that is the load-bearing part.
// A scan of /proc/<pid>/task at the point the check runs would include the ICD's own
// threads, and those are not the daemon's to answer for: measured on this host, every one
// of Mesa's 64 `llvmpipe-N` rasterizer threads and its `...:disk$0` cache thread carries
// SigBlk=0xfffffffe3ffbfaff -- SIGPIPE included -- while the daemon's own three threads
// carry 0x4003 and 0x0. A union over all tasks is therefore RED on a correct daemon under
// Lavapipe, which is the red-when-correct state this suite exists to avoid. Filtering by
// thread name would be an allowlist of driver internals that goes stale the first time a
// driver renames a thread. Taking the tid set before any GPU work exists excludes them by
// construction instead.
//
// Not covered by that choice, and said out loud: a thread the daemon creates *after* its
// first render would not be sampled. It creates none today -- every thread appearing after
// ensureGpu() belongs to the ICD -- so the set is exactly "the threads this program made".
SignalMasks readSignalMasks(pid_t pid, const std::vector<pid_t>& tids) {
    SignalMasks masks;
    const std::string taskDir = "/proc/" + std::to_string(pid) + "/task/";

    // The leader is required, and it is what `read` is keyed on. A thread can exit between
    // the tid snapshot and this read, so a failed open on a *non-leader* task is tolerated
    // -- but never counted as a clean mask, because a union that silently skipped the one
    // thread with SIGPIPE blocked is the vacuous pass this shape exists to remove. The
    // count of what was actually sampled is returned and asserted by the caller.
    uint64_t leaderBlocked = 0;
    uint64_t leaderIgnored = 0;
    if (!readTaskMasks(taskDir + std::to_string(pid) + "/status", leaderBlocked, leaderIgnored)) {
        return masks;
    }
    masks.read = true;
    masks.blocked = leaderBlocked;
    masks.ignored = leaderIgnored;  // process-wide; any task reports the same value
    masks.threadsRead = 1;

    for (const pid_t tid : tids) {
        if (tid == pid) continue;
        uint64_t blocked = 0;
        uint64_t ignored = 0;
        if (!readTaskMasks(taskDir + std::to_string(tid) + "/status", blocked, ignored)) continue;
        masks.blocked |= blocked;
        ++masks.threadsRead;
    }
    return masks;
}

// GOAL: S6.36 -- SIGHUP shuts the daemon down through the same chain as SIGINT and
//       SIGTERM: a normal exit with status 0, having printed which signal it saw and
//       "stopped cleanly". Left at its default disposition it dies at 129 and none of
//       that chain runs.
// WHY-THIS-SHAPE: exitedNormally() is asserted separately from `exit == 0`, and before
//       it, because reap() reports "killed by a signal" and "never exited, SIGKILLed by
//       the test" as exit code -1 alike. A check written only as `exit == 0` would be red
//       for all three causes with no way to tell which; and the one this goal is about --
//       death by default disposition -- is precisely a signal death. The signal name is
//       matched via strsignal() rather than the literal "Hangup" because the daemon
//       inherits this process's locale and a translated string would fail a correct
//       daemon.
// CONTROL: `sigaddset(&shutdownSignals, SIGHUP)` deleted from main.cpp -> "FAIL: the
//       daemon exits through main() on SIGHUP rather than being killed by it (terminated
//       by signal 1)", plus the exit-0, signal-name and stopped-cleanly checks. Reverted.
void checkHangupShutdown(NameWatcher& names, const Check& check) {
    SpawnedDaemon hungUp;
    const bool cameUp = names.waitFor(true);
    check(cameUp, "a fresh daemon acquires the name before the SIGHUP phase");
    if (!cameUp) {
        std::printf("daemon_system_test: SIGHUP checks skipped -- that daemon never acquired the bus name, so "
                    "its exit says nothing about the shutdown path.\n");
        return;
    }

    const int exitCode = hungUp.shutdown(SIGHUP);
    const std::string output = hungUp.output();

    check(hungUp.exitedNormally(),
          "the daemon exits through main() on SIGHUP rather than being killed by it");
    if (!hungUp.exitedNormally()) {
        std::fprintf(stderr, "  terminated by signal %d%s\n", hungUp.termSignal(),
                     hungUp.timedOut() ? " (it had to be killed after the timeout)" : "");
    }

    // Same shape as the SIGTERM and SIGINT blocks: under a sanitizer the exit code is the
    // sanitizer's, so the question stops being about the shutdown path and is reported as
    // that rather than as an accusation against main.cpp.
    if (output.find("Sanitizer: reported") != std::string::npos) {
        check(false, "the SIGHUP daemon's sanitizer reported no warnings (its exit code is the sanitizer's "
                     "here, so the SIGHUP exit-code check cannot run)");
    } else {
        check(exitCode == 0, "the daemon exits 0 on SIGHUP");
    }
    check(output.find(std::string(strsignal(SIGHUP)) + " " + kSignalShutdownLine) != std::string::npos,
          "the daemon names SIGHUP as the signal it is shutting down for");
    check(output.find("stopped cleanly") != std::string::npos,
          "the daemon runs its destructor chain on SIGHUP as well as on SIGINT and SIGTERM");
    check(names.waitFor(false), "the bus name is released after the SIGHUP shutdown");
}

// GOAL: S6.37 -- a closed stdout does not kill the daemon. The write that fails must
//       report EPIPE and the daemon must go on serving; and SIGPIPE must be *ignored*,
//       not blocked, because blocked it stays pending forever instead of letting the
//       write report.
// WHY-THIS-SHAPE: three parts, and none of them is redundant.
//       (1) The behavioural half: a SetWallpaper issued after the reader of the daemon's
//       stdout is gone still gets a reply, still renders, and GetStatus still answers.
//       That is what `wallpaperx-daemon | head` does, and at the default disposition the
//       daemon dies inside the handler at exit 141 with an empty stderr.
//       (2) The disposition half, read from /proc: the behavioural half alone cannot tell
//       ignored from blocked -- both survive the write -- and the goal explicitly rejects
//       blocking. SigBlk/SigIgn is the only externally visible difference between the two.
//       Read as a union over the daemon's own threads, not from /proc/<pid>/status: the
//       disposition is per-process but the blocked mask is per-thread, and that file is
//       the thread-group leader's alone. The daemon's render worker and sdbus dispatch
//       threads both printf, so either can take the failing write, and a mask installed on
//       one of them is invisible to the leader-only read. "Own threads" is the tid set
//       snapshotted before the first render rather than a scan at check time -- the ICD's
//       thread pool blocks nearly every signal and would make this red on a correct daemon
//       under Lavapipe; readSignalMasks carries the measurement.
//       (3) The coverage assert below, because everything here rests on the daemon
//       actually writing to stdout while serving that call. If that printf ever goes
//       away, nothing generates EPIPE and all of the above passes on a daemon with the
//       bug reintroduced -- so the write is asserted rather than assumed.
// CONTROL: three, all re-run 2026-08-05 against this shape of the phase.
//       (a) `std::signal(SIGPIPE, SIG_IGN)` deleted from main.cpp -> 5 FAILs: SetWallpaper
//       came back [org.freedesktop.DBus.Error.NoReply], GetStatus came back
//       ServiceUnknown, "the daemon ignores SIGPIPE", and the exit checks with
//       "terminated by signal 13".
//       (b) SIGPIPE *blocked* (pthread_sigmask) instead of ignored -> exactly 2 FAILs,
//       "the daemon ignores SIGPIPE" and "SIGPIPE is ignored rather than merely blocked"
//       (SigBlk=0x5003 SigIgn=0x0), while every behavioural check above stayed green --
//       which is precisely the configuration S6.37 says must fail and which part (1)
//       alone cannot see.
//       (c) the per-thread half of (b), which is why part (2) reads the daemon's other
//       threads and not just the leader: `pthread_sigmask(SIG_BLOCK, {SIGPIPE})` installed
//       inside main.cpp's sigwait watcher lambda only -> "FAIL: SIGPIPE is ignored rather
//       than merely blocked", SigBlk=0x5003 SigIgn=0x1000, on BOTH the default-ICD and the
//       lavapipe entry. The counterfactual was measured, not assumed: with the sample
//       restricted back to the leader alone and the same mutation still in place, the run
//       printed "all checks passed". All reverted.
// COVERAGE-ASSERT: the trigger is asserted on THIS daemon's fd 1, from a render issued
//       while its stdout still has a reader, and not from the earlier daemon's captured
//       output. That capture is stdout and stderr merged onto one pipe (the constructor
//       dup2()s one write end onto both), so it can only establish "the binary printed
//       this somewhere" -- and dbus_service.cpp has three fprintf(stderr, ...) SetWallpaper
//       lines right beside the printf, so a refactor moving the entry log to stderr would
//       leave the assert green while nothing in this phase generated EPIPE at all. Reading
//       the Breakable daemon's own stdout pipe is stream-pure by construction. The exact
//       needle, quotes included, is coupled to sanitizeForLog's Quoted framing on purpose:
//       that framing landing is what silently broke the unquoted needle before.
//       Controlled twice. (i) Harness: needle -> `SetWallpaper(type="zzz"`, a string the
//       daemon never prints -> "FAIL: the daemon writes to stdout, specifically, while
//       handling SetWallpaper", alone, followed by the phase's named skip and no cascade.
//       (ii) Production, and this is the one the old merged-capture form could not see:
//       DbusService::setWallpaper's entry printf changed to fprintf(stderr, ...) -> the
//       same single FAIL. The merged capture still contained the line 76 times in that
//       same run, so the assert this replaced would have stayed green while nothing in
//       this phase generated an EPIPE at all. Both reverted.
void checkClosedStdout(NameWatcher& names, const std::string& bluePath, const std::string& validationEnv,
                       bool wantValidation, const Check& check) {
    // Spawned with the validation env, unlike its siblings: this is the only phase that
    // renders in a child process of its own, so it is the only one where an unwatched
    // child means the daemon's Vulkan usage in this configuration -- a second Device after
    // another daemon has exited, a render while the process's stdout is in a permanent
    // error state -- has no oracle at all. Scanned after shutdown, at the end.
    SpawnedDaemon deaf({validationEnv}, SpawnedDaemon::Stdout::Breakable);
    const bool cameUp = names.waitFor(true);
    check(cameUp, "a fresh daemon acquires the name before the closed-stdout phase");
    if (!cameUp) {
        std::printf("daemon_system_test: closed-stdout checks skipped -- that daemon never acquired the bus "
                    "name, so nothing it does afterwards is attributable to its stdout.\n");
        return;
    }

    FrameReadyLog signals;
    auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                    sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

    // Snapshotted before the first SetWallpaper, which is where main.cpp creates the
    // Vulkan device: after that the process is full of the ICD's threads (64 of them under
    // Lavapipe), and those are not this daemon's to answer for. See readSignalMasks.
    const std::vector<pid_t> daemonOwnThreads = daemonThreads(deaf.pid());

    // One render with the pipe still readable, to establish the trigger. Draining a few
    // hundred bytes and then closing the read end is faithful to what this phase models:
    // `wallpaperx-daemon | head` is a reader that reads and then goes away.
    bool triggerIssued = false;
    try {
        setWallpaper(*proxy, "static", bluePath, kMonitorClosedStdoutPre);
        triggerIssued = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  SetWallpaper before the stdout reader was closed: %s\n", e.what());
    }
    check(triggerIssued, "the daemon answers SetWallpaper while its stdout still has a reader");
    // Both early returns abandon the phase rather than continue: without the trigger, the
    // EPIPE checks below would pass on a daemon that never wrote to the broken pipe, which
    // is the vacuous configuration this assert exists to detect. The wait is deadline-
    // bounded and its expiry fails the check -- it is never fallen through into.
    if (!triggerIssued) {
        std::printf("daemon_system_test: closed-stdout checks skipped -- the daemon did not answer the render "
                    "that establishes the trigger, so breaking its stdout would prove nothing.\n");
        return;
    }
    const bool sawOnStdout = deaf.waitForStdout("SetWallpaper(type=\"static\"", std::chrono::seconds(30));
    check(sawOnStdout,
          "the daemon writes to stdout, specifically, while handling SetWallpaper (this is what makes the "
          "closed-stdout phase below trigger anything at all)");
    if (!sawOnStdout) {
        std::printf("daemon_system_test: closed-stdout checks skipped -- nothing arrived on the daemon's own "
                    "stdout, so a broken stdout would generate no failing write to survive.\n");
        return;
    }

    // From here the daemon's stdout is a pipe with no reader and no other holder of the
    // write end: exactly `wallpaperx-daemon | head` once head has exited. Not graded --
    // breakStdout() returns false only when there is no descriptor to close, which the
    // Breakable constructor has already ruled out (it sets the descriptor or throws), so a
    // check here would be one that cannot fail and an `if` here would be dead code.
    deaf.breakStdout();

    bool answered = false;
    try {
        setWallpaper(*proxy, "static", bluePath, kMonitorClosedStdout);
        answered = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  SetWallpaper after the stdout reader was closed: %s\n", e.what());
    }
    check(answered,
          "the daemon answers SetWallpaper after its stdout reader is gone (the handler's own printf is the "
          "write that fails there)");

    // Hoisted, because the validation scan at the end of this function is only evidence if
    // the daemon actually did GPU work: an empty stderr capture from a child that died at
    // startup would otherwise be graded as a clean bill of health.
    bool rendered = false;
    if (answered) {
        rendered = signals.wait(kMonitorClosedStdout, 1);
        check(rendered, "the frame requested after stdout closed is rendered and announced");
    }

    const StatusReply status = getStatusOrExplain(*proxy);
    check(status.ok(), "the daemon still answers GetStatus after the failed write");

    const SignalMasks masks = readSignalMasks(deaf.pid(), daemonOwnThreads);
    check(masks.read, "the daemon's signal masks could be read from /proc");
    if (masks.read) {
        check((masks.ignored & signalBit(SIGPIPE)) != 0, "the daemon ignores SIGPIPE");
        check((masks.blocked & signalBit(SIGPIPE)) == 0,
              "SIGPIPE is ignored rather than merely blocked (blocked, the failing write's signal stays "
              "pending forever instead of the write reporting EPIPE)");
        if ((masks.ignored & signalBit(SIGPIPE)) == 0 || (masks.blocked & signalBit(SIGPIPE)) != 0) {
            std::fprintf(stderr, "  SigBlk=0x%llx SigIgn=0x%llx, SIGPIPE bit=0x%llx\n",
                         static_cast<unsigned long long>(masks.blocked),
                         static_cast<unsigned long long>(masks.ignored),
                         static_cast<unsigned long long>(signalBit(SIGPIPE)));
        }
        // COVERAGE-ASSERT: what the blocked-mask check is worth is a function of how many
        // threads it actually sampled, and that number comes from runtime state (a /proc
        // scan) rather than from the source. This daemon has three threads before it
        // renders -- leader, render worker, sigwait watcher -- so a sample of one means the
        // snapshot or the read silently fell back to the leader and the line above is back
        // to the weaker property it used to assert. Measured, not assumed: with the
        // non-leader tasks skipped, the "SIGPIPE blocked on the watcher thread" control
        // printed "all checks passed" until this line existed.
        check(masks.threadsRead >= 2,
              "the blocked-signal check sampled the daemon's other threads, not just the group leader");
        if (masks.threadsRead < 2) {
            std::fprintf(stderr, "  only %d of the daemon's %zu pre-render thread(s) could be read\n",
                         masks.threadsRead, daemonOwnThreads.size());
        }
    }

    // The shutdown path writes to the broken stdout too ("... received, shutting down",
    // "stopped cleanly", plus the flush at exit), so this is a second, independent EPIPE
    // -- and the last chance for one to turn a clean exit into 141.
    const int exitCode = deaf.shutdown(SIGTERM);
    check(deaf.exitedNormally(), "a daemon whose stdout is closed still exits through main() on SIGTERM");
    if (!deaf.exitedNormally()) {
        std::fprintf(stderr, "  terminated by signal %d%s\n", deaf.termSignal(),
                     deaf.timedOut() ? " (it had to be killed after the timeout)" : "");
    }
    // Its stdout is broken by construction, so "stopped cleanly" cannot be looked for
    // here: the exit code is the only evidence left, which is exactly the position an
    // operator is in when this happens for real.
    if (deaf.output().find("Sanitizer: reported") != std::string::npos) {
        check(false, "the closed-stdout daemon's sanitizer reported no warnings (its exit code is the "
                     "sanitizer's here, so the exit-code check cannot run)");
    } else {
        check(exitCode == 0, "a daemon whose stdout is closed exits 0 rather than 141");
    }
    check(names.waitFor(false), "the bus name is released after the closed-stdout daemon exits");

    // GOAL: S6.33 -- zero Vulkan validation warnings across every daemon path the suite
    //       exercises, this child's render and teardown included.
    // WHY-THIS-SHAPE: scanned here rather than folded into main()'s single scan, because
    //       main() reads only the first daemon's output and this is the only other child
    //       that builds a Device. Gated on `rendered` and not merely on `wantValidation`:
    //       deaf.output() is stderr only and is empty for a child that died at startup, so
    //       an ungated `warnings.empty()` would report the GPU-side oracle as clean for a
    //       run in which no Vulkan work happened at all -- the same hollowing main() guards
    //       against with its "serving" anchor. Read after shutdown(), because output() is
    //       complete only once the reader thread has seen EOF. stderr, not stdout, is where
    //       Device's messenger writes ("[vulkan] ..." via std::cerr), which is why breaking
    //       fd 1 does not take this oracle with it.
    // CONTROL: deaf spawned with WALLPAPERX_VALIDATION=0 while wantValidation was true ->
    //       the run printed the named skip below instead of a green check, i.e. the gate
    //       refuses to grade an unwatched child. Reverted.
    // The waived / no-layer case prints nothing here on purpose: main() announces it once,
    // loudly, at the top of the run, and repeating it per phase would train a reader to
    // scroll past the one warning that says the GPU oracle is off.
    if (wantValidation && !rendered) {
        std::printf("daemon_system_test: closed-stdout validation scan skipped -- that daemon never rendered, "
                    "so its silence about Vulkan is not evidence. The failure that caused it is above.\n");
    } else {
        const std::vector<std::string> warnings = validationLines(deaf.output());
        check(warnings.empty(),
              "the daemon that rendered with its stdout closed produced no Vulkan validation warnings");
        for (const std::string& line : warnings) {
            std::fprintf(stderr, "  %s\n", line.c_str());
        }
    }
}

// GOAL: S6.35 -- losing the bus connection unwinds through main() instead of aborting.
//       enterEventLoop() throws on connection loss, and every object between it and
//       main's handler must be destructible on that path; a joinable std::thread
//       destroyed during that unwind is std::terminate -- exit 134, core dumped, and none
//       of the destructors S6.30 exists to guarantee.
// WHY-THIS-SHAPE: the loss is driven by killing a bus this test started itself, not by
//       the mechanism that found the bug (~1000 unread GetFrameFd calls exhausting
//       dbus-broker's unix-fd quota). That mechanism cannot be used here and a check
//       built on it would be worse than none: ctest runs under dbus-run-session, which
//       starts the reference dbus-daemon, which enforces no such quota -- so the flood
//       would be absorbed, the connection would survive, and the check would pass green
//       against a daemon with the bug reintroduced. What the daemon actually sees is a
//       connection that ends, which is what SIGKILLing its bus produces (verified:
//       "fatal: [System.Error.ENOTCONN] Failed to get bus poll data"), on any bus
//       implementation and with no quota involved. What this does NOT establish is that
//       dbus-broker's fd quota is reachable at ~1000 calls; that is the broker's
//       behaviour, not this daemon's.
//       exitedNormally() is asserted before and separately from `exit == 1` for the same
//       reason as in the SIGHUP phase: the failure mode here IS a signal death, and
//       reap() folds signal deaths and timeouts into the same -1.
//       NOT COVERED here, stated because the phase reads as if it were: the orphan is
//       killed before it is ever sent a SetWallpaper, and main.cpp creates the Vulkan
//       device on the first one -- so no Device exists, no validation layer is ever
//       loaded, and the GPU half of the teardown on this unwind path is graded by nothing.
//       The phase therefore takes no validation env; it used to be handed one, which was
//       inert and made the coverage look wider than it was.
// CONTROL: three, and the first two are about this phase being able to report at all.
//       (i) the startup needle changed to one the daemon never prints -> "FAIL: the daemon
//       comes up on a bus of the test's own...", alone, with the four unwind checks
//       skipped and the daemon's own output printed. (ii) the tree reconfigured with
//       DBUS_DAEMON_EXECUTABLE empty (the dbus-daemon-less host) -> "NOT VERIFIED -- the
//       connection-loss checks (S6.35) could not be exercised: no dbus-daemon binary was
//       found at configure time", exit 77, and ctest reported `daemon_system ...Skipped`
//       instead of Passed. Both reverted.
//       (iii) main.cpp's ShutdownWatch guard replaced by a bare `std::thread shutdownWatch`
//       with the pthread_kill/join moved to after enterEventLoop() -- i.e. the code as it
//       was before f1aa32c. Re-run 2026-08-06: the daemon printed "terminate called
//       without an active exception" and died on signal 6, and exactly 3 FAILs followed --
//       "losing the bus connection unwinds through main() instead of aborting the
//       process", the exit-1 line and the fatal-message line -- with the startup check
//       above still green. Reverted. (This entry read "4 FAILs ... and the watcher-joined
//       line" until 2026-08-06. That fourth check no longer exists: it asserted the
//       PRESENCE of the shutdown line and was inverted below, and an inverted check does
//       not fire on this mutation -- a daemon that aborts prints no shutdown line either.
//       The count was measured again rather than adjusted on paper.)
void checkBusLoss(const Check& check) {
    NestedBus bus;
    if (!bus.ok()) {
        // X.3: a bus this test could not start is a fault in the harness or the host, and
        // must never be reported as the daemon aborting -- so this is not a check().
        // Counted as NOT VERIFIED instead of merely printed: the run's exit status used to
        // be a pure function of `failures`, so "S6.35 held" and "S6.35 never ran" produced
        // byte-identical results and ctest reported both as PASS. notVerified() makes the
        // run exit 77, which ctest reports as a skip.
        notVerified("the connection-loss checks (S6.35)", bus.reason());
        return;
    }

    SpawnedDaemon orphan({std::string("DBUS_SESSION_BUS_ADDRESS=") + bus.address()});
    // The daemon's own startup line is the up-signal here rather than NameOwnerChanged:
    // the name is acquired on the nested bus, which this process is not connected to, and
    // connecting to it would put another client on the very connection being destroyed.
    // Built from kBusName rather than spelled out, so renaming the bus is a compile-time
    // problem rather than a needle that silently stops matching and disables the phase.
    //
    // Graded, unlike the bus above: the bus is the harness's to start, but `orphan` is the
    // shipped binary with nothing changed but DBUS_SESSION_BUS_ADDRESS, so its failing to
    // print its own startup line within 30s is a fault of the code under test -- and 30s
    // for a line printed before any Vulkan work is already a stall, not a slow host. The
    // early return stays: grading the four unwind checks against a daemon that never
    // started would turn one fault into five FAILs pointing at main's unwind path.
    const bool cameUp =
        orphan.waitForOutput(std::string("serving ") + DbusService::kBusName, std::chrono::seconds(30));
    check(cameUp, "the daemon comes up on a bus of the test's own, so the connection-loss path can be exercised");
    if (!cameUp) {
        std::fprintf(stderr,
                     "  the remaining connection-loss checks (S6.35) cannot run: killing a bus a daemon never "
                     "connected to says nothing about the unwind path. What it printed:\n%s\n",
                     orphan.output().c_str());
        return;
    }

    bus.kill();

    const int exitCode = orphan.waitForExit();
    const std::string output = orphan.output();

    check(!orphan.timedOut(),
          "the daemon notices that its bus connection is gone and exits, rather than running on forever");
    check(orphan.exitedNormally(),
          "losing the bus connection unwinds through main() instead of aborting the process");
    if (!orphan.exitedNormally()) {
        std::fprintf(stderr, "  terminated by signal %d%s\n", orphan.termSignal(),
                     orphan.timedOut() ? " (it had to be killed after the timeout)" : "");
    }
    if (output.find("Sanitizer: reported") != std::string::npos) {
        check(false, "the orphaned daemon's sanitizer reported no warnings (its exit code is the sanitizer's "
                     "here, so the connection-loss exit-code check cannot run)");
    } else {
        check(exitCode == 1, "the daemon exits 1 through main's exception handler when its bus is destroyed");
    }
    // Named, because the check below is gated on it rather than merely printed after it.
    const bool reachedTheHandler = output.find("fatal:") != std::string::npos;
    check(reachedTheHandler, "the daemon says why it exited after losing the bus rather than dying silently");

    // GOAL: on the connection-loss path the daemon does not report a signal it was never
    //       sent. It died because the broker dropped its connection; ~ShutdownWatch wakes
    //       the sigwait watcher with a pthread_kill(SIGTERM) that is an implementation
    //       detail of the wake, not a signal from anybody, and the log must not present it
    //       as one.
    //
    // WHY-THIS-SHAPE: this line used to assert the PRESENCE of kSignalShutdownLine, using
    //       it as the oracle for "the watcher was woken and joined". That oracle was the
    //       bug. The watcher could not distinguish the destructor's wake from a real
    //       SIGTERM, so on this path -- where no signal was sent at all -- it printed
    //       `Terminated received, shutting down`, and an operator triaging a daemon that
    //       vanished read a genuine-format line pointing at the session manager instead of
    //       at the bus quota. The check passed *because* of that, so it was asserting on a
    //       defect.
    //
    //       The wake-and-join it was standing in for is carried by the checks above, and
    //       each half was run rather than argued: ~ShutdownWatch's pthread_kill deleted, so
    //       the watcher is never woken, leaves join() blocking and the daemon never exits
    //       -> "FAIL: the daemon notices that its bus connection is gone and exits", plus
    //       the exitedNormally, exit-1 and fatal-line checks, at 30.6s (2026-08-06). The
    //       same four, at 30.7s, with the watcher woken but parked for an hour before
    //       returning. And the pre-f1aa32c bare joinable thread -> "terminate called
    //       without an active exception", signal 6, three FAILs (exitedNormally, exit-1,
    //       fatal-line; NOT !timedOut, which that mutation does not reach because the
    //       abort is immediate). All three reverted.
    //
    //       One thing the presence check did cover that this one cannot: a watcher that
    //       returns early from a FAILED sigwait prints nothing, so it now satisfies this
    //       line. That case is not uncovered, it moved -- stubbing the watcher to return
    //       before sigwait leaves this phase entirely green and produces 11 FAILs in the
    //       SIGTERM/SIGINT/SIGHUP phases, whose daemons then never leave the event loop
    //       (measured 2026-08-06, 120s of timeouts). It could not stay here: a working
    //       watcher and a dead one are indistinguishable on this path by design.
    //
    //       Not gated on `exitCode == 1`, which would read as the stronger gate: that check
    //       is itself replaced by a sanitizer-attribution failure under a sanitized build,
    //       so gating on it would silently disable this one exactly there. The fatal line
    //       is the load-bearing evidence anyway -- main's catch handler prints it, and the
    //       handler runs only after ~ShutdownWatch has already woken and joined the
    //       watcher, so anything the watcher printed is in `output` before it.
    //
    // COVERAGE-ASSERT: two, because an absence check has two ways to go quiet.
    //       (i) It passes trivially when the daemon printed nothing at all, which is not a
    //       hypothetical: ungated, it stayed green under all three mutations above while
    //       the daemon was catastrophically broken. So it is not graded unless the daemon
    //       demonstrably reached main's handler -- otherwise it prints that it was not
    //       graded, and the failure that caused that is already reported above.
    //       Gating cannot hide a regression here: the gate is the check on the line above,
    //       so a daemon that never spoke is red either way.
    //       (ii) A needle that no longer matches anything can never fire. kSignalShutdownLine
    //       is shared with the SIGTERM and SIGHUP phases, which assert its PRESENCE, so a
    //       reworded message in main.cpp turns those red instead of leaving this one
    //       vacuously green.
    //
    // CONTROL: main.cpp's `wakingForShutdown` early return deleted from the watcher -- i.e.
    //       the code as it was before this fix. Re-run 2026-08-06: the orphan printed
    //       "wallpaperx-daemon: Terminated received, shutting down" ahead of its ENOTCONN
    //       fatal line, and this was the ONLY failure in the run -- "FAIL: the daemon does
    //       not report a signal it was never sent when its bus connection drops". Reverted.
    // CONTROL (the gate): the same bare-thread mutation, run against this shape on
    //       2026-08-06, printed "the fabricated-signal check is not graded -- the orphan
    //       never reached main's exception handler ..." and 3 FAILs, where before the gate
    //       it printed 3 FAILs and a silent pass on this line. Reverted.
    if (!reachedTheHandler) {
        std::printf("daemon_system_test: the fabricated-signal check is not graded -- the orphan never reached "
                    "main's exception handler, so it never got as far as the point where the watcher would have "
                    "spoken. The failure that caused that is reported above.\n");
    } else {
        check(output.find(kSignalShutdownLine) == std::string::npos,
              "the daemon does not report a signal it was never sent when its bus connection drops");
    }
}

// The goal string every skip row in this phase shares, so a reader grepping for the goal
// finds the phase whether it ran or not.
constexpr const char* kOrphanGoal = "X.8 (a daemon does not outlive an aborted test process)";

// GOAL: X.8 -- every forked child is armed so it cannot outlive a parent that dies
//       abnormally. False if a daemon is still running once its spawner has been killed by
//       SIGABRT, which is how the next run comes to meet an occupied bus name and an
//       occupied config port it did not create.
//       X.8 and not X.4: X.4's "no child process is leaked" is satisfied on every path
//       where destructors run, and this is the path where none do.
// WHY-THIS-SHAPE: the property is only observable from outside the process whose death
//       triggers it, so the abort happens in a child of this test (--orphan-probe) that
//       spawned the daemon through the ordinary SpawnedDaemon path, and the daemon is then
//       looked for by pid from out here. The obvious simpler check -- "the suite still
//       passes" -- establishes nothing at all about this: ~SpawnedDaemon does not run on
//       the abort path, so a green suite is exactly what a leaked daemon also produces.
//       The probe's own death is graded as SETUP, not as the property: a probe that never
//       got a daemon up, or that exited instead of aborting, has not created the state
//       this phase is about, and reporting that as a leak would blame the daemon for the
//       harness. A survivor is killed here rather than left running -- the phases after
//       this one need the bus name free.
// CONTROL: four, all reverted. (a) the prctl deleted from armAgainstParentDeath -> "FAIL: a
//       daemon spawned by a test process that then died on SIGABRT does not outlive it",
//       the survivor killed here, and the phases after it still running. From outside the
//       suite the same mutation left the daemon with ppid 1 and 127.0.0.1:18462 still
//       LISTEN after its spawner had aborted. (b) the probe's startup needle made
//       unmatchable -> this phase NOT VERIFIED and no FAIL. (c) the probe made to loop
//       instead of aborting -> the "did not finish within 60 s" skip, where an unbounded
//       wait gave a ctest timeout with no output at all. (d) waitpid given a pid that fails
//       ECHILD -> the reap-failure skip, where before it read "the probe died on signal 0".
void orphanProbeVerdict(const Check& check) {
    const SelfProbeRun probe = runSelfProbe("--orphan-probe", kSelfProbeTimeout);
    for (size_t start = 0; start < probe.output.size();) {
        const size_t eol = probe.output.find('\n', start);
        const size_t end = eol == std::string::npos ? probe.output.size() : eol;
        std::printf("  [orphan-probe] %.*s\n", static_cast<int>(end - start), probe.output.data() + start);
        if (eol == std::string::npos) break;
        start = eol + 1;
    }
    std::fflush(stdout);

    // X.1: an expired wait is never data. runSelfProbe kills a probe that overruns and says
    // so, and this branch comes first because every field below it -- termSignal above all
    // -- then describes OUR SIGKILL rather than the probe's own death.
    if (probe.timedOut) {
        notVerified(kOrphanGoal, "the probe did not finish within " +
                                     std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                        kSelfProbeTimeout).count()) +
                                     " s and was killed, so nothing it might have orphaned is attributable");
        return;
    }
    // Before the `started` branch, because harnessError is also set for a reap that failed
    // AFTER a successful fork. Consulted only when `started` is false, that case fell
    // through with termSignal at 0 and printed "the probe died on signal 0".
    if (!probe.harnessError.empty()) {
        notVerified(kOrphanGoal, "this test could not run a probe process: " + probe.harnessError);
        return;
    }
    if (!probe.started) {
        notVerified(kOrphanGoal, "this test could not start a probe process");
        return;
    }

    const size_t marker = probe.output.find(kOrphanPidPrefix);
    if (marker == std::string::npos) {
        notVerified(kOrphanGoal, "the probe never reported a daemon pid, so no daemon was ever orphaned");
        return;
    }
    // SIGABRT specifically. A probe that exited normally, or died on some other signal, did
    // not reach the state this phase is about -- and PR_SET_PDEATHSIG would fire for a
    // normal exit anyway, so grading that would be grading a path that is not the point.
    if (probe.termSignal != SIGABRT) {
        notVerified(kOrphanGoal, probe.exitedNormally
                                     ? "the probe exited " + std::to_string(probe.exitCode) +
                                           " instead of aborting with a daemon running"
                                     : "the probe died on signal " + std::to_string(probe.termSignal) +
                                           " rather than SIGABRT");
        return;
    }
    // Its own branch, and after the signal branch rather than sharing it. Folded together,
    // a malformed pid line following a correct abort printed "the probe died on signal 6
    // rather than SIGABRT" -- a reason that is not merely vague but false.
    const pid_t orphan = static_cast<pid_t>(std::strtol(probe.output.c_str() + marker + std::strlen(kOrphanPidPrefix),
                                                        nullptr, 10));
    if (orphan <= 0) {
        notVerified(kOrphanGoal, "the probe's pid line did not parse as a pid, so there is no process to look for");
        return;
    }

    // Bounded, and the deadline is not data: the loop exits on the first look that finds
    // the daemon gone, and reaching the deadline means it was still there every time --
    // which is the failure, not a slow host. The kill is immediate on the parent's death,
    // so seconds here is orders of magnitude of margin.
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    bool survived = true;
    while (Clock::now() < deadline) {
        if (!daemonRunning(orphan)) {
            survived = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    check(!survived, "a daemon spawned by a test process that then died on SIGABRT does not outlive it");
    if (survived) {
        std::fprintf(stderr, "  daemon %d was still running after its spawner aborted; killing it so the rest of "
                             "this run has a free bus name\n",
                     static_cast<int>(orphan));
        ::kill(orphan, SIGKILL);
        // Not this process's child, so it cannot be waited for -- init reaps it. Polled
        // instead, and the wait is bounded by the same reasoning as above.
        const auto killDeadline = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < killDeadline && daemonRunning(orphan)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

// Returns false when this phase left something owning the bus name, in which case the port
// phases after it must not run.
//
// GOAL: X.3 -- a fault of this harness is never graded as a fault of the daemon.
// WHY-THIS-SHAPE: the settle is out here, past every early return in orphanProbeVerdict,
//       because it is what the phases AFTER this one depend on and a skip is exactly when
//       it is most likely to be needed. Three of those returns used to jump over it. And
//       the result is returned rather than printed: a leaked daemon still holding
//       127.0.0.1:18462 makes the port phases report a bind failure against a daemon they
//       spawned themselves, which is this harness's leak wearing a production defect's
//       name.
// CONTROL: the prctl deleted from armAgainstParentDeath AND this phase's kill of the
//       survivor suppressed -- both needed, since a phase that clears the leak never
//       reaches this branch -> the leak FAIL above, then one NOT VERIFIED row naming
//       S7.8/S7.11/S7.14/S7.15. Ungated, the same mutation produced an extra FAIL from
//       checkHttpLifecycle and a skip reason blaming "another process on this machine" for
//       this harness's own leaked daemon. Reverted.
bool checkAbortedParentLeavesNoDaemon(NameWatcher& names, const Check& check) {
    orphanProbeVerdict(check);

    // Settling, not a check: the name's release is asserted by the shutdown phases, and
    // asserting it here as well would print a second failure for the one defect above.
    // The next phase's waitFor(true) would otherwise be satisfied by this daemon's name.
    if (names.waitFor(false)) return true;
    std::printf("daemon_system_test: the bus name was still owned after the orphan phase.\n");
    return false;
}

// Used by both phases below: phase 10 asks the daemon what it thinks of its config port
// when the line it was waiting for never arrived, phase 11 grades the answer.

// GetStatus's config_server object, or nullptr when the reply does not carry one. Same
// reason wallpaperStatus() exists: operator[] on a const json missing the key dereferences
// end() under NDEBUG, so a daemon that dropped the field would crash the test instead of
// failing the check written to catch exactly that.
const nlohmann::json* configServerReport(const StatusReply& status) {
    const nlohmann::json& json = status.json();
    if (!json.is_object() || !json.contains("config_server")) return nullptr;
    const nlohmann::json& report = json["config_server"];
    return report.is_object() ? &report : nullptr;
}

// The daemon's own verdict on its config port ("ok", "fail", "pending"), or empty when it
// could not be asked -- with `why` saying which, since "the daemon says it failed to bind"
// and "the daemon is gone" lead to different decisions. Opens its own proxy: the caller
// that needs this has not built one yet, and a proxy is cheap next to a spawn.
std::string askConfigServerStatus(std::string& detail, std::string& why) {
    try {
        auto proxy =
            sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
        const StatusReply status = getStatus(*proxy);
        if (!status.ok()) {
            why = status.error();
            return {};
        }
        const nlohmann::json* report = configServerReport(status);
        if (report == nullptr) {
            why = "GetStatus carried no config_server object";
            return {};
        }
        detail = report->value("detail", std::string());
        return report->value("status", std::string());
    } catch (const std::exception& e) {
        why = e.what();
        return {};
    }
}

// The daemon's bind-failure wording, as this file has to match it in another process's
// stderr. Duplicated from the `bindError` local http_server.cpp builds on its failed-bind
// path -- a test cannot read a literal out of a program it only spawns -- and that
// duplication is silent by nature: a reworded message would turn checkHttpLifecycle's skip
// reason vague and, worse, turn checkConfigServerLostPort's "the daemon died of the port
// conflict" FAIL into a skip. So the marker is cross-checked against the daemon's own live
// report, where the same string arrives over DBus. See the check on `reportDetail` in
// checkConfigServerLostPort.
std::string bindFailureMarker(int port) {
    return std::string("could not bind ") + wallpaperx::HttpServer::kLoopbackHost + ":" + std::to_string(port);
}

// A /health body's `checks` object, or nullptr when the body does not carry one. Third site
// of the wallpaperStatus()/configServerReport() pattern, and needed for the same reason:
// `body["checks"]` on a const json is JSON_ASSERT plus a dereference, so a daemon that
// dropped the key would abort this test rather than fail the check written to catch it.
// The order matters -- ask whether the key is there, then read it; reading it and then
// asking `.is_object()` is already too late.
// CONTROL: `{"checks", checksJson}` dropped from the /health body in http_server.cpp ->
//       four named FAIL lines starting "the real daemon's /health names shaders, gpu and
//       wallpapers as separate checks", and the run finishes. With the read-then-ask idiom
//       the same mutation aborted on json.hpp's `it != object->end()` assertion with no
//       FAIL line at all. Reverted.
const nlohmann::json* healthChecks(const nlohmann::json& body) {
    if (!body.is_object() || !body.contains("checks")) return nullptr;
    const nlohmann::json& checks = body["checks"];
    return checks.is_object() ? &checks : nullptr;
}

// ---------------------------------------------------------------------------- //
// Phase 10 -- the HTTP config server, in the shipped binary, on the shipped port.
// ---------------------------------------------------------------------------- //
//
// Everything else about http_server.cpp is checked in process, on an ephemeral port
// (http_server_test). Three things are only observable from out here:
//
//   1. main.cpp wires it up at all, on 18462, with no port argument to help it.
//   2. Nothing is left listening after the process exits (S7.8). In process the port
//      always closes, because the process that opened it is the one asserting.
//   3. A poll landing during shutdown cannot read state being torn down (S7.11). That
//      ordering lives in main.cpp's declaration order -- the HttpServer is declared last
//      so it is destroyed first -- and an in-process test cannot exercise a main() it
//      never runs.
//
// The port is the one thing this phase cannot choose, so it never assumes it: if the
// spawned daemon could not bind 18462 (a developer's real daemon is running, most
// likely), the phase says so and is reported as a skip. Reading a bind failure as a
// defect here is exactly the "port conflict wearing a defect's clothes" this suite must
// not do.
void checkHttpLifecycle(NameWatcher& names, const std::string& bluePath, const std::string& validationEnv,
                        const Check& check) {
    using wallpaperx::testing::ConnectResult;
    using wallpaperx::testing::describe;
    using wallpaperx::testing::HttpReply;
    using wallpaperx::testing::Poller;
    using wallpaperx::testing::tryConnect;

    constexpr int kPort = wallpaperx::HttpServer::kDefaultPort;
    constexpr int32_t kMonitorHttp = 26;

    SpawnedDaemon daemon({validationEnv});
    if (!names.waitFor(true)) {
        notVerified("S7.8/S7.11 (the HTTP server's process lifecycle)",
                    "the daemon for this phase never acquired the bus name");
        return;
    }

    // Which of the two lines the daemon prints decides whether this phase can run at all.
    // waitForOutput returns false on timeout AND on the daemon exiting, and neither is
    // treated as data -- see the bind-failure branch.
    const std::string bindOk = std::string("config server on http://127.0.0.1:") + std::to_string(kPort);
    const std::string bindFailed = bindFailureMarker(kPort);
    bool listening = daemon.waitForOutput(bindOk, std::chrono::seconds(10));
    if (!listening) {
        // GOAL: X.3, in the direction that hides coverage rather than accusing production --
        //       a rewording of the daemon's bind-success line must arrive here as a named
        //       failure, never as a skip. `bindOk` duplicates a literal no test can read out
        //       of a process it only spawns, so a stale duplicate would silently delete this
        //       whole phase.
        // WHY-THIS-SHAPE: bindFailureMarker's trick -- cross-check the duplicate against the
        //       daemon's own DBus detail -- does not transfer: the success path records an
        //       EMPTY detail, so there is no string to compare. What DBus does carry is the
        //       verdict. main.cpp answers GetStatus only from its event loop, which it
        //       reaches after the HttpServer constructor and so after that printf has run;
        //       a daemon reporting ok that never printed the line therefore means the literal
        //       here has drifted, not that the daemon is slow. The second wait covers only
        //       the capture pipe's drain, and it expires into the FAIL, never into a pass.
        // CONTROL: the success printf in http_server.cpp reworded to "config server
        //       listening on http://%s:%d" -> "FAIL: the bind-success wording this phase
        //       waits for is still the daemon's".
        std::string reportDetail;
        std::string askWhy;
        const std::string reportStatus = askConfigServerStatus(reportDetail, askWhy);
        if (reportStatus == "ok") listening = daemon.waitForOutput(bindOk, std::chrono::seconds(2));
        if (!listening) {
            if (reportStatus == "ok") {
                check(false, "the bind-success wording this phase waits for is still the daemon's");
                std::fprintf(stderr,
                             "  waited for \"%s\", but the daemon never printed it and reports config_server ok / %s\n",
                             bindOk.c_str(), reportDetail.empty() ? "(empty detail)" : reportDetail.c_str());
            } else {
                const std::string daemonSays =
                    reportStatus.empty() ? std::string("; the daemon could not be asked over DBus (") + askWhy + ")"
                                         : std::string("; the daemon reports config_server ") + reportStatus;
                const std::string why =
                    daemon.output().find(bindFailed) != std::string::npos
                        ? std::string("port ") + std::to_string(kPort) +
                              " is held by another process on this machine (a real wallpaperx-daemon?), "
                              "so the spawned daemon could not bind it" + daemonSays
                        : "the daemon printed neither the bind-success nor the bind-failure line for port " +
                              std::to_string(kPort) + daemonSays;
                notVerified("S7.8/S7.11 (the HTTP server's process lifecycle)", why);
            }
            daemon.shutdown(SIGTERM);
            return;
        }
    }

    // GOAL: S7.10/S7.10b end to end -- /health on 127.0.0.1:18462, asked of the real
    //       binary by a process that is not it. A daemon that has just started has not
    //       touched its shader directory or created a device, so every check is `pending`
    //       and so is the aggregate. A watchdog polling this must not be told the daemon
    //       is healthy on the strength of checks nobody has run.
    // WHY-THIS-SHAPE: the in-process tier already pins the JSON in far more detail. What
    //       only this tier can establish is that the shipped binary reaches this state at
    //       all -- an HttpServer that was never wired into main.cpp, or a daemon that
    //       eagerly created a device at startup, would leave every in-process check green.
    //       So this asserts the aggregate and the three names, not the details.
    // CONTROL: the `wallpaperx::HttpServer httpServer(manager);` line removed from
    //       main.cpp -> this phase reports NOT VERIFIED ("the daemon printed neither the
    //       bind-success nor the bind-failure line for port 18462; the daemon reports
    //       config_server pending") rather than spending a FAIL on the daemon; the run's
    //       failures for that mutation all come from phase 11, which grades it directly.
    //       For the body itself: the
    //       `else if (check->status == HealthStatus::Pending ...)` arm deleted from
    //       http_server.cpp's aggregation -> "FAIL: the real daemon reports the aggregate
    //       as pending before it has used anything" with "  fresh /health: ... status ok".
    //       And for the three names: the `{"gpu", &snap.health.gpu}` entry removed from
    //       that same array -> "FAIL: the real daemon's /health names shaders, gpu and
    //       wallpapers as separate checks" and "FAIL: the real daemon reports the GPU as
    //       untried before its first wallpaper", the body coming back with two checks in
    //       it. All reverted.
    httplib::Client cli = wallpaperx::testing::makeClient(kPort);
    const HttpReply fresh = wallpaperx::testing::get(cli, "/health");
    check(fresh.ok(), "the real daemon answers GET /health on 127.0.0.1:18462");
    if (fresh.ok()) {
        const nlohmann::json* checks = healthChecks(fresh.json);
        const bool named = checks != nullptr && checks->contains("shaders") && checks->contains("gpu") &&
                           checks->contains("wallpapers");
        check(named, "the real daemon's /health names shaders, gpu and wallpapers as separate checks");
        check(fresh.json.value("status", std::string()) == "pending",
              "the real daemon reports the aggregate as pending before it has used anything");
        check(named && (*checks)["gpu"].is_object() &&
                  (*checks)["gpu"].value("status", std::string()) == "pending",
              "the real daemon reports the GPU as untried before its first wallpaper");
        if (fresh.json.value("status", std::string()) != "pending" || !named) {
            std::fprintf(stderr, "  fresh /health: %s\n", fresh.body.c_str());
        }
        std::printf("daemon_system_test: /health on the shipped port before any wallpaper: %s\n",
                    fresh.body.c_str());
        std::fflush(stdout);
    }

    // A render, so the device exists and the tier becomes reportable.
    FrameReadyLog signals;
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

    setWallpaper(*proxy, "static", bluePath, kMonitorHttp);
    const bool rendered = signals.wait(kMonitorHttp, 1);
    if (!rendered) {
        // X.1/X.3: the wait expiring is not evidence about /health, and a host with no
        // working Vulkan is not a failure of the HTTP server.
        notVerified("S7.10 through the real binary",
                    "the daemon never rendered a frame, so nothing in the shipped binary ever moved a health "
                    "check off pending");
    } else {
        // GOAL: S7.10/S7.10b's other direction in the shipped binary -- once the real
        //       daemon has rendered a wallpaper, the checks that read `pending` above read
        //       `ok`, and the GPU one names the device the same daemon reports over DBus.
        // WHY-THIS-SHAPE: the pending assertions above are worth nothing unless `ok` is
        //       reachable in this binary -- an endpoint stuck at pending satisfies every
        //       one of them. The GPU detail is compared against GetStatus's own name and
        //       tier rather than a literal, because the tier varies by host (Heap on
        //       NVIDIA, Buffer on Lavapipe, Classic when forced) and a non-empty check
        //       would pass for a daemon that made a string up. This replaces the
        //       `descriptor_tier` key /health used to carry, which was dropped 2026-08-06
        //       along with PLAN.md's Definition of Done #2 conflict.
        // CONTROL: two, and the first one's result is why the second exists. (a)
        //       `recordHealth(&DaemonHealth::gpu, HealthStatus::Ok, ...)` deleted from
        //       ensureGpu -> only "FAIL: the real daemon's /health names the GPU GetStatus
        //       reports" (detail empty). The status stayed ok because renderPending's
        //       commit records it too, so the status assertion needed that line deleted as
        //       well: with both gone -> "FAIL: ... reports the GPU as ok after a render",
        //       "FAIL: ... reports the aggregate as ok after a successful render" and the
        //       detail one. (b) the detail alone replaced with the literal "gpu" -> only
        //       the names-the-GPU check red, with "/health gpu detail: gpu  dbus: NVIDIA
        //       GeForce RTX 3080 Ti / Heap". All reverted.
        const HttpReply afterRender = wallpaperx::testing::get(cli, "/health");
        // The GetStatus half is graded below through dbusTier/dbusName: an unanswered call
        // leaves both empty, which is the "nothing to compare /health's detail against"
        // check, so the failure is named rather than swallowed.
        const StatusReply status = getStatusOrExplain(*proxy);
        const nlohmann::json& statusJson = status.json();
        const std::string dbusTier = statusJson.contains("gpu") && statusJson["gpu"].is_object()
                                         ? statusJson["gpu"].value("descriptor_tier", std::string())
                                         : std::string();
        const std::string dbusName = statusJson.contains("gpu") && statusJson["gpu"].is_object()
                                         ? statusJson["gpu"].value("name", std::string())
                                         : std::string();
        const nlohmann::json* afterChecks = healthChecks(afterRender.json);
        // ok() stays in the condition: a gpu verdict read out of a body /health did not
        // answer with 200 would satisfy the assertions below without the endpoint working.
        const bool gpuReported = afterRender.ok() && afterChecks != nullptr && afterChecks->contains("gpu") &&
                                 (*afterChecks)["gpu"].is_object();
        const std::string gpuStatus =
            gpuReported ? (*afterChecks)["gpu"].value("status", std::string()) : std::string();
        const std::string gpuDetail =
            gpuReported ? (*afterChecks)["gpu"].value("detail", std::string()) : std::string();
        check(gpuStatus == "ok", "the real daemon's /health reports the GPU as ok after a render");
        check(afterRender.ok() && afterRender.json.value("status", std::string()) == "ok",
              "the real daemon's /health reports the aggregate as ok after a successful render");
        check(!dbusTier.empty() && !dbusName.empty(),
              "GetStatus reports a GPU name and tier for /health's detail to be compared against");
        check(!dbusTier.empty() && !dbusName.empty() && gpuDetail.find(dbusTier) != std::string::npos &&
                  gpuDetail.find(dbusName) != std::string::npos,
              "the real daemon's /health names the GPU GetStatus reports");
        if (gpuDetail.find(dbusTier) == std::string::npos || gpuDetail.find(dbusName) == std::string::npos) {
            std::fprintf(stderr, "  /health gpu detail: %s  dbus: %s / %s\n", gpuDetail.c_str(), dbusName.c_str(),
                         dbusTier.c_str());
        }
        std::printf("daemon_system_test: /health after one wallpaper: %s\n", afterRender.body.c_str());
        std::fflush(stdout);
    }

    // GOAL: S7.11 -- a request in flight cannot read state that is being torn down, and
    //       S7.8 -- nothing is left listening once the process is gone.
    // WHY-THIS-SHAPE: the polling runs ACROSS the SIGTERM rather than before it. A
    //       shutdown with nothing in flight is the easy case and would pass for a daemon
    //       that destroyed its manager under a live handler; what has to be observed is a
    //       poll landing during the teardown, which on this daemon takes milliseconds
    //       (GPU teardown) -- long enough for a loopback poller to land several requests
    //       in. The graded facts are then the ones a broken ordering destroys: the daemon
    //       still exits 0 through its own shutdown path, still prints the line that means
    //       the destructor chain ran, and never answered a poll with a malformed body.
    //       The alternative -- asserting that some specific request got a 200 during the
    //       window -- would be an assertion on the scheduler (X.2), since when the poller
    //       stops being served is exactly what shutdown decides.
    // CONTROL: main.cpp's `HttpServer httpServer(manager);` replaced with
    //       `auto* leaked = new HttpServer(manager); (void)leaked;` -- the server then
    //       outlives the manager it reads, which is precisely the ordering the
    //       declare-it-last comment exists to guarantee. Observed: the daemon died on
    //       SIGSEGV during shutdown -> "FAIL: the daemon exits 0 on SIGTERM with HTTP
    //       polls in flight" and "FAIL: the daemon still runs its destructor chain with
    //       HTTP polls in flight". Reverted.
    // COVERAGE-ASSERT: the number of responses the poller actually collected is asserted,
    //       not assumed. A poller that never connected would make the malformed-response
    //       check trivially true, and this phase would then be grading an ordinary
    //       shutdown with nothing in flight at all.
    Poller poller(kPort, "/config?monitor=" + std::to_string(kMonitorHttp));
    for (int i = 0; i < 500 && poller.recorded() < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const size_t recordedBeforeSignal = poller.recorded();

    const int exitCode = daemon.shutdown(SIGTERM);
    const std::string output = daemon.output();
    poller.stop();

    check(poller.error().empty(), "the /health poller across the shutdown ran without throwing");
    check(recordedBeforeSignal >= 5, "HTTP polls really were in flight when the daemon was signalled");

    int answered = 0;
    int malformed = 0;
    for (const Poller::Sample& s : poller.samples()) {
        if (!s.reached) continue;  // refused/closed after shutdown -- expected, not a fault
        ++answered;
        if (s.status != 200 || !s.bodyParsed) ++malformed;
    }
    check(malformed == 0, "no HTTP poll during the daemon's shutdown got a malformed or error reply");
    if (malformed != 0) {
        std::fprintf(stderr, "  %d of %d answered polls were not a parseable 200\n", malformed, answered);
    }

    if (output.find("Sanitizer: reported") != std::string::npos) {
        // Same shape as the SIGTERM and SIGINT phases: a sanitized build replaces the
        // exit code, so the exit-code question stops being about the shutdown path.
        check(false, "the HTTP-lifecycle daemon's sanitizer reported no warnings (its exit code is the "
                     "sanitizer's here, so the exit-code check cannot run)");
    } else {
        check(exitCode == 0, "the daemon exits 0 on SIGTERM with HTTP polls in flight");
    }
    check(!daemon.timedOut(), "the daemon exits rather than hanging when its serving thread has to be joined");
    check(output.find("stopped cleanly") != std::string::npos,
          "the daemon still runs its destructor chain with HTTP polls in flight");
    check(names.waitFor(false), "the bus name is released after the HTTP-lifecycle daemon exits");

    // S7.8's own assertion. Deliberately after the reap: `shutdown()` does not return
    // until the process is gone, so a still-open socket here is a socket the exited
    // process left behind, not one it is still using.
    const ConnectResult afterExit = tryConnect("127.0.0.1", kPort, 1000);
    check(afterExit == ConnectResult::Refused, "nothing is listening on 18462 once the daemon has exited");
    if (afterExit != ConnectResult::Refused) {
        std::fprintf(stderr, "  connect to 127.0.0.1:%d %s (expected refused)\n", kPort, describe(afterExit));
    }
}

// ---------------------------------------------------------------------------- //
// Phase 11 -- S7.14: the daemon says on DBus whether it got its own config port.
// ---------------------------------------------------------------------------- //
//
// Both directions run against the real binary, because main.cpp is the only place the
// bind result is ever handed to the manager. The failing direction needs a process holding
// 18462 before the daemon starts, which an in-process test cannot arrange for a main() it
// never runs.

// Who holds the listening socket on 127.0.0.1:`port`. Read from /proc, deliberately: the
// whole question is whether the daemon's own report about that port can be trusted, so the
// answer has to come from somewhere the daemon does not write.
enum class Listener { Daemon, Other, Nobody, Unknown };

// The inode of the socket listening on 127.0.0.1:`port`, or 0 if there is none. Returns
// false only when /proc/net/tcp could not be read -- "unreadable" and "nothing is
// listening" are different answers and only one of them says anything about the daemon.
bool listeningSocketInode(int port, unsigned long long& outInode, std::string& why) {
    std::ifstream tcp("/proc/net/tcp");
    if (!tcp) {
        why = "/proc/net/tcp could not be opened";
        return false;
    }
    char wanted[32];
    std::snprintf(wanted, sizeof(wanted), "0100007F:%04X", port);  // 127.0.0.1, as /proc prints it

    outInode = 0;
    std::string line;
    std::getline(tcp, line);  // column header
    while (std::getline(tcp, line)) {
        std::istringstream fields(line);
        std::string sl, local, remote, state, queues, timer, retransmit, uid, timeout, inode;
        if (!(fields >> sl >> local >> remote >> state >> queues >> timer >> retransmit >> uid >> timeout >> inode)) {
            continue;
        }
        if (state != "0A" || local != wanted) continue;  // 0A == TCP_LISTEN
        outInode = std::strtoull(inode.c_str(), nullptr, 10);
        return true;
    }
    return true;
}

Listener whoListens(int port, pid_t pid, std::string& why) {
    unsigned long long inode = 0;
    if (!listeningSocketInode(port, inode, why)) return Listener::Unknown;
    if (inode == 0) return Listener::Nobody;
    if (pid <= 0) {
        why = "the daemon's pid is no longer known";
        return Listener::Unknown;
    }

    const std::string target = "socket:[" + std::to_string(inode) + "]";
    const std::string fdDir = "/proc/" + std::to_string(pid) + "/fd";
    std::error_code ec;
    std::filesystem::directory_iterator entries(fdDir, ec);
    if (ec) {
        why = fdDir + " could not be read (" + ec.message() + ")";
        return Listener::Unknown;
    }
    const std::filesystem::directory_iterator end;
    while (entries != end) {
        std::error_code linkEc;
        // Descriptors open and close under the walk; one that cannot be read at this
        // instant is not evidence either way.
        const std::filesystem::path link = std::filesystem::read_symlink(entries->path(), linkEc);
        if (!linkEc && link.native() == target) return Listener::Daemon;
        entries.increment(ec);
        if (ec) {
            why = fdDir + " could not be walked (" + ec.message() + ")";
            return Listener::Unknown;
        }
    }
    return Listener::Other;
}

const char* describeListener(Listener who) {
    switch (who) {
        case Listener::Daemon: return "the spawned daemon";
        case Listener::Other: return "some other process";
        case Listener::Nobody: return "nobody";
        case Listener::Unknown: return "could not be determined";
    }
    return "?";
}

// A barrier between phases that each spawn a daemon, deliberately not a check: shutdown()
// does not return until the process is reaped, and a reaped process has had its name
// released by the kernel whatever the daemon did -- so no mutation makes this red. Release
// on shutdown is graded, controllably, by the SIGTERM phase. A timeout is reported as what
// it is: the next phase cannot be set up.
void awaitNameRelease(NameWatcher& names, const char* nextUp) {
    if (!names.waitFor(false)) {
        notVerified(nextUp, "the bus name was still owned after this phase's daemon had been reaped");
    }
}

// Direction 1: the port is free, so the daemon is the process listening on it.
void checkConfigServerOwnsPort(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr int kPort = wallpaperx::HttpServer::kDefaultPort;

    SpawnedDaemon daemon({validationEnv});
    if (!names.waitFor(true)) {
        notVerified("S7.14 (the ok direction)", "the daemon for this phase never acquired the bus name");
        return;
    }
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

    const StatusReply status = getStatus(*proxy);
    if (!status.ok()) {
        check(false, "the daemon answers GetStatus once it holds the bus name");
        std::fprintf(stderr, "  %s\n", status.error().c_str());
        daemon.shutdown(SIGTERM);
        awaitNameRelease(names, "S7.14 (the fail direction, which follows this one)");
        return;
    }

    // GOAL: S7.14's ok direction -- config_server reads ok exactly when this daemon is the
    //       process listening on 18462, not merely whenever the daemon is up.
    // WHY-THIS-SHAPE: /proc is asked who holds the socket, rather than the test believing
    //       the daemon or believing that a free port stayed free. Asserting only
    //       `status == "ok"` here passes for a hardcoded string and for a daemon that
    //       reports ok about a port some other process took between the spawn and the
    //       call. Sampled AFTER a GetStatus reply on purpose: main.cpp constructs the
    //       HttpServer, whose constructor records the bind result, before entering the
    //       event loop -- so a reply having come back is proof the decision is made, and
    //       nothing answers GetStatus before then. The bus name, which is taken earlier,
    //       is not that proof.
    // CONTROL: three. (a) main.cpp's HttpServer constructed on kDefaultPort + 1, its
    //       constructor still recording the bind -> "FAIL: config_server reads ok exactly
    //       when this daemon is the process holding 18462" with "  config_server: ok /
    //       listening, but 127.0.0.1:18462 is held by nobody". (b) the `status
    //       ["config_server"] = ...` line deleted from DbusService::getStatus -> "FAIL:
    //       GetStatus carries a config_server object". (c) recordConfigServer's "listening"
    //       detail replaced with "" -> "FAIL: the config_server report carries a detail, not
    //       a bare status". All reverted.
    std::string why;
    const Listener who = whoListens(kPort, daemon.pid(), why);
    const nlohmann::json* report = configServerReport(status);
    const std::string reportStatus = report ? report->value("status", std::string()) : std::string();
    const std::string reportDetail = report ? report->value("detail", std::string()) : std::string();

    if (who == Listener::Unknown) {
        // A harness fault, not the daemon's: X.3 forbids spending a FAIL line on it.
        notVerified("S7.14 (the ok direction)", "who is listening on 127.0.0.1:" + std::to_string(kPort) +
                                                    " could not be established: " + why);
    } else if (who == Listener::Other) {
        notVerified("S7.14 (the ok direction)", "port " + std::to_string(kPort) +
                                                    " is held by another process on this machine, so the spawned "
                                                    "daemon could never have owned it");
    } else {
        // Nobody-listening is not a skip: a daemon claiming ok about a port no process
        // holds is exactly what this goal says must be false, and the equality below still
        // grades it. It only stops the ok direction being *claimed* when the daemon agrees
        // it never got the port.
        check(report != nullptr, "GetStatus carries a config_server object");
        if (report != nullptr) {
            check((reportStatus == "ok") == (who == Listener::Daemon),
                  "config_server reads ok exactly when this daemon is the process holding 18462");
            check(!reportDetail.empty(), "the config_server report carries a detail, not a bare status");
            if ((reportStatus == "ok") != (who == Listener::Daemon) || reportDetail.empty()) {
                std::fprintf(stderr, "  config_server: %s / %s, but 127.0.0.1:%d is held by %s\n",
                             reportStatus.c_str(), reportDetail.empty() ? "(empty)" : reportDetail.c_str(), kPort,
                             describeListener(who));
            }
        }
        if (who == Listener::Nobody && reportStatus != "ok") {
            notVerified("S7.14 (the ok direction)", "the spawned daemon did not take port " + std::to_string(kPort) +
                                                        " and nothing else did either, so there was no ok direction "
                                                        "to observe");
        }
    }

    // GOAL: S7.14's placement -- the bind result travels on DBus and NOT in /health's
    //       checks. On the failing side /health is answered by whatever took the port, so a
    //       config_server check there could only ever read ok: a check that cannot fail.
    // WHY-THIS-SHAPE: asserting the key's absence on its own is satisfied by a /health that
    //       returned nothing at all, so the same check demands the three real checks are
    //       present and that GetStatus carries config_server at the same moment. What is
    //       graded is the two channels *differing*, which is the decision worth protecting.
    // CONTROL: `{"config_server", &snap.health.configServer},` added to http_server.cpp's
    //       checks array (sized 4) -> "FAIL: the bind result is on DBus only -- /health
    //       names its three checks and not config_server". Reverted.
    httplib::Client cli = wallpaperx::testing::makeClient(kPort);
    const wallpaperx::testing::HttpReply health = wallpaperx::testing::get(cli, "/health");
    const nlohmann::json* checks = healthChecks(health.json);
    const bool healthUsable = health.ok() && checks != nullptr;
    if (!healthUsable) {
        // Whether /health answers at all is graded by the HTTP-lifecycle phase, against a
        // daemon of its own. Grading it again here would give one fault two names.
        std::printf("daemon_system_test: the /health-placement check is not graded -- /health did not answer with a "
                    "checks object on this daemon.\n");
    } else {
        check(checks->contains("shaders") && checks->contains("gpu") && checks->contains("wallpapers") &&
                  !checks->contains("config_server") && configServerReport(status) != nullptr,
              "the bind result is on DBus only -- /health names its three checks and not config_server");
    }

    daemon.shutdown(SIGTERM);
    awaitNameRelease(names, "S7.14 (the fail direction, which follows this one)");
}

// Direction 2: this test holds the port first, so the daemon cannot have it.
//
// `hostRendered` says whether any daemon in this run got a frame out of this host. It is
// what separates "the conflict crippled the daemon" from "this machine has no working
// Vulkan", and it must come from outside this phase -- asking the crippled daemon itself
// would answer the question with the thing being questioned.
void checkConfigServerLostPort(NameWatcher& names, const std::string& bluePath, const std::string& validationEnv,
                               bool hostRendered, const Check& check) {
    constexpr int kPort = wallpaperx::HttpServer::kDefaultPort;

    // The conflict is created here rather than hoped for, and the decision to skip is taken
    // on THIS process's bind() -- never on config_server, the field under test. A gate that
    // read the field would turn a daemon reporting the wrong thing into a green skip.
    wallpaperx::testing::HeldPort impostor;
    if (!impostor.open(INADDR_LOOPBACK, kPort)) {
        notVerified("S7.14 (the fail direction)", "this test could not take port " + std::to_string(kPort) +
                                                      " itself, so the port conflict could not be created");
        return;
    }

    SpawnedDaemon daemon({validationEnv});
    if (!names.waitFor(true)) {
        const std::string startupOutput = daemon.output();
        if (startupOutput.find(bindFailureMarker(kPort)) != std::string::npos) {
            // It reached its bind attempt, failed it, and then never made it onto the bus.
            // That is the daemon dying of a port conflict, which is a finding and not a
            // reason to skip.
            // UNCONTROLLED: a throwing bind branch does reach this state, but which of the
            // two branches wins is a race with the name request that happens first -- the
            // run that was tried took the other one, reporting through the GetStatus check
            // below. Kept because the alternative is calling a daemon that died of the
            // conflict a skip.
            check(false, "the daemon still reaches the bus when its config port is already taken");
        } else {
            notVerified("S7.14 (the fail direction)",
                        "the daemon for this phase never acquired the bus name and never reached its bind attempt");
        }
        return;
    }

    // Declared before the proxy so it is destroyed after it: the FrameReady callback runs on
    // the proxy's dispatch thread, and a log destroyed first is written to by a signal still
    // in flight during teardown. ThreadSanitizer reported exactly that race when these two
    // were the other way round.
    FrameReadyLog signals;
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

    const StatusReply status = getStatus(*proxy);

    // COVERAGE-ASSERT for everything below: that the conflict was in force when the report
    // was read. Held sockets are not shareable -- HeldPort sets SO_REUSEADDR and never
    // SO_REUSEPORT -- so a daemon found listening here means the phase graded a daemon that
    // got the port after all, and every "fail" assertion under it would be nonsense.
    // Sampled after the GetStatus reply, which is proof main() is past its bind attempt.
    // CONTROL: SOCK_CLOEXEC dropped from HeldPort, so the forked daemon inherits this
    //       socket -> "FAIL: the port conflict is real: the daemon is not the process
    //       listening on 18462". That was a live bug in the first draft, not a hypothetical.
    //       Restored.
    std::string listenerWhy;
    const Listener who = whoListens(kPort, daemon.pid(), listenerWhy);
    if (who == Listener::Unknown) {
        std::printf("daemon_system_test: could not confirm from /proc who holds 127.0.0.1:%d (%s); the conflict is "
                    "still real -- this test holds the socket -- so the checks below stand.\n",
                    kPort, listenerWhy.c_str());
    } else {
        check(who != Listener::Daemon,
              "the port conflict is real: the daemon is not the process listening on 18462");
    }

    // GOAL: S7.14's substantive half -- a daemon that lost its config port says so on DBus,
    //       with a cause an operator can act on, while still serving DBus at all.
    // WHY-THIS-SHAPE: the port is held by this test before the daemon is spawned, so the
    //       failure is certain rather than opportunistic; and "still serving" is asserted
    //       from the same reply that carries the report, because a GetStatus that came back
    //       is itself the evidence the daemon is alive. The alternative -- reading the
    //       daemon's stderr line -- says only that it printed something on the way past,
    //       which is the state this goal exists to improve on.
    // CONTROL: three, all in HttpServer's constructor. (a) the bind-failure branch's
    //       `m_manager.recordConfigServer(false, bindError)` deleted -> "FAIL: a daemon
    //       that could not bind its config port reports config_server fail" and "FAIL: the
    //       failed bind report names the port it could not take", with "  config_server:
    //       pending / (empty), while 127.0.0.1:18462 is held by some other process". The
    //       success-path record next to it is the wrong line to cut: deleting that one
    //       instead leaves both checks green and reddens only the ok direction above.
    //       (b) bindError reworded so the host:port moves into parentheses -> the
    //       wording check below red, the fail one and the names-the-port one both still
    //       green (that rewording keeps "127.0.0.1:18462" inside the detail).
    //       (c) the bind branch's
    //       `return` replaced with `throw std::runtime_error(bindError)` -> "FAIL: a
    //       daemon that could not bind its config port still answers GetStatus", plus the
    //       two shutdown checks below; which DBus error comes back depends on whether the
    //       daemon dies before or after its name request, NoReply and ServiceUnknown both
    //       observed. All reverted.
    check(status.ok(), "a daemon that could not bind its config port still answers GetStatus");
    if (!status.ok()) {
        std::fprintf(stderr, "  %s\n", status.error().c_str());
    } else {
        const nlohmann::json* report = configServerReport(status);
        const std::string reportStatus = report ? report->value("status", std::string()) : std::string();
        const std::string reportDetail = report ? report->value("detail", std::string()) : std::string();
        const std::string hostPort =
            std::string(wallpaperx::HttpServer::kLoopbackHost) + ":" + std::to_string(kPort);

        check(status.json().value("running", false),
              "a daemon that could not bind its config port still reports running");
        check(report != nullptr, "GetStatus carries a config_server object");
        check(reportStatus == "fail", "a daemon that could not bind its config port reports config_server fail");
        // Not merely non-empty: "fail" with no cause sends an operator back to the stderr
        // this report exists to replace, and the port is the one fact they need to go and
        // look for the process that took it. Built from the daemon's own two constants, so
        // a change of host or port fails the build rather than this assertion.
        check(reportDetail.find(hostPort) != std::string::npos,
              "the failed bind report names the port it could not take");

        // GOAL: X.3 -- two fail-or-skip decisions in this file match the daemon's own
        //       bind-failure wording in its stderr, and a test cannot read a literal out of
        //       a program it only spawns. This asserts the duplicate is still the daemon's
        //       string, so a rewording arrives as a named failure here rather than as those
        //       two sites quietly deciding "skip" and "never reached its bind attempt".
        // WHY-THIS-SHAPE: compared against the DETAIL the daemon just reported over DBus,
        //       which http_server.cpp records from the same local `bindError` it prints.
        //       Matching the daemon's stderr instead would be circular -- it is exactly the
        //       text whose stability is in question, and both sides would move together. The
        //       existing names-the-port check above does not cover this: it looks for
        //       "127.0.0.1:18462", which survives any rewording around it.
        // CONTROL: bindError reworded to "the config port could not be bound (" +
        //       kLoopbackHost + ":" ... in http_server.cpp -> "FAIL: the bind-failure
        //       wording this file matches on in the daemon's stderr is still the daemon's"
        //       with '  bindFailureMarker(18462) = "could not bind 127.0.0.1:18462", but
        //       the daemon reports "the config port could not be bound (127.0.0.1:18462)
        //       (is another process using it?)"', and it was the run's only failure -- the
        //       names-the-port check above stayed green, since the rewording kept the
        //       host:port inside it. Reverted.
        const std::string marker = bindFailureMarker(kPort);
        check(reportDetail.find(marker) != std::string::npos,
              "the bind-failure wording this file matches on in the daemon's stderr is still the daemon's");
        if (reportDetail.find(marker) == std::string::npos) {
            std::fprintf(stderr, "  bindFailureMarker(%d) = \"%s\", but the daemon reports \"%s\"\n", kPort,
                         marker.c_str(), reportDetail.c_str());
        }

        if (reportStatus != "fail" || reportDetail.find(hostPort) == std::string::npos) {
            std::fprintf(stderr, "  config_server: %s / %s, while %s is held by %s\n",
                         report ? reportStatus.c_str() : "(absent)",
                         reportDetail.empty() ? "(empty)" : reportDetail.c_str(), hostPort.c_str(),
                         describeListener(who));
        }
    }

    // GOAL: S7.14 is only worth having because the daemon keeps working -- a report from a
    //       corpse would be a crash, not a diagnosis. So it must still render after losing
    //       the port.
    // WHY-THIS-SHAPE: a render, not a second GetStatus: answering the bus is the cheap half
    //       and GOALS.md claims DBus *and* rendering continue. `hostRendered` is what
    //       decides whether a missing frame is a failure or an unexercised goal: an earlier
    //       phase of this same run got a frame out of the same binary, so a frame that never
    //       arrives here cannot be blamed on the Vulkan stack. Without that distinction this
    //       would either accuse a host with no ICD or -- the version that matters -- let a
    //       daemon crippled by the conflict pass as a skip. SetWallpaper is inside the try
    //       because a daemon killed by the conflict would otherwise throw out of the phase
    //       and take these diagnostics with it.
    // CONTROL: two. (a) `throw InvalidRequest("control")` at the top of
    //       WallpaperManager::requestWallpaper -> "FAIL: a daemon that could not bind its
    //       config port still accepts SetWallpaper" with the daemon's error text. (b) the
    //       FrameReady emission in dbus_service.cpp suppressed for this phase's monitor
    //       only, so the run still renders elsewhere and hostRendered stays true -> "FAIL: a
    //       daemon that could not bind its config port renders a wallpaper anyway". Both
    //       reverted.
    if (status.ok()) {
        bool rendered = false;
        std::string renderError;
        try {
            setWallpaper(*proxy, "static", bluePath, kMonitorLostPort);
            rendered = signals.wait(kMonitorLostPort, 1);
        } catch (const std::exception& e) {
            renderError = e.what();
        }
        if (!renderError.empty()) {
            check(false, "a daemon that could not bind its config port still accepts SetWallpaper");
            std::fprintf(stderr, "  SetWallpaper failed: %s\n", renderError.c_str());
        } else if (!rendered && !hostRendered) {
            notVerified("S7.14 (rendering continues after a lost port)",
                        "no daemon rendered a frame at all in this run, so this host cannot tell a daemon crippled by "
                        "the port conflict from one with no working Vulkan");
        } else {
            check(rendered, "a daemon that could not bind its config port renders a wallpaper anyway");
        }
    }

    // The port conflict must not change how the daemon goes down: the HttpServer destructor
    // still runs on a server that never started a serving thread.
    // CONTROL: control (c) above -- a throwing bind branch -> "FAIL: a daemon that could not
    //       bind its config port still exits 0 on SIGTERM" and "FAIL: ... still runs its
    //       destructor chain", the daemon having printed "fatal: could not bind
    //       127.0.0.1:18462 ..." and never "stopped cleanly". Reverted.
    const int exitCode = daemon.shutdown(SIGTERM);
    const std::string output = daemon.output();
    if (output.find("Sanitizer: reported") != std::string::npos) {
        // Same shape as every other exit-code site here: a sanitized build replaces the
        // exit code, so this stops being a question about the shutdown path.
        check(false, "the port-conflict daemon's sanitizer reported no warnings (its exit code is the sanitizer's "
                     "here, so the exit-code check cannot run)");
    } else {
        check(exitCode == 0, "a daemon that could not bind its config port still exits 0 on SIGTERM");
    }
    check(output.find("stopped cleanly") != std::string::npos,
          "a daemon that could not bind its config port still runs its destructor chain");
    awaitNameRelease(names, "any phase added after this one");
}

}  // namespace

int main(int argc, char** argv) {
    // The child half of checkAbortedParentLeavesNoDaemon, and nothing else: it spawns a
    // daemon and aborts, so it must never be reached by an ordinary run.
    if (argc == 2 && std::string_view(argv[1]) == "--orphan-probe") return runOrphanProbe();

    int failures = 0;
    const Check check = [&](bool cond, const char* msg) {
        if (!cond) {
            std::fprintf(stderr, "FAIL: %s\n", msg);
            ++failures;
        }
    };

    const bool wantValidation = !validationWaived() && validationLayerAvailable();
    if (validationWaived()) {
        std::printf("daemon_system_test: WARNING -- running with WALLPAPERX_ALLOW_NO_VALIDATION=1. The daemon is "
                    "started with validation off and the check for validation warnings in its output is DISABLED; "
                    "this run proves only that the process, wire and threading behaviour holds.\n");
    } else if (!wantValidation) {
        std::printf("daemon_system_test: WARNING -- VK_LAYER_KHRONOS_validation is not installed, so the check "
                    "for validation warnings in the daemon's output cannot fail. This run proves only that the "
                    "process, wire and threading behaviour holds.\n");
    }

    std::string daemonOutput;
    // Distinguishes "the daemon printed nothing" from "we never got as far as reading its
    // output". daemonOutput is filled only after the phases, so on the exception path it
    // is empty no matter what the daemon actually said -- and the anchor below would then
    // report a capture failure on top of the exception that was already reported.
    bool daemonOutputRead = false;
    // Hoisted out of the try for the same reason: a daemon that never reached the bus has
    // already been reported, and its silence about Vulkan is a consequence of that rather
    // than a separate finding.
    bool daemonCameUp = false;
    try {
        const std::string bluePath = writeSolidImage("system_blue.png", 0, 0, 255);
        const std::string redPath = writeSolidImage("system_red.png", 255, 0, 0);
        const std::string corruptPath = wallpaperx::testing::writeUndecodableFile(testAssetDir(), "system_corrupt.png");

        // Declared before the daemon and the proxy so it outlives both: the FrameReady
        // callback runs on the proxy's dispatch thread, and a log destroyed first would
        // be locked by a signal still in flight during teardown.
        FrameReadyLog signals;

        // Subscribed before anything is spawned, so the name's appearance cannot be
        // missed between the fork and the subscription.
        NameWatcher names;
        check(!names.ownedNow(), "the bus name is free before the daemon starts (the session bus is private)");

        const size_t fdsBefore = openFdCount();

        const std::string validationEnv = std::string("WALLPAPERX_VALIDATION=") + (wantValidation ? "1" : "0");
        SpawnedDaemon daemon({validationEnv});
        const bool cameUp = names.waitFor(true);
        daemonCameUp = cameUp;
        check(cameUp, "the daemon acquires io.wallpaperx.Daemon on the session bus");

        if (cameUp) {
            // A second instance must lose. createSessionBusConnection() requests the name
            // without SD_BUS_NAME_REPLACE_EXISTING, so the second process is expected to
            // fail its name request and exit non-zero rather than take the desktop's
            // wallpaper daemon away from the one already serving it.
            SpawnedDaemon second;
            const int secondExit = second.waitForExit();
            // Both conditions, because reap() reports a timeout as -1 too: a second daemon
            // that wrongly took the name and then ran forever would be SIGKILLed by the
            // test and satisfy `secondExit != 0` -- reporting the name as refused when it
            // was in fact stolen.
            check(second.exitedNormally(),
                  "the second daemon instance exits on its own rather than crashing or having to be killed");
            if (!second.exitedNormally() && second.termSignal() != 0) {
                std::fprintf(stderr, "  it died on signal %d\n", second.termSignal());
            }
            check(second.exitedNormally() && secondExit != 0,
                  "a second daemon instance refuses to start while the first owns the name");
            check(names.ownedNow(), "the first daemon still owns the name after the second one exits");
            const std::string secondOutput = second.output();
            check(secondOutput.find("fatal:") != std::string::npos,
                  "the second instance says why it exited rather than dying silently");

            // GOAL: S6.28 -- the second instance is refused with
            //       org.freedesktop.DBus.Error.FileExists, i.e. it loses the name
            //       request, rather than failing for some unrelated reason that
            //       happens to be fatal.
            // WHY-THIS-SHAPE: "the output contains fatal:" is satisfied by any startup
            //       failure at all -- a missing shader directory, a broken ICD, an
            //       unwritable path. Each of those would make this phase pass while
            //       proving nothing about name ownership, and one of them is a
            //       plausible future regression. The error name is what pins the
            //       reason, and it is the name a caller of
            //       createSessionBusConnection() would branch on.
            // CONTROL: main.cpp patched to throw before requesting the name
            //       (`throw std::runtime_error("no")` above createSessionBusConnection)
            //       -> "FAIL: the second instance is refused with
            //       org.freedesktop.DBus.Error.FileExists", while the pre-existing
            //       "says why it exited" check stayed green. Reverted.
            check(secondOutput.find("org.freedesktop.DBus.Error.FileExists") != std::string::npos,
                  "the second instance is refused with org.freedesktop.DBus.Error.FileExists");

            auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                            sdbus::ObjectPath{DbusService::kObjectPath});
            proxy->uponSignal("FrameReady")
                .onInterface(DbusService::kInterface)
                .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

            checkFirstFrameUnderStatusLoad(*proxy, signals, bluePath, check);
            checkResponsivenessUnderLoad(*proxy, signals, bluePath, check);
            checkConcurrentClients(*proxy, signals, bluePath, redPath, corruptPath, check);
        }

        // SIGTERM, which is what systemd sends (Step 14) and what a `kill` defaults to.
        // Safe to send now and not earlier: the daemon blocks both shutdown signals
        // before it creates any thread, and it does that before requesting the bus name
        // -- so a name that is on the bus is a mask that is already installed. Sent
        // before that point, the default disposition would kill it outright.
        const int exitCode = daemon.shutdown(SIGTERM);
        daemonOutput = daemon.output();
        daemonOutputRead = true;
        if (!cameUp) {
            // The daemon never reached the bus, which is already reported above. Signalling
            // it anyway is right -- it must not outlive the test -- but grading its exit
            // would add four more FAIL lines blaming main.cpp's shutdown path for a fault
            // that happened before the shutdown path was ever reached.
            std::printf("daemon_system_test: shutdown checks skipped -- the daemon never acquired the bus name, so "
                        "its exit says nothing about the shutdown path.\n");
        } else {
            // A sanitizer that reported anything replaces the process's exit code with its
            // own, so under a sanitized build "did the daemon exit 0" stops being a question
            // about the shutdown path. Reported as what it actually is instead: a
            // non-zero exit that reads "the shutdown path is broken" would send the next
            // reader to main.cpp, which is not where the problem is.
            const bool sanitizerReported = daemonOutput.find("Sanitizer: reported") != std::string::npos;
            if (sanitizerReported) {
                check(false, "the daemon's sanitizer reported no warnings (its exit code is the sanitizer's here, so "
                             "the SIGTERM exit-code check cannot run)");
            } else {
                check(exitCode == 0, "the daemon exits 0 on SIGTERM");
            }
            check(daemonOutput.find(kSignalShutdownLine) != std::string::npos,
                  "the daemon reports which signal it is shutting down for");
            check(daemonOutput.find("stopped cleanly") != std::string::npos,
                  "the daemon runs its destructor chain on the way out (worker join, GPU teardown, leak report)");
            check(names.waitFor(false), "the bus name is released once the daemon exits");
        }

        // The other signal in the same sigset. Cheap -- it never renders -- and without
        // it a typo that dropped SIGINT from the mask would be invisible, since SIGTERM
        // alone exercises one sigaddset call out of two.
        {
            SpawnedDaemon interrupted;
            const bool interruptedCameUp = names.waitFor(true);
            check(interruptedCameUp, "a fresh daemon acquires the name after the first released it");
            const int interruptExit = interrupted.shutdown(SIGINT);
            const std::string interruptOutput = interrupted.output();
            if (!interruptedCameUp) {
                // Same reasoning as the SIGTERM block: a daemon that never started tells
                // us nothing about how it shuts down, and grading it anyway turns one
                // startup fault into three failures pointing at the signal path.
                std::printf("daemon_system_test: SIGINT checks skipped -- that daemon never acquired the bus "
                            "name either.\n");
            } else {
                // Same shape as the SIGTERM branch above, deliberately. Wrapping this in a
                // bare `if (no sanitizer report)` made the assertion vanish with no FAIL, no
                // print and no change in the count, so a sanitized run looked like it had
                // verified SIGINT when it had verified nothing.
                if (interruptOutput.find("Sanitizer: reported") != std::string::npos) {
                    check(false, "the SIGINT daemon's sanitizer reported no warnings (its exit code is the "
                                 "sanitizer's here, so the SIGINT exit-code check cannot run)");
                } else {
                    check(interruptExit == 0, "the daemon exits 0 on SIGINT");
                }
                check(interruptOutput.find("stopped cleanly") != std::string::npos,
                      "the daemon shuts down cleanly on SIGINT as well as SIGTERM");
                check(names.waitFor(false), "the bus name is released after the SIGINT shutdown");
            }
        }

        // The third signal in that sigset, and the one a terminal close or a logout
        // sends. Its own phase rather than a third branch of the block above, because it
        // is a distinct goal (S6.36) with a distinct failure -- exit 129, none of the
        // chain -- rather than more coverage of the same one.
        checkHangupShutdown(names, check);

        // S6.37 and S6.35. Both spawn daemons of their own and shut them down, so they
        // run after the phases above have released the name, and before the descriptor
        // accounting below.
        checkClosedStdout(names, bluePath, validationEnv, wantValidation, check);
        checkBusLoss(check);

        // Spawns a daemon of its own on this bus and leaves nothing behind, so it belongs
        // between the phases that own the name one at a time -- and before the port phases
        // below, whose whole subject is who is listening on 18462.
        const bool busClearAfterOrphan = checkAbortedParentLeavesNoDaemon(names, check);

        // Every phase below spawns a daemon that must take the bus name and bind 18462. A
        // daemon the phase above failed to clear still holds both, so all three would
        // report the *harness's* leak as the production defect they were written to detect
        // (X.3). The leak itself is already graded above; this is not a second verdict.
        if (!busClearAfterOrphan) {
            notVerified("S7.8, S7.11 (HTTP lifecycle), S7.14, S7.15 (who owns the config port)",
                        "a daemon this run could not clear still owns the bus name, so every bind failure below "
                        "would be this harness's leak rather than the daemon's behaviour");
        } else {
            // S7.8 and S7.11. Spawns a daemon of its own and shuts it down, so it runs
            // after the phases above have released the name, and before the descriptor
            // accounting below -- its httplib clients close their sockets on destruction,
            // which that accounting would otherwise report as a leak in this process.
            checkHttpLifecycle(names, bluePath, validationEnv, check);

            // S7.14, both directions. After checkHttpLifecycle rather than beside it: that
            // phase asserts nothing is listening on 18462 once its daemon has gone, and the
            // second of these deliberately puts a socket of this test's own there. Before
            // the descriptor accounting, since both hold sockets that close at scope exit.
            checkConfigServerOwnsPort(names, validationEnv, check);
            // signals is this run's whole FrameReady history, from the daemons of the
            // phases above: a non-empty one is proof this host can render, which is what
            // lets the port-conflict phase grade a missing frame instead of skipping on it.
            checkConfigServerLostPort(names, bluePath, validationEnv, signals.total() > 0, check);
        }

        // Every Frame's UnixFd closes at scope exit, and the daemon is gone, so this
        // process must be back where it started. A descriptor leaked per GetFrameFd is
        // an eventual EMFILE in a polling consumer, not a one-off.
        const size_t fdsAfter = openFdCount();
        check(fdsAfter <= fdsBefore, "the client leaks no descriptors across the whole run");
        if (fdsAfter > fdsBefore) {
            std::fprintf(stderr, "  %zu open fds before, %zu after\n", fdsBefore, fdsAfter);
        }
    } catch (const std::exception& e) {
        // Without this an escaping sdbus::Error -- what a host with no usable Vulkan ICD
        // produces -- would abort the process, so ctest would report an opaque SIGABRT
        // instead of the named failures above.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++failures;
    }

    // GOAL: S6.31 -- the daemon reports its GPU name and descriptor tier once the device
    //       exists (Definition of Done #1). The startup line alone does not satisfy it:
    //       an operator needs to know which device and which tier the daemon resolved,
    //       and the tier is the whole point of the three-tier descriptor system.
    // WHY-THIS-SHAPE: asserted against the daemon's captured stdout rather than read back
    //       through GetStatus, because DoD #1 is a claim about what the daemon *prints*.
    //       GetStatus carries the same two fields and is already checked (S6.15, S6.16),
    //       so going over the wire here would restate that check and leave the printed
    //       output -- the thing this goal is about -- unverified. Matched on the two
    //       labels rather than the whole line, so the GPU name, which differs per host
    //       and per ICD, is not baked into the assertion.
    // CONTROL: with the printf deleted from WallpaperManager::ensureGpu, this reports
    //       "FAIL: the daemon reports its GPU and descriptor tier once the device exists"
    //       while every other check stays green. Reverted.
    if (!daemonOutputRead || !daemonCameUp) {
        std::printf("daemon_system_test: GPU/tier report check skipped -- %s, and the failure that caused "
                    "that is reported above.\n",
                    daemonCameUp ? "the daemon's output was never collected" : "the daemon never started");
    } else {
        // Both labels on ONE line, not merely both present somewhere in the capture.
        // Two unanchored find()s are a strictly weaker property than DoD #1 states: a
        // daemon that printed the GPU name at startup and the tier from some unrelated
        // path would satisfy them, and so would one that printed a tier without ever
        // creating a device -- which the printf's own comment says must not happen. The
        // GPU name itself stays out of the assertion because it is host-specific.
        bool reported = false;
        for (size_t start = 0; start < daemonOutput.size();) {
            const size_t eol = daemonOutput.find('\n', start);
            const std::string_view line(daemonOutput.data() + start,
                                        (eol == std::string::npos ? daemonOutput.size() : eol) - start);
            const size_t gpu = line.find("GPU: ");
            if (gpu != std::string_view::npos && line.find("Descriptor tier: ", gpu) != std::string_view::npos) {
                reported = true;
                break;
            }
            if (eol == std::string::npos) break;
            start = eol + 1;
        }
        check(reported, "the daemon reports its GPU and descriptor tier on one line once the device exists");
        if (!reported) {
            std::fprintf(stderr, "  no single 'GPU: ... | Descriptor tier: ...' line in the daemon's output\n");
        }
    }

    // The daemon's own Vulkan usage, judged from its output. Only meaningful when the
    // layer is installed; the warning at the top of main says so when it is not.
    if (wantValidation && (!daemonOutputRead || !daemonCameUp)) {
        std::printf("daemon_system_test: validation check skipped -- %s, and the failure that caused that is "
                    "reported above.\n",
                    daemonCameUp ? "the run did not reach the point where the daemon's output is collected"
                                 : "the daemon never started");
    } else if (wantValidation) {
        // Absence of warnings is only evidence when the daemon actually ran and rendered.
        // On an empty or truncated capture -- the daemon died at startup, or the pipe was
        // lost -- validationLines() trivially returns nothing and this would report the
        // GPU-side oracle as clean for a run in which no Vulkan work was ever observed.
        // Anchored on the daemon's own startup line, which it prints before any render.
        // Built from kBusName so that renaming the bus breaks the build rather than
        // quietly turning this anchor into a needle that never matches.
        const bool daemonSpoke =
            daemonOutput.find(std::string("serving ") + DbusService::kBusName) != std::string::npos;
        if (!daemonSpoke) {
            // One failure, not two. The daemon never got far enough to use Vulkan, which
            // is already reported above; grading its silence as well would be a second
            // FAIL line for a cause the reader has already been told about.
            check(false, "the daemon's output was captured, so its silence about validation means something");
        } else {
            const std::vector<std::string> warnings = validationLines(daemonOutput);
            check(warnings.empty(),
                  "the daemon produced no Vulkan validation warnings across render, export and teardown");
            for (const std::string& line : warnings) {
                std::fprintf(stderr, "  %s\n", line.c_str());
            }
        }
    }

    if (failures > 0) {
        // Asked before the not-verified count, so a run that is both broken and incomplete
        // reports as broken: a skip would hide the failures behind ctest's "Skipped".
        std::fprintf(stderr, "daemon_system_test: %d check(s) failed\n", failures);
        return 1;
    }
    if (g_notVerified > 0) {
        std::printf("daemon_system_test: every check that ran passed, but %d goal(s) could not be exercised on "
                    "this host (listed above) -- reported to ctest as a skip, not a pass.\n",
                    g_notVerified);
        return kNotVerifiedExitCode;
    }
    std::printf("daemon_system_test: all checks passed\n");
    return 0;
}
