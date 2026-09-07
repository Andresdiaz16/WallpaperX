// SPDX-License-Identifier: GPL-2.0-or-later

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
// For defaultConfigPath() only, and only so the isolation guard in main() asks the same
// question the daemon does: where would a daemon inheriting THIS process's environment
// write? A path spelled out here would stop matching the daemon's the moment the rule
// changed, which is the run where the guard has to work.
#include "daemon/config_store.h"
// For sanitizeForLog only: the restart phase asserts that GetStatus publishes the
// display-safe form of a path while the config file holds the raw bytes, and the expected
// display form has to come from the daemon's own function rather than be spelled out here.
#include "daemon/log_escape.h"
#include "tests/client/blocking_config_write.h"
#include "tests/client/daemon_client.h"
#include "tests/client/http_probe.h"
#include "tests/client/scratch_dir.h"
#include "tests/spawned_daemon.h"
#include "tests/test_report.h"

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
#include <array>
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
using wallpaperx::testing::ScratchDir;
using wallpaperx::testing::SpawnedDaemon;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::StatusReply;
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
constexpr int32_t kMonitorLoadBase = 2;    // .. kMonitorLoadBase + loadCount - 1, clamped by kMaxLoadMonitors
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
// job while reporting that it lost its config port.
constexpr int32_t kMonitorLostPort = 27;
// 18, 19, 28 and 29 belong to the phases that each drive a daemon and an XDG_CONFIG_HOME of
// their own -- the Step 8 config phases and the Step 10 connector-name phase -- and are
// declared where they are used. That ownership is what makes reuse legal at all: such a
// phase's ids are only ever compared with its own, so several of them share 18 and 19. No
// tally of who currently holds which is kept here, because a tally is what goes stale; grep
// the kMonitor* declarations. 30 and 31 are the barrier phase's, above. A new phase needs
// an id retired by an old one, or a daemon of its own; ids are never derived by arithmetic
// over a case list, since the block of ids consumed then grows silently past 31.

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

// How much dearer a SetWallpaper that will render may be to issue than one that will not,
// before S6.9 calls it rendering inline. The property says "no more", i.e. 1.0; the slack
// is for measurement, not for the daemon.
//
// Sized from the measured spread, not chosen for comfort. Unmutated the ratio stays within
// 0.99-1.03 over twenty runs across NVIDIA, lavapipe and tsan+lavapipe alike; a daemon
// whose handler waits for the render reads 11.96, 19.71 and 5.33 on those same three. Two
// clears the worst benign reading by ~1.9x and sits ~2.7x under the narrowest defective
// one.
constexpr double kIssueCostRatioBound = 2.0;

using wallpaperx::testing::kProcessTimeout;

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
// build the precondition -- as opposed to goals it exercised and that held. The channel
// itself is tests/test_report.h; what matters here is why this file uses it.
//
// The two were previously indistinguishable from outside: the exit status was a pure
// function of the failure count, so a phase that returned early after printing a reason
// left ctest reporting PASS for coverage that never ran. That is the vacuous-green trap,
// and it sat on the phase written for the worst crash the QA pass found. Reported the way
// the renderer probes report it: exit 77, which ctest turns into a SKIP through the
// SKIP_RETURN_CODE property set in backend/CMakeLists.txt. A skip is not a pass, and no
// reader can mistake it for one.
//
// Not a check() failure, deliberately: X.3 forbids reporting a fault of the host or the
// harness as a fault of the daemon, and every caller here names a cause on this side of
// the boundary.
using wallpaperx::testing::notVerified;

// This file failed to start a child process, so whatever the phase was going to grade was
// never exercised. Its own channel, separate from both check() and notVerified(): a fork
// preamble that died is not the daemon misbehaving (X.3 forbids grading it as one), and it
// is not this host declining to run a case either -- it is the harness itself, and the run
// must go red naming it rather than skip.
//
// Both lines on stdout, for the reason device_affinity_probe's harnessFault() carries: split
// across stdout and stderr they need an fflush between them to stay adjacent under ctest.
//
// Named apart from tests/test_report.h's g_harnessFaults / harnessFailure() rather than
// shadowing them: the two counters have different epilogues, and a site here that reached the
// shared name would file a fault into a count this file never reads.
int g_phaseSetupFaults = 0;

void phaseSetupFault(const char* what, const char* detail) {
    std::printf("daemon_system_test: HARNESS FAULT -- %s could not be set up: %s. This says nothing about the "
                "daemon.\n",
                what, detail);
    std::fflush(stdout);
    ++g_phaseSetupFaults;
}

using wallpaperx::testing::Clock;

double millis(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(d).count();
}

// The cheapest call in a burst: the one that met the least contention and the least
// preemption, and so the closest thing to the handler's own cost. Corroborated by the
// median beside it, which is what the mean or the total cannot be -- a burst is a few
// hundred microseconds per call, and one preemption anywhere in it moves a total by more
// than the effect being measured.
double minMillis(const std::vector<Clock::duration>& samples) {
    if (samples.empty()) return 0.0;
    return millis(*std::min_element(samples.begin(), samples.end()));
}

// Takes its argument by value because it sorts.
double medianMillis(std::vector<Clock::duration> samples) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t n = samples.size();
    if (n % 2 == 1) return millis(samples[n / 2]);
    return (millis(samples[n / 2 - 1]) + millis(samples[n / 2])) / 2.0;
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


// The prefix every scratch directory this run creates shares, so the isolation guard can
// tell a config file written by one of this run's daemons from one written by a process it
// does not own. Must stay in step with ScratchDir's naming in tests/client/scratch_dir.h.
//
// Empty when this host has no temporary directory, through temp_directory_path()'s
// error_code overload rather than the throwing one: this is called from the isolation guard,
// which runs outside main()'s try on purpose, so a throw here dies by std::terminate and
// takes every diagnostic the run was about to print with it. The one caller must treat empty
// as "unavailable" and never as a needle -- find("") matches at position 0 of every string.
std::string harnessTempPrefix() {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    return (base / ("wpx_test_" + std::to_string(::getpid()) + "_")).string();
}

// Empty when this host can give the suite a scratch directory; otherwise why it cannot.
//
// Probed once, up front, because every daemon this file spawns needs a scratch directory for
// its XDG_CONFIG_HOME: without one SpawnedDaemon's constructor throws, main()'s catch-all
// prints "FAIL: unexpected exception", and a machine with no /tmp is recorded as a defect of
// the daemon. Asked as a positive probe of the host rather than by classifying an exception,
// so nothing the daemon does can reach this answer.
std::string scratchDirectoryFault() {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        // The value is quoted, and empty and unset are worded apart: an empty $TMPDIR is a
        // real way to hit this and prints as nothing at all otherwise.
        const char* tmpdir = std::getenv("TMPDIR");
        std::string where = "$TMPDIR is unset, so /tmp was tried";
        if (tmpdir != nullptr) {
            where = *tmpdir == '\0' ? "$TMPDIR is set but empty" : "$TMPDIR is '" + std::string(tmpdir) + "'";
        }
        return "std::filesystem::temp_directory_path() failed (" + ec.message() + "); " + where;
    }
    const ScratchDir probe("hostprobe");
    if (!probe.ok()) return "a directory could not be created under " + base.string();
    return {};
}

// The developer's own config file, as this process's environment names it, and whatever was
// in it when the run started. Used by the isolation guard in main().
struct RealConfigWitness {
    std::filesystem::path file;
    bool existed = false;
    std::string contents;
};

RealConfigWitness witnessRealConfig() {
    RealConfigWitness witness;
    witness.file = wallpaperx::defaultConfigPath();
    if (witness.file.empty()) return witness;
    std::error_code ec;
    witness.existed = std::filesystem::exists(witness.file, ec) && !ec;
    if (!witness.existed) return witness;
    std::ifstream in(witness.file, std::ios::binary);
    if (in) witness.contents.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return witness;
}

// True when this daemon's child died in its fork preamble, having reported the fault on the
// harness channel first -- so a caller only has to bail out. `phase` names what could not be
// set up.
//
// Every site below that grades a daemon's startup goes through this one function rather than
// testing exit codes itself: a site that handled one preamble code and not the others would
// read the unhandled ones as a verdict on the daemon, which is how 126 was left behind in the
// renderer tier.
bool reportedStartupFault(const SpawnedDaemon& daemon, const char* phase) {
    const char* fault = daemon.startupFault();
    if (fault == nullptr) return false;
    phaseSetupFault(phase, fault);
    return true;
}

// The soft RLIMIT_CORE a running process has, as /proc spells it ("0", "unlimited"), or
// empty when the file could not be read.
std::string softCoreLimit(pid_t pid) {
    std::ifstream limits("/proc/" + std::to_string(static_cast<int>(pid)) + "/limits");
    constexpr const char* kRow = "Max core file size";
    std::string line;
    while (std::getline(limits, line)) {
        if (line.rfind(kRow, 0) != 0) continue;
        std::istringstream rest(line.substr(std::strlen(kRow)));
        std::string soft;
        rest >> soft;
        return soft;
    }
    return "";
}

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
            if (dup2(fds[1], STDOUT_FILENO) == -1) _exit(wallpaperx::testing::kChildDup2FailedExit);
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

        // GOAL: X.3 -- a bus child that died in its fork preamble is this harness failing,
        //       and must not be reported as dbus-daemon misbehaving.
        // WHY-THIS-SHAPE: the reasons recorded above are all written as statements about
        //       dbus-daemon ("exited before printing an address", "printed no address within
        //       10s"), and an arming refusal produces exactly the first of them -- measured:
        //       with armAgainstParentDeath forced false this phase skipped with "dbus-daemon
        //       exited before printing an address", accusing the host's bus binary of
        //       something this file did. Consulted only when no address arrived, so a bus
        //       that started normally never pays for it.
        // CONTROL: the same forced-false arming -> "HARNESS FAULT -- the connection-loss
        //       checks (S6.35) could not be set up: the forked child could not arm itself
        //       against its parent's death", and no NOT VERIFIED line naming dbus-daemon.
        //       Reverted.
        if (m_address.empty() && m_pid > 0) {
            siginfo_t info{};
            info.si_pid = 0;
            if (waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                info.si_pid != 0 && info.si_code == CLD_EXITED) {
                m_preambleFault = wallpaperx::testing::forkPreambleFault(info.si_status);
            }
        }

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
    // Non-null when the bus never started because its child died in the fork preamble --
    // a fault of this harness rather than of dbus-daemon. See reason() for everything else.
    const char* preambleFault() const { return m_preambleFault; }
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
    // A string literal from forkPreambleFault(), so no ownership to manage.
    const char* m_preambleFault = nullptr;
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

    // How many NameOwnerChanged events for this name have arrived. The only evidence OUTSIDE
    // the daemon's own log that the name was ever claimed, which is what a phase grading that
    // log needs -- it cannot use the log as its own witness. A counter rather than a cached
    // ownership bool, for the reason waitFor's comment below gives.
    uint64_t events() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_events;
    }

    // Waits, to a deadline, for that counter to move off `from`. False means it did not, which
    // for a daemon that has already exited means the name was never taken and never released.
    bool waitForEventsPast(uint64_t from, std::chrono::milliseconds patience) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, patience, [&] { return m_events != from; });
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
    mutable std::mutex m_mutex;
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
//   - The cost of *issuing* a request that will render, against the cost of issuing one
//     that will not. See the S6.9 block below for why the comparison is against a `video`
//     burst rather than against the render drain.
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
// violation. The weaker half -- a handler doing slow *non-GPU* work -- has no mechanical
// enforcement and none of these checks bounds it either: the issue-cost comparison below is
// a ratio of two runs of the same handler, so any cost both pay cancels out. What it does
// establish is that the handler neither performs nor waits for the render before replying.
// ---------------------------------------------------------------------------- //

