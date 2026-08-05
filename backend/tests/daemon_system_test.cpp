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
#include "tests/client/daemon_client.h"

#include "support/exported_frame_map.h"

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
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
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
using wallpaperx::testing::openFdCount;
using wallpaperx::testing::setWallpaper;
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
// Inside the daemon's 0..31 monitor range, and in the gap the phases above leave between
// the responsiveness burst (2..17) and the storm (20..23).
constexpr int32_t kMonitorClosedStdout = 24;

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

std::string testAssetDir() { return std::string(WALLPAPERX_TEST_IMAGE_DIR); }

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
            execve(binary.c_str(), argv, envp.data());
            _exit(127);  // only reached if execve failed
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
    // makes to fd 1 fails. Returns false if there was nothing to close, which a caller
    // must treat as a setup failure rather than as a result.
    bool breakStdout() {
        if (m_stdoutFd < 0) return false;
        close(m_stdoutFd);
        m_stdoutFd = -1;
        return true;
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

        m_pid = fork();
        if (m_pid < 0) {
            close(fds[0]);
            close(fds[1]);
            m_reason = "fork failed";
            return;
        }
        if (m_pid == 0) {
            if (dup2(fds[1], STDOUT_FILENO) == -1) _exit(126);
            execv(binary.c_str(), argv);
            _exit(127);
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
        close(fds[0]);

        const size_t newline = line.find('\n');
        if (newline != std::string::npos) m_address = line.substr(0, newline);
        if (m_address.empty() && m_reason.empty()) m_reason = "dbus-daemon printed an empty address";
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
        if (m_pid <= 0) return;
        ::kill(m_pid, SIGKILL);
        int status = 0;
        waitpid(m_pid, &status, 0);
        m_pid = -1;
    }

private:
    pid_t m_pid = -1;
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
        try {
            const nlohmann::json status = getStatus(*proxy);
            ++watch.replies;
            if (!announced) {
                announced = true;
                ++watch.ready;
            }
            const bool initialized = status.contains("gpu") && status["gpu"].is_object() &&
                                     status["gpu"].value("initialized", false);
            if (initialized) {
                ++watch.postInit;
            } else {
                ++watch.preInit;
            }
            if (!wallpaperx::testing::statusSelfConsistent(status)) ++watch.inconsistent;
        } catch (const std::exception&) {
            // A failed GetStatus is not a consistency violation. Counted, and reported
            // below only if it swallowed the whole sample. Deliberately does NOT mark
            // this watcher ready: readiness means "answered once", so that the pre-init
            // sample the phase depends on is guaranteed rather than raced for. A watcher
            // that only ever errors therefore never becomes ready, and the barrier below
            // falls through on its deadline rather than hanging -- the phase then reports
            // the zero readiness directly instead of blaming the timing.
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
    // jthread, not thread: getStatus() below is a blocking DBus call that throws whenever
    // the daemon dies or times out mid-burst -- which is exactly what a daemon answering
    // GetFrameFd on the event loop does. Unwinding past a still-joinable std::thread is
    // std::terminate, so the one regression this phase exists to catch would have
    // produced a bare SIGABRT with no FAIL lines instead of reaching main's handler. The
    // explicit join() below stays: it is the happens-before edge that makes the plain
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

    const nlohmann::json status = getStatus(proxy);
    const auto statusDoneAt = Clock::now();
    check(!status.is_discarded(), "GetStatus still returns parseable JSON while renders are queued");

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
                    const nlohmann::json status = getStatus(*proxy);
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
        const nlohmann::json status = getStatus(proxy);
        check(!status.is_discarded(), "the daemon still answers GetStatus after the storm");
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

    const nlohmann::json after = getStatus(proxy);
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

// One thread's signal masks, read from /proc. Every field is a hex bitmask in which
// signal N is bit N-1.
struct SignalMasks {
    bool read = false;
    uint64_t blocked = 0;
    uint64_t ignored = 0;
};

constexpr uint64_t signalBit(int signo) { return 1ull << (signo - 1); }

SignalMasks readSignalMasks(pid_t pid) {
    SignalMasks masks;
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    if (!status) return masks;
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
            masks.blocked = value;
            sawBlocked = true;
        } else {
            masks.ignored = value;
            sawIgnored = true;
        }
    }
    masks.read = sawBlocked && sawIgnored;
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
    check(output.find(std::string(strsignal(SIGHUP)) + " received, shutting down") != std::string::npos,
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
//       (2) The disposition half, read from /proc/<pid>/status: the behavioural half
//       alone cannot tell ignored from blocked -- both survive the write -- and the goal
//       explicitly rejects blocking. SigBlk/SigIgn is the only externally visible
//       difference between the two.
//       (3) The coverage assert below, because everything here rests on the daemon
//       actually writing to stdout while serving that call. If that printf ever goes
//       away, nothing generates EPIPE and all of the above passes on a daemon with the
//       bug reintroduced -- so the write is asserted, from the captured run, instead of
//       assumed.
// CONTROL: two, and the second is the one that matters.
//       (a) `std::signal(SIGPIPE, SIG_IGN)` deleted from main.cpp -> 5 FAILs: SetWallpaper
//       came back [org.freedesktop.DBus.Error.NoReply], GetStatus came back
//       ServiceUnknown, "the daemon ignores SIGPIPE", and the exit checks with
//       "terminated by signal 13".
//       (b) SIGPIPE *blocked* (pthread_sigmask) instead of ignored -> exactly 2 FAILs,
//       "the daemon ignores SIGPIPE" and "SIGPIPE is ignored rather than merely blocked"
//       (SigBlk=0x5003 SigIgn=0x0), while every behavioural check above stayed green --
//       which is precisely the configuration S6.37 says must fail and which part (1)
//       alone cannot see. Both reverted.
// COVERAGE-ASSERT: asserts that the captured daemon printed its SetWallpaper line, i.e.
//       that this phase's trigger still exists in the shipped binary. Controlled from the
//       harness side by searching for a string the daemon never prints ("type=zzz"):
//       "FAIL: the daemon writes to stdout while handling SetWallpaper", alone, with
//       every other check green. Reverted.
void checkClosedStdout(NameWatcher& names, const std::string& bluePath, bool capturedRunUsable,
                       const std::string& capturedRunOutput, const Check& check) {
    if (!capturedRunUsable) {
        std::printf("daemon_system_test: closed-stdout checks skipped -- the earlier daemon's output was never "
                    "collected, so there is no evidence that SetWallpaper writes to stdout at all, and without "
                    "that this phase would prove nothing.\n");
        return;
    }
    check(capturedRunOutput.find("SetWallpaper(type=static") != std::string::npos,
          "the daemon writes to stdout while handling SetWallpaper (this is what makes the closed-stdout "
          "phase below trigger anything at all)");

    SpawnedDaemon deaf({}, SpawnedDaemon::Stdout::Breakable);
    const bool cameUp = names.waitFor(true);
    check(cameUp, "a fresh daemon acquires the name before the closed-stdout phase");
    if (!cameUp) {
        std::printf("daemon_system_test: closed-stdout checks skipped -- that daemon never acquired the bus "
                    "name, so nothing it does afterwards is attributable to its stdout.\n");
        return;
    }

    // From here the daemon's stdout is a pipe with no reader and no other holder of the
    // write end: exactly `wallpaperx-daemon | head` once head has exited.
    const bool closed = deaf.breakStdout();
    check(closed, "the test could close the read end of the daemon's stdout");
    if (!closed) return;

    FrameReadyLog signals;
    auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                    sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

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

    if (answered) {
        check(signals.wait(kMonitorClosedStdout, 1),
              "the frame requested after stdout closed is rendered and announced");
    }

    bool statusAnswered = false;
    try {
        const nlohmann::json status = getStatus(*proxy);
        statusAnswered = !status.is_discarded();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  GetStatus after the stdout reader was closed: %s\n", e.what());
    }
    check(statusAnswered, "the daemon still answers GetStatus after the failed write");

    const SignalMasks masks = readSignalMasks(deaf.pid());
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
// CONTROL: main.cpp's ShutdownWatch guard replaced by a bare `std::thread shutdownWatch`
//       with the pthread_kill/join moved to after enterEventLoop() -- i.e. the code as it
//       was before f1aa32c. Observed directly: "terminate called without an active
//       exception", "Aborted (core dumped)", wait status 134, and none of the shutdown
//       output. Against this check that is "FAIL: losing the bus connection unwinds
//       through main() instead of aborting the process" plus the exit-1, fatal-message
//       and watcher-joined checks. Reverted.
void checkBusLoss(const std::string& daemonEnvValidation, const Check& check) {
    NestedBus bus;
    if (!bus.ok()) {
        // X.3: a bus this test could not start is a fault in the harness or the host, and
        // must never be reported as the daemon aborting.
        std::printf("daemon_system_test: SKIPPED the connection-loss checks (S6.35) -- %s. Nothing about the "
                    "daemon's behaviour when it loses the bus was verified by this run.\n",
                    bus.reason().c_str());
        return;
    }

    SpawnedDaemon orphan({std::string("DBUS_SESSION_BUS_ADDRESS=") + bus.address(), daemonEnvValidation});
    // The daemon's own startup line is the up-signal here rather than NameOwnerChanged:
    // the name is acquired on the nested bus, which this process is not connected to, and
    // connecting to it would put another client on the very connection being destroyed.
    if (!orphan.waitForOutput("serving io.wallpaperx.Daemon", std::chrono::seconds(30))) {
        std::printf("daemon_system_test: SKIPPED the connection-loss checks (S6.35) -- the daemon never came up "
                    "on the nested bus, so killing it would say nothing about the unwind path.\n");
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
    check(output.find("fatal:") != std::string::npos,
          "the daemon says why it exited after losing the bus rather than dying silently");
    // The guard doing its job, not merely the absence of a crash: on the way out it wakes
    // the sigwait watcher and joins it, and that watcher prints before it returns. A bare
    // joinable thread reaches std::terminate instead and this line never appears.
    check(output.find("received, shutting down") != std::string::npos,
          "the sigwait watcher is woken and joined while the connection-loss exception unwinds");
}

}  // namespace

int main() {
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
            check(daemonOutput.find("received, shutting down") != std::string::npos,
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
        checkClosedStdout(names, bluePath, daemonOutputRead && daemonCameUp, daemonOutput, check);
        checkBusLoss(validationEnv, check);

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
        const bool daemonSpoke = daemonOutput.find("serving io.wallpaperx.Daemon") != std::string::npos;
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

    if (failures == 0) {
        std::printf("daemon_system_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "daemon_system_test: %d check(s) failed\n", failures);
    return 1;
}