// Per-call timing rather than one interval around a loop, because the statistic taken from
// the samples has to be a median.
Clock::duration timeSetWallpaper(sdbus::IProxy& proxy, const char* type, const std::string& path, int32_t monitorId) {
    const auto callStart = Clock::now();
    setWallpaper(proxy, type, path, monitorId);
    return Clock::now() - callStart;
}

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

    // Enough queued work that a GPU-blocking handler could not hide, and the sample count
    // for both bursts below. Distinct monitors, so nothing coalesces away: coalescing is
    // what makes a burst at *one* monitor finish in one render, and that would leave
    // nothing to be busy with.
    const double perRenderMs = std::max(1.0, millis(singleRender));
    const int loadCount = std::clamp(static_cast<int>(400.0 / perRenderMs) + 1, 4, kMaxLoadMonitors);

    // The second client is built before the clock starts. Creating a connection costs
    // milliseconds, and those would land inside the interval being measured.
    auto observer =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

    // Unmeasured, and it has to come first: it puts every monitor the measured calls will
    // touch into the daemon's map, so the config save each of those calls performs writes a
    // file of constant size. Without it the file would grow entry by entry underneath the
    // measurement and part of the difference being measured would be that.
    for (int i = 0; i < loadCount; ++i) {
        setWallpaper(proxy, "video", bluePath, kMonitorLoadBase + i);
    }

    // GOAL: the premise the S6.9 assertion below rests on -- that a `video` request is
    //       recorded and then NOT rendered. If it ever renders, the two bursts stop
    //       differing and the assertion measures nothing while staying green.
    // WHY-THIS-SHAPE: asked of the daemon over the wire rather than asserted in the test,
    //       because the test cannot see isRenderableType(). The alternative -- waiting for
    //       the absence of a FrameReady -- cannot distinguish "never renders" from "has not
    //       rendered yet", and would have to spend a timeout to say so. `unsupported` is
    //       the daemon's own published answer and shares isRenderableType() with the
    //       render path, so the two cannot disagree.
    // CONTROL: isRenderableType() changed to `type == "static" || type == "video"` ->
    //       "FAIL: the video burst is recorded as a wallpaper this build does not render",
    //       with "0/16 monitors report unsupported" beside it. The ratio on that run read
    //       1.00 -- two rendering bursts compared against each other, the vacuous green
    //       this converts into a red. Reverted.
    const StatusReply videoStatus = getStatusOrExplain(proxy);
    int unsupportedSeen = 0;
    for (int i = 0; i < loadCount; ++i) {
        const nlohmann::json* entry = wallpaperStatus(videoStatus, std::to_string(kMonitorLoadBase + i));
        if (entry != nullptr && entry->value("unsupported", false)) ++unsupportedSeen;
    }
    const bool videoIsInert = unsupportedSeen == loadCount;
    if (!videoIsInert) {
        // Says which of the two ways this failed: the daemon renders video now, or the
        // reply that would have said so never arrived (getStatusOrExplain has printed the
        // reason for that case immediately above).
        std::fprintf(stderr, "  %d/%d monitors report unsupported%s\n", unsupportedSeen, loadCount,
                     videoStatus.ok() ? "" : " -- and GetStatus did not answer, see above");
    }
    check(videoIsInert, "the video burst is recorded as a wallpaper this build does not render "
                        "(the S6.9 comparison below is meaningless otherwise)");

    // The control burst. Same monitors as the renderable burst below, so the config save
    // each call performs writes an identically sized file.
    //
    // A separate burst rather than interleaved with it. Alternating the two types pair by
    // pair sounds fairer -- both calls would meet the same worker -- and measured worse: it
    // puts the issue loop and the worker loop into a fixed phase relationship, and one call
    // of each pair comes out ~3x the other. Which one is a coin flip per run (a tsan run
    // read 3.47 and the next 0.31, the second with the INERT call dearer), so the number
    // stops being about the wallpaper type at all. X.2 forbids asserting on that.
    std::vector<Clock::duration> videoSamples;
    videoSamples.reserve(static_cast<size_t>(loadCount));
    for (int i = 0; i < loadCount; ++i) {
        videoSamples.push_back(timeSetWallpaper(proxy, "video", bluePath, kMonitorLoadBase + i));
    }

    std::vector<Clock::duration> staticSamples;
    staticSamples.reserve(static_cast<size_t>(loadCount));
    const auto loadStart = Clock::now();
    for (int i = 0; i < loadCount; ++i) {
        staticSamples.push_back(timeSetWallpaper(proxy, "static", bluePath, kMonitorLoadBase + i));
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

    const double videoFloor = minMillis(videoSamples);
    const double staticFloor = minMillis(staticSamples);
    const double ratio = videoFloor > 0 ? staticFloor / videoFloor : 0.0;
    // The sample count is printed rather than asserted: both bursts are the same
    // unconditional loop over one clamped counter, so no state of the daemon, the host or
    // the GPU can make them differ -- an assertion on it could only be reddened by editing
    // the clamp three lines away, which is a check that cannot fail dressed as a guard.
    std::printf("daemon_system_test: cheapest SetWallpaper cost %.3f ms for a request that renders vs %.3f ms for one "
                "that does not (ratio %.2f, bound %.1f) over %zu and %zu samples; medians %.3f and %.3f\n",
                staticFloor, videoFloor, ratio, kIssueCostRatioBound, staticSamples.size(), videoSamples.size(),
                medianMillis(staticSamples), medianMillis(videoSamples));

    // The comparison means something only if exactly one of the two bursts rendered. Not a
    // silent pass either way: both premises are asserted above, so a run that lands here is
    // already red and the reason is already printed against the right component.
    if (!round1Ok || !videoIsInert) {
        std::printf("daemon_system_test: the S6.9 issue-cost comparison is abandoned -- %s, so the two bursts do "
                    "not differ in the way the comparison assumes.\n",
                    round1Ok ? "the video burst was not inert" : "the static burst never drained");
        return;
    }

    // GOAL: S6.9 -- issuing a burst of SetWallpaper calls that WILL render costs no more
    //       than issuing an identical burst that renders nothing. False for a daemon whose
    //       handler renders (or waits for the render) before replying.
    // WHY-THIS-SHAPE: the obvious form -- issue time as a fraction of the render drain --
    //       is what this used to be, and its denominator is not daemon code. Under
    //       ThreadSanitizer the drain is mostly uninstrumented rasterisation inside the ICD
    //       while the handler is instrumented daemon code, so the two sides scale by
    //       different factors: the tsan tier read 0.2932 then 0.9375 against a 0.25 bound
    //       on a daemon that is not defective, and 0.9375 is what the defect itself scores.
    //       The `video` burst is the same handler, the same validation, the same requestSeq
    //       bookkeeping and the same config save, differing only in isRenderableType() --
    //       so sanitizer distortion and host GPU speed appear on both sides and cancel.
    //       The cheapest call of each burst, not the median or the total: the renderable
    //       burst -- and only it -- is issued while the worker is drawing, so its calls
    //       carry lock and stdout contention the inert ones never meet, and a sanitizer
    //       charges roughly ten times as much for it. That put the tsan median ratio at
    //       1.11-1.58 and the totals at 2.19 with nothing wrong. Comparing the least
    //       contended call on each side removes the confound instead of budgeting for it:
    //       1.01-1.03 over the same runs. It cannot let the defect through, because a
    //       handler that renders inline has no cheap call to offer -- every one of the
    //       sixteen monitors is distinct, so every request is a fresh render.
    // CONTROL: DbusService::setWallpaper made synchronous -- after m_worker.post(), post a
    //       task carrying a promise and wait on its future, which is the FIFO's completion
    //       edge for the render just queued. Rendering on the worker keeps the Device's
    //       thread pinning from aborting first, so the daemon survives to be measured;
    //       emitFrameReady had to be detached for the same run, because a worker blocking
    //       on the connection lock while the handler waits on the worker is a deadlock, not
    //       a slow reply. Observed: NVIDIA 1.725 ms vs 0.144 ms, ratio 11.96; lavapipe
    //       2.983 vs 0.151, ratio 19.71; tsan+lavapipe 4.143 vs 0.778, ratio 5.33 -- FAIL
    //       in all three. Reverted.
    //
    // What this deliberately does NOT cover: the handler's absolute cost. The config save
    // runs on both sides, so it cancels -- ConfigStore::save given a 5 ms sleep, some 500x
    // its real cost, took both sides from 0.14 ms to 5.25 ms and left the ratio at 1.00 and
    // green. Nothing in this suite bounds the absolute cost of a DBus handler: the in-process
    // S8.10 phase establishes that the save is not the durable variant, which is a different
    // question, and a second bound here would reintroduce exactly the host-dependent absolute
    // number this shape removed. That gap is real and is stated here rather than implied.
    //
    // A validity gate, not part of the assertion: a cheapest call measured at 0 ms means this
    // host's clock cannot resolve a round trip, and a ratio computed from it grades nothing.
    // Folded into the check -- which is what `videoFloor > 0 &&` used to do -- it printed
    // "ratio 1.00" beside a FAIL accusing the daemon of rendering inline. The gate cannot
    // hide the defect it is next to: a handler that rendered inline has no zero-cost call to
    // offer, since every one of these monitors is a distinct, fresh render.
    if (videoFloor <= 0.0 || staticFloor <= 0.0) {
        notVerified("S6.9 (SetWallpaper does not render inline)",
                    "the cheapest call in a burst measured 0 ms, so this host's clock cannot resolve a SetWallpaper "
                    "round trip and no ratio from this run means anything");
        return;
    }
    check(ratio < kIssueCostRatioBound,
          "issuing a burst that will render costs no more than issuing one that renders nothing "
          "(SetWallpaper does not render inline)");
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
// every reply is either well-formed or one of the three named errors that are legitimate
// mid-flight, and `generation` never goes backwards for a monitor -- which is the
// observable consequence of both paths being correct. The third of those errors -- the
// producer refusing to hand over its last free image -- is counted per monitor rather than
// merely tolerated, so admitting it here does not turn the phase green for a daemon that
// refuses everything.
// ---------------------------------------------------------------------------- //
struct StormResult {
    std::atomic<int> unexpectedErrors{0};
    std::atomic<int> generationRegressions{0};
    std::atomic<int> generationComparisons{0};
    std::atomic<int> calls{0};
    std::mutex mutex;
    std::string firstUnexpected;

    // Per storm monitor: how many hand-overs were served, and how many were refused because
    // granting one would have left the producer nowhere to draw. Read only after every thread
    // has joined -- comparing them mid-storm is a race, since the thread that was served and
    // the thread that was refused record in whichever order they are scheduled.
    std::array<std::atomic<int>, kStormMonitors> served{};
    std::array<std::atomic<int>, kStormMonitors> refusedForReserve{};

    // Every reading of GetStatus's deferred_renders that was above zero, and how many readings
    // were taken. renderPending()'s deferral path is a backstop the producer's reserve is meant
    // to keep unreachable, so any non-zero reading is a report that a hand-over got past it.
    std::atomic<int> deferralReadings{0};
    std::atomic<int> deferralsSeen{0};
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
                    //       clients see only the three named errors that are legitimate
                    //       mid-render", with "29 unexpected failure(s); first was:
                    //       GetStatus failed with org.sdbuscpp.Error". Reverted.
                    if (!status.ok()) recordUnexpected(result, status.error());
                    // Called for the generation observation below. Snapshot consistency
                    // is deliberately NOT graded from these replies -- see the note at
                    // the end of this function.
                    const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
                    if (entry != nullptr) {
                        observeGeneration(result, marks, monitorId, entry->value("generation", uint64_t{0}));
                        // Read on every pass rather than once at the end, because the count is
                        // cleared by the next commit: a deferral that happened mid-storm would
                        // be gone by the time a single reading after the storm was taken.
                        if (entry->contains("deferred_renders") && (*entry)["deferred_renders"].is_number_unsigned()) {
                            ++result.deferralReadings;
                            if (entry->value("deferred_renders", uint32_t{0}) > 0) ++result.deferralsSeen;
                        }
                    }
                    break;
                }
                default: {
                    const Frame frame = getFrameFd(*proxy, monitorId);
                    if (fcntl(frame.fd.get(), F_GETFD) == -1) {
                        recordUnexpected(result, "GetFrameFd returned a closed fd");
                    }
                    ++result.served.at(static_cast<size_t>(monitorId - kMonitorStormBase));
                    observeGeneration(result, marks, monitorId, frame.generation);
                    break;
                }
            }
            ++result.calls;
        } catch (const sdbus::Error& e) {
            // Three legitimate answers mid-storm: a monitor whose render has not committed
            // yet has no frame; a monitor whose most recent request was superseded before
            // it rendered reports the previous failure; and a monitor whose other images
            // are all held refuses the hand-over that would take the producer's last one.
            // Anything else -- a timeout, a disconnect, an unhandled exception escaping a
            // handler -- is a defect.
            //
            // The third is counted per monitor rather than merely tolerated, because
            // admitting a name to this list is otherwise a pure weakening: a daemon that
            // refused EVERY hand-over would leave this phase green. What that count is
            // asserted against is at the end of checkConcurrentClients().
            const std::string name = e.getName();
            if (name == "io.wallpaperx.Daemon.Error.NoFreeImage") {
                ++result.refusedForReserve.at(static_cast<size_t>(monitorId - kMonitorStormBase));
            } else if (name != "io.wallpaperx.Daemon.Error.NoFrame" &&
                       name != "io.wallpaperx.Daemon.Error.RenderFailed") {
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

    check(result.unexpectedErrors == 0, "concurrent clients see only the three named errors that are legitimate "
                                        "mid-render");
    if (result.unexpectedErrors != 0) {
        std::lock_guard<std::mutex> lock(result.mutex);
        std::fprintf(stderr, "  %d unexpected failure(s); first was: %s\n", result.unexpectedErrors.load(),
                     result.firstUnexpected.c_str());
    }

    // GOAL: the producer's reserve refuses a hand-over only when it has images out on loan. A
    //       monitor that answered "no free image" must have handed at least one image out
    //       during this storm -- a daemon that refused unconditionally, or one whose admission
    //       test disagreed with its own bookkeeping, would refuse a monitor it had never served.
    // WHY-THIS-SHAPE: evaluated after every thread has joined, and per monitor, because that is
    //       the only form of this that is not an assertion on the scheduler. Comparing the two
    //       counters mid-storm is a race -- the thread that was served and the thread that was
    //       refused record in whichever order they run -- and asserting that any particular
    //       number of refusals happened would be asserting on the interleaving too, since how
    //       many clients hold how many images at any instant is a scheduling outcome. This is
    //       also what stops NoFreeImage's admission to the legitimate-error list above from
    //       being a pure weakening.
    // CONTROL: a refusal for every named caller on this phase's monitors alone, inserted ahead
    //       of the admission block in WallpaperManager::exportCurrentFrame (backend/daemon/
    //       wallpaper_manager.cpp) as `if (!client.empty() && monitorId >= 20) { out.reason =
    //       NoFreeImage; return out; }` -> "FAIL: a monitor was only refused for want of a free
    //       image after it had handed one out", printing "4 monitor(s) refused without ever
    //       serving -- monitor 20: 0 served, 1 refused; ...". Scoped to monitor ids 20 and up
    //       on purpose: refusing every caller everywhere throws out of an earlier phase, which
    //       ends the run before this one is reached and would leave the check unattested. It
    //       also reddens the monotonicity coverage assertion below, because a refused hand-over
    //       records no generation. Reverted.
    // COVERAGE-ASSERT: how many refusals this run actually saw is printed beside the verdict,
    //       so a future change that stopped producing any -- which would make the check above
    //       vacuously true -- is visible in the log rather than silent.
    {
        int refusalsTotal = 0;
        int monitorsRefusedWithoutServing = 0;
        std::string detail;
        for (int i = 0; i < kStormMonitors; ++i) {
            const int refused = result.refusedForReserve.at(static_cast<size_t>(i)).load();
            const int servedHere = result.served.at(static_cast<size_t>(i)).load();
            refusalsTotal += refused;
            if (refused > 0 && servedHere == 0) ++monitorsRefusedWithoutServing;
            detail += " monitor " + std::to_string(kMonitorStormBase + i) + ": " + std::to_string(servedHere) +
                      " served, " + std::to_string(refused) + " refused;";
        }
        check(monitorsRefusedWithoutServing == 0,
              "a monitor was only refused for want of a free image after it had handed one out");
        if (monitorsRefusedWithoutServing != 0) {
            std::fprintf(stderr, "  %d monitor(s) refused without ever serving --%s\n",
                         monitorsRefusedWithoutServing, detail.c_str());
        }
        std::printf("daemon_system_test: the storm saw %d hand-over(s) refused for want of a free image --%s\n",
                    refusalsTotal, detail.c_str());
        std::fflush(stdout);
    }

    // GOAL: no render is ever deferred for want of a free image. exportCurrentFrame()'s reserve
    //       is what keeps one, and renderPending()'s deferral path is a backstop that should now
    //       be unreachable -- so any reading above zero is a report that a hand-over got past the
    //       reserve, which is a wallpaper that stops changing on a monitor under contention.
    // WHY-THIS-SHAPE: sampled on every GetStatus the storm makes rather than once afterwards,
    //       because the count is cleared by the next commit: a deferral that really happened
    //       would be gone by the time a single reading at the end was taken. Non-zero at ANY
    //       reading is the violation, which makes it independent of the interleaving -- unlike
    //       an assertion that some particular number of deferrals did or did not occur.
    // CONTROL: `if (deferred)` in WallpaperManager::renderPending (backend/daemon/
    //       wallpaper_manager.cpp) changed to `if (true)`, so every render defers -> "FAIL: no
    //       render was ever deferred for want of a free image, which the producer's reserve is
    //       what prevents", printing "21 of 22 readings reported a deferred render". Reverted.
    //       Deleting exportCurrentFrame()'s reserve does NOT redden this: measured, that run
    //       produced no deferral on any storm monitor at all, because whether four clients come
    //       to hold all three images of one monitor is a scheduling outcome. That is the reason
    //       the deferral claim is graded here as an invariant over every reading rather than as
    //       a state this phase drives the daemon into -- the deterministic version of it is in
    //       dbus_service_test's buffer-hold phase.
    // COVERAGE-ASSERT: the number of readings is asserted, so a reply that stopped carrying
    //       deferred_renders -- which would leave the check above true of nothing -- fails
    //       instead of going quiet. One reading per thread is far below the measured count
    //       (roughly a third of 4 x 24 calls land on the GetStatus arm; 22 readings measured on
    //       this host) and is a bound a slow host cannot flake.
    // CONTROL, for the coverage assertion: `entry["deferred_renders"] = monitor.deferredRenders;`
    //       in DbusService::getStatus (backend/daemon/dbus_service.cpp) guarded by
    //       `if (monitor.deferredRenders > 0)` -> "FAIL: deferred_renders was actually readable
    //       from the storm's GetStatus replies", with the invariant check below still green on
    //       zero readings, which is exactly the vacuous pass it exists to catch. Reverted.
    check(result.deferralReadings >= kStormThreads,
          "deferred_renders was actually readable from the storm's GetStatus replies");
    check(result.deferralsSeen == 0,
          "no render was ever deferred for want of a free image, which the producer's reserve is what prevents");
    if (result.deferralsSeen != 0) {
        std::fprintf(stderr, "  %d of %d readings reported a deferred render\n", result.deferralsSeen.load(),
                     result.deferralReadings.load());
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
    // CONTROL: an unconditional throw cannot reach this line -- the `before` reading above
    //       grades and returns first -- so the mutation is scoped: DbusService::getStatus
    //       made to throw only once the reply contains monitor 31, which is the post-storm
    //       barrier and is set after that reading. One FAIL, "the daemon still answers
    //       GetStatus after the post-storm barrier", plus this abandonment line and neither
    //       monitor-state FAIL. Reverted.
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
// CONTROL: `sigaddset(&shutdownSignals, SIGHUP)` deleted from backend/daemon/main.cpp -> "FAIL: the
//       daemon exits through main() on SIGHUP rather than being killed by it (terminated
//       by signal 1)", plus the exit-0, signal-name and stopped-cleanly checks. Reverted.
void checkHangupShutdown(NameWatcher& names, const Check& check) {
    SpawnedDaemon hungUp;
    const bool cameUp = names.waitFor(true);
    if (!cameUp && reportedStartupFault(hungUp, "the SIGHUP shutdown checks (S6.36)")) return;
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
//       (a) `std::signal(SIGPIPE, SIG_IGN)` deleted from backend/daemon/main.cpp -> 5 FAILs: SetWallpaper
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
    if (!cameUp && reportedStartupFault(deaf, "the closed-stdout checks (S6.37)")) return;
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
//       (iii) backend/daemon/main.cpp's ShutdownWatch guard replaced by a bare `std::thread shutdownWatch`
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
        // A bus child that never execed is this file's fault specifically, not the host's,
        // and bus.reason() would name dbus-daemon for it -- see the block that sets this.
        if (const char* fault = bus.preambleFault()) {
            phaseSetupFault("the connection-loss checks (S6.35)", fault);
            return;
        }
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
    // Ahead of the check, not after it: waitForOutput returns false for a child that died in
    // its preamble exactly as it does for a daemon that would not speak, and only one of
    // those is the daemon's doing.
    if (!cameUp && reportedStartupFault(orphan, "the connection-loss checks (S6.35)")) return;
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
    // CONTROL: backend/daemon/main.cpp's `wakingForShutdown` early return deleted from the watcher -- i.e.
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

// ---------------------------------------------------------------------------- //
// A daemon whose teardown STICKS, and the two things that end one.
//
// Both phases below need a stop that does not finish, and nothing in this suite produced
// one: every fixture here is refused early or bounded by a deadline of its own. What is
// used instead is the render worker. ~DbusService joins it, so a render that never returns
// is a join that never returns -- and a wallpaper file whose open blocks forever is a FIFO.
//
// requestWallpaper() refuses a FIFO outright (is_regular_file), and correctly: the check
// runs on the bus thread, and the render runs later on the worker. So the fixture points
// the request at a SYMLINK to a real image and re-points that symlink at a FIFO in between.
// That is a race, and it is treated as one: each attempt is verified, a lost attempt is
// retried, and a host where none lands reports the phase unexercised rather than passing.
// ---------------------------------------------------------------------------- //

// Whether the daemon's render worker is provably stuck: a wallpaper set on `witnessMonitor`
// after the wedge never produces a frame. The worker is one FIFO thread, so a monitor
// queued behind a wedged render cannot be serviced -- which is a much stronger signal than
// the wedged monitor's own silence, since that is also what a render that merely failed
// looks like.
bool workerIsWedged(sdbus::IProxy& proxy, const std::string& goodImage, int witnessMonitor,
                    std::chrono::milliseconds patience) {
    try {
        setWallpaper(proxy, "static", goodImage, witnessMonitor, std::chrono::seconds(10));
    } catch (const std::exception&) {
        return false;
    }
    const std::string id = std::to_string(witnessMonitor);
    const auto deadline = Clock::now() + patience;
    while (Clock::now() < deadline) {
        const StatusReply status = getStatus(proxy, std::chrono::seconds(10));
        const nlohmann::json* entry = wallpaperStatus(status, id);
        // A frame means the worker got through, so nothing is wedged.
        if (entry != nullptr && entry->value("has_frame", false)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // No frame for the whole patience, and the sanity render before the wedge proved this
    // daemon renders, so the worker is not coming back.
    return true;
}

// Wedges the worker, or returns false having said why in `why`. `link` is re-pointed
// between the request and the render; `fifo` is what it ends up naming.
bool wedgeRenderWorker(sdbus::IProxy& proxy, const std::string& goodImage, const std::filesystem::path& link,
                       const std::filesystem::path& fifo, std::string& why) {
    // The sanity render first: without it, "no frame appeared" below is equally consistent
    // with a host whose daemon cannot render at all, and the phases would grade an unwind
    // that was never wedged.
    constexpr int kSanityMonitor = 20;
    try {
        setWallpaper(proxy, "static", goodImage, kSanityMonitor, std::chrono::seconds(10));
    } catch (const std::exception& e) {
        why = std::string("the daemon refused the wallpaper this phase needs as a baseline: ") + e.what();
        return false;
    }
    bool rendered = false;
    const auto sanityDeadline = Clock::now() + std::chrono::seconds(15);
    while (!rendered && Clock::now() < sanityDeadline) {
        const StatusReply status = getStatus(proxy, std::chrono::seconds(10));
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kSanityMonitor));
        rendered = entry != nullptr && entry->value("has_frame", false);
        if (!rendered) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!rendered) {
        why = "this daemon never produced a frame for an ordinary wallpaper, so a render cannot be wedged on this "
              "host and nothing below would be measuring a stuck teardown";
        return false;
    }

    // Each attempt gets monitor ids of its own: a re-used id coalesces with the request
    // still outstanding on it, so a second attempt would queue nothing at all.
    constexpr int kAttempts = 12;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const int wedgeMonitor = 1 + attempt * 2;
        const int witnessMonitor = 2 + attempt * 2;
        std::error_code ec;
        std::filesystem::remove(link, ec);
        std::filesystem::create_symlink(goodImage, link, ec);
        if (ec) {
            why = "a symlink could not be created at " + link.string() + ": " + ec.message();
            return false;
        }
        try {
            setWallpaper(proxy, "static", link.string(), wedgeMonitor, std::chrono::seconds(10));
        } catch (const std::exception&) {
            // The daemon saw the FIFO at validation -- the previous attempt's link, or this
            // one re-pointed too early. Not a defect, just a lost race.
            continue;
        }
        // The whole fixture, in two calls: the request is accepted against a real file and
        // the render opens whatever this now names.
        std::filesystem::remove(link, ec);
        std::filesystem::create_symlink(fifo, link, ec);
        if (ec) continue;

        if (workerIsWedged(proxy, goodImage, witnessMonitor, std::chrono::milliseconds(2500))) return true;
    }
    why = "the render never landed on the fifo in " + std::to_string(kAttempts) +
          " attempts -- this host services a render faster than the harness can re-point a symlink, so no stuck "
          "teardown could be staged";
    return false;
}

// GOAL: a teardown that STICKS is ended by the alarm the unwind arms at its top, rather than
//       running forever. False if the alarm is armed only in ~ShutdownWatch, which is
//       destroyed last: a destructor that blocks before it is then reached by nothing, and
//       the daemon has to be SIGKILLed -- which for a systemd unit means every `systemctl
//       stop` on that machine ends in SIGKILL after the unit's own timeout.
// WHY-THIS-SHAPE: driven on the CONNECTION-LOSS path, not on a signal, and that is the whole
//       reason the phase exists separately. On a signal the sigwait watcher is already
//       running a deadline of its own and ends the process at five seconds whatever the
//       guards do, so a signal-driven version of this check is green with them deleted --
//       measured, and it is why the obvious shape was rejected. With the bus destroyed
//       instead, no signal is ever received, the watcher is still parked in sigwait, and the
//       guards are the only thing left.
//       `timedOut()` is asserted rather than the exit code alone: shutdown()/waitForExit()
//       SIGKILL after their own wait and report a code either way, so a code-only check
//       grades a process that never exited as one that exited for the right reason.
// CONTROL: both `ArmTeardown` guards (armBeforeService and armBeforeHttp) deleted from
//       backend/daemon/main.cpp, i.e. the arming left to ~ShutdownWatch as it was before the
//       fix -> "FAIL: a daemon whose teardown blocks still ends, rather than having to be
//       killed", printing "it was still running 32.8 s after its bus was destroyed, with its
//       render worker wedged; nothing armed the watchdog" -- the process had to be SIGKILLed
//       at the harness timeout, and the whole binary went from 21 s to 46 s. One red in the
//       run, no skips. The escalation phase below stayed GREEN under it, which is the
//       separation the two phases exist for: the second signal does not need the alarm.
//       Observed 2026-08-19. Reverted.
// COVERAGE-ASSERT: the wedge is verified before the bus is killed -- a wallpaper set after it
//       must produce no frame, on a daemon that has already been seen to render one -- and a
//       run that could not wedge the worker reports NOT VERIFIED. Without that, a daemon
//       whose teardown was never stuck exits promptly and this phase reads green having
//       graded an ordinary shutdown.
void checkStuckTeardownIsBounded(const std::string& validationEnv, const std::string& bluePath, const Check& check) {
    constexpr const char* kGoal = "S8.20's watchdog clause (a teardown that blocks is ended by the alarm the unwind "
                                  "arms at its top, not left to a destructor that is never reached)";
    ScratchDir work("stuckteardown");
    if (!work.ok()) {
        notVerified(kGoal, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path link = work.path() / "wallpaper-link.png";
    const std::filesystem::path fifo = work.path() / "wallpaper.fifo";
    if (::mkfifo(fifo.c_str(), 0644) != 0) {
        notVerified(kGoal, "a fifo could not be created, so no render can be made to block on this host");
        return;
    }

    NestedBus bus;
    if (!bus.ok()) {
        if (const char* fault = bus.preambleFault()) {
            phaseSetupFault(kGoal, fault);
            return;
        }
        notVerified(kGoal, bus.reason());
        return;
    }

    SpawnedDaemon daemon({validationEnv, std::string("DBUS_SESSION_BUS_ADDRESS=") + bus.address()});
    if (!daemon.waitForOutput(std::string("serving ") + DbusService::kBusName, std::chrono::seconds(30))) {
        if (reportedStartupFault(daemon, kGoal)) return;
        notVerified(kGoal, "the daemon for this phase never reached the bus");
        return;
    }

    {
        // Scoped, and closed before the bus is killed: this phase's own client must be gone
        // by then, or destroying the broker would be graded against a connection the harness
        // still held open.
        auto connection = sdbus::createSessionBusConnectionWithAddress(bus.address());
        connection->enterEventLoopAsync();
        auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        std::string why;
        if (!wedgeRenderWorker(*proxy, bluePath, link, fifo, why)) {
            notVerified(kGoal, why);
            return;
        }
    }

    // Timed from HERE, not from the top of the phase: the wedge above takes seconds of its
    // own, and folding those into the figure would put the printed line -- and the failure
    // branch's "still running N s after its bus was destroyed" -- several seconds out.
    const auto killedAt = Clock::now();
    bus.kill();
    const int exitCode = daemon.waitForExit();
    const double elapsedMs = millis(Clock::now() - killedAt);
    const std::string output = daemon.output();

    check(!daemon.timedOut(), "a daemon whose teardown blocks still ends, rather than having to be killed");
    if (daemon.timedOut()) {
        std::fprintf(stderr, "  it was still running %.1f s after its bus was destroyed, with its render worker "
                             "wedged; nothing armed the watchdog\n",
                     elapsedMs / 1000.0);
        return;
    }
    // SIGALRM specifically. The process could also leave by the sigwait watcher's own
    // deadline, and that path prints a line of its own -- so the two are told apart rather
    // than both accepted as "it exited somehow".
    const bool endedByTheAlarm = !daemon.exitedNormally() && daemon.termSignal() == SIGALRM;
    check(endedByTheAlarm, "the stuck teardown is ended by the alarm the unwind armed");
    if (!endedByTheAlarm) {
        std::fprintf(stderr, "  it %s (exit code %d, signal %d) after %.1f s\n",
                     daemon.exitedNormally() ? "exited normally" : "was terminated by a signal", exitCode,
                     daemon.termSignal(), elapsedMs / 1000.0);
    }
    std::printf("daemon_system_test: a daemon with a wedged render worker was ended %.1f s after its bus was "
                "destroyed\n",
                elapsedMs / 1000.0);
}

// GOAL: a second SIGINT/SIGTERM ends a stop that is stuck, promptly, and says it is doing so.
//       False if the second signal does nothing: the user who pressed Ctrl-C twice waits out
//       the full teardown deadline, and a supervisor's escalation lands on a process that
//       cannot answer it.
// WHY-THIS-SHAPE: the same wedged worker as the phase above, because a second signal only
//       has anything to end if the first stop is stuck -- on an ordinary stop the process is
//       gone before a second signal can be sent, and a check written that way would be
//       grading a race. BOTH the message and the elapsed time are asserted: without the
//       handler the process still exits, at the five-second deadline, so an exit-code check
//       alone is green under the control. The absence of the deadline's own line is asserted
//       for the same reason -- it is what the daemon prints when nothing shortened the stop.
// CONTROL: the sigaction/pthread_sigmask block that installs forceExitOnSecondSignal deleted
//       from the watcher in backend/daemon/main.cpp -> two red. "FAIL: the daemon says that a
//       second signal is what ended it", printing "it left with exit code -1, signal 14, and
//       said none of it"; and "FAIL: the second signal ends the daemon promptly rather than at
//       the deadline", printing "the daemon took 3884 ms to go after the second signal,
//       against a 5000 ms teardown deadline". The second signal simply stayed pending -- it is
//       blocked in every thread and the watcher has left its sigwait -- so the process died on
//       the unwind's own SIGALRM instead.
//       Note which check did NOT fire: "the stop ends on the second signal rather than on the
//       teardown deadline" stayed green, because the alarm beat the watcher's deadline and
//       "shutdown blocked" was never printed. That is exactly why the message and the elapsed
//       time are asserted as well; the absence check alone would have been green here.
//       Observed 2026-08-19. Reverted.
// COVERAGE-ASSERT: the wedge is verified before the first signal, exactly as above, and a run
//       that could not stage it reports NOT VERIFIED rather than grading a stop that was
//       never stuck.
void checkSecondSignalEndsAStuckStop(const std::string& validationEnv, const std::string& bluePath,
                                     const Check& check) {
    constexpr const char* kGoal = "S8.20's escalation clause (a second shutdown signal ends a stop that is stuck, "
                                  "rather than leaving the user to wait out the teardown deadline)";
    ScratchDir work("secondsignal");
    if (!work.ok()) {
        notVerified(kGoal, "a scratch directory could not be created");
        return;
    }
    const std::filesystem::path link = work.path() / "wallpaper-link.png";
    const std::filesystem::path fifo = work.path() / "wallpaper.fifo";
    if (::mkfifo(fifo.c_str(), 0644) != 0) {
        notVerified(kGoal, "a fifo could not be created, so no render can be made to block on this host");
        return;
    }

    NestedBus bus;
    if (!bus.ok()) {
        if (const char* fault = bus.preambleFault()) {
            phaseSetupFault(kGoal, fault);
            return;
        }
        notVerified(kGoal, bus.reason());
        return;
    }

    SpawnedDaemon daemon({validationEnv, std::string("DBUS_SESSION_BUS_ADDRESS=") + bus.address()});
    if (!daemon.waitForOutput(std::string("serving ") + DbusService::kBusName, std::chrono::seconds(30))) {
        if (reportedStartupFault(daemon, kGoal)) return;
        notVerified(kGoal, "the daemon for this phase never reached the bus");
        return;
    }

    {
        auto connection = sdbus::createSessionBusConnectionWithAddress(bus.address());
        connection->enterEventLoopAsync();
        auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        std::string why;
        if (!wedgeRenderWorker(*proxy, bluePath, link, fifo, why)) {
            notVerified(kGoal, why);
            return;
        }
    }

    ::kill(daemon.pid(), SIGTERM);
    // Long enough for the watcher to have printed, installed the handler and unblocked these
    // signals, and far short of the five-second deadline the control has to fall back on.
    // A shorter wait would make the second signal race the handler's installation, which is
    // the daemon's setup rather than the behaviour being graded.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const auto secondSignal = Clock::now();
    ::kill(daemon.pid(), SIGTERM);
    const int exitCode = daemon.waitForExit();
    const double afterSecondMs = millis(Clock::now() - secondSignal);
    const std::string output = daemon.output();

    check(!daemon.timedOut(), "a daemon asked twice to stop does not have to be killed");
    if (daemon.timedOut()) {
        std::fprintf(stderr, "  it was still running %.1f s after the second signal\n", afterSecondMs / 1000.0);
        return;
    }
    const bool saidSo = output.find("asked again, exiting now") != std::string::npos;
    check(saidSo, "the daemon says that a second signal is what ended it");
    if (!saidSo) {
        std::fprintf(stderr, "  it left with exit code %d, signal %d, and said none of it\n", exitCode,
                     daemon.termSignal());
    }
    // The other route out of a stuck stop, and the one the control falls back on: the
    // watcher's own deadline. Its presence here means the second signal changed nothing.
    const bool notTheDeadline = output.find("shutdown blocked") == std::string::npos;
    check(notTheDeadline, "the stop ends on the second signal rather than on the teardown deadline");
    // A quarter of the deadline: the handler runs in the signal itself, so anything near
    // five seconds is the deadline having fired, and a relative bound would accept it.
    const bool prompt = afterSecondMs < 1250;
    check(prompt, "the second signal ends the daemon promptly rather than at the deadline");
    if (!prompt) {
        std::fprintf(stderr, "  the daemon took %.0f ms to go after the second signal, against a 5000 ms teardown "
                             "deadline\n",
                     afterSecondMs);
    }
    std::printf("daemon_system_test: a wedged stop ended %.0f ms after the second SIGTERM\n", afterSecondMs);
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
        // is_string() first, both times: value() converts rather than defaults, so it
        // throws type_error.302 on a present non-string. A throw here leaves the phase and
        // every phase after it ungraded behind one opaque line (X.4), where a daemon that
        // published the wrong type should read as "could not be asked" and skip.
        if (report->contains("detail") && (*report)["detail"].is_string()) {
            detail = (*report)["detail"].get<std::string>();
        }
        if (!report->contains("status") || !(*report)["status"].is_string()) {
            why = "GetStatus's config_server object carries no string status";
            return {};
        }
        return (*report)["status"].get<std::string>();
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

// The daemon's bind-SUCCESS wording, duplicated for the same reason the failure wording is.
// This one has no live DBus counterpart to cross-check against: the success path's health
// detail is the fixed word "listening" (WallpaperManager::recordConfigServer), not the
// printf's wording. What guards it is the FAIL arm in awaitConfigServerBind below.
std::string bindSuccessMarker(int port) {
    return std::string("config server on http://") + wallpaperx::HttpServer::kLoopbackHost + ":" +
           std::to_string(port);
}

// GOAL: X.3 in both of its directions, for every phase whose checks need the spawned daemon
//       to be the process listening on `port`. A daemon that reworded its bind-success line
//       must arrive as a named failure, never as a skip that deletes the phase; a daemon
//       that quit of its own accord must arrive as a named failure; a daemon the machine
//       killed must arrive as a skip, never as a verdict on the daemon; and a port genuinely
//       held by another process must arrive as a skip, asserted only where the daemon's own
//       bind-failure line says so.
// WHY-THIS-SHAPE: one function, not a copy per phase. The obvious alternative -- each phase
//       inlining its own wait-and-classify -- is what this replaces, and the cost of it is
//       measurable: a copy that drops the drain re-wait accuses production of rewording a
//       log line the daemon did print, and a copy that drops the bind-failure cross-check
//       files a daemon that died mid-phase as another process holding 18462. The arm order
//       is the substance: each positive signal (the daemon printed the failure line; the
//       process is gone; the daemon says ok) is consulted before anything is concluded from
//       an absence, because "no bind line in the buffer" is equally true of all of them.
//       The process-gone arm asks waitid for the real exit status rather than concluding
//       from the capture pipe's end-of-file, which is the tempting shorter form and is
//       wrong in both directions: it carries no status, so an OOM kill reads as the daemon
//       quitting, and a broken capture pipe reads as a dead daemon that is in fact running.
//       The second wait covers only the capture pipe's drain -- the daemon answers GetStatus
//       from an event loop it enters after the HttpServer constructor has printed, so an
//       "ok" verdict proves the printf already ran -- and it expires into the same FAIL, so
//       it can never turn a real rewording into a pass (X.1).
// CONTROL: six, all reverted, each reported by both calling phases. (a) the success printf
//       in backend/daemon/http_server.cpp reworded to "config server listening on
//       http://%s:%d" -> "FAIL: the bind-success wording the connector-name phase waits for
//       is still the daemon's" and the same for the HTTP lifecycle phase. (b)
//       `wallpaperx::HttpServer httpServer(manager);` commented out in
//       backend/daemon/main.cpp -> "NOT VERIFIED -- ... the daemon printed neither the
//       bind-success nor the bind-failure line for port 18462; the daemon reports
//       config_server pending", and no FAIL from either phase. (c) that construction replaced
//       in backend/daemon/main.cpp by a detached thread that `std::_Exit(7)`s two seconds in,
//       which is a daemon that quit of its own accord mid-phase -> "FAIL: the daemon of the
//       connector-name phase is still running when its config port is asked about" and the
//       same for the HTTP lifecycle phase. (d) the same replacement but `kill(getpid(),
//       SIGKILL)` instead of the exit, which is a daemon the machine took away (the signal is
//       what this classifies on, not who sent it) -> "NOT VERIFIED -- ... the daemon of the
//       connector-name phase was killed by signal 9 by something outside this test before its
//       config port could be graded", from both phases, and no FAIL: the same run under the
//       old capture-pipe verdict produced (c)'s two FAIL lines instead. (e) readLoop() made to
//       set m_readerDone (and m_readFailed) and return at once, which is a live daemon behind
//       a lost capture pipe -> "HARNESS FAULT -- the connector-name phase could not be set up:
//       the daemon's output pipe failed while it was still running", from both phases; with
//       the StillRunning arm removed the same build printed (c)'s two FAIL lines against a
//       daemon that was running the whole time. (f) an unrelated process made to hold port
//       18462 -> the port-conflict skip carrying the daemon's own detail, "port 18462 is held
//       by another process on this machine (a real wallpaperx-daemon?) ...; the daemon reports
//       config_server fail / could not bind 127.0.0.1:18462 (is another process using it?)",
//       and with bindFailureMarker additionally made to match nothing the daemon prints, the
//       neutral skip with the same detail appended -- where restoring the dropped
//       `|| reportStatus == "fail"` disjunct claimed the phantom holding process instead.
//       Timing, same port-conflict run: 17.7 s with the two-marker wait above, 37.4 s with the
//       single-marker wait it replaced.
bool awaitConfigServerBind(const SpawnedDaemon& daemon, int port, const char* goals, const char* phase,
                           const Check& check) {
    const std::string bindOk = bindSuccessMarker(port);
    const std::string bindFail = bindFailureMarker(port);
    // Watches both markers. They are the two branches of one `if` in HttpServer's
    // constructor, so on a host where the port is taken the answer is in the buffer within
    // milliseconds; waiting only for the success wording spends the whole ten seconds per
    // calling phase before reading a failure line it already had. The loop decides only when
    // to stop waiting -- every arm below concludes exactly as it would have.
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (true) {
        if (daemon.waitForOutput(bindOk, std::chrono::milliseconds(100))) return true;
        if (daemon.output().find(bindFail) != std::string::npos) break;
        if (daemon.exited() || Clock::now() >= deadline) break;
    }

    std::string reportDetail;
    std::string askWhy;
    const std::string reportStatus = askConfigServerStatus(reportDetail, askWhy);
    if (reportStatus == "ok" && daemon.waitForOutput(bindOk, std::chrono::seconds(2))) return true;

    const std::string daemonSays =
        reportStatus.empty()
            ? std::string("; the daemon could not be asked over DBus (") + askWhy + ")"
            : std::string("; the daemon reports config_server ") + reportStatus +
                  (reportDetail.empty() ? std::string() : std::string(" / ") + reportDetail);

    // First, because it is the daemon's own words about the port and they do not stop being
    // true when it later dies. Only this line is evidence about the host: config_server
    // "fail" alone is equally true of a bind refused for any other reason (EACCES under a
    // sandbox, EADDRNOTAVAIL with no loopback), and claiming a conflicting process for those
    // sends the reader hunting a daemon that does not exist -- `daemonSays` carries the real
    // reason into the neutral skip instead.
    if (daemon.output().find(bindFail) != std::string::npos) {
        notVerified(goals, std::string("port ") + std::to_string(port) +
                               " is held by another process on this machine (a real wallpaperx-daemon?), "
                               "so the spawned daemon could not bind it" +
                               daemonSays + (daemon.exited() ? "; the daemon has since exited" : ""));
        return false;
    }
    if (daemon.exited()) {
        int signo = 0;
        switch (daemon.departure(signo)) {
            case SpawnedDaemon::Departure::KilledFromOutside:
                notVerified(goals, std::string("the daemon of ") + phase + " was killed by signal " +
                                       std::to_string(signo) +
                                       " by something outside this test before its config port could be graded");
                return false;
            case SpawnedDaemon::Departure::StillRunning:
                phaseSetupFault(phase, daemon.readFailed()
                                           ? "the daemon's output pipe failed while it was still running"
                                           : "the daemon's output pipe reached end of file while it was still "
                                             "running");
                return false;
            case SpawnedDaemon::Departure::Unknown:
                phaseSetupFault(phase, "the daemon is gone and its exit status could not be read");
                return false;
            case SpawnedDaemon::Departure::QuitOnItsOwn:
                break;
        }
        check(false, (std::string("the daemon of ") + phase + " is still running when its config port is asked about")
                         .c_str());
        std::fprintf(stderr, "  the daemon exited on its own, before this phase signalled it; %s\n",
                     askWhy.empty() ? "it still answered GetStatus" : askWhy.c_str());
        return false;
    }
    if (reportStatus == "ok") {
        check(false, (std::string("the bind-success wording ") + phase + " waits for is still the daemon's").c_str());
        std::fprintf(stderr, "  waited for \"%s\", but the daemon never printed it and reports config_server ok / %s\n",
                     bindOk.c_str(), reportDetail.empty() ? "(empty detail)" : reportDetail.c_str());
        return false;
    }
    notVerified(goals, "the daemon printed neither the bind-success nor the bind-failure line for port " +
                           std::to_string(port) + daemonSays);
    return false;
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

// The names of the checks reading `pending`, sorted, and a printable form of that list. The
// aggregate counts failures only, so "ok overall while a dependency is untried" has to name
// the untried ones: `status == "ok"` alone is equally true of an endpoint with no `pending`
// state left to report.
std::vector<std::string> pendingCheckNames(const nlohmann::json& body) {
    std::vector<std::string> names;
    const nlohmann::json* checks = healthChecks(body);
    if (checks == nullptr) return names;
    for (const auto& [name, entry] : checks->items()) {
        if (entry.is_object() && entry.value("status", std::string()) == "pending") names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out.empty() ? "(none)" : out;
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
        if (reportedStartupFault(daemon, "S7.8/S7.11 (the HTTP server's process lifecycle)")) return;
        notVerified("S7.8/S7.11 (the HTTP server's process lifecycle)",
                    "the daemon for this phase never acquired the bus name");
        return;
    }

    // Whether this daemon is the process on 18462 decides whether this phase can run at all,
    // and the four outcomes are told apart in awaitConfigServerBind -- one classifier shared
    // with the connector-name phase, so the two cannot drift into disagreeing about which
    // outcome is the daemon's fault and which is the machine's.
    if (!awaitConfigServerBind(daemon, kPort, "S7.8/S7.11 (the HTTP server's process lifecycle)",
                               "the HTTP lifecycle phase", check)) {
        daemon.shutdown(SIGTERM);
        return;
    }

    // GOAL: S7.10/S7.10b end to end -- /health on 127.0.0.1:18462, asked of the real
    //       binary by a process that is not it. A daemon that has just started has not
    //       touched its shader directory or created a device, so `shaders` and `gpu` read
    //       `pending` in their own entries while the aggregate, which counts failures only,
    //       reads `ok`. False in either direction: an aggregate of `pending` has a watchdog
    //       restarting an idle daemon that is working, and a `pending` that never appears
    //       has the shipped binary claiming health nobody established.
    // WHY-THIS-SHAPE: the in-process tier already pins the JSON in far more detail. What
    //       only this tier can establish is that the shipped binary reaches this state at
    //       all -- an HttpServer never wired into main.cpp, or a daemon that eagerly
    //       created a device at startup, would leave every in-process check green. So this
    //       asserts the names and the pending set beside the aggregate, and no details.
    //       The pending set is asserted rather than `status == "ok"` alone because alone it
    //       is vacuous: a daemon that reported everything `ok` from birth satisfies it.
    // CONTROL: the `wallpaperx::HttpServer httpServer(manager);` line removed from
    //       backend/daemon/main.cpp -> this phase reports NOT VERIFIED ("the daemon printed neither the
    //       bind-success nor the bind-failure line for port 18462; the daemon reports
    //       config_server pending") rather than spending a FAIL on the daemon; the run's
    //       failures for that mutation all come from phase 11, which grades it directly.
    //       For the body itself, two: the `else if (check->status ==
    //       HealthStatus::Pending && overall != HealthStatus::Fail)` arm restored in
    //       http_server.cpp's aggregation -> "FAIL: the real daemon reports ok overall
    //       before it has used anything, with the dependencies it has not used pending"
    //       ("pending checks: gpu, shaders; aggregate: pending"); and `HealthStatus status
    //       = HealthStatus::Ok;` as HealthCheck's default in wallpaper_manager.h -> the
    //       same line red the other way ("pending checks: (none); aggregate: ok") beside
    //       the GPU one. And for the three names: the `{"gpu", &snap.health.gpu}` entry
    //       removed from that same array -> "FAIL: the real daemon's /health names
    //       shaders, gpu and wallpapers as separate checks" and "FAIL: the real daemon
    //       reports the GPU as untried before its first wallpaper", the body coming back
    //       with two checks in it. All reverted.
    httplib::Client cli = wallpaperx::testing::makeClient(kPort);
    const HttpReply fresh = wallpaperx::testing::get(cli, "/health");
    check(fresh.ok(), "the real daemon answers GET /health on 127.0.0.1:18462");
    if (fresh.ok()) {
        const nlohmann::json* checks = healthChecks(fresh.json);
        const bool named = checks != nullptr && checks->contains("shaders") && checks->contains("gpu") &&
                           checks->contains("wallpapers");
        check(named, "the real daemon's /health names shaders, gpu and wallpapers as separate checks");
        const std::string aggregate = fresh.json.value("status", std::string());
        const std::vector<std::string> untried = pendingCheckNames(fresh.json);
        const bool idleIsHealthy = untried == std::vector<std::string>{"gpu", "shaders"} && aggregate == "ok";
        check(idleIsHealthy, "the real daemon reports ok overall before it has used anything, with the "
                             "dependencies it has not used pending");
        check(named && (*checks)["gpu"].is_object() &&
                  (*checks)["gpu"].value("status", std::string()) == "pending",
              "the real daemon reports the GPU as untried before its first wallpaper");
        if (!idleIsHealthy || !named) {
            std::fprintf(stderr, "  pending checks: %s; aggregate: %s\n  fresh /health: %s\n",
                         joinNames(untried).c_str(), aggregate.c_str(), fresh.body.c_str());
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
    // CONTROL: backend/daemon/main.cpp's `HttpServer httpServer(manager);` replaced with
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
        if (reportedStartupFault(daemon, "S7.14 (the ok direction)")) return;
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
    // CONTROL: three. (a) backend/daemon/main.cpp's HttpServer constructed on kDefaultPort + 1, its
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
        // Before the bind-failure branch below, which grades a daemon that reached its bind
        // attempt: a child that never execed printed nothing, so that branch would fall
        // through to the skip and name the daemon for it.
        if (reportedStartupFault(daemon, "S7.14 (the fail direction)")) return;
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

// ---------------------------------------------------------------------------- //
// Phase 12 -- Step 8: config persistence, in the shipped binary.
// ---------------------------------------------------------------------------- //
//
// Every check below needs a daemon that is a *process*: a wallpaper surviving a restart is
// two processes and a file between them, and the worse half of "no config content stops the
// daemon" is a SIGABRT, which an in-process test cannot report because it dies with it
// (measured: the const `operator[]` control killed config_store_test outright, printing
// json.hpp's assertion and no FAIL line).
//
// Each of these phases points the daemon at an XDG_CONFIG_HOME it owns and deletes.
// SpawnedDaemon injects a fresh one per spawn precisely so no test can reach the
// developer's real ~/.config/wallpaperx -- an earlier run of this suite wrote 28 test
// wallpapers there and restored them on the next login. A phase that wants state to survive
// a restart passes its own, which is the only reason to override that default.

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeTextFile(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return static_cast<bool>(out);
}

struct PersistedEntry {
    std::string type;
    std::string path;
    // Empty when the row carries no connector name, which is the ordinary case: a screen is
    // only named once the app has run for it.
    std::string output;
};

// What the daemon wrote, read straight off the disk. Parsed here rather than through
// ConfigStore::load() on purpose: this tier's job is to grade the file the shipped binary
// produced, and a reader that shares the writer's field-name constants agrees with it by
// construction, including when both are wrong.
std::map<int, PersistedEntry> configEntries(const std::filesystem::path& file, std::string& why) {
    std::map<int, PersistedEntry> entries;
    const std::string text = readTextFile(file);
    if (text.empty()) {
        why = "no config file at " + file.string();
        return entries;
    }
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("monitors") || !root["monitors"].is_array()) {
        why = "the config file is not an object with a monitors array";
        return entries;
    }
    for (const nlohmann::json& entry : root["monitors"]) {
        if (!entry.is_object() || !entry.contains("monitor_id") || !entry.contains("type") ||
            !entry.contains("path")) {
            continue;
        }
        if (!entry["monitor_id"].is_number_integer() || !entry["type"].is_string() || !entry["path"].is_string()) {
            continue;
        }
        // `output` is optional and is not part of the required-field guard above: a row
        // written before connector names existed carries none, and dropping such a row
        // would make the wallpaper checks in the other phases silently stop comparing.
        std::string output;
        if (entry.contains("output") && entry["output"].is_string()) output = entry["output"].get<std::string>();
        entries[entry["monitor_id"].get<int>()] =
            PersistedEntry{entry["type"].get<std::string>(), entry["path"].get<std::string>(), std::move(output)};
    }
    return entries;
}

// Every file under `dir`, so a phase can say "nothing was written here" and name what was
// if something was. False, with a reason, when the walk could not be made: "the directory
// could not be read" and "the directory was empty" have to be different answers, or the
// absence check above becomes evidence of absence on no evidence at all. The non-throwing
// increment(ec) is what keeps a mid-walk error -- a daemon creating a mode-0000 directory
// under its cwd, which is exactly what this phase hunts -- from throwing out of the phase
// and taking every later phase with it.
bool filesUnder(const std::filesystem::path& dir, std::vector<std::string>& found, std::string& why) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(dir, ec);
    if (ec) {
        why = dir.string() + " could not be read (" + ec.message() + ")";
        return false;
    }
    const std::filesystem::recursive_directory_iterator last;
    for (; it != last; it.increment(ec)) {
        if (ec) {
            why = dir.string() + " could not be walked (" + ec.message() + ")";
            return false;
        }
        found.push_back(it->path().string());
    }
    return true;
}

// The daemon's /health body, when the daemon is the process serving 18462. Returns false
// with a reason otherwise -- a developer's own daemon holding the port is a skip, not a
// finding, and this phase has a GetStatus-based answer for that case that does not need
// the port at all.
bool fetchHealth(const StatusReply& status, nlohmann::json& out, std::string& why, const Check& check) {
    const nlohmann::json* report = configServerReport(status);
    const std::string reportStatus = report ? report->value("status", std::string()) : std::string();
    if (reportStatus != "ok") {
        why = "the daemon reports config_server " + (reportStatus.empty() ? std::string("(absent)") : reportStatus) +
              ", so port " + std::to_string(wallpaperx::HttpServer::kDefaultPort) + " is not its own to answer on";
        return false;
    }
    httplib::Client cli = wallpaperx::testing::makeClient(wallpaperx::HttpServer::kDefaultPort);
    const wallpaperx::testing::HttpReply reply = wallpaperx::testing::get(cli, "/health");
    if (!reply.ok()) {
        // Not a skip: the daemon just said it owns that port, so a /health that will not
        // answer is the daemon failing, and the http lifecycle phase grades the same thing.
        // `why` is deliberately left empty as the marker for "already graded red" -- with a
        // reason set, callers reported the one fault twice, once as a failure and once as
        // coverage that did not run, and the run summary exists to keep those apart.
        check(false, "a daemon reporting config_server ok answers GET /health");
        return false;
    }
    out = reply.json;
    return true;
}

std::string healthCheckStatusOf(const nlohmann::json& body, const char* name) {
    const nlohmann::json* checks = healthChecks(body);
    if (checks == nullptr || !checks->contains(name) || !(*checks)[name].is_object()) return {};
    return (*checks)[name].value("status", std::string());
}

void checkConfigRestart(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr int32_t kMonitorRestore = 18;
    constexpr const char* kGoals = "S8.1/S8.3/S8.11 (a wallpaper survives a restart, byte for byte)";

    ScratchDir configHome("restart");
    if (!configHome.ok()) {
        notVerified(kGoals, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }

    // A filename carrying every byte class sanitizeForLog rewrites: U+202E, DEL, and a
    // newline. Legal on Linux, and the whole point -- what is persisted has to be the bytes
    // that open this file, not the display-safe rendering of them.
    const std::string nastyName = std::string("system_persist_\xE2\x80\xAE\x7F\nblue.png");
    std::string nastyPath;
    try {
        nastyPath = writeSolidImage(nastyName.c_str(), 0, 0, 255);
    } catch (const std::exception& e) {
        notVerified(kGoals, std::string("this filesystem would not take a wallpaper filename containing U+202E, DEL "
                                        "and a newline: ") +
                                e.what());
        return;
    }

    bool firstRendered = false;
    {
        SpawnedDaemon first({validationEnv, configHome.env()});
        if (!names.waitFor(true)) {
            if (!reportedStartupFault(first, kGoals)) {
                notVerified(kGoals, "the first daemon of this phase never acquired the bus name");
            }
            return;
        }

        // Declared before the proxy so it outlives it: the FrameReady callback runs on the
        // proxy's dispatch thread.
        FrameReadyLog signals;
        auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        proxy->uponSignal("FrameReady")
            .onInterface(DbusService::kInterface)
            .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

        // GOAL: S8.11, first half -- HealthStatus::Pending still means "no dependency has
        //       been tried yet", and after Step 8 it is reachable exactly when there is no
        //       config file. False if a daemon that restored nothing already reports a GPU.
        // WHY-THIS-SHAPE: asked of a daemon whose XDG_CONFIG_HOME this phase created empty
        //       seconds ago, so "no config file" is a fact rather than an assumption, and
        //       asked before this phase's own SetWallpaper. gpu.initialized on GetStatus is
        //       asserted unconditionally; the `pending` wording is asserted only when the
        //       daemon owns port 18462, because a developer's own daemon on that port is a
        //       skip and not a finding.
        // CONTROL: this block moved below the SetWallpaper and the FrameReady wait, so the
        //       same daemon is asked once a device provably exists -> "FAIL: a daemon with
        //       no config file has created no GPU device at startup" and "FAIL: /health
        //       reports the GPU as pending on a daemon that restored nothing". Moved back.
        //       A control that instead gave THIS daemon a populated config file was tried
        //       and rejected: whether the restore's render has committed by the time the
        //       reply is built is a race, so it would have been a check graded on the
        //       scheduler. The restore direction is controlled below, after a FrameReady.
        const StatusReply beforeAnything = getStatusOrExplain(*proxy);
        if (beforeAnything.ok()) {
            check(!beforeAnything.json().value("gpu", nlohmann::json::object()).value("initialized", true),
                  "a daemon with no config file has created no GPU device at startup");
            nlohmann::json health;
            std::string why;
            if (!fetchHealth(beforeAnything, health, why, check)) {
                if (!why.empty()) notVerified("S8.11's /health half", why);
            } else {
                check(healthCheckStatusOf(health, "gpu") == "pending",
                      "/health reports the GPU as pending on a daemon that restored nothing");
                check(healthCheckStatusOf(health, "persistence") == "ok",
                      "/health reports persistence as ok on a daemon whose config file does not exist yet");
            }
        }

        setWallpaper(*proxy, "static", nastyPath, kMonitorRestore);
        firstRendered = signals.wait(kMonitorRestore, 1);
        if (!firstRendered) {
            std::printf("daemon_system_test: the first daemon of the restart phase never rendered; the restart "
                        "checks below grade the restored STATE, and the restored FRAME is skipped rather than "
                        "blamed on the restore.\n");
        }
        first.shutdown(SIGTERM);
        awaitNameRelease(names, "the restarted daemon of the config phase");
    }

    // GOAL: S8.3 -- the daemon persists the path it was given, byte for byte. False if the
    //       file holds the sanitized form: /config and GetStatus both publish
    //       sanitizeForLog(path), and persisting what they publish would restore a
    //       different file (PLAN.md Step 8 trap 4).
    // WHY-THIS-SHAPE: read off the disk and compared to the exact bytes this test passed
    //       over the bus, and the same reply's published path is asserted to be the
    //       SANITIZED one. Asserting only the file would pass for a daemon that had stopped
    //       sanitizing its published surface; asserting only the wire would pass for one
    //       that persists the display form. The pair is what pins the two boundaries apart.
    // CONTROL: `{kKeyPath, entry.path}` in ConfigStore::save() replaced with
    //       `sanitizeForLog(entry.path, 4096, LogFraming::Bare)` -> three FAILs: "the
    //       daemon persists the path it was given, byte for byte" ("persisted:
    //       ...system_persist_\xe2\x80\xae\x7f\nblue.png"), "GetStatus publishes the
    //       restored path in its sanitized form" (it publishes the escaped form escaped
    //       again), and "the restored wallpaper renders in the new process without any
    //       client asking" -- the sanitized name names no file. Reverted.
    std::string why;
    const std::map<int, PersistedEntry> saved = configEntries(configHome.configFile(), why);
    const bool savedEntry = saved.count(kMonitorRestore) == 1;
    check(savedEntry, "the daemon writes the wallpaper it was given into $XDG_CONFIG_HOME/wallpaperx/config.json");
    if (!savedEntry) {
        std::fprintf(stderr, "  %s\n", why.empty() ? "the entry is not in the file" : why.c_str());
    } else {
        const bool byteIdentical = saved.at(kMonitorRestore).path == nastyPath;
        check(byteIdentical, "the daemon persists the path it was given, byte for byte");
        if (!byteIdentical) {
            std::fprintf(stderr, "  set:       %s\n  persisted: %s\n", nastyPath.c_str(),
                         saved.at(kMonitorRestore).path.c_str());
        }
        check(saved.at(kMonitorRestore).type == "static", "the persisted entry carries the wallpaper's type");
    }

    // Sampled while no daemon is running, so anything that moves them belongs to the
    // restart. See the S8.5 check at the end of this phase.
    std::error_code timeEc;
    const std::string bytesBeforeRestart = readTextFile(configHome.configFile());
    const auto mtimeBeforeRestart = std::filesystem::last_write_time(configHome.configFile(), timeEc);

    // Subscribed BEFORE the restarted daemon is spawned, and after the first daemon's name
    // is provably gone. The restore renders on the daemon's own initiative, and main.cpp
    // acquires the bus name before it queues that render -- so a subscription made after
    // "the name is up" opens its window after the signal can already have been emitted, and
    // FrameReadyLog scans a history that starts empty. The window is wide today only because
    // the first restore render pays for Vulkan device creation. A match rule naming a
    // well-known sender is resolved by the bus at delivery time, so installing it while
    // nobody owns the name still delivers the new daemon's signals. `restored` is declared
    // before the proxy so it outlives the dispatch thread that writes to it.
    FrameReadyLog restored;
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&restored](int32_t monitorId, uint64_t generation) { restored.record(monitorId, generation); });

    // The restart. No client call of any kind against this daemon before the assertions:
    // that is the property.
    SpawnedDaemon second({validationEnv, configHome.env()});
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(second, kGoals)) {
            notVerified(kGoals, "the restarted daemon never acquired the bus name");
        }
        return;
    }

    // GOAL: S8.1 -- a wallpaper set before a restart is on the same monitor after it, with
    //       no client acting. This is the whole of Step 8; everything else constrains how
    //       it is reached. False if the monitor comes back reporting nothing set.
    // WHY-THIS-SHAPE: the STATE is graded unconditionally and the FRAME only when the
    //       daemon before the restart managed to render. A frame-only check would report a
    //       host with no working Vulkan as a broken restore -- and a state-only check would
    //       pass for a daemon that restored a path it can no longer open, which is exactly
    //       what persisting the sanitized form produces. The frame subscription is installed
    //       before the restarted daemon is spawned, so no startup render can be emitted
    //       unobserved -- subscribing after the name appears would not do, since the daemon
    //       owns its name before it queues that render.
    // CONTROL: `service.restoreSaved();` deleted from backend/daemon/main.cpp -> seven FAILs, led by "the
    //       wallpaper set before the restart is on that monitor after it, with no client
    //       asking" and "the restored wallpaper renders in the new process without any
    //       client asking", and including this phase's two S8.11 lines and the
    //       unreadable-restore phase's three. Reverted.
    const StatusReply afterRestart = getStatusOrExplain(*proxy);
    const nlohmann::json* entry = wallpaperStatus(afterRestart, std::to_string(kMonitorRestore));
    check(entry != nullptr,
          "the wallpaper set before the restart is on that monitor after it, with no client asking");
    if (entry != nullptr) {
        check(entry->value("type", std::string()) == "static", "the restored monitor carries the type it was set with");
        // GetStatus publishes the sanitized path -- display-safe, not round-trip-safe --
        // while the file above holds the raw bytes. Both are asserted, because the defect
        // this pair rules out is the two surfaces being fed from the same string.
        const std::string published = entry->value("path", std::string());
        const std::string expected = wallpaperx::sanitizeForLog(nastyPath, 4096, wallpaperx::LogFraming::Bare);
        check(published == expected, "GetStatus publishes the restored path in its sanitized form");
        if (published != expected) {
            std::fprintf(stderr, "  published: %s\n  expected:  %s\n", published.c_str(), expected.c_str());
        }
    }

    if (!firstRendered) {
        notVerified("S8.1's render half",
                    "the daemon before the restart never rendered either, so a missing frame here says nothing "
                    "about the restore");
    } else {
        // Generation 1: this is a fresh process, and the counter lives in it, not in the
        // file. A restored wallpaper is that process's first render.
        check(restored.wait(kMonitorRestore, 1),
              "the restored wallpaper renders in the new process without any client asking");

        // GOAL: S8.11, second half -- Pending is NOT what a daemon that restored a
        //       wallpaper reports, because it resolved the GPU at startup on its own.
        // WHY-THIS-SHAPE: read after the FrameReady above, so the render is known to have
        //       committed rather than assumed; polling GetStatus until initialized flips
        //       would pass on a timeout if it were written the other way round.
        // CONTROL: the same `service.restoreSaved()` deletion -> "FAIL: a daemon that
        //       restored a wallpaper has resolved its GPU with no client involved" and
        //       "FAIL: /health no longer reports the GPU as untried once a restore has
        //       rendered". Reverted.
        const StatusReply afterRender = getStatusOrExplain(*proxy);
        if (afterRender.ok()) {
            check(afterRender.json().value("gpu", nlohmann::json::object()).value("initialized", false),
                  "a daemon that restored a wallpaper has resolved its GPU with no client involved");
            nlohmann::json health;
            std::string healthWhy;
            if (!fetchHealth(afterRender, health, healthWhy, check)) {
                if (!healthWhy.empty()) notVerified("S8.11's /health half after a restore", healthWhy);
            } else {
                const std::string gpu = healthCheckStatusOf(health, "gpu");
                check(gpu != "pending" && !gpu.empty(),
                      "/health no longer reports the GPU as untried once a restore has rendered");
                if (gpu == "pending" || gpu.empty()) {
                    std::fprintf(stderr, "  /health gpu: '%s'\n", gpu.c_str());
                }
            }
        }
    }

    // GOAL: S8.5 end to end -- a daemon that restores entries needing no repair does not write
    //       the config file. In process this is asserted on a hand-written file
    //       (config_store_test); here it is asserted of the shipped binary, where main.cpp's
    //       own call order is what could break it. The other half of the rule is the opposite
    //       property: a restore that DOES repair something writes the repair at that start,
    //       which config_store_test's startup-repair phase grades. This fixture reaches neither
    //       -- it is the file the first daemon wrote from one valid SetWallpaper, so there is
    //       nothing in it to repair.
    // WHY-THIS-SHAPE: the MTIME is what is graded, not the bytes. Bytes cannot work at this
    //       tier and that is not obvious: the file under test was written by the first
    //       daemon's own save(), so a second save reproduces it byte for byte -- measured, a
    //       a `persist(...)` of the save rows added to restoreSaved() left a byte comparison
    //       completely green. Both samples are taken while no daemon is running, so nothing
    //       but the restart can have moved either.
    // CONTROL: the `if (repaired)` guard on `m_repair = m_kept.rowsToWrite(liveWallpapers());`
    //       at the end of WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp)
    //       deleted, so every start hands writeRepairedConfig() a snapshot -> "FAIL: a daemon
    //       that restores entries needing no repair leaves the config file exactly as it found
    //       it", with "  the file's mtime moved across a start that only restored". Six more went
    //       red with it, all in the unreadable-config phases: an empty snapshot written after the
    //       file was moved aside recreates config.json and replaces the detail saying why it was
    //       refused. Re-observed 2026-08-19 in the row builder's new home. Reverted. This tier is
    //       now the only one that answers for the rule: config_store_test's twin reads the file
    //       inside the manager's lifetime, and since the repair write became asynchronous that
    //       read beats it.
    second.shutdown(SIGTERM);
    awaitNameRelease(names, "the phase after the config restart");
    std::error_code afterEc;
    const auto mtimeAfterRestart = std::filesystem::last_write_time(configHome.configFile(), afterEc);
    const bool untouched = !bytesBeforeRestart.empty() && !timeEc && !afterEc &&
                           mtimeAfterRestart == mtimeBeforeRestart &&
                           readTextFile(configHome.configFile()) == bytesBeforeRestart;
    check(untouched,
          "a daemon that restores entries needing no repair leaves the config file exactly as it found it");
    if (!untouched) {
        std::fprintf(stderr, "  %s\n",
                     mtimeAfterRestart != mtimeBeforeRestart
                         ? "the file's mtime moved across a start that only restored"
                         : "the file's contents changed, or it could not be read on both sides of the restart");
    }
}

// ---------------------------------------------------------------------------- //
// Step 10 -- the connector name a plugin identifies itself by, across a restart.
// ---------------------------------------------------------------------------- //
//
// Everything else about connector names is graded in process: the store keeps the field
// (config_store_test), the manager rebinds what it reads (config_store_test), the bus
// records it (dbus_service_test), and /config resolves it (http_server_test). None of
// those crosses a process boundary, and S10.5 is about exactly that boundary -- a screen's
// name has to come back in a daemon that was started fresh, from the file alone, with no
// client to re-tell it. A plugin instance cannot re-tell it: only the app can, and the app
// may not run for weeks.
void checkOutputRestart(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    using wallpaperx::testing::boundMonitor;
    using wallpaperx::testing::HttpReply;
    using wallpaperx::testing::setMonitorOutput;

    constexpr int kPort = wallpaperx::HttpServer::kDefaultPort;
    // This phase drives a daemon and an XDG_CONFIG_HOME of its own, so these ids are only
    // ever compared against its own -- the rule the id table at the top of this file sets
    // out for a phase that does not share a daemon with any other.
    constexpr int32_t kMonitorNamedA = 18;
    constexpr int32_t kMonitorNamedB = 19;
    constexpr const char* kGoals = "S10.5 (a connector name a plugin can still identify itself by after a restart)";

    const std::string nameA = "DP-3";
    const std::string nameB = "HDMI-A-1";
    const std::string neverAssigned = "DVI-I-99-never-told";

    ScratchDir configHome("outputrestart");
    if (!configHome.ok()) {
        notVerified(kGoals, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }

    // COVERAGE-ASSERT: every "not the other screen's wallpaper" clause below is vacuous if
    // the two screens share a file. Pinned at build time rather than by a check(): the two
    // names are literals in this source, so no behaviour of the daemon, the host or the
    // harness can make a runtime comparison of them false -- it would occupy a line in the
    // pass/fail report that nothing could ever turn red. Giving both calls the same name now
    // stops the file compiling instead.
    constexpr std::string_view kImageA = "system_output_a.png";
    constexpr std::string_view kImageB = "system_output_b.png";
    static_assert(kImageA != kImageB, "the two screens this phase tells apart need two different wallpapers");

    std::string pathA;
    std::string pathB;
    try {
        pathA = writeSolidImage(kImageA.data(), 0, 0, 255);
        pathB = writeSolidImage(kImageB.data(), 0, 255, 0);
    } catch (const std::exception& e) {
        notVerified(kGoals, std::string("the two wallpapers this phase tells apart could not be written: ") + e.what());
        return;
    }

    // Both halves of each row, not the name alone: the checks after the restart compare the
    // wallpaper path as well, so a row that reached disk carrying the name and no path would
    // be graded as the restore having lost it. `configWhy` is rewritten on every call and so
    // always describes the most recent read.
    std::string configWhy;
    const auto rowsOnDisk = [&] {
        const std::map<int, PersistedEntry> saved = configEntries(configHome.configFile(), configWhy);
        const auto rowPersisted = [&saved](int32_t monitorId, const std::string& name, const std::string& path) {
            const auto it = saved.find(monitorId);
            return it != saved.end() && it->second.output == name && it->second.path == path;
        };
        return rowPersisted(kMonitorNamedA, nameA, pathA) && rowPersisted(kMonitorNamedB, nameB, pathB);
    };

    // The first daemon's own verdict on whether it could write its config file, taken while
    // it is still alive and used only if the rows turn out to be missing from that file.
    // "fail" here is a host that would not take the write, not a defect in the restore.
    std::string persistStatus;
    std::string persistDetail;
    {
        SpawnedDaemon first({validationEnv, configHome.env()});
        if (!names.waitFor(true)) {
            if (!reportedStartupFault(first, kGoals)) {
                notVerified(kGoals, "the first daemon of this phase never acquired the bus name");
            }
            // Left through the same door as every other exit: the next phase reads ownership
            // from the bus, not from the process table, so a reap alone is not the barrier.
            first.shutdown(SIGTERM);
            awaitNameRelease(names, "the phase after the connector-name restart");
            return;
        }
        try {
            auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                            sdbus::ObjectPath{DbusService::kObjectPath});
            setWallpaper(*proxy, "static", pathA, kMonitorNamedA);
            setWallpaper(*proxy, "static", pathB, kMonitorNamedB);
            setMonitorOutput(*proxy, kMonitorNamedA, nameA);
            setMonitorOutput(*proxy, kMonitorNamedB, nameB);
            // A bounded wait for the write to land, not a single health sample taken the
            // instant the last call returned. WallpaperManager::persist() waits on the write
            // worker only until its own deadline, and once a slow filesystem has marked that
            // worker stalled every later call enqueues and returns without waiting at all --
            // so "SetMonitorOutput returned" is not evidence of a write, and killing the
            // daemon here would discard a snapshot still in flight. Reading the file while
            // the daemon runs is safe: ConfigStore::save() renames a temporary over it, so a
            // reader sees the old file or the new one, never a torn one.
            constexpr auto kPersistBound = std::chrono::seconds(10);
            const auto persistDeadline = Clock::now() + kPersistBound;
            bool landed = false;
            while (true) {
                landed = rowsOnDisk();
                if (landed || Clock::now() >= persistDeadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            // Sampled where the wait gave up, so it reports the daemon's persistence health
            // at the moment the rows were last found missing rather than before the writes.
            if (!landed) {
                const StatusReply wrote = getStatusOrExplain(*proxy);
                if (wrote.ok() && wrote.json().contains("persistence") && wrote.json()["persistence"].is_object()) {
                    const nlohmann::json& persistence = wrote.json()["persistence"];
                    if (persistence.contains("status") && persistence["status"].is_string()) {
                        persistStatus = persistence["status"].get<std::string>();
                    }
                    if (persistence.contains("detail") && persistence["detail"].is_string()) {
                        persistDetail = persistence["detail"].get<std::string>();
                    }
                }
            }
        } catch (const sdbus::Error& e) {
            // Above the std::exception handler, which this derives from, or it would be dead
            // code. X.3 in the direction that is easy to get backwards: a name the daemon
            // itself chose means the daemon refused an ordinary screen id and an ordinary
            // connector name, which is its defect -- filing that as a skip would delete this
            // phase's only end-to-end grading of the restore. Every other name (the
            // transport's, and org.sdbuscpp.Error, which is what an exception escaping a
            // handler becomes) is unattributable and keeps the skip.
            //
            // daemonErrorName() alone is not enough here, and measuring it is the only way to
            // find that out: DbusService rejects a bad argument under the STANDARD name
            // org.freedesktop.DBus.Error.InvalidArgs (kErrorInvalidArgs in
            // backend/daemon/dbus_service.cpp), which is outside the daemon's own namespace,
            // so a daemon that refused "DP-3" would still have skipped. That name is safe to
            // attribute here because the bus does not generate it for a well-formed call to a
            // service that exists -- only the callee marshalling arguments does.
            //
            // CONTROL: both arms, both reverted. DbusService::setMonitorOutput
            //       (backend/daemon/dbus_service.cpp) made to throw sdbus::Error under
            //       kErrorInvalidArgs for every call -> "FAIL: the first daemon records a
            //       connector name for a valid screen id and name"; with daemonErrorName()
            //       as the only test the same daemon produced a skip and no FAIL at all.
            //       Made to throw std::runtime_error instead, which sdbus-c++ delivers as
            //       org.sdbuscpp.Error -> the skip below, naming that error.
            constexpr const char* kRefusedArgs = "org.freedesktop.DBus.Error.InvalidArgs";
            const std::string name = e.getName();
            const std::string message = e.getMessage();
            if (wallpaperx::testing::daemonErrorName(name) || name == kRefusedArgs) {
                check(false, "the first daemon records a connector name for a valid screen id and name");
                std::fprintf(stderr, "  %s: %s -- so the restart below was not graded\n", name.c_str(),
                             message.c_str());
            } else {
                notVerified(kGoals,
                            std::string("the first daemon would not record the two screen names this phase restarts "
                                        "with: ") +
                                name + ": " + message);
            }
            first.shutdown(SIGTERM);
            awaitNameRelease(names, "the phase after the connector-name restart");
            return;
        } catch (const std::exception& e) {
            // X.3: the precondition could not be built, so nothing below is graded against
            // the daemon's restore. A skip naming the cause, not a FAIL against the restart.
            notVerified(kGoals,
                        std::string("the first daemon would not record the two screen names this phase restarts "
                                    "with: ") +
                            e.what());
            first.shutdown(SIGTERM);
            awaitNameRelease(names, "the phase after the connector-name restart");
            return;
        }
        // No FrameReady wait: this phase grades stored identity, not pixels. Naming a screen
        // starts no render at all, and the wallpapers are here only so the restored name has
        // something of its own to resolve to.
        first.shutdown(SIGTERM);
        awaitNameRelease(names, "the restarted daemon of the connector-name phase");
    }

    // GOAL: the precondition every check below rests on -- the first daemon put both rows,
    //       connector name and wallpaper path, in $XDG_CONFIG_HOME/wallpaperx/config.json.
    //       False if SetMonitorOutput returns and the file does not hold them.
    // WHY-THIS-SHAPE: "SetMonitorOutput returned" is not evidence of a write, so the write is
    //       given a bounded chance to land above and graded here, once the daemon has exited
    //       and nothing can still be moving the file. Both fields, because the checks after
    //       the restart consume both: a gate on the name alone lets a lost path arrive as
    //       "the restarted daemon answers that screen's name with that screen's wallpaper"
    //       going red for a write that never happened.
    //       The FAIL-versus-skip split is X.3, and the health field is only ever read on the
    //       failing side: a daemon reporting persistence ok after the wait expired is a
    //       daemon lying about a write, which is its defect; a daemon reporting a persistence
    //       failure is the host refusing the write, which is not. A host that is merely slow
    //       completes inside the wait and reaches neither arm.
    // CONTROL: three, all reverted, and they must land on different arms. (a)
    //       `row[kKeyOutput] = entry.output;` disabled in ConfigStore::save()
    //       (backend/daemon/config_store.cpp), so the name is accepted and never written ->
    //       "FAIL: the daemon writes the connector name and the wallpaper it was given into
    //       $XDG_CONFIG_HOME/wallpaperx/config.json", printing both rows of the file it did
    //       write, and NONE of the restore checks ran. (b) the same save() made to write a
    //       different path for any row carrying a name -- the half a name-only gate would let
    //       through -> the SAME failure above, where under a gate comparing only the name the
    //       identical daemon produced "FAIL: the restarted daemon answers that screen's name
    //       with that screen's wallpaper (DP-3)" and again for HDMI-A-1, accusing the restore
    //       of a write that never happened. (c) the same save() made to return an error
    //       unconditionally, which is the host refusing the write -> no FAIL from this phase
    //       at all, only "NOT VERIFIED -- ... the first daemon could not persist the two
    //       screen rows (it reports persistence fail: ...)".
    if (!rowsOnDisk()) {
        if (persistStatus == "ok") {
            check(false, "the daemon writes the connector name and the wallpaper it was given into "
                         "$XDG_CONFIG_HOME/wallpaperx/config.json");
            std::fprintf(stderr, "  %s\n  config file: %s\n",
                         configWhy.empty() ? "the two rows are not in the file" : configWhy.c_str(),
                         readTextFile(configHome.configFile()).c_str());
        } else {
            notVerified(kGoals, std::string("the first daemon could not persist the two screen rows (it reports "
                                            "persistence ") +
                                    (persistStatus.empty() ? std::string("(absent)") : persistStatus) + ": " +
                                    (persistDetail.empty() ? std::string("no detail") : persistDetail) +
                                    "), so the restart below would grade a restore against a file that was never "
                                    "written");
        }
        return;
    }

    // The restart. No client call of any kind against this daemon before the assertions --
    // no SetMonitorOutput, no SetWallpaper -- because the reads below are the whole point:
    // a check that named the screens again would establish nothing about the file.
    SpawnedDaemon second({validationEnv, configHome.env()});
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(second, kGoals)) {
            notVerified(kGoals, "the restarted daemon never acquired the bus name");
        }
        // As the first daemon's equivalent exit: the destructor kills and reaps, which is not
        // the barrier the next phase reads. Without the release wait the next phase's daemon
        // can lose its own name request to a name the bus has not dropped yet, and report as
        // a second instance.
        second.shutdown(SIGTERM);
        awaitNameRelease(names, "the phase after the connector-name restart");
        return;
    }
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

    // GOAL: S10.5 on the DBus surface -- a connector name bound before a restart resolves to
    //       the SAME monitor in a daemon that was started fresh and told nothing. False if a
    //       named screen comes back anonymous, or bound to another monitor.
    // WHY-THIS-SHAPE: read straight after the bus name appears, with no poll and no retry.
    //       That is sound because backend/daemon/main.cpp calls restoreSaved() BEFORE
    //       requestName(), so the name appearing on the bus already means the file has been
    //       read -- the wait for the name IS the barrier. A poll-until-bound loop would be
    //       the dangerous alternative: its success path is reachable by waiting, so a daemon
    //       that bound the names late for the wrong reason would still pass, and a daemon
    //       that never bound them at all would spend the timeout and then be graded on
    //       whatever the last sample happened to hold. TWO screens with two names, because
    //       one cannot distinguish a restored map from a map with a single entry in it.
    // CONTROL: the whole `if (nameValid && entry.monitorId < kMaxMonitors &&
    //       bindOutputLocked(entry.monitorId, entry.output) == BindResult::Displaced)`
    //       statement in WallpaperManager::restoreSaved()
    //       (backend/daemon/wallpaper_manager.cpp) deleted -- the bind is a side effect of
    //       the CONDITION, so removing only the `++displacedNames;` body disables nothing
    //       and leaves every check here green. With the statement gone: "FAIL: both
    //       connector names resolve to their own monitors in a daemon that was told
    //       nothing", printing "'DP-3' -> -1 (expected 18), 'HDMI-A-1' -> -1 (expected 19)"
    //       and the config file it read them out of, plus the four /config lines below. The
    //       reply arrives either way under that mutation -- it simply carries no entry for
    //       either name -- so the "answers GetStatus" line stays green and only the restore
    //       line goes red. Reverted.
    //       Whether the reply arrived is graded on its own line rather than left to
    //       boundMonitor()'s sentinel: a StatusReply that failed carries an empty object, so
    //       both names come back as -1 and an unanswered call would otherwise print as the
    //       restore having lost them both. The unanswered case is still red -- a daemon
    //       holding the bus name and not answering is the daemon's defect -- and the skip
    //       beside it keeps the restore property visible as untested rather than absent.
    //       CONTROL: DbusService::getStatus (backend/daemon/dbus_service.cpp) made to throw,
    //       so the reply never arrives -> "FAIL: the restarted daemon answers GetStatus" plus
    //       "NOT VERIFIED -- ... the restarted daemon did not answer GetStatus, so the
    //       connector-name map could not be read over the bus", and the restore line was NOT
    //       printed -- which is the accusation this guard removes. Reverted.
    const StatusReply afterRestart = getStatusOrExplain(*proxy);
    check(afterRestart.ok(), "the restarted daemon answers GetStatus");
    if (!afterRestart.ok()) {
        notVerified(kGoals, "the restarted daemon did not answer GetStatus, so the connector-name map could not be "
                            "read over the bus; the GET /config half below still grades the restore");
    } else {
        const int boundA = boundMonitor(afterRestart, nameA);
        const int boundB = boundMonitor(afterRestart, nameB);
        const bool bothBound = boundA == kMonitorNamedA && boundB == kMonitorNamedB;
        check(bothBound, "both connector names resolve to their own monitors in a daemon that was told nothing");
        if (!bothBound) {
            std::fprintf(stderr, "  '%s' -> %d (expected %d), '%s' -> %d (expected %d)\n  config file: %s\n",
                         nameA.c_str(), boundA, kMonitorNamedA, nameB.c_str(), boundB, kMonitorNamedB,
                         readTextFile(configHome.configFile()).c_str());
        }
    }

    // Whether this daemon is the process on 18462 decides whether the HTTP half can run at
    // all: a developer's own daemon holding the port is a skip, a restarted daemon killed by
    // the machine is a skip, a restarted daemon that quit of its own accord is a FAIL, and a
    // drifted log literal is a FAIL. They are told apart in awaitConfigServerBind, shared
    // with the HTTP lifecycle phase so the two cannot drift.
    if (awaitConfigServerBind(second, kPort, "S10.5's GET /config?output= half", "the connector-name phase", check)) {
        // GOAL: S10.5 on the surface the plugin actually uses -- GET /config?output=<name>
        //       answers "known": true with THAT screen's wallpaper, in a daemon that has
        //       never been told the name. False if it answers known=false, which is the
        //       state GOALS.md names, and equally false if it answers another screen's
        //       wallpaper.
        // WHY-THIS-SHAPE: asserted beside the GetStatus check above rather than instead of
        //       it, because the two can disagree and only this one is what a plugin instance
        //       reads. It is also over a real socket to a real process, so a manager that
        //       restored the map but an HTTP route that consulted something else would be
        //       caught here and nowhere else. The never-assigned name is asked in the same
        //       breath: without it "known": true is satisfied by a daemon that says yes to
        //       every name, and then the whole endpoint means nothing after a restart.
        //       The query is built by pluginPoll() rather than as a bare `output=<name>`,
        //       because the bare form is not the shape this GOAL claims: the plugin appends
        //       a cache-buster to every poll, so a daemon that started refusing requests
        //       carrying one would answer this phase and fail every real screen.
        // CONTROL: the same restoreSaved() bind statement deleted (the one quoted in full
        //       above) -> "FAIL: a screen named before the restart is known to the restarted
        //       daemon over HTTP (DP-3)" and "FAIL: the restarted daemon answers that
        //       screen's name with that screen's wallpaper (DP-3)", both again for
        //       HDMI-A-1, each printing `{"known":false,"monitor_id":null,...}`, while the
        //       never-assigned line stayed green -- which is what keeps this about the
        //       restore rather than about the field. Four more, all in the /config handler
        //       in backend/daemon/http_server.cpp and all reverted. `known = bound !=
        //       snap.outputs.end()` forced to `known = true`, with the id assignment guarded
        //       so an unknown name does not dereference end() -> "FAIL: a name the restarted
        //       daemon was never given is still reported as not known" alone, which is the
        //       line that stops known=true being a constant. `monitorId = bound->second`
        //       replaced by `snap.outputs.begin()->second`, the shape of a lookup that
        //       ignores which name was asked for -> "FAIL: the restarted daemon answers that
        //       screen's name with that screen's wallpaper (HDMI-A-1)" and "FAIL: a restored
        //       screen is never answered with the other screen's wallpaper (HDMI-A-1)",
        //       HDMI-A-1 coming back with monitor 18 and DP-3's file. The output arm made to
        //       throw, so it answers through its own 500 handler -> all seven lines of this
        //       block red, both cross-serve lines among them; with that clause gated on
        //       `reached` instead of `ok()` the same 500 left both of them green. And
        //       `out["known"] = nullptr` on the output arm -> five red (both known lines, both
        //       own-wallpaper lines, and the never-assigned one) and the run finished; with
        //       `value("known", false)` in place of the is_boolean() read, the same daemon
        //       killed the run with "FAIL: unexpected exception: [json.exception.type_error
        //       .302] type must be boolean, but is null" as the only line, and the eight later
        //       phases never ran. For pluginPoll()'s `t=` in particular: parseIdentity() in
        //       backend/daemon/http_server.cpp made to answer 400 for any parameter other
        //       than monitor and output -> all seven lines red, each printing `answered 400:
        //       {"error":"..."}`, where the bare `output=<name>` this replaced stayed green
        //       under the same daemon. Reverted.
        // COVERAGE-ASSERT: two static_asserts, one on the wallpaper names above and one on
        //       the table's size below, so neither the loop nor the not-the-other-screen's
        //       clause can be hollowed out without the file failing to compile.
        httplib::Client cli = wallpaperx::testing::makeClient(kPort);
        // The plugin's own query, cache-buster and all: main.qml builds
        // `output=<name>&t=<Date.now()>` on every poll. The names here are plain ASCII, so
        // the percent-encoding main.qml also applies is the identity on them and is left out
        // rather than reimplemented.
        const auto pluginPoll = [](const std::string& name) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            return "/config?output=" + name + "&t=" + std::to_string(now);
        };
        const struct {
            const std::string& name;
            int32_t monitorId;
            const std::string& path;
            const std::string& otherPath;
        } kNamed[] = {
            {nameA, kMonitorNamedA, pathA, pathB},
            {nameB, kMonitorNamedB, pathB, pathA},
        };
        // A build-time assertion, not a check(): the table's size is fixed by this source, so
        // an edit that halved it should stop the file compiling rather than quietly leave the
        // loop asserting about one screen -- which is the case that cannot tell a lookup from
        // a constant.
        static_assert(std::size(kNamed) == 2, "this phase needs two named screens to tell a lookup from a constant");
        for (const auto& screen : kNamed) {
            const HttpReply res = wallpaperx::testing::get(cli, pluginPoll(screen.name));
            // Every field read through a type predicate, never through value(): value()
            // converts rather than defaults, so it throws type_error.302 on a present null.
            // The handler's own not-known path emits `out["monitor_id"] = nullptr`
            // (backend/daemon/http_server.cpp), so the ordinary unknown-name response is
            // already enough to throw. `known` it emits as a boolean, `out["known"] =
            // known`, and the predicate there guards against that type drifting -- forcing
            // it to null is the mutation in the CONTROL above. A throw abandons this phase
            // and the eight after it behind one opaque line, where what is wanted is the
            // named FAIL (X.4).
            const bool known = res.ok() && res.json.contains("known") && res.json["known"].is_boolean() &&
                               res.json["known"].get<bool>();
            check(known, ("a screen named before the restart is known to the restarted daemon over HTTP (" +
                          screen.name + ")")
                             .c_str());
            const bool literalMatches = known && res.json.contains("path_literal") &&
                                        res.json["path_literal"].is_string() &&
                                        res.json["path_literal"].get<std::string>() == screen.path;
            const bool servedItsOwn = literalMatches && res.json.contains("monitor_id") &&
                                      res.json["monitor_id"].is_number_integer() &&
                                      res.json["monitor_id"].get<int>() == screen.monitorId;
            check(servedItsOwn, ("the restarted daemon answers that screen's name with that screen's wallpaper (" +
                                 screen.name + ")")
                                    .c_str());
            // ok(), not reached(): `reached` is true of any completed exchange, so a 500
            // from the handler's exception path -- body `{"error":"internal error"}`, which
            // contains no wallpaper path at all -- would pass this as a screen correctly
            // served. Named per screen because only one of the two iterations trips under
            // the cross-wiring defect this exists to catch, and an unqualified line does not
            // say which.
            const bool notCrossServed = res.ok() && res.body.find(screen.otherPath) == std::string::npos;
            check(notCrossServed, ("a restored screen is never answered with the other screen's wallpaper (" +
                                   screen.name + ")")
                                      .c_str());
            if (!servedItsOwn || !notCrossServed) {
                std::fprintf(stderr, "  %s answered %d: %s\n", screen.name.c_str(), res.status, res.body.c_str());
            }
        }
        const HttpReply stranger = wallpaperx::testing::get(cli, pluginPoll(neverAssigned));
        const bool refused = stranger.ok() && stranger.hasJsonObject() && stranger.json.contains("known") &&
                             stranger.json["known"].is_boolean() && !stranger.json["known"].get<bool>();
        check(refused, "a name the restarted daemon was never given is still reported as not known");
        if (!refused) {
            std::fprintf(stderr, "  %s answered %d: %s\n", neverAssigned.c_str(), stranger.status,
                         stranger.body.c_str());
        }
    }

    second.shutdown(SIGTERM);
    awaitNameRelease(names, "the phase after the connector-name restart");
}

void checkStartsOnAnyConfig(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr const char* kGoals = "S8.2 (no config file content stops the daemon)";
    ScratchDir configHome("anycontent");
    if (!configHome.ok()) {
        notVerified(kGoals, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }

    std::string deep = "{\"version\":1,\"monitors\":";
    deep.append(50000, '[');
    deep.append(50000, ']');
    deep += "}";

    // GOAL: S8.2 -- no content whatsoever in config.json prevents the daemon from starting
    //       and serving DBus. The two failure mechanisms are DIFFERENT and both are here: a
    //       throw escaping the load reaches main.cpp's catch and exits 1, while a const
    //       `operator[]` on a missing key is a JSON_ASSERT -> SIGABRT that no catch can
    //       intercept. A check that covers only the throw is blind to the worse half.
    // WHY-THIS-SHAPE: exitedNormally() and the name on the bus per case, plus exit code 0 on
    //       every build a sanitizer has not taken the exit code away from. "The daemon
    //       exited non-zero" is satisfied by an abort (reap() reports -1 for both), so the
    //       abort half needs exitedNormally(); "it came up" alone would pass for a daemon
    //       that aborted later. Under a sanitizer the exit code is the tool's, so that arm
    //       is replaced by a FAIL naming the sanitizer report rather than by treating the
    //       report as satisfying the check -- which is what it used to do, making a data
    //       race in the config-load path print as a pass. The corpus leads with the shapes that reach a
    //       field read of parsed input -- `{}`, wrong types, a wrong-typed version -- since
    //       those are the ones a const operator[] would abort on.
    // CONTROL: two, both reverted, and they redden different cases -- which is the point.
    //       (a) config_store.cpp's version read replaced with `root[kKeyVersion].get<int>()`
    //       -> exactly two FAILs, "with valid JSON of the wrong shape in config.json, the
    //       daemon starts and serves DBus" and "... exits 0 on SIGTERM rather than
    //       aborting", with json.hpp's `it != m_data.m_value.object->end()` assertion in the
    //       daemon's captured output. config_store_test could not report that one at all:
    //       the same mutation killed it. (b) `json::parse(text, nullptr, false)` made
    //       throwing and load()'s catch made to rethrow -> eight FAILs, over the truncated,
    //       empty, not-JSON and 100 MB cases, each printing "exit code 1" with the daemon's
    //       "fatal: [json.exception.parse_error.101] ..." line. (c) for the sanitizer arm,
    //       which no unsanitized run can reach: the match string pointed at a line the daemon
    //       always prints -> nine FAILs, "with <case> in config.json, the daemon's sanitizer
    //       reported no warnings", where the shape this replaced printed nine passes.
    //       Reverted.
    // COVERAGE-ASSERT: every case is accounted for -- spawned, or reported as one this host
    //       could not write -- against the corpus size, so a case that silently stops running
    //       cannot thin this out. Counting only spawns turned a /tmp too small for the
    //       100 MB case into a FAIL against the daemon, which is a fault of the host.
    //       CONTROL: `if (content.size() > 1000) continue;` added to the loop, which is what
    //       a well-meaning "skip the slow ones" edit looks like -> "FAIL: every
    //       config-content case was either spawned or reported unwritable, so this phase is
    //       as wide as it reads", and nothing else. Reverted.
    std::vector<std::pair<const char*, std::string>> corpus{
        {"valid JSON of the wrong shape", "{}"},
        {"an object whose fields are all the wrong type",
         "{\"version\":\"1\",\"monitors\":{\"0\":{\"path\":42}}}"},
        {"entries whose fields are the wrong type",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":\"a\",\"type\":null,\"path\":[]}]}"},
        {"a file truncated mid-object", "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"stat"},
        {"an empty file", ""},
        {"a file that is not JSON at all", "wallpapers = all of them"},
        {"a JSON array where an object belongs", "[{\"monitor_id\":0}]"},
        {"50 000 levels of nesting", deep},
    };
    // Appended rather than braced in: an initializer_list's backing array is const, so the
    // vector copies each element and the 100 MB case would exist twice at once.
    corpus.reserve(corpus.size() + 1);
    corpus.emplace_back("100 MB of no JSON whatsoever", std::string(100u * 1024u * 1024u, 'x'));

    int exercised = 0;
    int unwritable = 0;
    for (const auto& [what, content] : corpus) {
        if (!writeTextFile(configHome.configFile(), content)) {
            ++unwritable;
            notVerified(kGoals, std::string("the config file for the '") + what + "' case could not be written");
            continue;
        }
        SpawnedDaemon daemon({validationEnv, configHome.env()});
        const bool cameUp = names.waitFor(true);
        if (!cameUp && reportedStartupFault(daemon, kGoals)) return;

        std::string served;
        if (cameUp) {
            auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                            sdbus::ObjectPath{DbusService::kObjectPath});
            const StatusReply status = getStatus(*proxy);
            if (!status.ok()) served = status.error();
        }
        const bool serving = cameUp && served.empty();
        check(serving, (std::string("with ") + what + " in config.json, the daemon starts and serves DBus").c_str());
        if (!serving) {
            std::fprintf(stderr, "  %s: %s\n", what,
                         cameUp ? served.c_str() : "the daemon never acquired the bus name");
        }

        const int exitCode = daemon.shutdown(SIGTERM);
        const std::string output = daemon.output();
        const bool sanitizerReported = output.find("Sanitizer: reported") != std::string::npos;
        // exitedNormally() is the half that sees an abort: a process killed by SIGABRT and
        // one that timed out both report exit code -1, so a check written on the code alone
        // cannot tell a crash from a clean refusal. It is asserted unconditionally.
        check(daemon.exitedNormally(),
              (std::string("with ") + what + " in config.json, the daemon exits through main() rather than aborting")
                  .c_str());
        if (!daemon.exitedNormally()) {
            std::fprintf(stderr, "  %s: the daemon did not exit normally (signal %d, timed out %d)\n", what,
                         daemon.termSignal(), static_cast<int>(daemon.timedOut()));
        }
        if (sanitizerReported) {
            check(false, (std::string("with ") + what +
                          " in config.json, the daemon's sanitizer reported no warnings (its exit code is the "
                          "sanitizer's here, so the exit-code check cannot run)")
                             .c_str());
        } else {
            check(exitCode == 0,
                  (std::string("with ") + what + " in config.json, the daemon exits 0 on SIGTERM").c_str());
            if (exitCode != 0) std::fprintf(stderr, "  %s: exit code %d\n", what, exitCode);
        }
        ++exercised;
        awaitNameRelease(names, "the next config-content case");
    }
    check(exercised + unwritable == static_cast<int>(corpus.size()),
          "every config-content case was either spawned or reported unwritable, so this phase is as wide as it "
          "reads");
}

void checkUnreadableRestore(NameWatcher& names, const std::string& validationEnv, const std::string& bluePath,
                            const Check& check) {
    constexpr int32_t kMonitorMissing = 19;
    constexpr int32_t kMonitorPresent = 28;
    // No build can ever apply this one: a monitor id is an index, so it is dropped rather
    // than kept. Carried in this phase's fixture because it already restores a config and
    // reads GetStatus back, so covering the drop here costs no extra daemon.
    constexpr int32_t kMonitorNegative = -3;
    constexpr const char* kGoals = "S8.4 (a wallpaper whose file is unreadable at restore is not forgotten) and "
                                   "S8.26 (an entry no build can apply is dropped, counted and named)";

    ScratchDir configHome("unreadable");
    if (!configHome.ok()) {
        notVerified(kGoals, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }
    const std::string missingPath = (configHome.path() / "unmounted-drive" / "wallpaper.png").string();
    // Serialised, not concatenated: $TMPDIR may hold a quote or a backslash, and a fixture
    // that is not valid JSON would make the daemon correctly restore nothing while this
    // phase reported it as a daemon that forgot an entry.
    const nlohmann::json doc{
        {"version", 1},
        {"monitors", nlohmann::json::array({{{"monitor_id", kMonitorMissing}, {"type", "static"}, {"path", missingPath}},
                                            {{"monitor_id", kMonitorNegative},
                                             {"type", "static"},
                                             {"path", "/no/build/can/apply/this.png"}}})}};
    const std::string config = doc.dump();
    if (!writeTextFile(configHome.configFile(), config)) {
        notVerified(kGoals, "the config file naming an unreadable wallpaper could not be written");
        return;
    }

    SpawnedDaemon daemon({validationEnv, configHome.env()});
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(daemon, kGoals)) {
            notVerified(kGoals, "the daemon for this phase never acquired the bus name");
        }
        return;
    }
    FrameReadyLog signals;
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

    // GOAL: S8.4, the reporting half -- the monitor whose file is not there says WHY it is
    //       blank, rather than reporting nothing set. The case is a drive that mounts after
    //       the daemon starts (PLAN.md decision 3), and the user has to be able to tell
    //       "your disk is not mounted" from "no wallpaper was ever set here".
    // WHY-THIS-SHAPE: a bounded poll whose expiry FAILS the check, not a sleep and a look.
    //       The restore's render is asynchronous and produces no signal when it fails --
    //       FrameReady only fires for a frame -- so there is nothing to wait on but the
    //       state itself. Written as a wait that falls through into the assertion, a stuck
    //       render would be graded as "no last_error yet" and pass.
    // CONTROL: two, both reverted. An `is_regular_file` guard added to
    //       WallpaperManager::restoreSaved()'s loop, i.e. the entry dropped at restore ->
    //       "FAIL: a restored monitor whose file is missing appears in GetStatus at all"
    //       plus the two checks below it. And renderPending's failure path made to leave
    //       lastError empty -> only "FAIL: a restored monitor whose file is missing says
    //       why it is blank", printing the monitor's whole GetStatus entry with no
    //       last_error in it -- so the two checks fail independently rather than together.
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    std::string lastError;
    bool present = false;
    StatusReply status;
    while (Clock::now() < deadline) {
        status = getStatus(*proxy);
        if (!status.ok()) break;
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorMissing));
        present = entry != nullptr;
        if (present) lastError = entry->value("last_error", std::string());
        if (!lastError.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!status.ok()) {
        check(false, "the daemon that restored an unreadable wallpaper answers GetStatus");
        std::fprintf(stderr, "  %s\n", status.error().c_str());
    } else {
        check(present, "a restored monitor whose file is missing appears in GetStatus at all");
        check(!lastError.empty(), "a restored monitor whose file is missing says why it is blank");
        if (present && lastError.empty()) {
            std::fprintf(stderr, "  monitor %d after the restore: %s\n", kMonitorMissing,
                         wallpaperStatus(status, std::to_string(kMonitorMissing))->dump().c_str());
        }

        // GOAL: S8.26 -- the daemon drops an entry no build could ever apply, counts it, and
        //       says so over the wire. False if the negative id becomes a monitor (it indexes
        //       an array, so nothing good happens next), and false if it vanishes in silence:
        //       unlike a kept entry, nothing else will ever mention a dropped one, so the
        //       count in this detail is the user's only notice that a wallpaper they set is
        //       not coming back.
        // WHY-THIS-SHAPE: graded here rather than only in config_store_test, because the
        //       count is produced by restoreSaved() and read back through the persistence
        //       detail, and only the daemon runs both -- an in-process check calls
        //       restoreSaved() itself. Asserted together with the monitor's absence, so it
        //       cannot pass for a daemon that counted a drop it did not make.
        // CONTROL: `m_kept.countDroppedEntry();` deleted from restoreSaved()'s negative-id
        //       branch (backend/daemon/wallpaper_manager.cpp) -> "FAIL: the daemon says over
        //       DBus how many entries no build can apply", printing "persistence detail:
        //       .../config.json". The absence check stayed green, which is what separates "not
        //       counted" from "not dropped". One more goes red with it -- "the daemon writes the
        //       repaired config once the copy has been taken" -- because the count also decides
        //       whether the start has a repair to write at all. Re-observed 2026-08-19 against
        //       KeptRows' counter. Reverted.
        check(wallpaperStatus(status, std::to_string(kMonitorNegative)) == nullptr,
              "an entry with a negative monitor id never becomes a monitor");
        const nlohmann::json& statusJson = status.json();
        const std::string persistence = statusJson.contains("persistence") && statusJson["persistence"].is_object()
                                            ? statusJson["persistence"].value("detail", std::string())
                                            : std::string("(absent)");
        const bool counted = persistence.find("1 entries dropped, no build can apply them") != std::string::npos;
        check(counted, "the daemon says over DBus how many entries no build can apply");
        if (!counted) std::fprintf(stderr, "  persistence detail: %s\n", persistence.c_str());
    }

    // GOAL: S8.4, the survival half -- the entry stays in the file across a save made for a
    //       DIFFERENT monitor. False if setting any other wallpaper drops it: the user's
    //       wallpaper is then gone for good the moment anything else is set, which is the
    //       state that makes a late-mounting disk permanent.
    // WHY-THIS-SHAPE: the second monitor is set over the real bus, which is the only write
    //       path a user has. Checking the file straight after the restore would pass for a
    //       daemon that drops the entry at its first save -- and something always sets a
    //       wallpaper eventually.
    // CONTROL: the same restoreSaved() readability guard as above -> "FAIL: the unreadable
    //       monitor's entry survives a save made for another monitor", printing "ids on
    //       disk: 28". Reverted.
    try {
        setWallpaper(*proxy, "static", bluePath, kMonitorPresent);
    } catch (const std::exception& e) {
        check(false, "the daemon accepts a wallpaper for another monitor after restoring an unreadable one");
        std::fprintf(stderr, "  SetWallpaper failed: %s\n", e.what());
    }
    std::string why;
    const std::map<int, PersistedEntry> onDisk = configEntries(configHome.configFile(), why);
    const bool survived = onDisk.count(kMonitorMissing) == 1 && onDisk.at(kMonitorMissing).path == missingPath &&
                          onDisk.count(kMonitorPresent) == 1;
    check(survived, "the unreadable monitor's entry survives a save made for another monitor");
    if (!survived) {
        std::fprintf(stderr, "  %s; ids on disk:", why.empty() ? "read back" : why.c_str());
        for (const auto& entry : onDisk) std::fprintf(stderr, " %d", entry.first);
        std::fprintf(stderr, "\n");
    }

    daemon.shutdown(SIGTERM);
    awaitNameRelease(names, "the phase after the unreadable-restore phase");
}

void checkConfigLocation(NameWatcher& names, const std::string& validationEnv, const std::string& bluePath,
                         const Check& check) {
    constexpr int32_t kMonitorLocation = 29;
    constexpr const char* kGoals = "S8.9 (nothing is written outside the XDG location)";

    ScratchDir home("home");
    if (!home.ok()) {
        notVerified(kGoals, "the scratch HOME this phase inspects could not be created");
        return;
    }

    // GOAL: S8.9 -- with neither $XDG_CONFIG_HOME nor $HOME usable, persistence is DISABLED
    //       and no file is created anywhere, in particular not in the working directory; and
    //       a RELATIVE $XDG_CONFIG_HOME is ignored per the basedir spec rather than resolved
    //       against that working directory. False if a daemon started from a directory it
    //       does not own leaves a config.json in it.
    // WHY-THIS-SHAPE: the daemon is spawned with a working directory of this phase's own
    //       (SpawnedDaemon chdirs in the child, between fork and exec), and the whole tree
    //       is listed afterwards. Asserting only "no config.json in the cwd" would miss a
    //       daemon that created wallpaperx/ or relative/config/ there; listing everything
    //       makes the failure name what it found. The daemon's cwd is read back out of
    //       /proc before anything is graded, because a chdir that silently failed would
    //       leave this phase inspecting an empty directory and passing.
    // CONTROL: two, both reverted. `return {};` in defaultConfigPath() replaced with
    //       `current_path() / kAppDir / kFileName` -> three FAILs on the first case: "with
    //       no usable XDG_CONFIG_HOME or HOME, the daemon writes nothing into its working
    //       directory" (listing wallpaperx/ and wallpaperx/config.json under the cwd), "a
    //       daemon with nowhere to persist reports persistence disabled over DBus", and "...
    //       says so on its way up". And the `is_absolute()` guard removed from the XDG
    //       branch -> two on the second case: "with a relative XDG_CONFIG_HOME, the daemon
    //       writes nothing into its working directory" (listing relative/config/wallpaperx/
    //       config.json) and "with a relative XDG_CONFIG_HOME the daemon persists under
    //       $HOME/.config instead".
    struct LocationCase {
        const char* what;
        std::vector<std::string> env;
        bool expectHomeFile;
        const char* expectDisabledLine;
    };
    const std::vector<LocationCase> cases{
        {"no usable XDG_CONFIG_HOME or HOME",
         {validationEnv, "XDG_CONFIG_HOME=", "HOME="},
         false,
         "wallpapers will not survive a restart"},
        {"a relative XDG_CONFIG_HOME",
         {validationEnv, "XDG_CONFIG_HOME=relative/config", "HOME=" + home.path().string()},
         true,
         nullptr},
    };

    for (const LocationCase& c : cases) {
        // A working directory per case, not one shared by both. With one shared directory
        // the second case inherits whatever the first left there, so a control that makes
        // the daemon write into its cwd reddens both cases and the second one's failure
        // names the first one's file -- an attribution this phase exists to get right.
        ScratchDir working("cwd");
        if (!working.ok()) {
            notVerified(kGoals, std::string("the working directory for the '") + c.what +
                                    "' case could not be created");
            return;
        }
        SpawnedDaemon daemon(c.env, SpawnedDaemon::Stdout::Captured, working.path().string());
        // A refused chdir is one of forked_child.h's preamble codes, so reportedStartupFault
        // names it. Comparing waitForExit() here instead blocked for the full process
        // timeout on every ordinary "the name never appeared" failure, and only this phase
        // would ever have recognised the code.
        if (!names.waitFor(true)) {
            if (reportedStartupFault(daemon, kGoals)) return;
            notVerified(kGoals, std::string("the daemon for the '") + c.what + "' case never acquired the bus name");
            return;
        }

        // COVERAGE-ASSERT: everything below is a claim about what the daemon wrote into its
        // working directory, which is worthless if the daemon is not in fact working there.
        // CONTROL: the workingDir argument replaced with "" so the child never chdirs ->
        //       "FAIL: the daemon really is running in the working directory this phase
        //       inspects" on both cases, printing "daemon cwd: .../backend/build", while
        //       every "writes nothing into its working directory" check stayed green on a
        //       directory the daemon had never been near. Reverted.
        char cwdLink[4096];
        const ssize_t linkLen = ::readlink(("/proc/" + std::to_string(daemon.pid()) + "/cwd").c_str(), cwdLink,
                                           sizeof(cwdLink) - 1);
        if (linkLen <= 0) {
            notVerified(kGoals, "the daemon's /proc/<pid>/cwd could not be read, so the directory this phase "
                                "inspects cannot be confirmed to be the daemon's");
            daemon.shutdown(SIGTERM);
            awaitNameRelease(names, "the next config-location case");
            return;
        }
        cwdLink[linkLen] = '\0';
        const bool inPlace = std::filesystem::path(cwdLink) == working.path();
        check(inPlace, "the daemon really is running in the working directory this phase inspects");
        if (!inPlace) std::fprintf(stderr, "  daemon cwd: %s\n", cwdLink);

        auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        std::string setError;
        try {
            setWallpaper(*proxy, "static", bluePath, kMonitorLocation);
        } catch (const std::exception& e) {
            setError = e.what();
        }
        check(setError.empty(),
              (std::string("with ") + c.what + ", SetWallpaper still succeeds").c_str());
        if (!setError.empty()) std::fprintf(stderr, "  %s\n", setError.c_str());

        // GetStatus's persistence field is the only channel that says a wallpaper will not
        // survive a restart, and "disabled" is the answer that has to reach it.
        const StatusReply status = getStatusOrExplain(*proxy);
        if (status.ok() && c.expectDisabledLine != nullptr) {
            const nlohmann::json& json = status.json();
            const std::string reported =
                json.contains("persistence") && json["persistence"].is_object()
                    ? json["persistence"].value("detail", std::string())
                    : std::string("(absent)");
            check(reported == "disabled",
                  "a daemon with nowhere to persist reports persistence disabled over DBus");
            if (reported != "disabled") std::fprintf(stderr, "  persistence detail: %s\n", reported.c_str());
        }

        daemon.shutdown(SIGTERM);
        if (c.expectDisabledLine != nullptr) {
            check(daemon.output().find(c.expectDisabledLine) != std::string::npos,
                  "a daemon with nowhere to persist says so on its way up");
        }
        awaitNameRelease(names, "the next config-location case");

        std::vector<std::string> left;
        std::string walkWhy;
        if (!filesUnder(working.path(), left, walkWhy)) {
            // This phase created that directory, so one that cannot be listed is this
            // harness's problem and not the daemon's -- and an unreadable directory must not
            // read as an empty one, which is what "nothing was written here" would become.
            // CONTROL: the call pointed at a subdirectory that does not exist -> two
            //       "HARNESS FAULT -- ... could not be read (No such file or directory)"
            //       lines and no FAIL, where the previous shape returned an empty vector and
            //       passed the absence check. Reverted.
            phaseSetupFault(kGoals, walkWhy.c_str());
        } else {
            check(left.empty(),
                  (std::string("with ") + c.what + ", the daemon writes nothing into its working directory").c_str());
            for (const std::string& file : left) std::fprintf(stderr, "  left behind: %s\n", file.c_str());
        }

        // Only the positive direction is asserted. Case one is defined by having no usable
        // HOME, so it is never told this directory exists and could not write into it under
        // any daemon behaviour, correct or broken; the property it looked like it covered --
        // that a daemon with no HOME does not fall back to the developer's real one -- is
        // covered by the witness in main(), which compares the real config file across the
        // whole run.
        if (c.expectHomeFile) {
            const std::filesystem::path homeConfig = home.path() / ".config" / "wallpaperx" / "config.json";
            check(std::filesystem::exists(homeConfig),
                  "with a relative XDG_CONFIG_HOME the daemon persists under $HOME/.config instead");
        }
    }
}

std::string joined(const std::vector<std::string>& names) {
    if (names.empty()) return "(none)";
    std::string text;
    for (const std::string& name : names) {
        if (!text.empty()) text += ", ";
        text += name;
    }
    return text;
}

// Every <config.json>.bad[.N] the daemon may have written, sorted, so a phase can say how many
// copies exist and which ones rather than only whether one does. Both the state directory the
// copies go to and the config directory they used to go to are scanned: "no copy was made" has
// to be false for a copy taken anywhere.
std::vector<std::string> preservedCopyNames(const std::filesystem::path& file,
                                            const std::filesystem::path& stateDir) {
    std::vector<std::string> found;
    for (const std::filesystem::path& dir : {stateDir, file.parent_path()}) {
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.rfind(file.filename().string() + ".bad", 0) == 0) found.push_back(it->path().string());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

void checkConfigPreservationOwnership(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr const char* kGoals = "S8.13 (an unreadable config is preserved) and S8.25 (only the process that owns "
                                   "the bus name touches the config directory)";

    ScratchDir configHome("preserveowner");
    if (!configHome.ok()) {
        notVerified(kGoals, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }
    const std::filesystem::path file = configHome.configFile();
    // A hand edit that broke the syntax, with the user's wallpaper paths still legible in
    // it. That is what makes losing these bytes data loss rather than a reset.
    const std::string usersOwn =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/kept.png\"},]}";
    const std::string laterBroken = "{\"version\":1,\"monitors\":[{,";
    if (!writeTextFile(file, usersOwn)) {
        notVerified(kGoals, "the damaged config this phase reads could not be written");
        return;
    }

    SpawnedDaemon first({validationEnv, configHome.env(), configHome.stateEnv()});
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(first, kGoals)) {
            notVerified(kGoals, "the daemon for this phase never acquired the bus name");
        }
        return;
    }

    // GOAL: S8.13 at the daemon tier -- the shipped binary, not a manager a test drove by
    //       hand, moves a config it could not read aside and says where it went. False if
    //       nothing calls preserveUnreadableConfig(): every in-process check of preservation
    //       now calls it explicitly, so with this missing the whole suite would be green for
    //       a daemon that preserved nothing at all.
    // WHY-THIS-SHAPE: the copy is compared byte for byte and config.json is asserted GONE.
    //       "config.json.bad exists" alone would pass for a daemon that wrote a summary into
    //       it, and leaving the original in place is the state in which the next save
    //       overwrites the user's only copy -- moving it is what clears the path.
    // CONTROL: `manager.preserveUnreadableConfig();` deleted from backend/daemon/main.cpp -> "FAIL: the
    //       daemon moves a config it could not read aside", "FAIL: the copy the daemon kept
    //       holds the bytes of the file it could not read" and "FAIL: the daemon says where
    //       it kept the config it could not read", printing "copies beside it: (none)".
    //       Reverted.
    // CONTROL, for the copy being kept in the state directory rather than beside config.json:
    //       the copy directory in WallpaperManager::preserveConfigTo()
    //       (backend/daemon/wallpaper_manager.cpp) put back to the config file's own parent ->
    //       "FAIL: the daemon moves a config it could not read aside" and "FAIL: the copy the
    //       daemon kept holds the bytes of the file it could not read" at this tier, with the
    //       repair-after-preservation phase's copy check red too. Observed 2026-08-15.
    //       Reverted.
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    const StatusReply firstStatus = getStatusOrExplain(*proxy);
    // In the state directory, not beside config.json: the copy is deliberately kept out of the
    // directory the user hand-edits.
    const std::filesystem::path kept = configHome.preservedDir() / "config.json.bad";
    const bool moved = !std::filesystem::exists(file) && std::filesystem::exists(kept);
    check(moved, "the daemon moves a config it could not read aside");
    if (!moved) {
        std::fprintf(stderr, "  copies: %s\n",
                     joined(preservedCopyNames(file, configHome.preservedDir())).c_str());
    }
    check(readTextFile(kept) == usersOwn, "the copy the daemon kept holds the bytes of the file it could not read");
    if (!firstStatus.ok()) {
        check(false, "the daemon that preserved its config answers GetStatus");
        std::fprintf(stderr, "  %s\n", firstStatus.error().c_str());
    } else {
        const nlohmann::json& json = firstStatus.json();
        const std::string detail = json.contains("persistence") && json["persistence"].is_object()
                                       ? json["persistence"].value("detail", std::string())
                                       : std::string("(absent)");
        const bool said = detail.find(kept.filename().string()) != std::string::npos;
        check(said, "the daemon says where it kept the config it could not read");
        if (!said) std::fprintf(stderr, "  persistence detail: %s\n", detail.c_str());
    }

    // GOAL: S8.25 -- a process that loses the name request writes NOTHING into the config
    //       directory. False if a duplicate instance moves the running daemon's config aside
    //       before finding out it has no right to run: the user starts the daemon twice, the
    //       second start renames the file the first is using, and the wallpapers are gone
    //       from the name the first daemon will read on its next start. This is a real
    //       sequence -- a desktop autostart plus a manual launch -- not a contrived one.
    // WHY-THIS-SHAPE: the fixture is a config the duplicate WOULD move: unreadable, and so
    //       preservable, with the first free copy name (.bad) already taken by the daemon
    //       that is running. A readable config would make this green for any build at all,
    //       because there would be nothing to preserve. Both directions are asserted -- the
    //       file still holds its bytes, and no second copy appeared -- because a build that
    //       copied instead of moving would leave the first true and the second false. The
    //       duplicate is required to have exited before anything is read, so this cannot pass
    //       on a race in which it simply had not got there yet.
    // CONTROL: `manager.preserveUnreadableConfig()` moved back above the `requestName` call
    //       in backend/daemon/main.cpp, which is the ordering this goal exists to forbid -> "FAIL: a daemon
    //       that lost the name request leaves config.json exactly as it found it" and "FAIL:
    //       a daemon that lost the name request makes no copy of the config", printing
    //       "config.json now holds: (nothing)" and "copies beside it: config.json.bad,
    //       config.json.bad.1". Reverted.
    if (!writeTextFile(file, laterBroken)) {
        notVerified("S8.25", "the second damaged config this phase needs could not be written");
    } else {
        const std::vector<std::string> before = preservedCopyNames(file, configHome.preservedDir());
        SpawnedDaemon second({validationEnv, configHome.env(), configHome.stateEnv()});
        const int secondExit = second.waitForExit();
        if (reportedStartupFault(second, "S8.25 (only the name owner touches the config directory)")) {
            // X.3: that child never ran the daemon, so what it did or did not write to the
            // config directory says nothing about the daemon's ordering.
        } else {
            check(second.exitedNormally() && secondExit != 0,
                  "the duplicate instance this phase needs did lose the name request");
            const std::string onDisk = readTextFile(file);
            const bool untouched = onDisk == laterBroken;
            check(untouched, "a daemon that lost the name request leaves config.json exactly as it found it");
            if (!untouched) {
                std::fprintf(stderr, "  config.json now holds: %s\n",
                             onDisk.empty() ? "(nothing)" : onDisk.c_str());
            }
            const std::vector<std::string> copies = preservedCopyNames(file, configHome.preservedDir());
            check(copies == before, "a daemon that lost the name request makes no copy of the config");
            if (copies != before) {
                std::fprintf(stderr, "  copies: %s (they were: %s)\n", joined(copies).c_str(),
                             joined(before).c_str());
            }
        }
    }

    first.shutdown(SIGTERM);
    awaitNameRelease(names, "the phase after the config-preservation phase");
}

void checkRepairWrittenAfterPreservation(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr const char* kGoals = "S8.23 with S10.14 (a start that both copies its config aside and repairs it "
                                   "preserves the bytes it was given, then writes the repair)";

    ScratchDir configHome("repairafterbad");
    if (!configHome.ok()) {
        notVerified(kGoals, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }
    const std::filesystem::path file = configHome.configFile();
    const std::filesystem::path kept = configHome.preservedDir() / "config.json.bad";
    // Three rows the daemon treats three ways: one it restores, one it cannot parse -- the
    // shape a NEWER build writes, which nothing but the copy will hold once this build
    // rewrites the file -- and one it repairs away. The third is what makes this start owe
    // the file a write at all.
    const std::string usersOwn =
        "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/photos/kept.png\"},"
        "{\"monitor_id\":\"two\",\"type\":\"static\",\"path\":\"/photos/newer.png\"},"
        "{\"monitor_id\":-4,\"type\":\"static\",\"path\":\"/photos/gone.png\"}]}";
    if (!writeTextFile(file, usersOwn)) {
        notVerified(kGoals, "the config this phase reads could not be written");
        return;
    }

    SpawnedDaemon daemon({validationEnv, configHome.env(), configHome.stateEnv()});
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(daemon, kGoals)) {
            notVerified(kGoals, "the daemon for this phase never acquired the bus name");
        }
        return;
    }
    // The barrier, and the reason nothing here polls the file. main.cpp claims the bus name
    // FIRST and then copies aside and writes the repair, so the name appearing says nothing
    // about either. But the name appearing does prove the post-name block was entered, and
    // that block runs to its end whatever a signal does -- so a daemon that has exited has
    // provably finished both. A poll-until-the-copy-appears loop would instead have a
    // success path reachable by waiting, and would grade whichever sample it stopped on.
    const int exitCode = daemon.shutdown(SIGTERM);
    awaitNameRelease(names, "the phase after the repair-after-preservation phase");
    if (!daemon.exitedNormally()) {
        notVerified(kGoals, "the daemon for this phase did not exit through main(), so what it had written by then "
                            "is not this build's answer");
        return;
    }
    check(exitCode == 0, "the daemon that copied its config aside and repaired it exits 0");

    // GOAL: S8.23 with S10.14, of the SHIPPED BINARY -- the copy taken aside holds the bytes
    //       the user's file held, on a start that also owes that file a repair. False if the
    //       repair is written first: the copy is then a copy of the daemon's own rewrite, the
    //       "monitor_id":"two" row exists in neither file, and the daemon's health note still
    //       tells the user the config was preserved.
    // WHY-THIS-SHAPE: at this tier and not only in process, because the ordering under test
    //       lives in backend/daemon/main.cpp -- which method it calls first. The in-process
    //       phase (config_store_test's malformed-entry copy phase) drives those methods
    //       itself, in an order it chooses, so it is green for any main.cpp at all. Both
    //       halves are asserted: the copy's bytes AND the repair reaching config.json.
    //       Without the second, a build that never wrote the repair would satisfy the first
    //       by doing nothing.
    // CONTROL: `manager.writeRepairedConfig();` moved above `manager.preserveUnreadableConfig();`
    //       in backend/daemon/main.cpp -- the ordering this goal exists to forbid -> "FAIL: the
    //       daemon writes the repaired config once the copy has been taken", printing config.json
    //       still holding all three of the user's rows. The copy half stays green now, because
    //       the rewrite is refused outright when no copy of what it replaces exists: since
    //       2026-08-15 the ordering is enforced in the manager rather than only relied on here,
    //       so the wrong order costs the repair instead of the copy's contents. And with
    //       `manager.writeRepairedConfig();` deleted from backend/daemon/main.cpp -> the same
    //       single FAIL. Both re-observed 2026-08-16. Both reverted.
    const std::string copied = readTextFile(kept);
    const bool preserved = copied == usersOwn;
    check(preserved, "the copy the daemon took aside holds the bytes of the file the user had");
    if (!preserved) {
        std::fprintf(stderr, "  copies: %s\n  the copy holds: %s\n  the user had:   %s\n",
                     joined(preservedCopyNames(file, configHome.preservedDir())).c_str(),
                     copied.empty() ? "(nothing)" : copied.c_str(), usersOwn.c_str());
    }

    std::string why;
    const std::map<int, PersistedEntry> onDisk = configEntries(file, why);
    const bool repairWritten = onDisk.size() == 1 && onDisk.count(0) == 1 && onDisk.at(0).path == "/photos/kept.png";
    check(repairWritten, "the daemon writes the repaired config once the copy has been taken");
    if (!repairWritten) {
        std::fprintf(stderr, "  %s; config.json now holds: %s\n", why.empty() ? "read back" : why.c_str(),
                     readTextFile(file).c_str());
    }
}

// A config.json of a shape or a size the daemon must refuse without opening it.
struct ConfigFileCase {
    const char* what;
    // Returns false when this host would not let the case be staged at all.
    bool (*plant)(const std::filesystem::path& file);
    // A fragment the daemon's persistence detail has to carry, so the refusal is reported
    // for the reason the case is about rather than incidentally.
    const char* detailFragment;
};

void checkStoppableOnAnyConfigFile(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr const char* kGoals = "S8.12 (no config file content or type can stop the daemon being stopped)";

    // GOAL: S8.12 -- a config.json that is not an ordinary file, or is over the size cap, is
    //       refused without the daemon ever waiting on it: it reaches the bus, answers, and
    //       exits on SIGTERM. False if any of these makes it unreachable or SIGKILL-only.
    //       (S8.12's wording says "refused without being opened", which was the mechanism
    //       when it was written; the file is now opened O_NONBLOCK and inspected through the
    //       descriptor, which is what makes opening a FIFO safe. The property the goal is
    //       about -- the daemon never blocks on it -- is unchanged, and is what is graded
    //       here.) This is not
    //       hypothetical and not about exotic input: measured before the guard existed, a
    //       named pipe at config.json left the daemon parked in wait_for_partner with one
    //       thread, no bus name, no log line, and immune to SIGTERM and SIGINT, because the
    //       config was read while shutdown signals were blocked and before the thread that
    //       consumes them existed. A symlink to /dev/zero did the same while growing
    //       43 MB/s; a 200 MB file did it for 14 seconds.
    // WHY-THIS-SHAPE: the daemon is required to be STOPPED, not merely to have started. The
    //       obvious shape -- S8.2's, "it came up and exited 0" -- passes on exactly the
    //       daemon this goal exists to catch if it is written as "did not exit non-zero", and
    //       S8.2's own corpus is regular files, which the FIFO case is not. So each case
    //       asserts three separable things: the name appears (it did not, before the fix),
    //       GetStatus answers on it, and the process leaves through main() on SIGTERM without
    //       having to be killed. `timedOut()` is asserted explicitly because shutdown()
    //       SIGKILLs after its wait expires and returns an exit code either way -- a check on
    //       the code alone would grade a hang that had to be killed as a clean exit.
    //       KNOWN GAP, stated rather than papered over, and MEASURED rather than assumed:
    //       a config file on a genuinely hung mount blocks the daemon inside the read, which
    //       none of the cases below reproduce -- they are refused before any byte is asked
    //       for. Measured out of suite by making that block happen (see CONTROL (b) below):
    //       the daemon takes the bus name, never serves a request, and then exits 0 five
    //       seconds after SIGTERM, because the watcher thread stops waiting for a teardown
    //       that is not coming. So a hung mount now costs the daemon its usefulness rather
    //       than its stoppability. Nothing in this suite covers it and no committed check
    //       can: the fixture would have to be a filesystem this harness can hang on demand
    //       (FUSE), and without one there is no way to reach that state from outside the
    //       process.
    // CONTROL: two, both reverted, and the pair is the finding. (a) `O_NONBLOCK` dropped from
    //       load()'s open in config_store.cpp, so the FIFO blocks in open() exactly as it did
    //       when the hang was found -> "FAIL: with a named pipe at config.json, the daemon
    //       says why it refused the file", the detail reading "not saving over it; ...: did
    //       not respond within 2s, starting without it" instead of naming the file type. The
    //       daemon still came up and still exited: the two-second load deadline abandons the
    //       read. (b) that plus `kLoadDeadline` raised to 3600 s in wallpaper_manager.cpp, so
    //       nothing abandons it -> "FAIL: with a named pipe at config.json, the daemon reaches
    //       the bus" and "FAIL: ... the daemon exits 0" ("exit code 1"), and the run took
    //       37.6 s instead of 4.6. The name is what moves under (b): main() parks in the
    //       manager's constructor, which now runs BEFORE requestName, so that daemon never
    //       reaches the bus at all -- the phase's GetStatus and detail checks are skipped
    //       rather than red, and the FAILs land on the two that do not depend on the name.
    //       What stayed GREEN under (b) is worth as much: the daemon still exited without
    //       being killed, because backend/daemon/main.cpp's five-second teardown deadline fires and ends the
    //       process -- and it is that deadline, not a clean stop, that the exit code 1 above
    //       comes from. So stoppability here rests on three independent mechanisms -- the type
    //       and size guards, the load deadline, and that watchdog -- and no single-mutation
    //       control can redden the "exits on SIGTERM without having to be killed" checks.
    // COVERAGE-ASSERT: every case is accounted for -- exercised, or reported as one this
    //       host could not stage -- against the table's size, so a case that quietly stops
    //       being planted fails instead of shrinking the phase in silence.
    const std::vector<ConfigFileCase> cases{
        {"a named pipe",
         [](const std::filesystem::path& file) { return ::mkfifo(file.c_str(), 0644) == 0; },
         "is not a regular file"},
        {"a symlink to /dev/zero",
         [](const std::filesystem::path& file) {
             std::error_code ec;
             std::filesystem::create_symlink("/dev/zero", file, ec);
             return !ec;
         },
         "is not a regular file"},
        {"a directory",
         [](const std::filesystem::path& file) {
             std::error_code ec;
             std::filesystem::create_directories(file, ec);
             return !ec;
         },
         "is not a regular file"},
        {"a regular file over the size cap",
         [](const std::filesystem::path& file) { return writeTextFile(file, std::string(2u * 1024u * 1024u, 'x')); },
         "over the"},
        // The other arm of the guard: a path the open itself refuses. ELOOP is the one an
        // unprivileged test can produce; a hung mount is the case that matters and the KNOWN
        // GAP above is about it. Distinct from the rows above because it never gets a
        // descriptor at all, so the file is NOT eligible to be moved aside --
        // config_store_test's preserved-copy phase grades that half.
        {"a self-referential symlink",
         [](const std::filesystem::path& file) {
             std::error_code ec;
             std::filesystem::create_symlink(file, file, ec);
             return !ec;
         },
         "could not be opened"},
    };

    int exercised = 0;
    int unstageable = 0;
    for (const ConfigFileCase& c : cases) {
        ScratchDir configHome("stoppable");
        if (!configHome.ok()) {
            ++unstageable;
            notVerified(kGoals, std::string("the scratch XDG_CONFIG_HOME for the '") + c.what +
                                    "' case could not be created");
            continue;
        }
        std::error_code ec;
        std::filesystem::create_directories(configHome.configFile().parent_path(), ec);
        if (ec || !c.plant(configHome.configFile())) {
            ++unstageable;
            notVerified(kGoals, std::string("this host would not let the '") + c.what + "' case be staged");
            continue;
        }
        ++exercised;

        SpawnedDaemon daemon({validationEnv, configHome.env()});
        const bool cameUp = names.waitFor(true);
        if (!cameUp && reportedStartupFault(daemon, kGoals)) return;
        check(cameUp, (std::string("with ") + c.what + " at config.json, the daemon reaches the bus").c_str());

        std::string served;
        std::string persistence;
        if (cameUp) {
            auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                            sdbus::ObjectPath{DbusService::kObjectPath});
            const StatusReply status = getStatus(*proxy);
            if (!status.ok()) {
                served = status.error();
            } else {
                const nlohmann::json& json = status.json();
                persistence = json.contains("persistence") && json["persistence"].is_object()
                                  ? json["persistence"].value("detail", std::string())
                                  : std::string("(absent)");
            }
            check(served.empty(),
                  (std::string("with ") + c.what + " at config.json, the daemon answers GetStatus").c_str());
            if (!served.empty()) std::fprintf(stderr, "  %s: %s\n", c.what, served.c_str());

            // The refusal has to be reported, or a user whose config is a directory is told
            // nothing at all and their wallpapers silently stop surviving restarts.
            const bool named = persistence.find(c.detailFragment) != std::string::npos;
            check(named, (std::string("with ") + c.what + " at config.json, the daemon says why it refused the file")
                             .c_str());
            if (!named) std::fprintf(stderr, "  %s: persistence detail '%s'\n", c.what, persistence.c_str());
        }

        const int exitCode = daemon.shutdown(SIGTERM);
        const bool stopped = daemon.exitedNormally() && !daemon.timedOut();
        check(stopped, (std::string("with ") + c.what +
                        " at config.json, the daemon exits on SIGTERM without having to be killed")
                           .c_str());
        if (!stopped) {
            std::fprintf(stderr, "  %s: exited normally %d, timed out %d, signal %d\n", c.what,
                         static_cast<int>(daemon.exitedNormally()), static_cast<int>(daemon.timedOut()),
                         daemon.termSignal());
        }
        if (daemon.output().find("Sanitizer: reported") != std::string::npos) {
            check(false, (std::string("with ") + c.what +
                          " at config.json, the daemon's sanitizer reported no warnings (its exit code is the "
                          "sanitizer's here, so the exit-code check cannot run)")
                             .c_str());
        } else {
            check(exitCode == 0, (std::string("with ") + c.what + " at config.json, the daemon exits 0").c_str());
            if (exitCode != 0) std::fprintf(stderr, "  %s: exit code %d\n", c.what, exitCode);
        }
        awaitNameRelease(names, "the next unreadable-config-file case");
    }
    check(exercised + unstageable == static_cast<int>(cases.size()),
          "every unreadable-config-file case was either exercised or reported unstageable");
}

// A config that parses and every entry of which the daemon rejects, grown to just under the
// store's 1 MiB cap. Its only purpose is to lengthen the work the daemon does *after* it owns
// the bus name: rejected entries make preserveUnreadableConfig() copy the whole file aside,
// and that call sits between requestName() and the event loop. A monitor id the JSON gives as
// a string is what the store refuses, so nothing is restored and no render is ever queued.
std::string rejectedEntriesNearCap(std::size_t targetBytes) {
    static constexpr char kEntry[] = "{\"monitor_id\":\"x\",\"type\":\"slideshow\",\"path\":\"/not/rendered/here.png\"}";
    std::string text = "{\"version\":1,\"monitors\":[";
    while (text.size() + sizeof(kEntry) + 2 < targetBytes) {
        if (text.back() != '[') text += ',';
        text += kEntry;
    }
    text += "]}";
    return text;
}

// A config that parses and every entry of which the daemon CAN apply, grown to just under the
// store's 1 MiB cap. The mirror of rejectedEntriesNearCap above, and it exists for the mirror
// reason: the read, the parse and restoreSaved() all run BEFORE requestName(), so this is what
// lengthens the window in which a signal can arrive with no name yet claimed.
std::string validEntriesNearCap(std::size_t targetBytes) {
    std::string text = "{\"version\":1,\"monitors\":[";
    int id = 0;
    for (;;) {
        std::string entry = "{\"monitor_id\":" + std::to_string(id) +
                            ",\"type\":\"static\",\"path\":\"/not/rendered/here.png\"}";
        if (text.size() + entry.size() + 3 >= targetBytes) break;
        if (text.back() != '[') text += ',';
        text += entry;
        ++id;
    }
    text += "]}";
    return text;
}

// GOAL: a start that a signal cut short before the bus name was requested says so, instead of
//       announcing a name it never held. False if it prints "serving io.wallpaperx.Daemon"
//       having skipped requestName(): the journal then records a daemon that took the name and
//       exited, and an operator debugging a name that is NOT held goes looking for the process
//       still holding it. Unconditional is exactly what that line used to be.
// WHY-THIS-SHAPE: the daemon's own log cannot be its own evidence here -- the log is what is
//       being graded -- so whether the name was ever claimed is decided by NameOwnerChanged on
//       this process's connection, which the daemon does not write. Each attempt is then
//       classified by that external fact and the matching invariant is asserted, so an attempt
//       that lands on either side of the window is graded rather than discarded. Sending the
//       signal and asserting one fixed message is the obvious simpler version and is a
//       scheduler assertion: which side of requestName() the signal lands on is a race.
//       The delays sweep rather than sit at one value because the ONE outcome that establishes
//       nothing -- the signal arriving before main() has blocked it, which kills the process
//       outright -- is the one a fixed short delay would produce every time on a fast host.
// CONTROL: the `if (shutdownRequested...)` arm around the startup message in
//       backend/daemon/main.cpp collapsed back to the unconditional
//       `printf("... serving %s at %s")` -> "FAIL: a start stopped before the bus name was
//       requested does not announce that name", twice, printing "the name was never taken, but
//       the daemon announced it was serving it" -- once per attempt that landed in the window,
//       and nothing else red. The run's own tally line was unchanged: "of 6 pre-claim attempts,
//       2 stopped before the name, 1 after it, and 3 before the signal could be blocked".
//       Observed 2026-08-19. Reverted.
// COVERAGE-ASSERT: the number of attempts that actually landed BEFORE the name was claimed is
//       asserted to be non-zero. Without it a host that starts the daemon faster than this
//       harness can signal it would grade only ordinary starts and report the goal as met on
//       evidence that never touched it; with it, such a host reports NOT VERIFIED.
void checkStopBeforeClaiming(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr const char* kGoal = "S8.20's log clause (a start stopped before the bus name was requested says so, "
                                  "rather than announcing a name it never held)";
    constexpr std::size_t kConfigBytes = 1000000;
    // Milliseconds between the spawn and the signal. Short enough to be inside the config read
    // on this host, and spread so a slower or faster one still lands somewhere useful.
    constexpr int kDelays[] = {4, 10, 20, 40, 80, 150};

    ScratchDir configHome("preclaim");
    if (!configHome.ok()) {
        notVerified(kGoal, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(configHome.configFile().parent_path(), ec);
    if (ec || !writeTextFile(configHome.configFile(), validEntriesNearCap(kConfigBytes))) {
        notVerified(kGoal, "the large config that widens this phase's startup window could not be written");
        return;
    }

    int beforeClaim = 0;
    int afterClaim = 0;
    int tooEarly = 0;
    for (const int delayMs : kDelays) {
        const uint64_t eventsBefore = names.events();
        SpawnedDaemon daemon({validationEnv, configHome.env(), configHome.stateEnv()});
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        daemon.shutdown(SIGTERM);

        if (!daemon.exitedNormally() && daemon.termSignal() == SIGTERM) {
            // The signal beat main()'s pthread_sigmask, so the kernel's default action ended
            // the process before any of this daemon's code ran. Nothing to grade, and it is
            // not a defect: no program can block a signal before it starts.
            ++tooEarly;
            continue;
        }

        // A daemon that claimed the name emitted two NameOwnerChanged before it exited, and it
        // exited before this wait began -- so the wait expiring means the name was never taken.
        const bool claimed = names.waitForEventsPast(eventsBefore, std::chrono::milliseconds(1500));
        const std::string output = daemon.output();
        const bool saysServing = output.find("serving " + std::string(DbusService::kBusName)) != std::string::npos;
        const bool saysStopped = output.find("stopped before claiming") != std::string::npos;

        if (claimed) {
            ++afterClaim;
            check(saysServing && !saysStopped,
                  "a start that did claim the bus name says it is serving it");
            if (!saysServing || saysStopped) {
                std::fprintf(stderr, "  the name was taken, but the daemon said%s%s\n",
                             saysStopped ? " it stopped before claiming" : "",
                             saysServing ? " both" : (saysStopped ? "" : " neither"));
            }
        } else {
            ++beforeClaim;
            check(saysStopped && !saysServing,
                  "a start stopped before the bus name was requested does not announce that name");
            if (!saysStopped || saysServing) {
                std::fprintf(stderr, "  the name was never taken, but the daemon %s\n",
                             saysServing ? "announced it was serving it" : "said nothing about it");
            }
        }
        awaitNameRelease(names, "the next pre-claim attempt");
    }

    check(beforeClaim > 0 || afterClaim > 0, "at least one attempt reached the daemon at all");
    if (beforeClaim == 0) {
        std::fprintf(stderr, "  %d attempt(s) landed after the name was claimed, %d before main() had blocked the "
                             "signal, 0 in the window this phase grades\n",
                     afterClaim, tooEarly);
        notVerified(kGoal, "no attempt reached the daemon between its signal mask and its name request, so the "
                           "message a start stopped in that window prints was never produced");
        return;
    }
    std::printf("daemon_system_test: of %zu pre-claim attempts, %d stopped before the name, %d after it, and %d "
                "before the signal could be blocked\n",
                std::size(kDelays), beforeClaim, afterClaim, tooEarly);
}

void checkShutdownDuringStartup(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    using wallpaperx::testing::ConnectResult;
    using wallpaperx::testing::describe;
    using wallpaperx::testing::tryConnect;

    constexpr const char* kGoal =
        "S8.20 (a shutdown signal that arrives before the event loop starts still stops the daemon)";
    constexpr int kPort = wallpaperx::HttpServer::kDefaultPort;
    constexpr int kAttempts = 12;
    constexpr std::size_t kConfigBytes = 1000000;

    ScratchDir configHome("earlyterm");
    if (!configHome.ok()) {
        notVerified(kGoal, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(configHome.configFile().parent_path(), ec);
    if (ec || !writeTextFile(configHome.configFile(), rejectedEntriesNearCap(kConfigBytes))) {
        notVerified(kGoal, "the large config that widens this phase's startup window could not be written");
        return;
    }

    // GOAL: S8.20 -- a SIGTERM delivered after the daemon owns the bus name but before it enters its
    //       event loop still terminates it, cleanly and through main(). False if such a
    //       daemon runs on holding io.wallpaperx.Daemon and port 18462 after being told to
    //       stop -- the user's `systemctl stop` returns, the name stays taken, and the next
    //       start fails with FileExists.
    // WHY-THIS-SHAPE: it asserts the OUTCOME, not the mechanism, and deliberately makes no
    //       claim about main.cpp's `shutdownRequested` guard. Measured: with that guard
    //       removed the daemon still exits cleanly, because sdbus-c++ latches a
    //       leaveEventLoop() issued before the loop starts. A check written as "the guard is
    //       what saves it" would therefore be a false claim, and one written to go red when
    //       the guard is deleted cannot exist.
    //       The window is entered by force rather than by luck. SIGSTOP freezes the daemon
    //       where it stands; a connect() to 18462 is then answered by the KERNEL, so it
    //       still discriminates on a process that cannot run -- refused means the HttpServer
    //       constructor has not completed, and that constructor runs before
    //       enterEventLoop(). Only then is SIGTERM queued and SIGCONT sent. The obvious
    //       simpler version -- signal as soon as the name appears and hope -- cannot tell
    //       which side of the loop it landed on, so it would grade an ordinary shutdown and
    //       report it as this one.
    //       The fixture is what makes that freeze land often enough to be usable, and it has
    //       to be a config whose cost falls AFTER the name: the config read and the restore
    //       both run before requestName(), so a merely large valid config -- which is what
    //       this phase used to plant -- lengthens the wrong side and buys nothing. A file
    //       whose entries are all rejected is copied aside by preserveUnreadableConfig(),
    //       which main() calls immediately after requestName(), so its size lands squarely in
    //       the graded window. Measured on this host, eight runs each: with rejected entries,
    //       caught on attempt 1 of 12 every time; with the same file made valid, attempts 1-3
    //       (mean 1.8) -- and with the phase as it stood before this change, attempts 1-6
    //       (mean 2.75), which is a 7% chance of no attempt landing at all and the goal
    //       silently going unexercised.
    // CONTROL: two, both reverted. (a) the guard alone deleted, so main enters the loop
    //       whatever the watcher recorded -> still green, four runs out of four, each with
    //       the freeze confirmed: that is the latch, measured here rather than taken on
    //       trust, and it is why this phase claims nothing about the guard. Run before the
    //       reordering that moved preserveUnreadableConfig() after requestName(), and left
    //       standing because it says the guard is not load-bearing, which nothing since has
    //       touched. (b) the guard deleted AND the latch removed with it -- the watcher
    //       calling leaveEventLoop() only when a flag says the loop is running -- re-run
    //       against the current binary -> "FAIL: a daemon signalled before its event loop
    //       began exits 0" ("exit code 1") and "FAIL: ... still runs its destructor chain".
    //       Note the signature has MOVED since that mutation was first run: the daemon is no
    //       longer SIGKILLed after the reap timeout but ends itself at the five-second
    //       teardown deadline with exit 1, so "exits without having to be killed" stays green
    //       and the exit code carries the failure. The same mutation still reddens the SIGINT
    //       and SIGHUP phases, which signal as soon as the name appears and so land in this
    //       window by accident; the SIGTERM phase, which does work first, stayed green.
    // COVERAGE-ASSERT: an attempt counts only when the connect said refused while the
    //       daemon was stopped. If no attempt in six lands in the window -- another process
    //       holds 18462, or this host starts the daemon faster than the harness can freeze
    //       it -- the phase reports NOT VERIFIED and grades nothing, rather than passing on
    //       shutdowns that all happened from inside the loop.
    int inWindow = 0;
    int attemptsUsed = 0;
    std::string lastPortAnswer = "not probed";
    for (int attempt = 0; attempt < kAttempts && inWindow == 0; ++attempt) {
        ++attemptsUsed;
        // The copy each attempt makes would occupy the next attempt's target name, and the
        // daemon's numbered search gives up after ten -- so without this the widening the
        // fixture buys would quietly stop happening partway through the loop.
        for (int n = 0; n < 12; ++n) {
            std::error_code removeEc;
            std::filesystem::remove(configHome.preservedDir() /
                                        ("config.json.bad" + (n == 0 ? std::string() : "." + std::to_string(n))),
                                    removeEc);
        }
        SpawnedDaemon daemon({validationEnv, configHome.env(), configHome.stateEnv()});
        if (!names.waitFor(true)) {
            if (reportedStartupFault(daemon, kGoal)) return;
            notVerified(kGoal, "the daemon for this phase never acquired the bus name");
            return;
        }

        const pid_t pid = daemon.pid();
        const bool frozen = pid > 0 && ::kill(pid, SIGSTOP) == 0;
        const ConnectResult port = frozen ? tryConnect("127.0.0.1", kPort, 500) : ConnectResult::Failed;
        lastPortAnswer = describe(port);
        const bool beforeLoop = frozen && port == ConnectResult::Refused;

        // Queued while the process is stopped, so it is provably delivered to a daemon that
        // had not reached enterEventLoop(); SIGCONT is what lets it be acted on.
        if (frozen) {
            ::kill(pid, SIGTERM);
            ::kill(pid, SIGCONT);
        }

        if (!beforeLoop) {
            // This attempt says nothing about the goal: either the daemon was already
            // serving, or the freeze failed. Shut it down and try again rather than
            // grading it as though it had been caught in the window.
            daemon.shutdown(SIGTERM);
            awaitNameRelease(names, "the next attempt at the startup-shutdown window");
            continue;
        }
        // waitForExit, not shutdown(): the SIGTERM has already been sent, and sending a
        // second one would let a daemon that ignored the first pass on the second.
        const int exitCode = daemon.waitForExit();
        const std::string output = daemon.output();

        // The refused connect is evidence only for a daemon that was going to bind that
        // port. One that could not bind it at all -- another process holds 18462 -- answers
        // refused from inside its event loop just as it does before reaching it, which would
        // let this phase grade an ordinary shutdown and report it as a startup-window one.
        if (output.find(bindFailureMarker(kPort)) != std::string::npos) {
            lastPortAnswer = "refused, but that daemon never bound the port at all";
            awaitNameRelease(names, "the next attempt at the startup-shutdown window");
            continue;
        }
        ++inWindow;

        const bool stopped = daemon.exitedNormally() && !daemon.timedOut();
        check(stopped, "the daemon signalled before its event loop began exits without having to be killed");
        if (!stopped) {
            std::fprintf(stderr, "  exited normally %d, timed out %d, signal %d\n",
                         static_cast<int>(daemon.exitedNormally()), static_cast<int>(daemon.timedOut()),
                         daemon.termSignal());
        }
        if (output.find("Sanitizer: reported") != std::string::npos) {
            check(false, "the startup-shutdown daemon's sanitizer reported no warnings (its exit code is the "
                         "sanitizer's here, so the exit-code check cannot run)");
        } else {
            check(exitCode == 0, "a daemon signalled before its event loop began exits 0");
            if (exitCode != 0) std::fprintf(stderr, "  exit code %d\n", exitCode);
        }
        check(output.find(kSignalShutdownLine) != std::string::npos,
              "a daemon signalled before its event loop began still says which signal it is stopping for");
        check(output.find("stopped cleanly") != std::string::npos,
              "a daemon signalled before its event loop began still runs its destructor chain");
        check(names.waitFor(false), "the bus name is released by a daemon signalled before its event loop began");
    }

    if (inWindow > 0) {
        std::printf("daemon_system_test: the daemon was frozen inside its startup window, before port %d was "
                    "bound, on attempt %d of %d\n",
                    kPort, attemptsUsed, kAttempts);
        std::fflush(stdout);
    } else {
        notVerified(kGoal, std::string("no attempt in ") + std::to_string(kAttempts) +
                               " froze the daemon before it had bound port " + std::to_string(kPort) +
                               " (the last connect answered '" + lastPortAnswer +
                               "'), so nothing was signalled inside the startup window");
    }
}

void checkFileSizeLimit(NameWatcher& names, const std::string& validationEnv, const std::string& bluePath,
                        const Check& check) {
    constexpr int32_t kMonitorFileLimit = 19;
    constexpr const char* kGoals = "S8.17 (a file-size limit hit during a config write does not kill the daemon)";

    ScratchDir configHome("fsize");
    if (!configHome.ok()) {
        notVerified(kGoals, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }

    // GOAL: S8.17 -- a config write that exceeds RLIMIT_FSIZE does not kill the daemon. The
    //       kernel's default disposition for SIGXFSZ is to terminate, so before this was
    //       ignored the first SetWallpaper under a `LimitFSIZE=` unit killed the daemon
    //       outright, with a core dump, no log line and no clean shutdown -- after the
    //       client's call had already returned success. Every monitor goes blank at once.
    // WHY-THIS-SHAPE: the wallpaper is a `video`, which requestWallpaper records and
    //       persists but renderPending refuses before touching the GPU. A `static` would
    //       create a Vulkan device under the same 1-byte file limit, and a driver that
    //       writes a shader cache would then die for a reason that has nothing to do with
    //       the config -- a failure this phase would report against the daemon. The limit is
    //       applied in the forked child rather than to this process, because a limit here
    //       would apply to the harness's own writes too.
    // CONTROL: `std::signal(SIGXFSZ, SIG_IGN)` disabled in backend/daemon/main.cpp -> four FAILs: the
    //       SetWallpaper call itself (the daemon dies mid-reply), "a config write that
    //       exceeds the file-size limit does not kill the daemon", and both shutdown checks,
    //       printing "  exited normally 0, timed out 0, signal 25". Reverted.
    SpawnedDaemon daemon({validationEnv, configHome.env()}, SpawnedDaemon::Stdout::Captured, {}, 1);
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(daemon, kGoals)) {
            notVerified(kGoals, "the daemon for this phase never acquired the bus name");
        }
        return;
    }

    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    std::string setError;
    try {
        setWallpaper(*proxy, "video", bluePath, kMonitorFileLimit);
    } catch (const std::exception& e) {
        setError = e.what();
    }
    check(setError.empty(), "a SetWallpaper whose config write cannot fit under the file-size limit still succeeds");
    if (!setError.empty()) std::fprintf(stderr, "  %s\n", setError.c_str());

    const StatusReply after = getStatusOrExplain(*proxy);
    check(after.ok(), "a config write that exceeds the file-size limit does not kill the daemon");
    if (!after.ok()) {
        std::fprintf(stderr, "  %s\n", after.error().c_str());
    } else {
        const nlohmann::json& json = after.json();
        const std::string detail = json.contains("persistence") && json["persistence"].is_object()
                                       ? json["persistence"].value("detail", std::string())
                                       : std::string("(absent)");
        // The write failed, and saying so is the difference between "your wallpaper will not
        // survive a restart" and silence.
        check(!detail.empty() && detail.find("config.json") != std::string::npos,
              "the daemon reports the config write that the file-size limit refused");
        if (detail.find("config.json") == std::string::npos) {
            std::fprintf(stderr, "  persistence detail: %s\n", detail.c_str());
        }
    }

    const int exitCode = daemon.shutdown(SIGTERM);
    const bool stopped = daemon.exitedNormally() && !daemon.timedOut();
    check(stopped, "a daemon whose config write hit the file-size limit still exits on SIGTERM");
    if (!stopped) {
        std::fprintf(stderr, "  exited normally %d, timed out %d, signal %d\n",
                     static_cast<int>(daemon.exitedNormally()), static_cast<int>(daemon.timedOut()),
                     daemon.termSignal());
    }
    if (daemon.output().find("Sanitizer: reported") != std::string::npos) {
        check(false, "the file-size-limit daemon's sanitizer reported no warnings (its exit code is the sanitizer's "
                     "here, so the exit-code check cannot run)");
    } else {
        check(exitCode == 0, "a daemon whose config write hit the file-size limit exits 0");
        if (exitCode != 0) std::fprintf(stderr, "  exit code %d\n", exitCode);
    }
    awaitNameRelease(names, "the phase after the file-size-limit phase");
}

// ---------------------------------------------------------------------------- //
// S8.29-S8.32: the daemon while the filesystem holding its config has stopped
// answering. In process, config_store_test grades the same properties against
// WallpaperManager; only here is the thing under test a bus that a real client is
// waiting on.
// ---------------------------------------------------------------------------- //
void checkStalledConfigWrite(NameWatcher& names, const std::string& validationEnv, const std::string& bluePath,
                             const Check& check) {
    // This phase drives a daemon of its own, so its ids are only ever compared with its own.
    constexpr int32_t kMonitorStalledFirst = 18;
    constexpr int32_t kMonitorStalledSecond = 19;
    constexpr const char* kGoals = "S8.29/S8.30/S8.31/S8.32 (the daemon while its config filesystem has stopped "
                                   "answering)";
    // The deadline a save is given, restated rather than shared: kWriteDeadline sits in an
    // anonymous namespace in config_writer.cpp, and a copy here means a change to it
    // fails this phase loudly instead of silently re-pointing it at the new value.
    constexpr double kSaveDeadlineMs = 2000.0;

    ScratchDir configHome("stalledwrite");
    if (!configHome.ok()) {
        notVerified(kGoals, "a scratch XDG_CONFIG_HOME could not be created under the temp directory");
        return;
    }

    // The fixture, proved before the daemon exists: see blocking_config_write.h. It runs a
    // save of this build's own ConfigStore -- the code the daemon executes -- through a fifo
    // and requires it to park. Without it, "the daemon answered instantly" has two causes
    // that look identical from out here, one of which is this harness's fault.
    std::string fixtureWhy;
    if (!wallpaperx::testing::blockingWriteFixtureWorks(configHome.path() / "wallpaperx" / "probe.json",
                                                       fixtureWhy)) {
        notVerified(kGoals, fixtureWhy);
        return;
    }

    SpawnedDaemon daemon({validationEnv, configHome.env()});
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(daemon, kGoals)) {
            notVerified(kGoals, "the daemon for this phase never acquired the bus name");
        }
        return;
    }

    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

    // A save that works, before anything is blocked, and it is a precondition rather than a
    // check: it is what makes "the next write is parked in this phase's fifo" a fact instead
    // of an assumption. This daemon is shown to write to THIS config file, and the fixture
    // probe above has shown a write to that file parks -- so a later save cannot be finishing
    // somewhere out of sight. It also gives S8.32 a last-good save to hide, which is the
    // wrong answer that goal names.
    //
    // `video`, not `static`, throughout: requestWallpaper records and persists it and
    // renderPending refuses it before touching the GPU, so nothing here creates a Vulkan
    // device. What is being measured is a handler's latency, and a driver's first-use cost
    // inside the same call would swamp it -- and would make this phase unanswerable on a host
    // with no working ICD, for a question that has nothing to do with the GPU.
    std::string baselineError;
    try {
        setWallpaper(*proxy, "video", bluePath, kMonitorStalledFirst);
    } catch (const std::exception& e) {
        baselineError = e.what();
    }
    std::string baselineWhy;
    const std::map<int, PersistedEntry> baseline = configEntries(configHome.configFile(), baselineWhy);
    if (!baselineError.empty() || baseline.count(kMonitorStalledFirst) != 1) {
        // X.3: a daemon that cannot persist at all is graded by the phases written for that.
        // Here it is a baseline that did not appear, so this phase establishes nothing.
        notVerified(kGoals, "the daemon did not persist a wallpaper into this phase's config file before anything "
                            "was blocked (" +
                                (baselineError.empty() ? baselineWhy : baselineError) + ")");
        return;
    }

    wallpaperx::testing::BlockingWrite blocked(configHome.configFile());
    if (!blocked.arm()) {
        notVerified(kGoals, "a fifo could not be created at " + blocked.temp() +
                                ", so no write of this daemon's can be made to block");
        return;
    }

    // GOAL: S8.29 at the process level -- SetWallpaper returns even though the write it
    //       triggers has stopped coming back, and returns on the deadline rather than when
    //       the filesystem answers. False if the reply waits for the mount: the caller is a
    //       client on the bus, and the daemon has one thread serving all of them.
    // WHY-THIS-SHAPE: the call carries a timeout of its own, unlike every other SetWallpaper
    //       in this file. With the write back on the bus thread this call never returns at
    //       all, and the bus's 25-second default would turn the control run into something
    //       that looks like a hung suite rather than a failed check -- and would put a
    //       25-second stall in front of every check below it.
    // CONTROL: `m_store.save(entries);` added at the top of WallpaperManager::persist(), i.e.
    //       the write put back on the bus thread -> "FAIL: the daemon answers a SetWallpaper
    //       whose config write has stopped coming back", printing
    //       "org.freedesktop.DBus.Error.Timeout: Connection timed out (after 15075 ms)", and
    //       six more: the deadline check here, both GetStatus checks, the second-save check,
    //       the stall-report check ("persistence reads (absent)") and both shutdown checks
    //       ("exit code 1", the watchdog line present). That is the state this whole change
    //       exists to prevent, reproduced end to end. Reverted.
    const auto firstStart = Clock::now();
    std::string stalledError;
    try {
        setWallpaper(*proxy, "video", bluePath, kMonitorStalledSecond, std::chrono::seconds(15));
    } catch (const sdbus::Error& e) {
        stalledError = e.getName() + ": " + e.getMessage();
    } catch (const std::exception& e) {
        stalledError = e.what();
    }
    const double firstMs = millis(Clock::now() - firstStart);
    check(stalledError.empty(), "the daemon answers a SetWallpaper whose config write has stopped coming back");
    if (!stalledError.empty()) {
        // Not an early return, deliberately. A daemon wedged in this handler fails every
        // check below too, and each of those is a true and separate statement about it -- the
        // bus does not answer, the stall is not reported, the process has to be forced out.
        // Returning here would leave the one property this phase exists for, that the bus
        // still answers, with no mutation that can redden it.
        std::fprintf(stderr, "  %s (after %.0f ms)\n", stalledError.c_str(), firstMs);
        std::printf("daemon_system_test: the stalled-write daemon did not answer the call that stalls it; the "
                    "failures below share that one cause.\n");
    }
    const bool boundedByTheDeadline = firstMs < 2 * kSaveDeadlineMs;
    check(boundedByTheDeadline, "that reply waits for the deadline and not for the filesystem");
    if (!boundedByTheDeadline) {
        std::fprintf(stderr, "  SetWallpaper took %.0f ms against a %.0f ms deadline\n", firstMs, kSaveDeadlineMs);
    }

    // GOAL: S8.30 -- the daemon goes on answering while a config write is outstanding. This
    //       is the whole reason the write left the bus thread: before it did, one
    //       SetWallpaper onto a dead mount stopped the daemon answering ANY method until its
    //       own teardown watchdog killed it, so every client -- the panel, a status poller,
    //       the plugin -- hung with it.
    // WHY-THIS-SHAPE: asked after the stalled SetWallpaper has returned, not concurrently
    //       with it. The write is provably still outstanding at this point -- nothing in this
    //       phase attaches a reader to the fifo, so it cannot have completed -- while a
    //       racing GetStatus issued from a second thread might legally be answered before the
    //       handler ever reached the write, which is a check graded on the interleaving.
    //       The latency is bounded as well as the answer required: "it replied eventually" is
    //       also true of a daemon that answers status only once the deadline it should not be
    //       paying expires.
    // CONTROL: the same `m_store.save(entries);` at the top of persist() -> "FAIL: the daemon
    //       answers GetStatus while a config write of its own is stalled", printing "GetStatus
    //       failed with org.freedesktop.DBus.Error.Timeout, which is not one of the daemon's
    //       own error names -- so it is the bus, the connection, or the daemon failing outside
    //       its own taxonomy: Connection timed out". Reverted.
    const auto statusStart = Clock::now();
    const StatusReply during = getStatus(*proxy, std::chrono::seconds(10));
    const double statusMs = millis(Clock::now() - statusStart);
    check(during.ok(), "the daemon answers GetStatus while a config write of its own is stalled");
    if (!during.ok()) std::fprintf(stderr, "  %s\n", during.error().c_str());
    const bool answeredPromptly = during.ok() && statusMs < kSaveDeadlineMs / 2;
    check(answeredPromptly, "and answers it without waiting on that write");
    if (during.ok() && !answeredPromptly) {
        std::fprintf(stderr, "  GetStatus took %.0f ms while a write was outstanding\n", statusMs);
    }

    // GOAL: S8.31 -- once one wait has run out, no later SetWallpaper pays the deadline
    //       again. False if an outage costs two seconds per wallpaper change rather than two
    //       seconds once.
    // COVERAGE-ASSERT: the first call's own elapsed time is asserted to be at least three
    //       quarters of the deadline. Without it this comparison is green on a daemon that
    //       waits for nothing at all: both calls return instantly and the phase reports that
    //       the deadline is not paid twice, having never seen it paid once. CONTROL for it:
    //       `kWriteDeadline` (backend/daemon/config_writer.cpp) replaced with 0 seconds -> "FAIL:
    //       the first call really did wait for the deadline, so the comparison above has a
    //       baseline", printing "the first call took 0 ms" and the phase's own summary line
    //       "SetWallpaper cost 0 ms then 0 ms", while the check it guards stayed green at 0 ms.
    //       Re-observed 2026-08-18. Reverted.
    // CONTROL: the `!worker->stalled &&` short-circuit dropped from ConfigWriter::write()'s wait
    //       (backend/daemon/config_writer.cpp) -> "FAIL: a second SetWallpaper during the stall
    //       does not wait for the deadline all over again", printing "the second call took 2000 ms
    //       against a 2000 ms deadline", and nothing else red. Re-observed 2026-08-18. Reverted.
    const auto secondStart = Clock::now();
    std::string secondError;
    try {
        setWallpaper(*proxy, "video", bluePath, kMonitorStalledFirst, std::chrono::seconds(15));
    } catch (const std::exception& e) {
        secondError = e.what();
    }
    const double secondMs = millis(Clock::now() - secondStart);
    const bool cheap = secondError.empty() && secondMs < kSaveDeadlineMs / 4;
    check(cheap, "a second SetWallpaper during the stall does not wait for the deadline all over again");
    if (!cheap) {
        std::fprintf(stderr, "  the second call took %.0f ms against a %.0f ms deadline%s%s\n", secondMs,
                     kSaveDeadlineMs, secondError.empty() ? "" : ", and failed: ", secondError.c_str());
    }
    const bool firstPaidIt = firstMs >= 0.75 * kSaveDeadlineMs;
    check(firstPaidIt, "the first call really did wait for the deadline, so the comparison above has a baseline");
    if (!firstPaidIt) {
        std::fprintf(stderr, "  the first call took %.0f ms, which is not a deadline being paid\n", firstMs);
    }

    // GOAL: S8.32 -- what the daemon PUBLISHES while a write is outstanding is that write,
    //       not the last save that succeeded. False if persistence still reads ok with the
    //       baseline save's detail: nothing else is slow any more -- that is what moving the
    //       write off the bus thread bought -- so a stale ok is the only thing an operator
    //       would ever see while nothing has reached the disk since the mount died.
    // WHY-THIS-SHAPE: read over the bus rather than from /health, so it does not depend on
    //       this daemon owning port 18462 -- a developer's own daemon on that port would turn
    //       this into a skip. Polled to a deadline with a failure branch, and the verdict is
    //       the LAST observation: the stall becomes reportable at almost the same instant the
    //       first caller is released, so a single reply taken here would land on either side
    //       of that by luck. Both the status and the wording are asserted, because a `fail`
    //       carrying the previous save's detail sends an operator to a disk error that does
    //       not exist.
    // CONTROL: `pastDeadline` in ConfigWriter::outcomeLocked() (backend/daemon/config_writer.cpp)
    //       forced to false, which is where the old stalledPastDeadline() test now lives, so a
    //       write that has stopped coming back reads as merely outstanding -> "FAIL: GetStatus
    //       reports the write that has not come back, rather than the last save that did",
    //       printing "persistence reads pending (<file>: a write has not come back, the choice
    //       may not be on disk yet)" -- one red in the run, no skips. Re-observed 2026-08-19
    //       against Outcome's current shape. Reverted.
    // WAS: (a) this said the mutation printed "persistence reads ok" -- the baseline save above.
    //       True when written 2026-08-12; bcac12f then made an outstanding write a failure in its
    //       own right. It is now neither: an outstanding write reports Pending, so what this
    //       block uniquely supplies is the STATUS being fail and the wording, against a mutation
    //       that leaves it pending. (b) it named ConfigWriter::stalledPastDeadline(), which no
    //       longer exists: the answer is derived once, inside outcomeLocked().
    std::string sawStatus;
    std::string sawDetail;
    bool namesTheStall = false;
    const auto pollDeadline = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < pollDeadline) {
        const StatusReply reply = getStatus(*proxy, std::chrono::seconds(10));
        if (reply.ok() && reply.json().contains("persistence") && reply.json()["persistence"].is_object()) {
            const nlohmann::json& persistence = reply.json()["persistence"];
            sawStatus = persistence.value("status", std::string());
            sawDetail = persistence.value("detail", std::string());
            namesTheStall =
                sawStatus == "fail" &&
                sawDetail.find("has not answered a write, the filesystem may be stalled") != std::string::npos;
            if (namesTheStall) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(namesTheStall, "GetStatus reports the write that has not come back, rather than the last save that did");
    if (!namesTheStall) {
        std::fprintf(stderr, "  persistence reads %s (%s)\n", sawStatus.empty() ? "(absent)" : sawStatus.c_str(),
                     sawDetail.c_str());
    }

    std::printf("daemon_system_test: with the config filesystem stalled, SetWallpaper cost %.0f ms then %.0f ms, "
                "and GetStatus %.0f ms\n",
                firstMs, secondMs, statusMs);

    // GOAL: S8.29's other half -- a daemon whose config write can never finish still stops.
    //       The write is on a thread nothing joins and that holds no reference to the manager,
    //       so shutdown must not wait for it. False if the process has to be killed: a user
    //       whose NFS server is down would be unable to stop or restart the daemon at all,
    //       and systemd would end up sending SIGKILL on every stop.
    // WHY-THIS-SHAPE: the fifo is deliberately never released. The write is therefore still
    //       parked in the kernel at the moment the daemon is asked to exit, which is the state
    //       being graded; releasing it first would grade an ordinary shutdown. The absence of
    //       the watchdog's line is asserted beside the exit code, because the watchdog also
    //       ends the process -- so an exit-code check alone would pass for a daemon that had
    //       to be forced out five seconds late.
    // CONTROL: `.detach()` in ConfigWriter::startLocked() (backend/daemon/config_writer.cpp)
    //       replaced by storing the thread in the writer and joining it in ~ConfigWriter ->
    //       "FAIL: a daemon with a config write it can never finish still exits 0 on SIGTERM"
    //       ("exit code 1") and "FAIL: it leaves through main() rather than through the teardown
    //       watchdog". The worker loops forever by design, so that mutation blocks EVERY
    //       teardown: ten other shutdown checks in this file went red with it, which is the sense
    //       in which abandoning this thread is load-bearing rather than incidental. Note what
    //       stayed green: "still exits on SIGTERM" above, because the forced path is still a
    //       normal exit -- which is why the exit CODE and the watchdog's line are asserted
    //       separately. Re-observed 2026-08-18. Reverted.
    //       That check has a control of its own: the same join plus `kTeardownDeadline` in
    //       backend/daemon/main.cpp raised to an hour, so nothing forces the process out at all -> "FAIL: a
    //       daemon with a config write it can never finish still exits on SIGTERM", printing
    //       "exited normally 0, timed out 1, signal 0" -- it had to be SIGKILLed. Reverted.
    const int exitCode = daemon.shutdown(SIGTERM);
    const std::string output = daemon.output();
    const bool stopped = daemon.exitedNormally() && !daemon.timedOut();
    check(stopped, "a daemon with a config write it can never finish still exits on SIGTERM");
    if (!stopped) {
        std::fprintf(stderr, "  exited normally %d, timed out %d, signal %d\n",
                     static_cast<int>(daemon.exitedNormally()), static_cast<int>(daemon.timedOut()),
                     daemon.termSignal());
    }
    if (output.find("Sanitizer: reported") != std::string::npos) {
        check(false, "the stalled-write daemon's sanitizer reported no warnings (its exit code is the sanitizer's "
                     "here, so the exit-code check cannot run)");
    } else {
        check(exitCode == 0, "a daemon with a config write it can never finish still exits 0 on SIGTERM");
        if (exitCode != 0) std::fprintf(stderr, "  exit code %d\n", exitCode);
    }
    check(output.find("shutdown blocked") == std::string::npos,
          "it leaves through main() rather than through the teardown watchdog");
    awaitNameRelease(names, "the phase after the stalled-write phase");
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "daemon_system_test";

    // Wrapped in a Check rather than called directly because every phase below takes one:
    // the std::function is what lets a phase be handed the grader without this file's
    // reporting internals.
    const Check check = wallpaperx::testing::check;

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

    // GOAL: X.3 -- a host that cannot supply a temporary directory makes this run report,
    //       not abort, and does not have the missing directory recorded against the daemon.
    //       False if `TMPDIR=/nonexistent ./wallpaperx-daemon-system-test` dies on SIGABRT,
    //       or exits with a FAIL line, or exits 0.
    // WHY-THIS-SHAPE: a positive probe of the host, taken before anything is spawned, rather
    //       than classifying what main()'s catch-all caught. Routing that catch-all to the
    //       skip channel would be the smaller edit and is the trap: it also catches the
    //       sdbus::Error a genuinely broken daemon raises, so every real defect that escapes
    //       a phase would become ctest "Skipped" instead of red. Nothing the daemon does can
    //       reach this branch -- it is decided before the first fork.
    //       Skip, not the phaseSetupFault channel this file grades a dead fork with: the sibling
    //       tier (config_store_test) already answers this same host state with 77, and one of
    //       the two reporting the machine's missing /tmp as red would have the two tiers
    //       disagree about whose fault it is.
    // CONTROL: run as `TMPDIR=/nonexistent/nowhere ./wallpaperx-daemon-system-test`. Before
    //       this branch: "FAIL: unexpected exception: scratch XDG_CONFIG_HOME could not be
    //       created", then "terminate called ... temp_directory_path" from harnessTempPrefix()
    //       in the isolation guard below, and exit 134 with the rest of the report lost.
    //       After: one NOT VERIFIED line naming temp_directory_path and $TMPDIR, and exit 77.
    //       Second arm, $TMPDIR pointed at a mode-500 directory: NOT VERIFIED naming "a
    //       directory could not be created under ...", exit 77.
    //       Countercheck that this did not widen the skip channel: with a normal $TMPDIR and
    //       the GPU/tier printf in backend/daemon/wallpaper_manager.cpp replaced by an
    //       unrelated line, the run still prints FAIL and exits 1, not 77. Reverted.
    if (const std::string scratchFault = scratchDirectoryFault(); !scratchFault.empty()) {
        notVerified("every goal in this file",
                    "this host cannot give the harness a scratch directory, and every daemon this file spawns "
                    "needs one for its XDG_CONFIG_HOME: " +
                        scratchFault);
        // Returned before the isolation guard below rather than falling through to it: no
        // daemon was spawned, so there is nothing for that guard to attribute, and it would
        // otherwise run with the empty prefix this same host fault produces.
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // GOAL: this suite must not touch the developer's own wallpaper config. Every daemon it
    //       spawns gets an XDG_CONFIG_HOME of the harness's own, and this is the check that
    //       the arrangement actually holds. False if a run creates or edits the config file
    //       named by THIS process's environment.
    // WHY-THIS-SHAPE: witnessed and compared, not asserted absent. Persistence is a shipped
    //       feature, so a developer running this suite may legitimately have a real
    //       config.json with their own wallpapers in it; "it does not exist" would be red on
    //       their machine and would teach the next reader to delete the check. Comparing
    //       contents protects the populated case and the empty one alike. Sampled before
    //       anything is spawned and graded outside the try, so an exception on the way
    //       through does not skip it.
    //       A change is attributed before it is graded: only a new file holding one of this
    //       run's own paths is this suite's doing. The developer's own daemon is tolerated
    //       elsewhere in this file (it holding port 18462 is a skip, not a finding) and it
    //       rewrites this file on every SetWallpaper, so an unattributable change is reported
    //       as a guard that could not conclude -- still red, through the harness channel --
    //       rather than as this suite having clobbered a real config.
    // CONTROL: both branches, run with this process's own XDG_CONFIG_HOME pointed at a
    //       scratch directory so nothing real was touched, and that directory's config.json
    //       rewritten from outside two seconds into the run. Holding a path under
    //       test_assets -> "FAIL: no daemon this run spawned touched the config file of the
    //       developer running it", printing "  wrote 141 bytes to ..., which held something
    //       else before this run". Holding an unrelated path -> "HARNESS FAULT -- the
    //       config-isolation guard ... changed during this run but holds none of this run's
    //       paths", and no FAIL. The scratch directory was removed afterwards. An earlier
    //       round controlled the accusing branch by removing the XDG_CONFIG_HOME injection in
    //       SpawnedDaemon's constructor (tests/spawned_daemon.h) instead; that mutation writes
    //       this suite's wallpapers into the developer's real config -- it did, once, by
    //       accident -- and is not repeated.
    const RealConfigWitness realConfig = witnessRealConfig();

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
        // GOAL: X.3 -- on a host where the fork preamble cannot complete (PR_SET_PDEATHSIG
        //       refused by a hardened kernel or a seccomp profile, a failed dup2, a binary
        //       that will not exec) this run reports the harness, not the daemon.
        // WHY-THIS-SHAPE: the fault is consulted only when the name never appeared, and the
        //       check is then skipped rather than reworded. Grading it as `check(cameUp ||
        //       startupFault() != nullptr, ...)` -- the smaller edit -- would turn every
        //       preamble death into a *pass*, which is the vacuous-green direction of the
        //       same defect.
        // CONTROL: armAgainstParentDeath() forced to return false. Before this branch the
        //       run printed "FAIL: the daemon acquires io.wallpaperx.Daemon on the session
        //       bus" and three more name-acquisition FAILs; after it, 8 HARNESS FAULT lines
        //       naming PR_SET_PDEATHSIG, 0 FAILs, and exit 1. Reverted.
        const bool startupWasHarnessFault =
            !cameUp && reportedStartupFault(daemon, "the bus-name, rendering and SIGTERM checks");
        if (!startupWasHarnessFault) {
            check(cameUp, "the daemon acquires io.wallpaperx.Daemon on the session bus");
        }

        if (cameUp) {
            // GOAL: a process this harness execs inherits RLIMIT_CORE=0, so a daemon that
            //       dies on SIGABRT -- which is what a control run makes it do, repeatedly
            //       -- writes no core and does not feed the host's core collector.
            // WHY-THIS-SHAPE: read out of the DAEMON's /proc, not asserted in this process.
            //       The property is inheritance across execve, and the two mechanisms
            //       suppressCoreDump() sets behave oppositely there: execve resets
            //       PR_SET_DUMPABLE and leaves RLIMIT_CORE alone. A check on the forking
            //       process's own limit -- the obvious cheaper version -- would stay green
            //       with the only half that survives the exec removed.
            // CONTROL: two, reverted. The setrlimit deleted from suppressCoreDump() -> one
            //       FAIL, "the daemon inherits a core limit of 0 from the harness that
            //       spawned it", printing "its /proc reports 'unlimited'". And the pid
            //       replaced by -1, so no /proc entry exists -> the NOT VERIFIED row below
            //       and ctest Skipped, rather than a FAIL blaming the daemon.
            const std::string coreLimit = softCoreLimit(daemon.pid());
            if (coreLimit.empty()) {
                // X.3: an unreadable /proc is this host's answer, not the daemon's, and a
                // daemon that died before it could be read is already graded above.
                notVerified("the inherited core limit",
                            "the daemon's /proc/<pid>/limits could not be read for a 'Max core file size' row");
            } else {
                check(coreLimit == "0", "the daemon inherits a core limit of 0 from the harness that spawned it");
                if (coreLimit != "0") {
                    std::fprintf(stderr, "  its /proc reports '%s'\n", coreLimit.c_str());
                }
            }

            // A second instance must lose. createSessionBusConnection() requests the name
            // without SD_BUS_NAME_REPLACE_EXISTING, so the second process is expected to
            // fail its name request and exit non-zero rather than take the desktop's
            // wallpaper daemon away from the one already serving it.
            SpawnedDaemon second;
            const int secondExit = second.waitForExit();
            // GOAL: X.3 in its vacuous-green direction -- a second instance that died in the
            //       fork preamble must not be reported as one the bus refused.
            // WHY-THIS-SHAPE: this phase's verdict is an exit code, and 125/126/127 are
            //       non-zero normal exits, so "exited normally and non-zero" -- the check
            //       below -- is satisfied by a child that never ran the daemon at all. Unlike
            //       the name-acquisition sites this one goes GREEN rather than red, so no
            //       FAIL would ever point at it. Asked after waitForExit(), which is why
            //       startupFault() has a post-reap arm.
            // CONTROL: the binary path suffixed with "-nope" for the daemons spawned with no
            //       extra environment, so only THIS child fails to exec and the first daemon
            //       still comes up -> "HARNESS FAULT -- the second-instance checks (S6.28)
            //       could not be set up: the forked child could not execute the binary it was
            //       given", 0 FAILs. With this branch made inert and the same mutation, the
            //       two exit-code checks below stayed silent -- check() prints only on
            //       failure, so they passed on a child that never ran the daemon -- while
            //       "says why it exited" and the FileExists check went red instead, blaming
            //       the daemon for dying silently. Both reverted.
            const bool secondWasHarnessFault = reportedStartupFault(second, "the second-instance checks (S6.28)");
            if (secondWasHarnessFault) {
                std::printf("daemon_system_test: the second-instance checks are skipped -- see the harness fault "
                            "above; that process never became a daemon.\n");
            }
            if (!secondWasHarnessFault) {
                // Both conditions, because reap() reports a timeout as -1 too: a second
                // daemon that wrongly took the name and then ran forever would be SIGKILLed
                // by the test and satisfy `secondExit != 0` -- reporting the name as refused
                // when it was in fact stolen.
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
                // CONTROL: backend/daemon/main.cpp patched to throw before requesting the name
                //       (`throw std::runtime_error("no")` above createSessionBusConnection)
                //       -> "FAIL: the second instance is refused with
                //       org.freedesktop.DBus.Error.FileExists", while the pre-existing
                //       "says why it exited" check stayed green. Reverted.
                check(secondOutput.find("org.freedesktop.DBus.Error.FileExists") != std::string::npos,
                      "the second instance is refused with org.freedesktop.DBus.Error.FileExists");
            }

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
            std::printf("daemon_system_test: shutdown checks skipped -- %s, so its exit says nothing about the "
                        "shutdown path.\n",
                        startupWasHarnessFault ? "that daemon was never started (the harness fault above)"
                                               : "the daemon never acquired the bus name");
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

            // GOAL: the busiest daemon this suite runs -- a device created, wallpapers set,
            //       a hundred concurrent calls behind it -- still finishes teardown inside
            //       main.cpp's five-second budget, so it leaves through main() rather than
            //       through the watchdog's _Exit. False if the daemon starts taking the
            //       forced path routinely: teardown would then be abandoned every stop, and
            //       "stopped cleanly" above would go with it.
            // WHY-THIS-SHAPE: the absence of the forced line, asserted separately from
            //       "stopped cleanly" rather than folded into it. The two are not the same
            //       claim -- the forced path writes with write(2) straight to fd 2 while the
            //       clean line goes through stdio, so a build that printed both, or that
            //       raced them, would satisfy one and not the other -- and only this one
            //       names the deadline as the thing that fired.
            //       What it deliberately does NOT establish: what the forced path DOES when
            //       it is taken. That was measured out of suite (config_store.cpp's open made
            //       blocking and the load deadline raised to an hour, so main parks in the
            //       manager's constructor): the daemon wrote exactly this line to stderr and
            //       exited 1. No committed check can reach it -- the fixture would have to be
            //       teardown that genuinely wedges, which needs a filesystem this harness can
            //       hang on demand.
            // CONTROL: `kTeardownDeadline` in backend/daemon/main.cpp set to 0 seconds, so every stop takes
            //       the forced path -> "FAIL: the daemon leaves through main() rather than
            //       through the teardown watchdog", "FAIL: the daemon exits 0 on SIGTERM"
            //       ("exit code 1") and "FAIL: the daemon runs its destructor chain on the way
            //       out". Reverted.
            const bool leftThroughMain = daemonOutput.find("shutdown blocked") == std::string::npos;
            check(leftThroughMain, "the daemon leaves through main() rather than through the teardown watchdog");
            check(names.waitFor(false), "the bus name is released once the daemon exits");
        }

        // The other signal in the same sigset. Cheap -- it never renders -- and without
        // it a typo that dropped SIGINT from the mask would be invisible, since SIGTERM
        // alone exercises one sigaddset call out of two.
        {
            SpawnedDaemon interrupted;
            const bool interruptedCameUp = names.waitFor(true);
            const bool interruptedWasHarnessFault =
                !interruptedCameUp && reportedStartupFault(interrupted, "the SIGINT shutdown checks");
            if (!interruptedWasHarnessFault) {
                check(interruptedCameUp, "a fresh daemon acquires the name after the first released it");
            }
            const int interruptExit = interrupted.shutdown(SIGINT);
            const std::string interruptOutput = interrupted.output();
            if (!interruptedCameUp) {
                // Same reasoning as the SIGTERM block: a daemon that never started tells
                // us nothing about how it shuts down, and grading it anyway turns one
                // startup fault into three failures pointing at the signal path.
                std::printf("daemon_system_test: SIGINT checks skipped -- %s.\n",
                            interruptedWasHarnessFault ? "that daemon was never started either (the harness fault "
                                                         "above)"
                                                       : "that daemon never acquired the bus name either");
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

        // Step 8. Each spawns daemons of its own, pointed at an XDG_CONFIG_HOME it creates
        // and deletes, so they run after every phase above has released the name and before
        // the descriptor accounting below -- their proxies and httplib clients close their
        // sockets at scope exit, which that accounting would otherwise report as a leak.
        checkConfigRestart(names, validationEnv, check);
        // Step 10, and beside the restart phase above because it is the same shape one goal
        // further on: that one restarts a wallpaper, this one restarts the connector name a
        // plugin instance identifies itself by. Its own phase rather than more lines inside
        // checkConfigRestart, so that a host that cannot set it up reports S10.5 as
        // unexercised instead of hiding inside a skip that names only the Step 8 goals.
        checkOutputRestart(names, validationEnv, check);
        checkStartsOnAnyConfig(names, validationEnv, check);
        checkStoppableOnAnyConfigFile(names, validationEnv, check);
        checkConfigPreservationOwnership(names, validationEnv, check);
        // Beside the phase above because it grades the other half of the same ordering rule:
        // that one, that nothing writes before the bus name is held; this one, that the
        // repair a start owes its config is written after the copy aside rather than over it.
        checkRepairWrittenAfterPreservation(names, validationEnv, check);
        checkShutdownDuringStartup(names, validationEnv, check);
        // The other side of the same window: that one grades a signal landing after the name
        // was taken, this one a signal landing before it was asked for.
        checkStopBeforeClaiming(names, validationEnv, check);
        checkUnreadableRestore(names, validationEnv, bluePath, check);
        checkConfigLocation(names, validationEnv, bluePath, check);
        checkFileSizeLimit(names, validationEnv, bluePath, check);
        checkStalledConfigWrite(names, validationEnv, bluePath, check);
        // Last of the daemon-spawning phases, and on buses of their own: each wedges a render
        // worker on purpose, so the daemon it leaves behind cannot be stopped cleanly and must
        // not be holding the name any phase above still needs.
        checkStuckTeardownIsBounded(validationEnv, bluePath, check);
        checkSecondSignalEndsAStuckStop(validationEnv, bluePath, check);

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
        ++wallpaperx::testing::g_failures;
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

    if (realConfig.file.empty()) {
        // No usable XDG_CONFIG_HOME or HOME in this process either, so there is no file the
        // spawned daemons could have damaged. Printed rather than silent: the guard below
        // did not run, and a reader must not take its silence for a verdict.
        std::printf("daemon_system_test: this process has no usable XDG_CONFIG_HOME or HOME, so there is no real "
                    "config file for the spawned daemons to have touched.\n");
    } else {
        const RealConfigWitness after = witnessRealConfig();
        const bool untouched = after.existed == realConfig.existed && after.contents == realConfig.contents;
        // GOAL: a prefix this run could not compute never attributes the developer's config
        //       to this suite. False if a host that lost its temporary directory mid-run
        //       turns an unrelated edit into "a daemon this run spawned wrote it".
        // WHY-THIS-SHAPE: the empty case is tested before find(), not left to find(): an empty
        //       needle is found at position 0 of EVERY string, so passing it straight through
        //       -- the shorter line -- reads as a match for whatever the file now contains and
        //       takes the accusing branch on no evidence at all.
        // CONTROL: harnessTempPrefix() forced to return {}, this process's XDG_CONFIG_HOME
        //       pointed at a scratch config.json rewritten two seconds into the run with a
        //       path this run does not own. Guarded (as written) -> HARNESS FAULT naming the
        //       missing half of the attribution, no FAIL. With `!ourPrefix.empty() &&` removed
        //       -> "FAIL: no daemon this run spawned touched the config file of the developer
        //       running it", printing "wrote 40 bytes to ...". Both mutations reverted.
        const std::string ourPrefix = harnessTempPrefix();
        const bool holdsOurPaths = (!ourPrefix.empty() && after.contents.find(ourPrefix) != std::string::npos) ||
                                   after.contents.find(testAssetDir()) != std::string::npos;
        if (untouched) {
            // Nothing to say: the guard held. A message here would be one more line between a
            // reader and the failures above.
        } else if (holdsOurPaths) {
            // The file changed AND holds a path this run owns, so a daemon this run spawned
            // demonstrably wrote it.
            check(false, "no daemon this run spawned touched the config file of the developer running it");
            std::fprintf(stderr, "  wrote %zu bytes to %s, which %s before this run\n", after.contents.size(),
                         after.file.string().c_str(), realConfig.existed ? "held something else" : "did not exist");
        } else {
            // It changed while this run was in flight but holds none of this run's paths.
            // This suite already tolerates the developer's own daemon running (fetchHealth
            // treats it holding port 18462 as a skip), and that daemon rewrites the config on
            // every SetWallpaper -- so blaming the suite here would send the reader hunting
            // for a leak in SpawnedDaemon that is not there. Still red, through the harness
            // channel, because the guard could not conclude.
            const std::string why =
                "the developer's config file at " + after.file.string() +
                " changed during this run but holds none of this run's paths, so another process on this machine -- "
                "most likely the developer's own daemon -- wrote it and the guard could not conclude" +
                (ourPrefix.empty() ? " (and this host stopped supplying a temporary directory mid-run, so only the "
                                     "test-asset half of the attribution was available)"
                                   : "");
            phaseSetupFault("the config-isolation guard", why.c_str());
        }
    }

    // Asked before the not-verified count, so a run that is both broken and incomplete
    // reports as broken: a skip would hide the failures behind ctest's "Skipped".
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    // Red, and not a skip. Same grading as device_affinity_probe's harness faults: a host
    // that cannot fork-and-exec a child cannot run dbus_service or the renderer probes
    // either, so reporting this one alone as "Skipped" would mark it optional while its
    // siblings went red for the same cause. The lines above name the real reason.
    if (g_phaseSetupFaults > 0) {
        std::fprintf(stderr,
                     "daemon_system_test: %d phase(s) could not be set up by the harness -- see the HARNESS FAULT "
                     "lines above. No conclusion about the daemon either way.\n",
                     g_phaseSetupFaults);
        return 1;
    }
    // This reads tests/test_report.h's own harness counter, which nothing here increments and
    // this epilogue therefore never summarises. A future site reaching for the shared
    // harnessFailure() by mistake is caught by the guard inside rather than dropped.
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
