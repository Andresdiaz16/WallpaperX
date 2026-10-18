// SPDX-License-Identifier: GPL-2.0-or-later

// The real daemon binary, in its own process, over a private session bus. What only this can
// see: a frame mapped by a process that never allocated it, main.cpp as a program (name
// acquisition, a second instance refused, signals, the destructor chain), and the shipped
// 1920x1080 geometry, which every in-process test overrides.
//
// Nothing waits on a sleep for a render: FrameReady says a frame is ready, and a negative case
// is settled by a later request that does signal, the render thread being FIFO. A check that
// cannot be conclusive on this host says so and is not counted.

#include "test_image_dir.h"
#include "daemon/dbus_service.h"
// The daemon's own answers for its port, config path, display escaping and crash record, so a
// change there fails this file instead of leaving it checking a stale literal.
#include "daemon/http_server.h"
#include "daemon/config_store.h"
#include "daemon/log_escape.h"
#include "daemon/crash_record.h"
#include "daemon/ring_depth.h"
#include "tests/alloc_fail_control.h"
#include "tests/client/blocking_config_write.h"
#include "tests/client/daemon_client.h"
#include "tests/client/http_probe.h"
#include "tests/client/scratch_dir.h"
#include "tests/name_watcher.h"
#include "tests/self_exec.h"
#include "tests/served_pixels.h"
#include "tests/spawned_daemon.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include "support/exported_frame_map.h"
#include "support/forked_child.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <vulkan/vulkan.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cctype>
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
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using wallpaperx::DbusService;
using wallpaperx::testing::blockedIn;
using wallpaperx::testing::Check;
using wallpaperx::testing::validationWaived;
using wallpaperx::testing::Frame;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::getFrame;
using wallpaperx::testing::getStatus;
using wallpaperx::testing::execChildOrDie;
using wallpaperx::testing::getStatusOrExplain;
using wallpaperx::testing::NameWatcher;
using wallpaperx::testing::openFdCount;
using wallpaperx::testing::ScratchDir;
using wallpaperx::testing::ScratchDirectoryLost;
using wallpaperx::testing::SpawnedDaemon;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::sanitizerReportedIn;
using wallpaperx::testing::StatusReply;
using wallpaperx::testing::wallpaperStatus;

namespace {

// The daemon's shipped default, restated so a change to it fails here rather than moving this
// test along with it.
constexpr uint32_t kDaemonWidth = 1920;
constexpr uint32_t kDaemonHeight = 1080;

// Monitor ids are partitioned per phase: "a monitor with no frame" changes meaning once another
// phase has touched the id.
constexpr int32_t kMonitorFirstFrame = 0;
constexpr int32_t kMonitorTiming = 1;
constexpr int32_t kMonitorLoadBase = 2;    // .. kMonitorLoadBase + loadCount - 1, clamped by kMaxLoadMonitors
constexpr int32_t kMonitorStormBase = 20;  // .. kMonitorStormBase + kStormMonitors - 1
constexpr int32_t kStormMonitors = 4;
constexpr int32_t kMonitorBarrierPre = 30;
constexpr int32_t kMonitorBarrierPost = 31;
// Clear of the responsiveness burst (2..17) and the storm (20..23). Two, because the
// closed-stdout phase renders once with its stdout readable and once after breaking it.
constexpr int32_t kMonitorClosedStdoutPre = 25;
constexpr int32_t kMonitorClosedStdout = 24;
constexpr int32_t kMonitorLostPort = 27;
// 18, 19, 28 and 29 belong to phases that run a daemon and an XDG_CONFIG_HOME of their own and
// compare ids only with their own, so they share them; each declares its ids where used. Ids are
// never derived by arithmetic over a case list, which would grow silently past 31.

constexpr int kStormThreads = 4;

constexpr int kGpuInitWatchers = 3;
// Monotonicity is per observer, and an observer's first look at a monitor compares nothing, so
// a narrow pool and many iterations make most observations real comparisons (counted and asserted).
constexpr int kStormIterations = 24;

// Each queued monitor costs a 1080p exportable image (~8 MB) for the daemon's life.
constexpr int kMaxLoadMonitors = 16;

// How much dearer a SetWallpaper that renders may be to issue than one that does not, before S6.9
// calls it rendering inline. Measured: 0.99-1.03 unmutated over twenty runs (NVIDIA, lavapipe,
// tsan+lavapipe); 5.33-19.71 for a handler that waits for the render.
constexpr double kIssueCostRatioBound = 2.0;

using wallpaperx::testing::kProcessTimeout;

// One constant because two sites assert its presence and one its absence: a reworded message
// turns the presence checks red rather than silently hollowing the absence check.
constexpr const char* kSignalShutdownLine = "received, shutting down";

std::string testAssetDir() { return wallpaperx::testing::testImageDir(); }

// A goal the host or harness could not exercise: exit 77, a ctest skip, never a FAIL (X.3).
using wallpaperx::testing::notVerified;

// A child this file could not start: the harness's fault, red, but never graded as the daemon's.
// Kept apart from test_report.h's harnessFailure(), whose counter this file's epilogue does not read.
int g_phaseSetupFaults = 0;

void phaseSetupFault(const char* what, const char* detail) {
    std::printf("daemon_system_test: HARNESS FAULT -- %s could not be set up: %s. This says nothing about the "
                "daemon.\n",
                what, detail);
    std::fflush(stdout);
    ++g_phaseSetupFaults;
}

// Runs one phase. A lost scratch directory is the host's fault; any other exception out of a phase --
// an sdbus::Error from a daemon that died under it -- is a FAIL naming that phase, and the phases
// after it still run.
// GOAL: X.3 for a host that loses its scratch directory mid-run (the startup probe passed): each
//       phase that meets it reports a HARNESS FAULT naming itself, never a FAIL against the daemon.
//       S15.328's sibling: an exception escaping one phase fails that phase by name and ends no other.
// CONTROL: test-side: (a) tests/spawned_daemon.h's constructor made to throw ScratchDirectoryLost from
//       its third spawn on, --phase '^(hangupShutdown|outputVisibleLog|commandLine)$' -> "HARNESS FAULT --
//       commandLine could not be set up: control: the scratch directory went away", and no FAIL line;
//       (b) the same, with the ScratchDirectoryLost catch below removed -> "FAIL: commandLine: unexpected
//       exception: control: the scratch directory went away".
template <class Phase>
void hostGuarded(const char* phase, Phase&& run) {
    wallpaperx::testing::guardedPhase(phase, [&] {
        try {
            run();
        } catch (const ScratchDirectoryLost& e) {
            phaseSetupFault(phase, e.what());
        }
    });
}

using wallpaperx::testing::Clock;

double millis(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(d).count();
}

// The cheapest call in a burst, the closest thing to the handler's own cost; one preemption
// moves a total or a mean by more than the effect measured.
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

// A solid RGBA PNG: scaled to any size, every pixel of the frame is still a known value.
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


// The prefix of every scratch directory this run makes (in step with tests/client/scratch_dir.h),
// so the isolation guard can attribute a config file to this run. Empty when there is no temporary
// directory -- never thrown, since the guard runs outside main()'s try -- and the caller must not
// use empty as a needle: find("") matches every string.
std::string harnessTempPrefix() {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    return (base / ("wpx_test_" + std::to_string(::getpid()) + "_")).string();
}

// Empty when this host can give the suite a scratch directory; otherwise why it cannot. A
// positive probe before anything is spawned, so nothing the daemon does can reach this answer.
std::string scratchDirectoryFault() {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        // Empty and unset worded apart: an empty $TMPDIR otherwise prints as nothing.
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

// True when this daemon's child died in its fork preamble, the fault already reported on the
// harness channel. Every startup site goes through this, so no preamble exit code is read as a
// verdict on the daemon.
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

// As renderer/tests/support/private_bus.sh escapes them: a path as XML element text, and as a D-Bus
// address value (every byte outside [-0-9A-Za-z_/.*] as %hh, which leaves nothing XML would read).
std::string xmlText(const std::string& text) {
    std::string out;
    for (const char c : text) {
        out += c == '&' ? std::string("&amp;") : c == '<' ? std::string("&lt;") : c == '>' ? std::string("&gt;") : std::string(1, c);
    }
    return out;
}
std::string addressValue(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (plain || c == '-' || c == '_' || c == '/' || c == '.' || c == '*') {
            out += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02x", static_cast<unsigned>(c));
            out += hex;
        }
    }
    return out;
}

// A session bus of this test's own, to be killed under a running daemon; killing ctest's would
// take this test's own connection with it.
class NestedBus {
public:
    NestedBus() {
        const std::string binaryPath = WALLPAPERX_DBUS_DAEMON_BINARY;
        if (binaryPath.empty()) {
            m_reason = "no dbus-daemon binary was found at configure time";
            return;
        }

        // Not --session, which can activate every installed service and listens in the shared
        // /tmp: the wrapper's configuration, listening in the entry's private runtime directory.
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (runtime == nullptr || *runtime == '\0') {
            m_reason = "XDG_RUNTIME_DIR is unset, so the nested bus has nowhere private to listen";
            return;
        }
        static int started = 0;
        const std::string confPath =
            std::string(runtime) + "/nested-bus-" + std::to_string(getpid()) + "-" + std::to_string(++started) + ".conf";
        {
            std::ofstream conf(confPath);
            conf << "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
                    " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n<busconfig>\n  <listen>unix:dir="
                 << addressValue(runtime) << "</listen>\n  <include>" << xmlText(WALLPAPERX_ISOLATED_SESSION_CONF)
                 << "</include>\n</busconfig>\n";
            if (!conf.flush()) {
                m_reason = "the nested bus's configuration could not be written to " + confPath;
                return;
            }
        }

        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) {
            m_reason = "pipe2 failed";
            return;
        }

        // Before fork(): with sdbus threads running, allocating between fork() and exec() can deadlock.
        std::string binary = binaryPath;
        std::string configFile = "--config-file=" + confPath;
        std::string nofork = "--nofork";
        std::string printAddress = "--print-address";
        char* argv[] = {binary.data(), configFile.data(), nofork.data(), printAddress.data(), nullptr};

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

        // Bounded; a bus that fails to start closes the pipe and ends this at EOF. Either way a
        // setup failure, never a property of the daemon.
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

        // GOAL: X.3 -- a bus child that died in its fork preamble is this harness failing, not
        //       dbus-daemon misbehaving.
        // WHY-THIS-SHAPE: the reasons above all speak of dbus-daemon, and a refused arming produces
        //       "exited before printing an address" (measured), so the preamble is asked first.
        // CONTROL: armAgainstParentDeath forced false -> "HARNESS FAULT -- the connection-loss
        //       checks (S6.35) could not be set up: the forked child could not arm itself against
        //       its parent's death", no NOT VERIFIED naming dbus-daemon. Reverted.
        if (m_address.empty() && m_pid > 0) {
            siginfo_t info{};
            info.si_pid = 0;
            if (waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                info.si_pid != 0 && info.si_code == CLD_EXITED) {
                m_preambleFault = wallpaperx::testing::forkPreambleFault(info.si_status);
            }
        }

        // Drained for the child's whole life, not closed: dbus-daemon may write to fd 1 after
        // the address line, and a readerless pipe kills it (SIGPIPE) while an unread one stalls
        // it once full -- either blamed on the daemon under test.
        m_readFd = fds[0];
        try {
            m_drain = std::thread([this] { drainLoop(); });
        } catch (...) {
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
    // Non-null when the bus child died in the fork preamble: this harness's fault.
    const char* preambleFault() const { return m_preambleFault; }
    const std::string& address() const { return m_address; }

    // SIGKILL, not SIGTERM: a bus that vanishes, which a client cannot prepare for.
    void kill() {
        if (m_pid > 0) {
            ::kill(m_pid, SIGKILL);
            int status = 0;
            waitpid(m_pid, &status, 0);
            m_pid = -1;
        }
        // After the reap: the drain ends at EOF, which comes once the child's write end is gone.
        if (m_drain.joinable()) m_drain.join();
        if (m_readFd >= 0) {
            close(m_readFd);
            m_readFd = -1;
        }
    }

private:
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
    const char* m_preambleFault = nullptr;  // a literal from forkPreambleFault()
};

// Whether VK_LAYER_KHRONOS_validation loads here, asked with the environment the daemon child
// inherits. Without it, "no [vulkan] warnings" is equally true of a daemon nobody was checking.
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

// The validation layer's lines in `output`, less Device's notice that VK_EXT_validation_features
// is missing, which is about the layer build and not this daemon's Vulkan use.
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

// Whether `output` holds `line` as a whole line.
bool hasLine(const std::string& output, const std::string& line) {
    for (size_t at = output.find(line); at != std::string::npos; at = output.find(line, at + 1)) {
        const size_t end = at + line.size();
        if ((at == 0 || output[at - 1] == '\n') && (end == output.size() || output[end] == '\n')) return true;
    }
    return false;
}

// Whether one line carries "GPU: " and after it "Descriptor tier: ", the daemon's device line. One
// line: two unanchored find()s would accept the halves from unrelated lines.
bool reportsGpuAndTier(const std::string& output) {
    for (size_t start = 0; start < output.size();) {
        const size_t eol = output.find('\n', start);
        const std::string_view line(output.data() + start, (eol == std::string::npos ? output.size() : eol) - start);
        const size_t gpu = line.find("GPU: ");
        if (gpu != std::string_view::npos && line.find("Descriptor tier: ", gpu) != std::string_view::npos) return true;
        if (eol == std::string::npos) break;
        start = eol + 1;
    }
    return false;
}

// ---------------------------------------------------------------------------- //
// The pixel reader: this binary run again as `--read-blue-frame`, handed the frame's descriptors.
// ---------------------------------------------------------------------------- //
constexpr const char* kReaderMode = "--read-blue-frame";
constexpr int kReaderMatched = 0;
constexpr int kReaderMismatched = 1;
constexpr int kReaderBadArguments = 2;
constexpr int kReaderUnreadable = 77;

// `a;b;c`, or `-` for none.
std::vector<uint64_t> readerList(const std::string& text) {
    std::vector<uint64_t> values;
    if (text == "-") return values;
    std::stringstream in(text);
    for (std::string item; std::getline(in, item, ';');) values.push_back(std::stoull(item));
    return values;
}

std::string readerJoin(const std::vector<uint64_t>& values) {
    if (values.empty()) return "-";
    std::string out;
    for (const uint64_t v : values) out += (out.empty() ? "" : ";") + std::to_string(v);
    return out;
}

// Arguments: fd width height memorySize handleType modifier drmFormat uuid-bytes offsets pitches syncFds.
int readBlueFrameChild(int argc, char** argv) {
    wallpaperx::testing::FrameLayout layout;
    try {
        if (argc != 13) throw std::invalid_argument("expected 11 values after the mode");
        layout.fd = std::stoi(argv[2]);
        layout.width = static_cast<uint32_t>(std::stoul(argv[3]));
        layout.height = static_cast<uint32_t>(std::stoul(argv[4]));
        layout.memorySize = std::stoull(argv[5]);
        layout.handleType = std::stoi(argv[6]);
        layout.modifier = std::stoull(argv[7]);
        layout.drmFormat = static_cast<uint32_t>(std::stoul(argv[8]));
        for (const uint64_t byte : readerList(argv[9])) layout.deviceUuid.push_back(static_cast<uint8_t>(byte));
        layout.planeOffsets = readerList(argv[10]);
        layout.rowPitches = readerList(argv[11]);
        for (const uint64_t fd : readerList(argv[12])) layout.syncFds.push_back(static_cast<int>(fd));
    } catch (const std::exception& e) {
        std::printf("daemon_system_test (pixel reader): bad arguments: %s\n", e.what());
        return kReaderBadArguments;
    }

    std::string unreadable;
    const std::vector<unsigned char> image = wallpaperx::testing::readServedBgra(layout, unreadable);
    if (image.empty()) {
        std::printf("daemon_system_test (pixel reader): cannot read the frame: %s\n", unreadable.c_str());
        if (unreadable.rfind("(harness)", 0) == 0) return kReaderBadArguments;
        // readServedBgra() FAILs what the daemon served wrongly (a size or layout); only this exit code
        // carries that to the parent, which would otherwise read it as unreadable, a skip.
        return wallpaperx::testing::g_failures > 0 ? kReaderMismatched : kReaderUnreadable;
    }

    // The source is solid blue (R=0, G=0, B=255); the image is B,G,R,A.
    constexpr int kTolerance = 6;
    const size_t compared = image.size() / 4;
    unsigned char firstBad[4] = {0, 0, 0, 0};
    const size_t badPixels = wallpaperx::testing::countOffColour(image, 255, 0, 0, kTolerance, firstBad);
    const char* path = wallpaperx::testing::servedFrameIsMapped(layout) ? "mapped"
                                                                        : "imported through the plugin's GL importer";
    if (badPixels != 0) {
        std::printf("daemon_system_test (pixel reader): %zu of %zu pixels (%s) differ; expected B,G,R = 255,0,0, "
                    "first mismatch was %d,%d,%d\n",
                    badPixels, compared, path, firstBad[0], firstBad[1], firstBad[2]);
        return kReaderMismatched;
    }
    std::printf("daemon_system_test (pixel reader): %ux%u pixels (%s) verified across the process boundary\n",
                layout.width, layout.height, path);
    return kReaderMatched;
}

// Runs the reader on `frame`, whose descriptors are lent to it as inheritable copies at 10 or above
// (self_exec.h puts its readiness pipe on 3) and closed here once it has ended.
wallpaperx::testing::SelfExecResult runPixelReader(const wallpaperx::testing::FrameLayout& frame) {
    std::vector<int> lent;
    const auto lend = [&lent](int fd) {
        const int copy = ::fcntl(fd, F_DUPFD, 10);
        if (copy >= 0) lent.push_back(copy);
        return copy;
    };
    wallpaperx::testing::SelfExecResult result;
    const int fd = lend(frame.fd);
    std::vector<uint64_t> syncs;
    for (const int sync : frame.syncFds) {
        const int copy = lend(sync);
        if (copy >= 0) syncs.push_back(static_cast<uint64_t>(copy));
    }
    if (fd < 0 || syncs.size() != frame.syncFds.size()) {
        result.fault = std::string("the frame's descriptors could not be copied for the reader: ") + std::strerror(errno);
    } else {
        const std::vector<uint64_t> uuid(frame.deviceUuid.begin(), frame.deviceUuid.end());
        std::fflush(stdout);
        result = wallpaperx::testing::runSelf(
            {kReaderMode, std::to_string(fd), std::to_string(frame.width), std::to_string(frame.height),
             std::to_string(frame.memorySize), std::to_string(frame.handleType), std::to_string(frame.modifier),
             std::to_string(frame.drmFormat), readerJoin(uuid), readerJoin(frame.planeOffsets),
             readerJoin(frame.rowPitches), readerJoin(syncs)},
            std::chrono::seconds(60));
    }
    for (const int copy : lent) ::close(copy);
    return result;
}

// ---------------------------------------------------------------------------- //
// Phase 2 -- one frame, end to end, into a process that did not allocate it.
// ---------------------------------------------------------------------------- //
void checkFirstFrame(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& bluePath, const Check& check) {
    setWallpaper(proxy, "static", bluePath, kMonitorFirstFrame);
    const bool arrived = signals.wait(kMonitorFirstFrame, 1);
    check(arrived, "FrameReady(monitor, 1) arrives for the first static wallpaper");
    if (!arrived) return;

    const Frame frame = getFrame(proxy, kMonitorFirstFrame);

    check(fcntl(frame.fd.get(), F_GETFD) != -1, "the fd received from another process is open in this one");
    check(frame.width == kDaemonWidth && frame.height == kDaemonHeight,
          "the frame is the daemon's shipped default size (1920x1080), which no other test renders");

    // Printed only: an OPAQUE_FD (NVIDIA's /dev/nvidiactl) has no size, and -1 is its correct answer.
    // The mapped read checks a dma_buf's size itself (served_pixels.h).
    const off_t fdSize = lseek(frame.fd.get(), 0, SEEK_END);

    std::printf("daemon_system_test: monitor %d -> %ux%u, memory_size=%llu, handle_type=%d, modifier=0x%llx, "
                "fd is %s (%lld bytes)\n",
                kMonitorFirstFrame, frame.width, frame.height,
                static_cast<unsigned long long>(frame.memorySize), frame.handleType,
                static_cast<unsigned long long>(frame.drmModifier), wallpaperx::testing::fdDescription(frame.fd.get()).c_str(),
                static_cast<long long>(fdSize));

    // GOAL: S6.23 -- a process that never allocated the image reads every pixel of the real
    //       daemon's frame, on either export path: the opaque fd NVIDIA exports included.
    // WHY-THIS-SHAPE: readServedBgra() (tests/served_pixels.h) maps a LINEAR dma_buf and imports
    //       anything else through the plugin's GL importer, in a re-exec'd reader process: read
    //       in-process, NVIDIA's GL left 24 device descriptors open and failed the whole-run leak
    //       check below. Unreadable is notVerified(), a skip -- a printed "skipped" passed green.
    // CONTROL: renderer/renderers/static_renderer.cpp zeroing the decoded image, control_run.sh
    //       --phase '^firstFrameUnderStatusLoad$', 2026-10-08 -> "2073600 of 2073600 pixels
    //       (imported through the plugin's GL importer) differ", "FAIL: a process that never
    //       allocated the image reads the expected pixels" on daemon_system; "(mapped)" on the
    //       lavapipe twin. Held, restored. Test-side: GL UUID zeroed -> NOT VERIFIED, Skipped. Re-run
    //       2026-10-09 on daemon_system_lavapipe once the comparison moved to countOffColour()
    //       (served_pixels.h): "2073600 of 2073600 pixels (mapped) differ", the same FAIL. Held.
    const char* kContentGoal = "S6.23 -- the real daemon's frame read pixel by pixel from another process";
    const wallpaperx::testing::FrameLayout layout = wallpaperx::testing::layoutOf(frame);
    // GOAL: S15.431 -- a LINEAR dma_buf that carries a plane is read by mapping it, the path that grades
    //       the published offset, pitch and size; never handed to the GL importer, which reads through
    //       its own layout and would leave those unchecked.
    // WHY-THIS-SHAPE: asserted on the predicate the reader branches on, from the wire's own fields, so
    //       a FrameLayout producer that fills the plane in a form the predicate does not read goes red
    //       here rather than quietly importing.
    // CONTROL: test-side, by hand, 2026-10-09: servedFrameIsMapped() (tests/served_pixels.h) requiring a
    //       second plane -> "FAIL: a LINEAR dma_buf with a plane is read by mapping it" on
    //       daemon_system_lavapipe and dbus_service_lavapipe, while the reader printed "1920x1080 pixels
    //       (imported through the plugin's GL importer) verified" -- the pixels alone would not have shown it.
    //       Restored.
    if (frame.handleType == 1 && frame.drmModifier == wallpaperx::testing::kDrmModifierLinear && !frame.planes.empty()) {
        check(wallpaperx::testing::servedFrameIsMapped(layout), "a LINEAR dma_buf with a plane is read by mapping it");
    }

    // Read in a fresh process of this binary, not here: the GL driver keeps device descriptors open
    // past eglTerminate, which would land on this run's whole-process descriptor-leak check.
    const wallpaperx::testing::SelfExecResult reader = runPixelReader(layout);
    if (!reader.fault.empty() || !reader.reaped || !WIFEXITED(reader.status) ||
        (WEXITSTATUS(reader.status) != kReaderMatched && WEXITSTATUS(reader.status) != kReaderMismatched &&
         WEXITSTATUS(reader.status) != kReaderUnreadable)) {
        const std::string detail = reader.fault.empty() ? "it " + reader.how() : reader.fault;
        phaseSetupFault("the cross-process pixel reader", detail.c_str());
        return;
    }
    if (WEXITSTATUS(reader.status) == kReaderUnreadable) {
        notVerified(kContentGoal, "the reader process could not read the frame (its reason is printed above)");
        return;
    }
    check(WEXITSTATUS(reader.status) == kReaderMatched,
          "a process that never allocated the image reads the expected pixels through the published layout");
}

// GOAL: S6.18 -- a GetStatus reply never reports an uninitialised GPU beside a monitor that
//       already has a frame.
// WHY-THIS-SHAPE: falsifiable only while gpu.initialized is false, which ends at the first device
//       creation for good; so the watchers sample across checkFirstFrame's first SetWallpaper, the
//       only window in the suite, and are released only once each has had a reply (a barrier,
//       not the scheduler).
// COVERAGE-ASSERT: the flag must have been seen both false and true, or every reply was graded
//       after the window and zero inconsistencies would mean nothing.
// CONTROL: ensureGpu() publishing m_gpuStatus.initialized 1200 ms late from a detached thread ->
//       "FAIL: no GetStatus reply reports a frame before the GPU exists" over 8441 replies, and the
//       coverage assert red (0 post-init). Reverted. A narrow split of snapshot() into two lock
//       acquisitions does NOT reproduce (0 in ~25,000 replies): this catches halves that drift
//       far apart, not a narrow split.
// CONTROL (the Stopper scope guard): checkFirstFrame throwing on entry with the watchers live ->
//       "FAIL: unexpected exception: CONTROL: simulated sdbus failure mid-phase", exit 1; without
//       the guard, exit 134 and "terminate called". Reverted.
struct GpuInitWatch {
    std::atomic<bool> stop{false};
    // Watchers with one SUCCESSFUL reply: counting attempts would release the barrier for a
    // GetStatus that never worked and blame the timing.
    std::atomic<int> ready{0};
    std::atomic<int> replies{0};
    std::atomic<int> preInit{0};
    std::atomic<int> postInit{0};
    std::atomic<int> inconsistent{0};
    std::atomic<int> errors{0};
};

void gpuInitWatchThread(GpuInitWatch& watch) {
    // Everything inside a guard: an exception escaping a thread entry point is std::terminate.
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
            const StatusReply status = getStatus(*proxy);
            if (!status.ok()) {
                // Not a consistency violation, and not "ready": readiness is "answered once".
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
        // Paces the error path too: an instantly refusing daemon would otherwise spin three
        // threads at 100% and starve the render being measured.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void checkFirstFrameUnderStatusLoad(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& bluePath,
                                    const Check& check) {
    GpuInitWatch watch;
    std::vector<std::thread> watchers;
    watchers.reserve(kGpuInitWatchers);

    // Stops and joins on every path, exceptions included (checkFirstFrame's calls throw on a daemon
    // error), and exists before the first thread does: std::thread's constructor can itself throw.
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

    // Every watcher answered once before the first render is asked for; bounded, a watcher that
    // never reaches the bus is reported by the counts below.
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
// Graded by the cost of ISSUING a request that renders against one that does not (S6.9 below):
// "GetStatus answers quickly while renders are queued" cannot separate the designs, since an
// inline-rendering handler has finished the burst before GetStatus is sent.
//
// The GetStatus/GetFrame ordering is PRINTED, NOT ASSERTED: a GetFrame answered on the event
// loop does GPU work off the Device's thread, and the Device's pinning aborts the daemon before
// any ordering can be seen (controlled: the test reports a disconnect). That pinning is what
// enforces "no DBus handler touches the GPU". Slow non-GPU work in a handler is bounded by
// nothing here: the issue-cost ratio cancels any cost both bursts pay.
// ---------------------------------------------------------------------------- //

Clock::duration timeSetWallpaper(sdbus::IProxy& proxy, const char* type, const std::string& path, int32_t monitorId) {
    const auto callStart = Clock::now();
    setWallpaper(proxy, type, path, monitorId);
    return Clock::now() - callStart;
}

void checkResponsivenessUnderLoad(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& bluePath,
                                  const Check& check) {
    // One warm render (the device already exists) sizes the load.
    const auto renderStart = Clock::now();
    setWallpaper(proxy, "static", bluePath, kMonitorTiming);
    if (!signals.wait(kMonitorTiming, 1)) {
        check(false, "the timing render completes (responsiveness phase cannot run without it)");
        return;
    }
    const auto singleRender = Clock::now() - renderStart;

    // Distinct monitors, so nothing coalesces into one render.
    const double perRenderMs = std::max(1.0, millis(singleRender));
    const int loadCount = std::clamp(static_cast<int>(400.0 / perRenderMs) + 1, 4, kMaxLoadMonitors);

    // Built before the clock starts: a connection costs milliseconds.
    auto observer =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

    // Unmeasured: every monitor enters the map first, so each measured call saves a config of
    // constant size rather than one growing under the measurement.
    for (int i = 0; i < loadCount; ++i) {
        setWallpaper(proxy, "shader", bluePath, kMonitorLoadBase + i);
    }

    // GOAL: the premise of S6.9 below -- a `shader` request is recorded and NOT rendered; if it
    //       rendered, the two bursts would stop differing and the ratio would measure nothing.
    // WHY-THIS-SHAPE: the daemon's own `unsupported`, which shares isRenderableType() with the
    //       render path; waiting for an absent FrameReady cannot tell never from not yet.
    // CONTROL: isRenderableType() (backend/daemon/wallpaper_manager.h) accepting `shader` ->
    //       "FAIL: the shader burst is recorded as a wallpaper this build does not render", "0/16
    //       monitors report unsupported", on both ICD entries (the ratio read 0.99 and 0.93).
    //       Observed 2026-09-15. Reverted.
    const StatusReply inertStatus = getStatusOrExplain(proxy);
    int unsupportedSeen = 0;
    for (int i = 0; i < loadCount; ++i) {
        const nlohmann::json* entry = wallpaperStatus(inertStatus, std::to_string(kMonitorLoadBase + i));
        if (entry != nullptr && entry->value("unsupported", false)) ++unsupportedSeen;
    }
    const bool inertBurstIsInert = unsupportedSeen == loadCount;
    if (!inertBurstIsInert) {
        std::fprintf(stderr, "  %d/%d monitors report unsupported%s\n", unsupportedSeen, loadCount,
                     inertStatus.ok() ? "" : " -- and GetStatus did not answer, see above");
    }
    check(inertBurstIsInert, "the shader burst is recorded as a wallpaper this build does not render "
                             "(the S6.9 comparison below is meaningless otherwise)");

    // The control burst, on the same monitors. Not interleaved with the other: measured, pairs
    // lock the issue and worker loops into a phase where one call of each pair is ~3x the other,
    // which one by coin flip per run (X.2).
    std::vector<Clock::duration> inertSamples;
    inertSamples.reserve(static_cast<size_t>(loadCount));
    for (int i = 0; i < loadCount; ++i) {
        inertSamples.push_back(timeSetWallpaper(proxy, "shader", bluePath, kMonitorLoadBase + i));
    }

    std::vector<Clock::duration> staticSamples;
    staticSamples.reserve(static_cast<size_t>(loadCount));
    const auto loadStart = Clock::now();
    for (int i = 0; i < loadCount; ++i) {
        staticSamples.push_back(timeSetWallpaper(proxy, "static", bluePath, kMonitorLoadBase + i));
    }
    const auto issueTime = Clock::now() - loadStart;

    // The last monitor's frame, so its export sits behind every render in the FIFO; GetStatus is
    // issued after it. Plain variables: join() is the happens-before edge. jthread, so a daemon
    // dying mid-exchange unwinds rather than terminates.
    Clock::time_point frameDoneAt{};
    bool frameFailed = false;
    std::jthread frameRequest([&] {
        try {
            const Frame frame = getFrame(*observer, kMonitorLoadBase + loadCount - 1);
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
    check(!frameFailed, "the GetFrame issued while the queue was deep is answered rather than failing");

    bool round1Ok = true;
    for (int i = 0; i < loadCount; ++i) {
        if (!signals.wait(kMonitorLoadBase + i, 1)) round1Ok = false;
    }
    const auto drain = Clock::now() - loadStart;
    check(round1Ok, "every queued render completes and announces itself");

    const auto frameLatency = frameDoneAt - loadStart;
    std::printf("daemon_system_test: one render %.1f ms; issuing %d requests took %.1f ms, draining them %.1f ms; "
                "GetFrame behind the queue took %.1f ms and a GetStatus issued after it finished %.1f ms sooner\n",
                perRenderMs, loadCount, millis(issueTime), millis(drain), millis(frameLatency),
                millis(frameDoneAt - statusDoneAt));

    // Printed, never counted: see the phase comment.
    std::printf("daemon_system_test: GetStatus %s the GetFrame issued before it (corroborating, not asserted -- "
                "the Device's thread pinning is what makes a GPU-touching handler fatal)\n",
                statusDoneAt < frameDoneAt ? "overtook" : "did not overtake");

    const double inertFloor = minMillis(inertSamples);
    const double staticFloor = minMillis(staticSamples);
    const double ratio = inertFloor > 0 ? staticFloor / inertFloor : 0.0;
    // Sample counts printed, not asserted: one unconditional loop each, so nothing but an edit
    // here could make them differ.
    std::printf("daemon_system_test: cheapest SetWallpaper cost %.3f ms for a request that renders vs %.3f ms for one "
                "that does not (ratio %.2f, bound %.1f) over %zu and %zu samples; medians %.3f and %.3f\n",
                staticFloor, inertFloor, ratio, kIssueCostRatioBound, staticSamples.size(), inertSamples.size(),
                medianMillis(staticSamples), medianMillis(inertSamples));

    // Meaningful only if exactly one burst rendered; both premises are asserted above.
    if (!round1Ok || !inertBurstIsInert) {
        std::printf("daemon_system_test: the S6.9 issue-cost comparison is abandoned -- %s, so the two bursts do "
                    "not differ in the way the comparison assumes.\n",
                    round1Ok ? "the shader burst was not inert" : "the static burst never drained");
        return;
    }

    // GOAL: S6.9 -- issuing a burst of SetWallpaper calls that WILL render costs no more
    //       than issuing an identical burst that renders nothing. False for a daemon whose
    //       handler renders (or waits for the render) before replying.
    // WHY-THIS-SHAPE: against a `shader` burst, not the render drain: the drain is not daemon
    //       code and scales differently under a sanitizer (tsan read 0.94 against a 0.25 bound on
    //       a sound daemon). The two bursts share handler, validation and config save, differing
    //       only in isRenderableType(), so host and sanitizer cost cancel. The cheapest call of
    //       each, because only the renderable burst meets the worker's contention (tsan medians
    //       1.11-1.58 sound, cheapest 1.01-1.03); an inline renderer has no cheap call, each of the
    //       sixteen monitors being a fresh render.
    // CONTROL: DbusService::setWallpaper made to wait on a promise posted behind its render (and
    //       emitFrameReady detached, to avoid a deadlock) -> FAIL, NVIDIA 2.159 vs 0.040 ms (ratio
    //       53.90), lavapipe 5.343 vs 0.038 (138.88), bound 2.0. Observed 2026-09-15. Reverted.
    //       NOT covered: the handler's absolute cost, which cancels (a 5 ms sleep in
    //       ConfigStore::save left the ratio at 1.00); nothing in this suite bounds it.
    // A 0 ms cheapest call means this clock cannot resolve a round trip: not verified, never a
    // FAIL, and an inline renderer has no 0 ms call to hide behind it.
    if (inertFloor <= 0.0 || staticFloor <= 0.0) {
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
// Phase 4 -- several independent clients, concurrently, each on its own connection (one shared
// proxy would exercise sdbus's locking, not the daemon's). Only what holds under every
// interleaving is asserted: nothing crashes, every reply is well-formed or one of three named
// errors legitimate mid-flight, and a monitor's generation never goes backwards for one observer.
// ---------------------------------------------------------------------------- //
struct StormResult {
    std::atomic<int> unexpectedErrors{0};
    std::atomic<int> generationRegressions{0};
    std::atomic<int> generationComparisons{0};
    std::atomic<int> calls{0};
    std::mutex mutex;
    std::string firstUnexpected;

    // Per monitor, served and refused for the reserve; compared only after every thread joined.
    std::array<std::atomic<int>, kStormMonitors> served{};
    std::array<std::atomic<int>, kStormMonitors> refusedForReserve{};

    // GetStatus readings of deferred_renders, and those above zero.
    std::atomic<int> deferralReadings{0};
    std::atomic<int> deferralsSeen{0};
};

// Per client, never shared: two clients may record generations 5 and 6 in either order, so a
// shared mark would report a monotonic daemon as regressing.
using GenerationMarks = std::map<int32_t, uint64_t>;

void observeGeneration(StormResult& result, GenerationMarks& marks, int32_t monitorId, uint64_t generation) {
    const auto it = marks.find(monitorId);
    if (it == marks.end()) {
        // A first look compares nothing, so it is not counted.
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
    // Inside the guard: createProxy throws if the bus refuses, and a thread must not throw.
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
        proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                   sdbus::ObjectPath{DbusService::kObjectPath});
    } catch (const std::exception& e) {
        recordUnexpected(result, std::string("could not create a client connection: ") + e.what());
        return;
    }

    // Deterministic per thread, so a failing sequence is reproducible.
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
                    // getStatus() returns a failure as a value, which would otherwise read as an
                    // empty reply. CONTROL: DbusService::getStatus made to throw -> "FAIL:
                    // concurrent clients see only the three named errors that are legitimate
                    // mid-render", "29 unexpected failure(s); first was: GetStatus failed with
                    // org.sdbuscpp.Error". Reverted.
                    if (!status.ok()) recordUnexpected(result, status.error());
                    const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
                    if (entry != nullptr) {
                        observeGeneration(result, marks, monitorId, entry->value("generation", uint64_t{0}));
                        // Every pass: the next commit clears the count.
                        if (entry->contains("deferred_renders") && (*entry)["deferred_renders"].is_number_unsigned()) {
                            ++result.deferralReadings;
                            if (entry->value("deferred_renders", uint32_t{0}) > 0) ++result.deferralsSeen;
                        }
                    }
                    break;
                }
                default: {
                    const Frame frame = getFrame(*proxy, monitorId);
                    if (fcntl(frame.fd.get(), F_GETFD) == -1) {
                        recordUnexpected(result, "GetFrame returned a closed fd");
                    }
                    ++result.served.at(static_cast<size_t>(monitorId - kMonitorStormBase));
                    observeGeneration(result, marks, monitorId, frame.generation);
                    break;
                }
            }
            ++result.calls;
        } catch (const sdbus::Error& e) {
            // Legitimate mid-storm: no frame yet, the previous render's failure, and a refusal
            // to give up the producer's last image -- counted per monitor and graded after the
            // storm, or a daemon refusing everything would pass. Anything else is a defect.
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

    // jthread: emplace_back can throw with joinable threads already in the vector.
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

    // GOAL: the reserve refuses a hand-over only with images out on loan: a monitor that answered
    //       "no free image" handed at least one image out during the storm.
    // WHY-THIS-SHAPE: after every thread joined and per monitor, the one form that does not
    //       assert on the scheduler; a count of refusals would be the interleaving's.
    // CONTROL: `if (!client.empty() && monitorId >= 20) { out.reason = NoFreeImage; return out; }`
    //       ahead of the admission block in WallpaperManager::exportCurrentFrame
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a monitor was only refused for want of
    //       a free image after it had handed one out", "4 monitor(s) refused without ever serving".
    //       Scoped to this phase's ids: refusing everywhere ends the run in an earlier phase. Reverted.
    // COVERAGE-ASSERT: the refusals seen are printed beside the verdict, so a run with none is visible.
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

    // GOAL: no render is ever deferred for want of a free image: the reserve keeps one, so any
    //       non-zero deferred_renders reading is a hand-over that got past it.
    // WHY-THIS-SHAPE: read on every storm GetStatus (the next commit clears it), and non-zero at
    //       any reading is the violation, which holds under every interleaving. Deleting the
    //       reserve does NOT redden it -- whether clients come to hold every image is a scheduling
    //       outcome; the deterministic form is dbus_service_test's buffer-hold phase.
    // CONTROL: `if (deferred)` in WallpaperManager::renderPassLocked (backend/daemon/
    //       wallpaper_manager.cpp) made `if (true)` -> "FAIL: no render was ever deferred for want
    //       of a free image, which the producer's reserve is what prevents", "21 of 22 readings".
    //       Observed 2026-09-14. Reverted.
    // COVERAGE-ASSERT: at least one reading per thread (22 measured). CONTROL: DbusService::getStatus
    //       (backend/daemon/dbus_service.cpp) writing deferred_renders only when non-zero ->
    //       "FAIL: deferred_renders was actually readable from the storm's GetStatus replies".
    //       Reverted.
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
    // WHY-THIS-SHAPE: three per client, half the measured minimum (24-27 comparisons at 24
    //       iterations, steeply fewer below), so a slow host cannot flake it; not set at the
    //       margin, since how many calls meet a frameless monitor is a scheduling outcome.
    // CONTROL: kStormIterations 24 -> 14 (6 comparisons) -> "FAIL: the monotonicity check made
    //       enough repeat observations to be worth asserting". Reverted.
    check(result.generationComparisons >= kStormThreads * 3,
          "the monotonicity check made enough repeat observations to be worth asserting");

    // Snapshot consistency is not graded here: it can fail only before the first device exists
    // (checkFirstFrameUnderStatusLoad).
    std::printf("daemon_system_test: %d concurrent calls across %d clients completed, %d generation comparisons\n",
                result.calls.load(), kStormThreads, result.generationComparisons.load());

    // Each stormed monitor quiesced to a known request: a supersede bug leaves a replaced frame,
    // a lost request none. Drained FIRST (the FIFO barrier's signal proves every storm render was
    // serviced): a generation read while storm renders are queued is a threshold a leftover
    // render would satisfy, passing a worker that wedged when the storm ended.
    setWallpaper(proxy, "static", bluePath, kMonitorBarrierPre);
    const bool drained = signals.wait(kMonitorBarrierPre, 1);
    check(drained, "the storm's queued renders all drain (barrier render completes)");
    if (!drained) {
        std::printf("daemon_system_test: post-storm state checks abandoned -- the render queue never drained, so "
                    "any generation read now would be a stale threshold.\n");
        return;
    }

    std::map<int32_t, uint64_t> before;
    {
        const StatusReply status = getStatusOrExplain(proxy);
        check(status.ok(), "the daemon still answers GetStatus after the storm");
        // Acted on: an unanswered call leaves every threshold at 0, which any render satisfies.
        // CONTROL: DbusService::getStatus made to throw -> "FAIL: the daemon still answers
        //       GetStatus after the storm" and this abandonment line, later phases still running.
        //       Reverted.
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

    // Even ids end on a renderable image, odd on an undecodable one: both outcomes asserted.
    for (int i = 0; i < kStormMonitors; ++i) {
        const int32_t monitorId = kMonitorStormBase + i;
        setWallpaper(proxy, "static", (i % 2 == 0) ? bluePath : corruptPath, monitorId);
    }

    // waitAtLeast: how many storm requests coalesced is a scheduling outcome.
    bool quiesced = true;
    for (int i = 0; i < kStormMonitors; i += 2) {
        const int32_t monitorId = kMonitorStormBase + i;
        if (!signals.waitAtLeast(monitorId, before[monitorId] + 1)) quiesced = false;
    }
    check(quiesced, "every monitor still renders after the storm");

    // The undecodable ones announce nothing: a later FIFO request's FrameReady settles them.
    setWallpaper(proxy, "static", bluePath, kMonitorBarrierPost);
    check(signals.wait(kMonitorBarrierPost, 1), "the post-storm barrier render completes");

    // Acted on: an unanswered call would read every monitor as absent, two misattributed FAILs.
    // CONTROL: DbusService::getStatus throwing only once the reply holds monitor 31 (set after
    //       the `before` reading) -> one FAIL, "the daemon still answers GetStatus after the
    //       post-storm barrier", this abandonment line, no monitor-state FAIL. Reverted.
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

// A daemon's signal masks from /proc (signal N is bit N-1). `blocked` is the union over given
// threads, since a blocked mask is per-thread and /proc/<pid>/status shows only the leader's;
// `ignored` is per-process, from the leader.
struct SignalMasks {
    bool read = false;
    uint64_t blocked = 0;
    uint64_t ignored = 0;
    int threadsRead = 0;
};

constexpr uint64_t signalBit(int signo) { return 1ull << (signo - 1); }

// False if either field is missing, which is never a clean mask.
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

// Every thread of `pid`. Taken before the first render, when all of them are the daemon's own.
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

// Unions SigBlk over exactly `tids` (taken before the first render), never a scan at check time:
// the ICD's threads block nearly everything (Mesa's 64 llvmpipe threads: 0xfffffffe3ffbfaff,
// SIGPIPE included), which would be red on a correct daemon under Lavapipe. A thread the daemon
// created after its first render would not be sampled; it creates none today.
SignalMasks readSignalMasks(pid_t pid, const std::vector<pid_t>& tids) {
    SignalMasks masks;
    const std::string taskDir = "/proc/" + std::to_string(pid) + "/task/";

    // The leader is required; a non-leader that exited since the snapshot is skipped, never
    // counted clean, and the caller asserts how many were read.
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

using wallpaperx::testing::HolderEntry;
using wallpaperx::testing::holderEntry;

// "a, b, c", or "(none)".
std::string joined(const std::vector<std::string>& names) {
    if (names.empty()) return "(none)";
    std::string text;
    for (const std::string& name : names) {
        if (!text.empty()) text += ", ";
        text += name;
    }
    return text;
}

std::string joined(const std::vector<int>& numbers) {
    std::vector<std::string> names;
    for (const int n : numbers) names.push_back(std::to_string(n));
    return joined(names);
}

// "LD_PRELOAD=" with `library` after whatever this process preloads: a sanitizer runtime preloaded by
// the build must stay first.
std::string preloading(const char* library) {
    const char* inherited = std::getenv("LD_PRELOAD");
    const std::string before = inherited != nullptr && *inherited != '\0' ? std::string(inherited) + ":" : "";
    return "LD_PRELOAD=" + before + library;
}

// A malformed entry is failed here, so a negative check reading "not held" off it cannot pass.
bool holdsImage(sdbus::IProxy& asker, int32_t monitorId, const std::string& client, uint64_t bufferId) {
    const HolderEntry held = holderEntry(getStatus(asker), monitorId, client);
    wallpaperx::testing::check(!held.malformed, ("GetStatus lists the holder " + client + " in the documented shape (" +
                                                 wallpaperx::testing::describeHolder(held) + ")")
                                                    .c_str());
    return held.listed && std::find(held.buffers.begin(), held.buffers.end(), bufferId) != held.buffers.end();
}

// S15.302, S15.303 and S15.305 on the real daemon: one caller holding an image through two
// hand-overs of it, released by number, and the race a release used to lose against a hand-over
// still on its way.
void checkHandoverRelease(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& bluePath,
                          const Check& check) {
    // Drawn by no other phase of this daemon (the ids below 18 and from 20 on are theirs).
    constexpr int32_t kMonitor = 19;
    constexpr int kTrials = 100;
    const int depth = static_cast<int>(wallpaperx::kTargetDepth);
    const sdbus::ServiceName name{DbusService::kBusName};
    const sdbus::ObjectPath path{DbusService::kObjectPath};
    auto reader = sdbus::createProxy(name, path);
    auto observer = sdbus::createProxy(name, path);
    const std::string readerName = reader->getConnection().getUniqueName();

    uint64_t generation = 0;
    // One render of the monitor, waited for by its exact generation: nothing else draws it, so the
    // generations run 1, 2, ... A render that never comes ends the phase as a failure.
    const auto render = [&] {
        setWallpaper(proxy, "static", bluePath, kMonitor);
        ++generation;
        const bool drew = signals.wait(kMonitor, generation);
        if (!drew) {
            check(false, ("the daemon draws the hand-over monitor's generation " + std::to_string(generation)).c_str());
        }
        return drew;
    };
    // Which image the newest render drew into, read by a second caller that gives it straight back.
    const auto drawnInto = [&]() -> uint64_t {
        const Frame seen = getFrame(*observer, kMonitor);
        wallpaperx::testing::releaseFrame(*observer, kMonitor, seen);
        return seen.bufferId;
    };
    // Renders `count` frames, each read by the observer; the images drawn, in order, or fewer on failure.
    const auto drawRound = [&](int count) {
        std::vector<uint64_t> drawn;
        for (int i = 0; i < count; ++i) {
            if (!render()) break;
            drawn.push_back(drawnInto());
        }
        return drawn;
    };
    const auto ids = [](const std::vector<uint64_t>& list) {
        std::string out;
        for (uint64_t id : list) out += (out.empty() ? "" : ", ") + std::to_string(id);
        return "[" + out + "]";
    };

    if (!render()) return;

    // GOAL: S15.302 -- a release naming the earlier of two hand-overs of one image keeps the claim,
    //       and no render draws into that image while it stands; a release naming the later one
    //       ends it, and the image is drawn into again.
    // WHY-THIS-SHAPE: where renders land, read by a second caller, over a ring's depth plus one: the
    //       second half shows the same reading sees the image once free. GetStatus's holder list
    //       beside it says whether the claim or the render went wrong.
    // CONTROL: in WallpaperManager::releaseHandoversLocked (backend/daemon/wallpaper_manager.cpp),
    //       `handover >= held->newest` made `true || ...`, so any release ends the claim -> "FAIL: a
    //       release naming the earlier of two hand-overs keeps the caller's claim" and "FAIL: and no
    //       render draws into that image while the claim stands (image 1, drawn [2, 3, 4, 5, 1, 2])".
    // COVERAGE-ASSERT: the two hand-overs must name one image with rising numbers, and every render
    //       of both rounds must be read, or the phase stops before grading.
    const Frame first = getFrame(*reader, kMonitor);
    const Frame second = getFrame(*reader, kMonitor);
    if (first.bufferId == 0 || second.bufferId != first.bufferId || first.handover == 0 ||
        second.handover <= first.handover) {
        check(false, ("two fetches of one frame are two hand-overs of one image, numbered upward (images " +
                      std::to_string(first.bufferId) + " and " + std::to_string(second.bufferId) + ", hand-overs " +
                      std::to_string(first.handover) + " and " + std::to_string(second.handover) + ")")
                         .c_str());
        return;
    }
    const uint64_t image = first.bufferId;
    wallpaperx::testing::releaseFrame(*reader, kMonitor, first);
    check(holdsImage(*reader, kMonitor, readerName, image),
          "a release naming the earlier of two hand-overs keeps the caller's claim");
    const std::vector<uint64_t> whileHeld = drawRound(depth + 1);
    if (static_cast<int>(whileHeld.size()) != depth + 1) return;
    check(std::find(whileHeld.begin(), whileHeld.end(), image) == whileHeld.end(),
          ("and no render draws into that image while the claim stands (image " + std::to_string(image) +
           ", drawn " + ids(whileHeld) + ")")
              .c_str());
    wallpaperx::testing::releaseFrame(*reader, kMonitor, second);
    check(!holdsImage(*reader, kMonitor, readerName, image), "a release naming the later hand-over ends the claim");
    const std::vector<uint64_t> afterRelease = drawRound(depth + 1);
    if (static_cast<int>(afterRelease.size()) != depth + 1) return;
    check(std::find(afterRelease.begin(), afterRelease.end(), image) != afterRelease.end(),
          ("and the image is drawn into again within a ring's depth of renders (image " + std::to_string(image) +
           ", drawn " + ids(afterRelease) + ")")
              .c_str());

    // GOAL: S15.305 on the wire -- a release naming hand-over 0, or one above the latest issued, is
    //       refused InvalidArgs and changes nothing.
    // WHY-THIS-SHAPE: nothing else asks this daemon for a frame meanwhile, so one past this caller's
    //       newest number has not been issued.
    // CONTROL: in WallpaperManager::releaseBuffer (backend/daemon/wallpaper_manager.cpp), (a) the
    //       `handover == 0 ||` term deleted -> "FAIL: a release naming hand-over 0 is refused
    //       InvalidArgs and changes nothing (answered '', still held: yes)"; (b) the `|| handover >
    //       m_lastHandover` term deleted -> "FAIL: a release naming a hand-over not yet made is refused
    //       InvalidArgs and changes nothing (answered '', still held: no)".
    {
        const Frame held = getFrame(*reader, kMonitor);
        const std::string zero = wallpaperx::testing::errorNameFrom(
            [&] { wallpaperx::testing::releaseBuffer(*reader, kMonitor, held.bufferId, 0); });
        const bool afterZero = holdsImage(*reader, kMonitor, readerName, held.bufferId);
        check(zero == "org.freedesktop.DBus.Error.InvalidArgs" && afterZero,
              ("a release naming hand-over 0 is refused InvalidArgs and changes nothing (answered '" + zero +
               "', still held: " + (afterZero ? "yes" : "no") + ")")
                  .c_str());
        const std::string future = wallpaperx::testing::errorNameFrom(
            [&] { wallpaperx::testing::releaseBuffer(*reader, kMonitor, held.bufferId, held.handover + 1); });
        const bool afterFuture = holdsImage(*reader, kMonitor, readerName, held.bufferId);
        check(future == "org.freedesktop.DBus.Error.InvalidArgs" && afterFuture,
              ("a release naming a hand-over not yet made is refused InvalidArgs and changes nothing (answered '" +
               future + "', still held: " + (afterFuture ? "yes" : "no") + ")")
                  .c_str());
        wallpaperx::testing::releaseFrame(*reader, kMonitor, held);
    }

    // GOAL: S15.303 -- the race unforced: a GetFrame not waited on, then a release of the hand-over
    //       read before it, on one connection. The caller keeps the image in every trial.
    // WHY-THIS-SHAPE: 0 lost of kTrials holds under either order the daemon applies them in; each
    //       reply is waited for, bounded, and one that never comes fails rather than counts.
    // CONTROL: the same break as S15.302 -> "FAIL: a release racing a hand-over of the same image
    //       never leaves the caller without its claim (N of 100 trials lost it)", N measured 24, 37, 66,
    //       79 and 85 across five runs (NVIDIA and Lavapipe).
    // COVERAGE-ASSERT: every trial must be answered with the same image, numbered above the one
    //       released, or the phase stops; the count of trials graded is asserted.
    Frame read = getFrame(*reader, kMonitor);
    int lost = 0;
    int graded = 0;
    for (int trial = 0; trial < kTrials; ++trial) {
        auto answered = std::make_shared<std::promise<std::optional<Frame>>>();
        std::future<std::optional<Frame>> reply = answered->get_future();
        {
            sdbus::MethodCall call =
                reader->createMethodCall(sdbus::InterfaceName{DbusService::kInterface}, sdbus::MethodName{"GetFrame"});
            wallpaperx::testing::appendFrameArguments(call, kMonitor);
            reader->callMethodAsync(call, [answered](sdbus::MethodReply out, std::optional<sdbus::Error> error) {
                if (error) {
                    answered->set_value(std::nullopt);
                    return;
                }
                try {
                    answered->set_value(wallpaperx::testing::readFrameReply(out));
                } catch (...) {
                    answered->set_value(std::nullopt);
                }
            });
        }
        wallpaperx::testing::releaseFrame(*reader, kMonitor, read);
        if (reply.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            check(false, ("the GetFrame sent before the release is answered (trial " + std::to_string(trial) + ")").c_str());
            return;
        }
        std::optional<Frame> next = reply.get();
        if (!next || next->bufferId != read.bufferId || next->handover <= read.handover) {
            check(false, ("each racing GetFrame is answered with the same image, numbered upward (trial " +
                          std::to_string(trial) + ")")
                             .c_str());
            return;
        }
        if (!holdsImage(*reader, kMonitor, readerName, next->bufferId)) ++lost;
        ++graded;
        read = std::move(*next);
    }
    wallpaperx::testing::releaseFrame(*reader, kMonitor, read);
    check(graded == kTrials && lost == 0,
          ("a release racing a hand-over of the same image never leaves the caller without its claim (" +
           std::to_string(lost) + " of " + std::to_string(graded) + " trials lost it)")
              .c_str());
    std::printf("daemon_system_test: hand-over race, %d trial(s), %d lost the claim\n", graded, lost);
}

// GOAL: S15.341 -- in the real daemon, over the bus, an allocation that fails while a GetFrame is
//       answered never strands anything: the caller gets an answer (an error, or the frame), and
//       after an error no claim stands for it, no descriptor is charged to it, and the monitor --
//       unwatched beforehand -- is still unwatched. At least one of the failures lands after the
//       export, where the claim and the charge already exist and must be taken back.
// WHY-THIS-SHAPE: tests/alloc_fail_shim.cpp, preloaded, fails the Nth operator new (aligned forms
//       included) the daemon or sdbus-c++ makes on the thread that sends frame replies, for N = 1, 2,
//       ... until a GetFrame makes fewer than N and is served whole. Every such allocation of the call is
//       reached in turn, so which ones lie after the export need not be known; a failure there is told
//       apart by the daemon's own words, "could not be sent". The countdown stays armed until the shim
//       sees the daemon's own code on that thread wait for its next job (a driver's wait mid-call does
//       not count), so what the call does after its reply is failed too and never leaks into the next N. The validation layer's allocations are left alone: a
//       std::bad_alloc thrown inside it crosses the Vulkan C API and ends the process, which no driver
//       does. A static wallpaper, so nothing else runs on that thread between calls, and the monitor
//       is driven unwatched (republished, then left uncollected past two seconds) before the sweep and
//       again after any call served despite its failure, so a delivery recorded for a failed reply would
//       show at every N. GetStatus is answered on the bus thread.
// CONTROL: in DbusService::completeFrame (backend/daemon/dbus_service.cpp): (a) the withdrawHandover()
//       block in the catch around sendFrame() deleted -> "FAIL: ...: no claim and no descriptor stand for the caller
//       after an error (stranded at 8, 9, 10, 11, 12)"; (b) `m_manager.handoverDelivered(monitorId,
//       client)` called before `sendFrame(call, handle);` -> "FAIL: ...: the monitor stays unwatched after
//       an error (watched at 8, 9, 10)"; (c) the returnError() in that catch deleted -> "FAIL: ...: every
//       GetFrame is answered (unanswered at 8, 9, 10)" and "FAIL: at least one of the failures landed
//       after the export (0 answered "could not be sent")". Test-side: the shim's waitedByTheDaemon()
//       made `return false` -> "NOT VERIFIED -- ... never went back to waiting after call 1", so the
//       stack walk is what recognises the daemon's own wait. A driver's wait mid-call is not stageable
//       here (none was seen on NVIDIA: "0 wait(s) on that thread by code not the daemon's").
// COVERAGE-ASSERT: at least one failure after the export ("could not be sent"), the sweep ending on a
//       call served whole, and that last delivery making the monitor watched -- the reading can see one.
void checkFrameAllocationFailure(NameWatcher& names, const std::string& validationEnv, const std::string& bluePath,
                                 const Check& check) {
    constexpr const char* kGoal = "S15.341 (an allocation failing while the real daemon answers GetFrame strands nothing)";
    constexpr int32_t kMonitor = 7;
    constexpr int kMaxSweep = 2000;
    ScratchDir work("allocfail");
    if (!work.ok()) {
        throw ScratchDirectoryLost("a scratch directory could not be created");
    }
    const std::string controlPath = (work.path() / "control").string();
    const int controlFd = ::open(controlPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    void* mapped = MAP_FAILED;
    if (controlFd >= 0 && ::ftruncate(controlFd, sizeof(wallpaperx::testing::AllocFailControl)) == 0) {
        mapped = ::mmap(nullptr, sizeof(wallpaperx::testing::AllocFailControl), PROT_READ | PROT_WRITE, MAP_SHARED,
                        controlFd, 0);
    }
    if (controlFd >= 0) ::close(controlFd);
    if (mapped == MAP_FAILED) {
        phaseSetupFault(kGoal, "the shim's control file could not be made and mapped");
        return;
    }
    struct Unmap {
        void* at;
        ~Unmap() { ::munmap(at, sizeof(wallpaperx::testing::AllocFailControl)); }
    } unmap{mapped};
    auto& control = *new (mapped) wallpaperx::testing::AllocFailControl{};

    FrameReadyLog signals;
    auto subscription = wallpaperx::testing::subscribeFrameReady(signals);
    SpawnedDaemon daemon(std::vector<std::string>{validationEnv, preloading(WALLPAPERX_ALLOC_FAIL_SHIM),
                                                  "WALLPAPERX_TEST_ALLOC_FAIL=" + controlPath});
    const bool cameUp = names.waitFor(true);
    if (!cameUp && reportedStartupFault(daemon, kGoal)) return;
    check(cameUp, "a fresh daemon acquires the name before the allocation-failure phase");
    if (!cameUp) return;
    // On every way out, so the next phase's daemon finds the name free.
    struct Stop {
        SpawnedDaemon& daemon;
        NameWatcher& names;
        ~Stop() {
            daemon.shutdown(SIGTERM);
            static_cast<void>(names.waitFor(false));
        }
    } stop{daemon, names};

    const sdbus::ServiceName name{DbusService::kBusName};
    const sdbus::ObjectPath path{DbusService::kObjectPath};
    auto controller = sdbus::createProxy(name, path);
    auto asker = sdbus::createProxy(name, path);
    const std::string askerName = asker->getConnection().getUniqueName();
    const auto watched = [&] {
        const wallpaperx::testing::StatusReply status = getStatus(*controller);
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitor));
        return entry != nullptr && entry->is_object() && entry->value("watched", false);
    };

    setWallpaper(*controller, "static", bluePath, kMonitor);
    if (!signals.wait(kMonitor, 1)) {
        notVerified(kGoal, "the daemon drew nothing for the phase's monitor on this host");
        return;
    }
    // The first frame teaches the shim which thread sends frame replies.
    wallpaperx::testing::releaseFrame(*asker, kMonitor, getFrame(*asker, kMonitor));
    if (control.workerTid.load() == 0) {
        notVerified(kGoal, "the preloaded shim never saw a frame reply's descriptor appended, so it is not in effect");
        return;
    }
    // Republishes the monitor and waits, bounded, until nothing has collected its newest picture: the
    // state in which a delivery recorded for a reply that never went out would show.
    uint64_t generation = 1;
    const auto driveUnwatched = [&] {
        setWallpaper(*controller, "static", bluePath, kMonitor);
        ++generation;
        if (!signals.wait(kMonitor, generation)) return false;
        const auto unwatchedBy = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (watched()) {
            if (std::chrono::steady_clock::now() > unwatchedBy) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    };
    if (!driveUnwatched()) {
        notVerified(kGoal, "the republished monitor never went unwatched, so the state a recorded delivery would "
                           "change could not be built");
        return;
    }

    // A reply can go out while the worker is still finishing its call -- recording the delivery,
    // logging, a render the undo posted -- so the countdown stays armed until the shim sees the
    // daemon's own code on that thread wait for its next job.
    const auto workerBackWaiting = [&](int done0) {
        const auto by = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (control.callsDone.load() <= done0) {
            if (std::chrono::steady_clock::now() > by) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    };

    std::vector<int> unanswered, stranded, watchedAfter, servedAfterFailure;
    std::string erroredUnfailed;  // a GetFrame no allocation failed in, answered with an error
    int failed = 0, afterExport = 0, sweptTo = 0;
    bool servedWhole = false;
    for (int n = 1; n <= kMaxSweep && !servedWhole; ++n) {
        sweptTo = n;
        control.fired.store(0);
        const int done0 = control.callsDone.load();
        control.countdown.store(n);
        std::string errorName, errorText;
        std::optional<Frame> frame;
        try {
            frame = getFrame(*asker, kMonitor, {.timeout = std::chrono::seconds(10)});
        } catch (const sdbus::Error& e) {
            errorName = e.getName();
            errorText = e.getMessage();
        }
        const bool noReply =
            errorName == "org.freedesktop.DBus.Error.NoReply" || errorName == "org.freedesktop.DBus.Error.Timeout";
        if (daemon.exited()) {
            check(false, ("the daemon survives an allocation failing while it answers GetFrame (it died at " +
                          std::to_string(n) + ")")
                             .c_str());
            return;
        }
        // GOAL: a GetFrame the client gave up on is not taken as finished: the next N is armed only
        //       once the daemon's worker is back waiting for its next job.
        // WHY-THIS-SHAPE: a client-side timeout says nothing about the worker, which may still be in
        //       the call and would spend the next N's countdown there. A worker that never comes back
        //       after a call that went unanswered is a hang of the daemon's, so it fails; one that does
        //       not come back after an answer is reported not verified, as before.
        // CONTROL: in DbusService::completeFrame (backend/daemon/dbus_service.cpp), the second call made to
        //       sleep 25 s before it answers -> "FAIL: ... the frame-reply thread goes back to waiting after
        //       call 1, which went unanswered".
        const bool finished = workerBackWaiting(done0);
        control.countdown.store(0);
        const bool fired = control.fired.load() > 0;
        if (!finished && noReply) {
            check(false, ("an allocation failing while the real daemon answers GetFrame: the frame-reply thread goes "
                          "back to waiting after call " +
                          std::to_string(n) + ", which went unanswered")
                             .c_str());
            return;
        }
        if (!finished) {
            notVerified(kGoal, "the frame-reply thread never went back to waiting after call " + std::to_string(n) +
                                   ", so whether its allocations were done could not be told");
            return;
        }
        if (!fired) {
            servedWhole = frame.has_value();
            if (noReply) {
                unanswered.push_back(n);
            } else if (!servedWhole) {
                erroredUnfailed = std::to_string(n) + ": " + errorName + ": " + errorText;
            }
            break;
        }
        ++failed;
        if (noReply) {
            unanswered.push_back(n);
            continue;
        }
        if (frame) {
            // Failed after the reply went out: the claim is the caller's, and the monitor is now watched,
            // so it is driven unwatched again before the next allocation is failed.
            servedAfterFailure.push_back(n);
            wallpaperx::testing::releaseFrame(*asker, kMonitor, *frame);
            if (!driveUnwatched()) {
                notVerified(kGoal, "the monitor never went unwatched again after the call served at " +
                                       std::to_string(n) + ", so the rest of the sweep had no state to grade");
                return;
            }
            continue;
        }
        if (errorText.find("could not be sent") != std::string::npos) ++afterExport;
        const wallpaperx::testing::StatusReply status = getStatus(*controller);
        const HolderEntry held = holderEntry(status, kMonitor, askerName);
        if (held.listed && (held.malformed || !held.buffers.empty() || held.descriptors != 0)) stranded.push_back(n);
        if (watched()) watchedAfter.push_back(n);
    }
    std::printf("daemon_system_test: allocation failure on the frame-reply thread -- %d failed of %d tried, %d after "
                "the export, %zu served after the failure (%s); %d wait(s) on that thread by code not the daemon's, "
                "not counted as a call's end\n",
                failed, sweptTo, afterExport, servedAfterFailure.size(), joined(servedAfterFailure).c_str(),
                control.otherWaits.load());
    std::fflush(stdout);
    if (failed == 0) {
        notVerified(kGoal, "no allocation was ever failed: no preload took effect, or the runtime binds operator new "
                           "inside itself");
        return;
    }
    check(unanswered.empty(), ("an allocation failing while the real daemon answers GetFrame: every GetFrame is "
                               "answered (unanswered at " + joined(unanswered) + ")")
                                  .c_str());
    check(erroredUnfailed.empty(), ("a GetFrame in which no allocation failed is served (answered with an error at " +
                                    erroredUnfailed + ")")
                                       .c_str());
    check(stranded.empty(), ("an allocation failing while the real daemon answers GetFrame: no claim and no descriptor "
                             "stand for the caller after an error (stranded at " + joined(stranded) + ")")
                                .c_str());
    check(watchedAfter.empty(), ("an allocation failing while the real daemon answers GetFrame: the monitor stays "
                                 "unwatched after an error (watched at " + joined(watchedAfter) + ")")
                                    .c_str());
    check(afterExport >= 1, ("at least one of the failures landed after the export (" + std::to_string(afterExport) +
                             " answered \"could not be sent\")")
                                .c_str());
    check(servedWhole && watched(), ("the sweep ended on a GetFrame served whole, which made the monitor watched (" +
                                     std::to_string(sweptTo) + " tried)")
                                        .c_str());
}

// GOAL: S15.327 -- SetOutputVisible logs a line only when the report changes what is recorded: of the
//       six reports true, true, false, false, true, true for one screen, only the two changes are
//       logged (a first `true` from a screen that never reported changes nothing), in that order.
// WHY-THIS-SHAPE: a daemon of its own, graded on its complete output after shutdown, because the
//       capture is complete only then. The count of two is also the positive control: a daemon that
//       logged nothing would read 0 rather than pass.
// CONTROL: in DbusService::setOutputVisible (backend/daemon/dbus_service.cpp) the condition made
//       `m_manager.setOutputVisible(outputName, visible) || true`, via control_batch.sh, --phase
//       '^outputVisibleLog$', 2026-10-05 -> "FAIL: SetOutputVisible logs only the reports that change
//       what is recorded (6 line(s): ...)". Reverted.
void checkOutputVisibleLog(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    SpawnedDaemon daemon(std::vector<std::string>{validationEnv});
    const bool cameUp = names.waitFor(true);
    if (!cameUp && reportedStartupFault(daemon, "the SetOutputVisible log checks (S15.327)")) return;
    check(cameUp, "a fresh daemon acquires the name before the SetOutputVisible log phase");
    if (!cameUp) return;
    {
        auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        wallpaperx::testing::setMonitorOutput(*proxy, 23, "DP-LOGGED");
        for (const bool visible : {true, true, false, false, true, true}) {
            wallpaperx::testing::setOutputVisible(*proxy, "DP-LOGGED", visible);
        }
    }
    daemon.shutdown(SIGTERM);
    check(names.waitFor(false), "the bus name is released after the SetOutputVisible log phase");
    const std::string output = daemon.output();
    std::vector<std::string> lines;
    for (size_t at = output.find("SetOutputVisible(output="); at != std::string::npos;
         at = output.find("SetOutputVisible(output=", at + 1)) {
        lines.push_back(output.substr(at, output.find('\n', at) - at));
    }
    std::string listed;
    for (const std::string& line : lines) listed += " [" + line + "]";
    check(lines.size() == 2 && lines[0].find("visible=false") != std::string::npos &&
              lines[1].find("visible=true") != std::string::npos,
          ("SetOutputVisible logs only the reports that change what is recorded (" + std::to_string(lines.size()) +
           " line(s):" + listed + ")")
              .c_str());
}

// GOAL: S6.36 -- SIGHUP shuts the daemon down through the same chain as SIGINT and SIGTERM: exit
//       0, the signal named, "stopped cleanly". At its default disposition it dies at 129.
// WHY-THIS-SHAPE: exitedNormally() apart from `exit == 0`, since reap() reports a signal death and a
//       SIGKILL by the test both as -1; the name via strsignal(), since the daemon inherits this
//       process's locale.
// CONTROL: `sigaddset(&shutdownSignals, SIGHUP)` deleted from backend/daemon/main.cpp -> "FAIL: the
//       daemon exits through main() on SIGHUP rather than being killed by it (terminated by signal
//       1)" and the exit-0, signal-name and stopped-cleanly lines. Observed 2026-09-24 via
//       control_run.sh. Reverted.
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

    // Under a sanitizer the exit code is the sanitizer's, and is reported as that.
    if (sanitizerReportedIn(output)) {
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

// GOAL: S6.37 -- a closed stdout does not kill the daemon: the failing write reports EPIPE, the
//       daemon goes on serving, and SIGPIPE is IGNORED, not blocked (blocked, it stays pending).
// WHY-THIS-SHAPE: (1) behaviour -- after the reader goes (`wallpaperx-daemon | head`), SetWallpaper
//       is answered and rendered and GetStatus answers; (2) disposition from /proc, since
//       behaviour cannot tell ignored from blocked; SigBlk as a union over the daemon's own
//       threads (readSignalMasks), since the failing write is the log writer thread's
//       (backend/daemon/log_pump.cpp); (3) the trigger asserted below, or nothing raises EPIPE.
// CONTROL: in backend/daemon/main.cpp via control_run.sh, re-run 2026-09-26 on NVIDIA, reverted
//       (cpp-httplib also ignores SIGPIPE, so deleting main()'s ignore alone is no control).
//       (a) SIG_DFL after the HttpServer is built -> "the daemon ignores SIGPIPE", the SetWallpaper
//       and GetStatus answers, the thread-sampling and both exit lines red. (b) SIGPIPE blocked
//       (pthread_sigmask) instead of SIG_IGN -> "SIGPIPE is ignored rather than merely blocked",
//       every behavioural check green, S14.35's two reader-gone lines red. (c) The block in the
//       sigwait watcher only -> the same line, alone (a leader-only sample read "all checks
//       passed", measured 2026-08-05).
// COVERAGE-ASSERT: the trigger is read from THIS daemon's own stdout pipe, stream-pure (a merged
//       capture would accept the line from stderr). Needle quoted, as sanitizeForLog frames it.
//       Controlled twice, both reverted: the needle made `SetWallpaper(type="zzz"` -> "FAIL: the
//       daemon writes to stdout, specifically, while handling SetWallpaper", alone; and
//       DbusService::setWallpaper's entry printf moved to stderr -> the same single FAIL.
void checkClosedStdout(NameWatcher& names, const std::string& bluePath, const std::string& validationEnv,
                       bool wantValidation, const Check& check) {
    // With the validation env: the only other child that renders, so the only oracle for a second
    // Device and a render with stdout broken. Scanned after shutdown.
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

    // Before the first SetWallpaper creates the device and the ICD's threads.
    const std::vector<pid_t> daemonOwnThreads = daemonThreads(deaf.pid());

    // One render with the pipe still readable, to establish the trigger: a reader that reads, then goes.
    bool triggerIssued = false;
    try {
        setWallpaper(*proxy, "static", bluePath, kMonitorClosedStdoutPre);
        triggerIssued = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  SetWallpaper before the stdout reader was closed: %s\n", e.what());
    }
    check(triggerIssued, "the daemon answers SetWallpaper while its stdout still has a reader");
    // Without the trigger the EPIPE checks would pass vacuously, so both misses abandon the phase.
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

    // Not graded: false only with no descriptor, which the Breakable constructor rules out.
    deaf.breakStdout();

    bool answered = false;
    try {
        setWallpaper(*proxy, "static", bluePath, kMonitorClosedStdout);
        answered = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  SetWallpaper after the stdout reader was closed: %s\n", e.what());
    }
    check(answered,
          "the daemon answers SetWallpaper after its stdout reader is gone (its entry line reaches the broken "
          "pipe through the log writer, whose write is the one that fails)");

    // The validation scan at the end is evidence only if the daemon did GPU work.
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
        // COVERAGE-ASSERT: four threads exist before the first render; a sample of one is the
        //       leader-only read, under which control (c) above printed "all checks passed".
        check(masks.threadsRead >= 2,
              "the blocked-signal check sampled the daemon's other threads, not just the group leader");
        if (masks.threadsRead < 2) {
            std::fprintf(stderr, "  only %d of the daemon's %zu pre-render thread(s) could be read\n",
                         masks.threadsRead, daemonOwnThreads.size());
        }
    }

    // The shutdown lines go through the log writer, which dropped the broken destination at the
    // first failed write. CONTROL: its abandoned-destination branch (backend/daemon/log_pump.cpp,
    // Stream::write's `n == kGone`) made to abort, via control_run.sh 2026-09-27 -> both exit lines
    // red, with the answers after the break, the thread-sampling line and S14.35's two reader-gone
    // lines. Reverted.
    const int exitCode = deaf.shutdown(SIGTERM);
    check(deaf.exitedNormally(), "a daemon whose stdout is closed still exits through main() on SIGTERM");
    if (!deaf.exitedNormally()) {
        std::fprintf(stderr, "  terminated by signal %d%s\n", deaf.termSignal(),
                     deaf.timedOut() ? " (it had to be killed after the timeout)" : "");
    }
    // Its stdout is broken, so the exit code is the only evidence of a clean stop.
    if (deaf.sanitizerReported()) {
        check(false, "the closed-stdout daemon's sanitizer reported no warnings (its exit code is the "
                     "sanitizer's here, so the exit-code check cannot run)");
    } else {
        check(exitCode == 0, "a daemon whose stdout is closed exits 0 rather than 141");
    }
    check(names.waitFor(false), "the bus name is released after the closed-stdout daemon exits");

    // GOAL: S6.33 -- zero Vulkan validation warnings across every daemon path, this child's render
    //       and teardown included.
    // WHY-THIS-SHAPE: this child's own scan, gated on `rendered`: its capture is stderr only (where
    //       the messenger writes) and empty for a child that never did Vulkan work. Its stdout is
    //       broken, so the "layer active" line main() requires cannot be read here. A waived or
    //       absent layer is announced once by main(), not per phase.
    // CONTROL: deaf spawned with WALLPAPERX_VALIDATION=0 while wantValidation was true -> the named
    //       skip below instead of a green check. Reverted.
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

// GOAL: X.26 -- a bus this test starts for itself can activate nothing, and its socket is inside
//       the entry's private runtime directory, so killing it leaves nothing in the shared /tmp.
// WHY-THIS-SHAPE: the bus itself is asked; asking activates nothing.
// CONTROL: test-side, control_batch, 2026-10-04, in NestedBus's generated configuration: the
//       listen directory made $TMPDIR -> "FAIL: the test's own bus listens inside the entry's
//       private runtime directory"; the directory written unescaped under a TMPDIR holding ' +~@%&<'
//       -> NOT VERIFIED, Skipped, not red; a <servicedir> with one inert service -> "FAIL: nothing
//       can be activated on the test's own bus", listing it. `--session` was not run as a control:
//       it can activate the host's services.
void checkNestedBusPrivate(const Check& check) {
    NestedBus bus;
    if (!bus.ok()) {
        if (const char* fault = bus.preambleFault()) {
            phaseSetupFault("the nested bus's isolation (X.26)", fault);
            return;
        }
        notVerified("the nested bus's isolation (X.26)", bus.reason());
        return;
    }
    // dbus-daemon prints its address escaped, as addressValue() escapes the directory it was given.
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const bool inside =
        runtime != nullptr && bus.address().rfind("unix:path=" + addressValue(runtime) + "/", 0) == 0;
    check(inside, "the test's own bus listens inside the entry's private runtime directory");
    if (!inside) std::fprintf(stderr, "  its address is %s\n", bus.address().c_str());
    std::vector<std::string> activatable;
    try {
        auto connection = sdbus::createSessionBusConnectionWithAddress(bus.address());
        auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{"org.freedesktop.DBus"},
                                        sdbus::ObjectPath{"/org/freedesktop/DBus"});
        proxy->callMethod("ListActivatableNames").onInterface("org.freedesktop.DBus").storeResultsTo(activatable);
    } catch (const std::exception& e) {
        notVerified("the nested bus's isolation (X.26)", std::string("the bus could not be asked: ") + e.what());
        return;
    }
    const bool inert = activatable == std::vector<std::string>{"org.freedesktop.DBus"};
    check(inert, "nothing can be activated on the test's own bus (only org.freedesktop.DBus is listed)");
    if (!inert) {
        for (const std::string& name : activatable) std::fprintf(stderr, "  activatable: %s\n", name.c_str());
    }
}

// GOAL: S6.35 -- losing the bus connection unwinds through main() instead of aborting: a joinable
//       std::thread destroyed on that unwind is std::terminate, and none of S6.30's destructors run.
// WHY-THIS-SHAPE: the loss is a SIGKILL of a bus this test started ("fatal: [System.Error.ENOTCONN]
//       Failed to get bus poll data"), not the ~1000-unread-GetFrame flood that found the bug: the
//       reference dbus-daemon under ctest has no fd quota and would absorb it. exitedNormally()
//       apart from `exit == 1`, as in the SIGHUP phase. NOT COVERED: the GPU half of this unwind --
//       the daemon is killed before its first SetWallpaper, so no Device exists (hence no
//       validation env).
// CONTROL: (i) the startup needle changed to one never printed -> "FAIL: the daemon comes up on a
//       bus of the test's own...", alone; (ii) DBUS_DAEMON_EXECUTABLE empty -> "NOT VERIFIED -- the
//       connection-loss checks (S6.35) could not be exercised: no dbus-daemon binary was found at
//       configure time", Skipped; (iii) backend/daemon/main.cpp's ShutdownWatch guard replaced by a
//       bare std::thread joined after enterEventLoop() (the code before f1aa32c), re-run 2026-08-06
//       -> "terminate called without an active exception", signal 6, and 3 FAILs (unwinds through
//       main(), exit 1, the fatal message). All reverted.
void checkBusLoss(const Check& check) {
    NestedBus bus;
    if (!bus.ok()) {
        if (const char* fault = bus.preambleFault()) {
            phaseSetupFault("the connection-loss checks (S6.35)", fault);
            return;
        }
        // X.3: the host's or harness's fault, never the daemon aborting; a skip, not a pass.
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
    if (sanitizerReportedIn(output)) {
        check(false, "the orphaned daemon's sanitizer reported no warnings (its exit code is the sanitizer's "
                     "here, so the connection-loss exit-code check cannot run)");
    } else {
        check(exitCode == 1, "the daemon exits 1 through main's exception handler when its bus is destroyed");
    }
    // The check below is gated on this.
    const bool reachedTheHandler = output.find("fatal:") != std::string::npos;
    check(reachedTheHandler, "the daemon says why it exited after losing the bus rather than dying silently");

    // GOAL: on the connection-loss path the daemon does not report a signal it was never sent:
    //       ~ShutdownWatch's wake of the sigwait watcher is not a signal from anybody.
    // WHY-THIS-SHAPE: the ABSENCE of kSignalShutdownLine; a presence check would pass because of
    //       the defect. Wake-and-join is carried by the checks above (pthread_kill deleted -> the
    //       daemon never exits, four FAILs at 30.6 s, 2026-08-06). Gated on the fatal line, not
    //       `exitCode == 1`, which a sanitized build replaces; main's handler prints it after the
    //       watcher is joined.
    // COVERAGE-ASSERT: (i) not graded unless the daemon reached main's handler -- ungated it stayed
    //       green while the daemon was broken; (ii) the needle is shared with the presence checks,
    //       so a reworded message reddens those.
    // CONTROL: in backend/daemon/main.cpp, via control_run.sh, 2026-09-25, reverted: the watcher
    //       printing its "received" line for every signal it takes -> this line red, alone.
    //       (Deleting `if (teardown.finished) return;` no longer reaches it: the alarm kills the
    //       daemon first, 8 FAILs, this check printed as not graded.)
    // CONTROL (the gate): the pre-f1aa32c bare-thread mutation, 2026-08-06 -> "the fabricated-signal
    //       check is not graded ..." and 3 FAILs, where before the gate this line passed silently.
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
// A daemon whose teardown STICKS, and the three things that end one. Nothing the daemon can be
// asked produces such a stop, so tests/wedge_shim.cpp is preloaded: with
// WALLPAPERX_TEST_WEDGE_FREE=1 a decoder freed on the MAIN thread never returns, and the only such
// free is ~WallpaperManager's, after the render worker is joined.
// ---------------------------------------------------------------------------- //

// The wedge preloaded and armed, and a HOME and cache of its own for the shader cache.
std::vector<std::string> wedgedDaemonEnv(const std::string& validationEnv, const std::string& busAddress,
                                         const std::filesystem::path& home) {
    std::error_code ec;
    std::filesystem::create_directories(home / "cache", ec);
    return {validationEnv,
            "DBUS_SESSION_BUS_ADDRESS=" + busAddress,
            "HOME=" + home.string(),
            "XDG_CACHE_HOME=" + (home / "cache").string(),
            preloading(WALLPAPERX_WEDGE_SHIM),
            "WALLPAPERX_TEST_WEDGE_FREE=1"};
}

// Gives the daemon a video to play, so its teardown has a decoder to free -- which the preloaded
// wedge then never returns from. False with `why` set when this host cannot get that far.
bool primeTeardownWedge(sdbus::IProxy& proxy, const std::string& clip, std::string& why) {
    constexpr int kWedgeMonitor = 0;
    try {
        setWallpaper(proxy, "video", clip, kWedgeMonitor, std::chrono::seconds(10));
    } catch (const std::exception& e) {
        why = std::string("the daemon refused the video this phase wedges its teardown with: ") + e.what();
        return false;
    }
    // A frame, not an accepted request: the decoder exists only once the video opened.
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (Clock::now() < deadline) {
        const StatusReply status = getStatus(proxy, std::chrono::seconds(10));
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kWedgeMonitor));
        if (entry != nullptr && entry->value("has_frame", false)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    why = "this daemon never drew the video, so it holds no decoder and its teardown cannot be wedged on this host";
    return false;
}

// GOAL: a teardown that STICKS is ended by the alarm the unwind arms at its top. False if only
//       ~ShutdownWatch, destroyed last, arms it: every `systemctl stop` would end in SIGKILL.
// WHY-THIS-SHAPE: on the CONNECTION-LOSS path, where no signal arrives and the watcher stays in
//       sigwait; on a signal the watcher's own deadline ends the process with the guards deleted
//       (measured). timedOut() asserted, since waitForExit() reports a code even after its SIGKILL.
// CONTROL: both `ArmTeardown` guards (armBeforeService, armBeforeHttp) deleted from
//       backend/daemon/main.cpp -> "FAIL: a daemon whose teardown blocks still ends, rather than
//       having to be killed", "still running 30.0 s after its bus was destroyed ...", alone; the
//       escalation phase below stayed green. Observed 2026-09-26 via control_run.sh on NVIDIA.
//       Re-run 2026-10-08 with armBeforeHttp moved before the name claim, --phase
//       stuckTeardownIsBounded: the same line alone, 30.1 s. Reverted.
// COVERAGE-ASSERT: the wedge's own line must be in the output, or NOT VERIFIED: an unstuck stop
//       exits promptly and would read green.
void checkStuckTeardownIsBounded(const std::string& validationEnv, const std::string& clip, const Check& check) {
    constexpr const char* kGoal = "S8.20's watchdog clause (a teardown that blocks is ended by the alarm the unwind "
                                  "arms at its top, not left to a destructor that is never reached)";
    std::error_code ec;
    if (!std::filesystem::exists(clip, ec)) {
        notVerified(kGoal, "the video fixture " + clip + " was not generated, so no teardown frees a decoder");
        return;
    }
    ScratchDir work("stuckteardown");
    if (!work.ok()) {
        throw ScratchDirectoryLost("a scratch directory could not be created");
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

    SpawnedDaemon daemon(wedgedDaemonEnv(validationEnv, bus.address(), work.path()));
    if (!daemon.waitForOutput(std::string("serving ") + DbusService::kBusName, std::chrono::seconds(30))) {
        if (reportedStartupFault(daemon, kGoal)) return;
        notVerified(kGoal, "the daemon for this phase never reached the bus");
        return;
    }

    {
        // Closed before the bus is killed, so only the daemon's connection is lost.
        auto connection = sdbus::createSessionBusConnectionWithAddress(bus.address());
        connection->enterEventLoopAsync();
        auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        std::string why;
        if (!primeTeardownWedge(*proxy, clip, why)) {
            notVerified(kGoal, why);
            return;
        }
    }

    // Timed from the kill, not the phase start: the wedge itself takes seconds.
    const auto killedAt = Clock::now();
    bus.kill();
    const int exitCode = daemon.waitForExit();
    const double elapsedMs = millis(Clock::now() - killedAt);
    const std::string output = daemon.output();

    if (output.find("wedge_shim: holding") == std::string::npos) {
        notVerified(kGoal, "the stop never reached the wedged decoder free, so this daemon's teardown was not stuck");
        return;
    }
    check(!daemon.timedOut(), "a daemon whose teardown blocks still ends, rather than having to be killed");
    if (daemon.timedOut()) {
        std::fprintf(stderr, "  it was still running %.1f s after its bus was destroyed, with its teardown "
                             "wedged; nothing armed the watchdog\n",
                     elapsedMs / 1000.0);
        return;
    }
    // SIGALRM specifically, not the watcher's own deadline.
    const bool endedByTheAlarm = !daemon.exitedNormally() && daemon.termSignal() == SIGALRM;
    check(endedByTheAlarm, "the stuck teardown is ended by the alarm the unwind armed");
    if (!endedByTheAlarm) {
        std::fprintf(stderr, "  it %s (exit code %d, signal %d) after %.1f s\n",
                     daemon.exitedNormally() ? "exited normally" : "was terminated by a signal", exitCode,
                     daemon.termSignal(), elapsedMs / 1000.0);
    }
    std::printf("daemon_system_test: a daemon with a wedged teardown was ended %.1f s after its bus was "
                "destroyed\n",
                elapsedMs / 1000.0);
}

// GOAL: a second SIGINT/SIGTERM ends a stuck stop promptly, and says so.
// WHY-THIS-SHAPE: the same wedge, since an ordinary stop is gone before a second signal can land.
//       The message AND the elapsed time are asserted, and the deadline's line absent: without
//       the handler the process still exits, at the deadline.
// CONTROL: the block installing forceExitOnSecondSignal deleted from backend/daemon/main.cpp ->
//       "FAIL: the daemon says that a second signal is what ended it" and "FAIL: the second signal
//       ends the daemon promptly rather than at the deadline"; which 3 s deadline then wins is a
//       race, so "the stop ends on the second signal rather than on the teardown deadline" is red
//       only sometimes (three red 2026-09-26 via control_run.sh on NVIDIA). Reverted.
// COVERAGE-ASSERT: the wedge's own line must be in the output, or NOT VERIFIED.
void checkSecondSignalEndsAStuckStop(const std::string& validationEnv, const std::string& clip,
                                     const Check& check) {
    constexpr const char* kGoal = "S8.20's escalation clause (a second shutdown signal ends a stop that is stuck, "
                                  "rather than leaving the user to wait out the teardown deadline)";
    std::error_code ec;
    if (!std::filesystem::exists(clip, ec)) {
        notVerified(kGoal, "the video fixture " + clip + " was not generated, so no teardown frees a decoder");
        return;
    }
    ScratchDir work("secondsignal");
    if (!work.ok()) {
        throw ScratchDirectoryLost("a scratch directory could not be created");
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

    SpawnedDaemon daemon(wedgedDaemonEnv(validationEnv, bus.address(), work.path()));
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
        if (!primeTeardownWedge(*proxy, clip, why)) {
            notVerified(kGoal, why);
            return;
        }
    }

    ::kill(daemon.pid(), SIGTERM);
    // Past the handler's installation, well short of the 3 s deadline the control falls back on.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const auto secondSignal = Clock::now();
    ::kill(daemon.pid(), SIGTERM);
    const int exitCode = daemon.waitForExit();
    const double afterSecondMs = millis(Clock::now() - secondSignal);
    const std::string output = daemon.output();

    if (output.find("wedge_shim: holding") == std::string::npos) {
        notVerified(kGoal, "the first stop never reached the wedged decoder free, so there was no stuck stop for the "
                           "second signal to end");
        return;
    }
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
    // The watcher's own deadline line means the second signal changed nothing.
    const bool notTheDeadline = output.find("shutdown blocked") == std::string::npos;
    check(notTheDeadline, "the stop ends on the second signal rather than on the teardown deadline");
    // Under the 1.8 s the deadline has left: the handler runs in the signal itself.
    const bool prompt = afterSecondMs < 1250;
    check(prompt, "the second signal ends the daemon promptly rather than at the deadline");
    if (!prompt) {
        std::fprintf(stderr, "  the daemon took %.0f ms to go after the second signal, against a 3000 ms teardown "
                             "deadline from the first\n",
                     afterSecondMs);
    }
    std::printf("daemon_system_test: a wedged stop ended %.0f ms after the second SIGTERM\n", afterSecondMs);
}

// Every regular file under `dir` with its bytes, for "wrote nothing".
std::map<std::string, std::string> fileTree(const std::filesystem::path& dir) {
    std::map<std::string, std::string> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::ifstream in(it->path(), std::ios::binary);
        files[std::filesystem::relative(it->path(), dir, ec).string()] =
            std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return files;
}

std::string describeTreeChange(const std::map<std::string, std::string>& before,
                               const std::map<std::string, std::string>& after) {
    std::string out;
    for (const auto& [name, bytes] : after) {
        const auto was = before.find(name);
        if (was == before.end()) out += " added " + name + ";";
        else if (was->second != bytes) out += " changed " + name + ";";
    }
    for (const auto& [name, bytes] : before) {
        if (after.count(name) == 0) out += " removed " + name + ";";
    }
    return out.empty() ? " nothing" : out;
}

std::string readTextFile(const std::filesystem::path& file);
bool writeTextFile(const std::filesystem::path& file, const std::string& text);

// A copy of `source` with every third byte from 60% of its mdat payload onward changed: the first
// picture stays whole, the rest of each pass is damaged. Empty when the file has no mdat.
std::string damagedVideoBytes(const std::string& source) {
    std::string bytes = readTextFile(source);
    const size_t box = bytes.find("mdat");
    if (box == std::string::npos || box < 4) return {};
    size_t size = 0;
    for (size_t i = box - 4; i < box; ++i) size = (size << 8) | static_cast<unsigned char>(bytes[i]);
    const size_t start = box + 4, end = box - 4 + size;
    if (size < 8 || end > bytes.size()) return {};
    for (size_t at = start + (end - start) * 6 / 10; at < end; at += 3) bytes[at] ^= 0x5a;
    return bytes;
}

// GOAL: S15.669 -- the daemon binary routes FFmpeg's messages through its limiter: a looping video whose
//       packets make FFmpeg complain on every pass shows at most 10 "wallpaperx-daemon: ffmpeg" lines in
//       three seconds, and no line in FFmpeg's own "[h264 @ 0x...]" form. False if main() never installs it.
// WHY-THIS-SHAPE: the real binary, because the install is in main(); in-process tests call install()
//       themselves and so cannot see it missing. At least one limiter line is required, so a decode path
//       that says nothing about the damage is NOT VERIFIED rather than green.
// CONTROL: control_batch.sh 2026-10-17, phase ffmpegLogInstalled, reverted: backend/daemon/main.cpp's
//       `wallpaperx::ffmpeg_log::install();` deleted -> "FAIL: the daemon prints FFmpeg's messages only
//       through its limiter, never in FFmpeg's own form" and "FAIL: ... at least one limiter line" (0 limiter
//       lines, 81 in FFmpeg's own form; green: 10 and 0).
void checkFfmpegLogInstalled(const std::string& validationEnv, const std::string& clip, const Check& check) {
    constexpr const char* kGoal = "S15.669 (the daemon limits FFmpeg's log)";
    ScratchDir home("ffmpeglog");
    if (!home.ok()) throw ScratchDirectoryLost("a scratch HOME could not be created");
    const std::string damaged = damagedVideoBytes(clip);
    const std::string file = (home.path() / "damaged.mp4").string();
    if (damaged.empty() || !writeTextFile(file, damaged)) {
        notVerified(kGoal, "the damaged copy of " + clip + " could not be made");
        return;
    }
    std::error_code ec;
    for (const char* sub : {"config", "state", "cache"}) std::filesystem::create_directories(home.path() / sub, ec);
    NestedBus bus;
    if (!bus.ok()) {
        if (const char* fault = bus.preambleFault()) {
            phaseSetupFault(kGoal, fault);
            return;
        }
        notVerified(kGoal, bus.reason());
        return;
    }
    SpawnedDaemon daemon({validationEnv, std::string("DBUS_SESSION_BUS_ADDRESS=") + bus.address(),
                          "HOME=" + home.path().string(), "XDG_CONFIG_HOME=" + (home.path() / "config").string(),
                          "XDG_STATE_HOME=" + (home.path() / "state").string(),
                          "XDG_CACHE_HOME=" + (home.path() / "cache").string()});
    if (!daemon.waitForOutput(std::string("serving ") + DbusService::kBusName, std::chrono::seconds(30))) {
        if (reportedStartupFault(daemon, kGoal)) return;
        notVerified(kGoal, "the daemon for this phase never reached the bus");
        return;
    }
    bool drew = false;
    {
        auto connection = sdbus::createSessionBusConnectionWithAddress(bus.address());
        connection->enterEventLoopAsync();
        auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        try {
            setWallpaper(*proxy, "video", file, 0, std::chrono::seconds(10));
            const auto deadline = Clock::now() + std::chrono::seconds(20);
            while (!drew && Clock::now() < deadline) {
                const StatusReply status = getStatus(*proxy, std::chrono::seconds(10));
                const nlohmann::json* entry = wallpaperStatus(status, "0");
                drew = entry != nullptr && entry->value("has_frame", false);
                if (!drew) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (drew) std::this_thread::sleep_for(std::chrono::seconds(3));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  the damaged video could not be set: %s\n", e.what());
        }
    }
    daemon.shutdown(SIGTERM);
    if (!drew) {
        notVerified(kGoal, "the daemon drew no picture of the damaged video, so nothing played on");
        return;
    }
    int limited = 0, raw = 0;
    std::istringstream lines(daemon.output());
    for (std::string line; std::getline(lines, line);) {
        if (line.find("wallpaperx-daemon: ffmpeg") != std::string::npos) {
            ++limited;
        } else if (line.find(" @ 0x") != std::string::npos) {
            ++raw;
            if (raw <= 3) std::fprintf(stderr, "  raw: %s\n", line.c_str());
        }
    }
    std::printf("daemon_system_test: damaged video: %d limiter line(s), %d in FFmpeg's own form\n", limited, raw);
    if (limited == 0 && raw == 0) {
        notVerified(kGoal, "FFmpeg said nothing about the damaged video on this decode path");
        return;
    }
    check(raw == 0, "the daemon prints FFmpeg's messages only through its limiter, never in FFmpeg's own form");
    check(limited >= 1 && limited <= 10,
          "the daemon's FFmpeg messages for a damaged looping video are at most the limiter's 10, and at least one limiter line");
    if (limited > 10) std::fprintf(stderr, "  %d limiter lines\n", limited);
}

// GOAL: S13.191 -- a stop that cannot finish ends the process at the three-second deadline, and
//       that path writes NOTHING: config and state directory byte for byte as when the stop began
//       (a record left behind reads as a death at the next start).
// WHY-THIS-SHAPE: wedged in the manager's own teardown (tests/wedge_shim.cpp); the elapsed time is
//       bounded below by the wedge's own line and above at 4.5 s, which a five-second deadline
//       cannot meet; the directories are compared whole.
// CONTROL: (a) `kTeardownDeadline{3}` -> `{5}` in backend/daemon/main.cpp -> "FAIL: a stop wedged in
//       the manager's teardown ends at the three-second deadline", ~5.0 s, alone (2026-09-26 via
//       control_run.sh, NVIDIA); (b) ~WallpaperManager (backend/daemon/wallpaper_manager.cpp) freeing
//       each video session under an ApplyingRecord -> "FAIL: a stop the deadline ended wrote
//       nothing", "added wallpaperx/applying-0.json" (2026-09-21). Reverted.
// COVERAGE-ASSERT: the video drawn and the wedge's line present, or NOT VERIFIED.
void checkWatchdogEndsAStuckStopWritingNothing(const std::string& validationEnv, const Check& check) {
    constexpr const char* kGoal = "S13.191 (a stop not finished in 3 s ends the process, and writes nothing)";
    const std::string clip = std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_bands.mp4";
    std::error_code ec;
    if (!std::filesystem::exists(clip, ec)) {
        notVerified(kGoal, "the video fixture " + clip + " was not generated, so no teardown frees a decoder");
        return;
    }
    ScratchDir home("watchdoghome");
    if (!home.ok()) {
        throw ScratchDirectoryLost("a scratch HOME could not be created");
    }
    const std::filesystem::path configHome = home.path() / "config";
    const std::filesystem::path stateHome = home.path() / "state";
    std::filesystem::create_directories(configHome, ec);
    std::filesystem::create_directories(stateHome, ec);
    std::filesystem::create_directories(home.path() / "cache", ec);

    NestedBus bus;
    if (!bus.ok()) {
        if (const char* fault = bus.preambleFault()) {
            phaseSetupFault(kGoal, fault);
            return;
        }
        notVerified(kGoal, bus.reason());
        return;
    }
    SpawnedDaemon daemon({validationEnv, std::string("DBUS_SESSION_BUS_ADDRESS=") + bus.address(),
                          "HOME=" + home.path().string(), "XDG_CONFIG_HOME=" + configHome.string(),
                          "XDG_STATE_HOME=" + stateHome.string(), "XDG_CACHE_HOME=" + (home.path() / "cache").string(),
                          preloading(WALLPAPERX_WEDGE_SHIM), "WALLPAPERX_TEST_WEDGE_FREE=1"});
    if (!daemon.waitForOutput(std::string("serving ") + DbusService::kBusName, std::chrono::seconds(30))) {
        if (reportedStartupFault(daemon, kGoal)) return;
        notVerified(kGoal, "the daemon for this phase never reached the bus");
        return;
    }
    bool drew = false;
    {
        auto connection = sdbus::createSessionBusConnectionWithAddress(bus.address());
        connection->enterEventLoopAsync();
        auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        try {
            setWallpaper(*proxy, "video", clip, 0, std::chrono::seconds(10));
            const auto deadline = Clock::now() + std::chrono::seconds(20);
            while (!drew && Clock::now() < deadline) {
                const StatusReply status = getStatus(*proxy, std::chrono::seconds(10));
                const nlohmann::json* entry = wallpaperStatus(status, "0");
                drew = entry != nullptr && entry->value("has_frame", false);
                if (!drew) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  the video could not be set: %s\n", e.what());
        }
    }
    const std::map<std::string, std::string> configBefore = fileTree(configHome);
    const std::map<std::string, std::string> stateBefore = fileTree(stateHome);

    const auto signalledAt = Clock::now();
    const int exitCode = daemon.shutdown(SIGTERM);
    const double elapsed = millis(Clock::now() - signalledAt) / 1000.0;
    const std::string output = daemon.output();
    if (!drew || output.find("wedge_shim: holding") == std::string::npos) {
        notVerified(kGoal, drew ? "the stop never reached the wedged decoder free, so it was not stuck"
                                : "the daemon drew no video on this host, so its stop frees no decoder");
        return;
    }
    check(!daemon.timedOut(), "a stop wedged in the manager's teardown still ends, rather than having to be killed");
    const bool onTime = elapsed >= 2.5 && elapsed < 4.5;
    check(onTime, "a stop wedged in the manager's teardown ends at the three-second deadline");
    if (!onTime) std::fprintf(stderr, "  it ended %.1f s after SIGTERM\n", elapsed);
    const bool forced = output.find("stopped cleanly") == std::string::npos &&
                        (exitCode == 1 || (!daemon.exitedNormally() && daemon.termSignal() == SIGALRM));
    check(forced, "and it is the deadline that ended it, not a clean stop");
    if (!forced) {
        std::fprintf(stderr, "  exit code %d, signal %d\n", exitCode, daemon.termSignal());
    }
    const std::string configChange = describeTreeChange(configBefore, fileTree(configHome));
    const std::string stateChange = describeTreeChange(stateBefore, fileTree(stateHome));
    const bool untouched = configChange == " nothing" && stateChange == " nothing";
    check(untouched, "a stop the deadline ended wrote nothing: the config and the state directory are unchanged");
    if (!untouched) {
        std::fprintf(stderr, "  config:%s state:%s\n", configChange.c_str(), stateChange.c_str());
    }
    std::printf("daemon_system_test: a stop wedged in the manager's teardown ended %.1f s after SIGTERM, %zu state "
                "file(s) compared\n",
                elapsed, stateBefore.size());
}

// After the bus a daemon was serving on is destroyed: the moment its unwind installed the teardown
// alarm's handler, which /proc shows as SIGALRM moving from ignored to caught. `before` is the last
// look that did not see it yet. False if it was not seen within 10 s, or the daemon ended first.
bool awaitAlarmArmed(const SpawnedDaemon& daemon, Clock::time_point& before, Clock::time_point& seen) {
    before = Clock::now();
    for (const auto deadline = before + std::chrono::seconds(10); Clock::now() < deadline;) {
        const auto look = Clock::now();
        const std::optional<bool> caught = wallpaperx::testing::catchesSignal(daemon.pid(), SIGALRM);
        if (!caught) return false;
        if (*caught) {
            seen = look;
            return true;
        }
        before = look;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// GOAL: S14.30 -- a stop the teardown alarm ends (here a failed bus loop whose unwind is wedged)
//       delivers the lines queued before the alarm, and then its own "teardown deadline passed", to
//       a destination that resumes during the alarm's 250 ms flush; the process still dies of
//       SIGALRM. False if the alarm's handler dies without the flush: the queued lines, which say
//       why the stop happened, go with the process.
// WHY-THIS-SHAPE: the reader resumes 30 ms after the alarm is due, and the alarm is due 3 s after
//       its handler was installed, which /proc shows (SIGALRM caught): so the resume provably comes
//       after the alarm fired. The last look that did not see it bounds how late the resume can be;
//       more than 200 ms after the earliest the alarm could have fired is NOT VERIFIED, since the
//       flush may have given up by then. On the bus-loss path because a signal-driven stop also
//       arms the signal thread's own 3 s deadline, which races the alarm.
// CONTROL: in backend/daemon/main.cpp, `flushBeforeExit(kLine);` deleted from onTeardownDeadline's
//       last branch -> "the lines queued before the teardown alarm, then its own line, reach the
//       destination" red, alone. Observed 2026-09-25 via control_run.sh on NVIDIA; reverted. By
//       hand, reverted: the resume moved 300 ms later -> NOT VERIFIED, no FAIL.
void checkAlarmDeliversQueuedLines(const std::string& validationEnv, const std::string& clip, const Check& check) {
    constexpr const char* kGoal = "S14.30 (a stop the teardown alarm ends delivers what was queued, and says so)";
    std::error_code ec;
    if (!std::filesystem::exists(clip, ec)) {
        notVerified(kGoal, "the video fixture " + clip + " was not generated, so no teardown frees a decoder");
        return;
    }
    ScratchDir work("alarmflush");
    if (!work.ok()) {
        throw ScratchDirectoryLost("a scratch directory could not be created");
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
    SpawnedDaemon daemon(wedgedDaemonEnv(validationEnv, bus.address(), work.path()));
    if (!daemon.waitForOutput(std::string("serving ") + DbusService::kBusName, std::chrono::seconds(30))) {
        if (reportedStartupFault(daemon, kGoal)) return;
        notVerified(kGoal, "the daemon for this phase never reached the bus");
        return;
    }
    bool queued = false;
    {
        auto connection = sdbus::createSessionBusConnectionWithAddress(bus.address());
        connection->enterEventLoopAsync();
        auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        std::string why;
        if (!primeTeardownWedge(*proxy, clip, why)) {
            notVerified(kGoal, why);
            return;
        }
        if (!daemon.stallCapture(why)) {
            wallpaperx::testing::harnessFailure(std::string(kGoal) + ": " + why);
            return;
        }
        // A request the daemon refuses, logging one line to each stream: queued behind the stall.
        try {
            setWallpaper(*proxy, "alarmmark", "/nonexistent", 0, std::chrono::seconds(5));
        } catch (const sdbus::Error& e) {
            queued = e.getName() == "org.freedesktop.DBus.Error.InvalidArgs";
        }
    }
    bus.kill();
    Clock::time_point notYet;
    Clock::time_point armedBy;
    const bool armed = awaitAlarmArmed(daemon, notYet, armedBy);
    if (armed) std::this_thread::sleep_until(armedBy + std::chrono::milliseconds(2900));
    const std::optional<long> mainIn = blockedIn(daemon.pid(), daemon.pid());
    if (armed) std::this_thread::sleep_until(armedBy + std::chrono::milliseconds(3030));
    const bool held = daemon.captureStillStalled();
    const auto resumedAt = Clock::now();
    daemon.resumeCapture();
    daemon.waitForExit();
#ifdef SYS_pause
    const bool wedged = mainIn && *mainIn == SYS_pause;
#else
    const bool wedged = false;  // no pause system call here to recognise the wedge by
#endif
    if (!held) {
        wallpaperx::testing::harnessFailure(std::string(kGoal) + ": something drained the stalled stream");
        return;
    }
    if (!queued || !armed || !wedged) {
        notVerified(kGoal, !queued ? "the request that queues a line was not refused as expected"
                           : !armed ? "the daemon's unwind was never seen to install the teardown alarm"
                                    : "the stop was not seen stuck in the wedged decoder free");
        return;
    }
    if (resumedAt - (notYet + std::chrono::seconds(3)) > std::chrono::milliseconds(200)) {
        notVerified(kGoal, "this test resumed the stream too long after the alarm to be sure it was inside its flush");
        return;
    }
    const std::string output = daemon.output().substr(std::min(daemon.stalledAt(), daemon.output().size()));
    const size_t out = output.find("SetWallpaper(type=\"alarmmark\"");
    const size_t err = output.find("SetWallpaper rejected");
    const size_t wedgeLine = output.find("wedge_shim: holding");
    const size_t deadline = output.find("teardown deadline passed, exiting now");
    const bool delivered = out != std::string::npos && err != std::string::npos && wedgeLine != std::string::npos &&
                           deadline != std::string::npos && std::max({out, err, wedgeLine}) < deadline;
    check(delivered, "the lines queued before the teardown alarm, then its own line, reach the destination");
    if (!delivered) std::fprintf(stderr, "  after the resume: %s\n", output.c_str());
    const bool byAlarm = !daemon.exitedNormally() && daemon.termSignal() == SIGALRM;
    check(byAlarm, "and the process still dies of SIGALRM");
    if (!byAlarm) std::fprintf(stderr, "  exit code or signal: %d\n", daemon.termSignal());
}

// GOAL: S14.37 -- a start that fails (the bus loop throws) and then, during its teardown, sees the
//       device lost exits 1 as the failure it is, and does not say the GPU was lost. False if the
//       loss takes over the stop: 75 then has the service manager restart the daemon as for a GPU
//       loss, and the log names the wrong cause.
// WHY-THIS-SHAPE: the render is held at its submission by the shim until /proc shows the unwind has
//       installed the teardown alarm (so the failure is already recorded), and only then is the
//       loss released into it. Releasing it on a timer could put the loss before the throw, where
//       75 is the right answer. A run where the shim never failed a submission is NOT VERIFIED.
// CONTROL: in backend/daemon/main.cpp, the `StopReason::Failed` store deleted from ~ArmTeardown ->
//       both lines red, alone: the loss took over the stop, exit 75 and "the GPU device was lost".
//       Observed 2026-09-27 via control_run.sh on NVIDIA; reverted. By hand, reverted: the shim's
//       hold variable misspelt -> NOT VERIFIED "no render reached a submission", no FAIL.
void checkLossDuringFailedStart(const std::string& validationEnv, const std::string& bluePath, const Check& check) {
    constexpr const char* kGoal = "S14.37 (a loss seen while a failed start tears down is not a GPU-loss exit)";
    ScratchDir work("failedstartloss");
    if (!work.ok()) {
        throw ScratchDirectoryLost("a scratch directory could not be created");
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
    const std::filesystem::path gate = work.path() / "release";
    const std::filesystem::path trigger = work.path() / "lose";
    std::error_code ec;
    std::filesystem::create_directories(work.path() / "cache", ec);
    SpawnedDaemon daemon({validationEnv, "DBUS_SESSION_BUS_ADDRESS=" + bus.address(), "HOME=" + work.path().string(),
                          "XDG_CACHE_HOME=" + (work.path() / "cache").string(), preloading(WALLPAPERX_DEVICE_LOST_SHIM),
                          "WALLPAPERX_TEST_LOSE_DEVICE=" + trigger.string(),
                          "WALLPAPERX_TEST_HOLD_SUBMIT=" + gate.string()});
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
        try {
            setWallpaper(*proxy, "static", bluePath, 0, std::chrono::seconds(10));
        } catch (const std::exception& e) {
            notVerified(kGoal, std::string("the daemon refused the still whose render is held: ") + e.what());
            return;
        }
    }
    bool held = false;
    for (const auto deadline = Clock::now() + std::chrono::seconds(30); !held && Clock::now() < deadline;) {
        held = std::filesystem::exists(gate.string() + ".held", ec);
        if (!held) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!held) {
        daemon.shutdown(SIGKILL);
        notVerified(kGoal, "no render reached a submission for the shim to hold");
        return;
    }
    bus.kill();
    Clock::time_point notYet;
    Clock::time_point armedBy;
    const bool armed = awaitAlarmArmed(daemon, notYet, armedBy);
    const bool released = std::ofstream(trigger).good() && std::ofstream(gate).good();
    const int exitCode = daemon.waitForExit();
    const std::string output = daemon.output();
    if (!released) {
        wallpaperx::testing::harnessFailure(std::string(kGoal) + ": the trigger or the gate file could not be created");
        return;
    }
    if (!armed || !std::filesystem::exists(trigger.string() + ".fired", ec)) {
        notVerified(kGoal, !armed ? "the daemon's unwind was never seen to install the teardown alarm"
                                  : "the shim never failed the held submission, so no loss was seen");
        return;
    }
    if (daemon.sanitizerReported()) {
        check(false, "a failed start that then sees a loss exits 1 -- not graded: the sanitizer reported, and the "
                     "exit status is its");
        return;
    }
    check(daemon.exitedNormally() && exitCode == 1, "a failed start that then sees the device lost exits 1, not 75");
    if (!(daemon.exitedNormally() && exitCode == 1)) {
        std::fprintf(stderr, "  exit code %d, signal %d\n", exitCode, daemon.termSignal());
    }
    check(output.find("the GPU device was lost") == std::string::npos,
          "and it does not report the loss as why it stopped");
}

// GOAL: S13.197 -- a restored video's renders are posted only after the playing record is open, so
//       a death in that video's playback NAMES it (else a restart loop's first crash names nobody).
// WHY-THIS-SHAPE: the preloaded library widens the microsecond window (creating playing.json takes
//       3 s) and faults on the render thread at the first picture asked of the decoder after one
//       was delivered -- the clock's work, never the open; a packet count landed inside the open on
//       the software decoder.
// CONTROL: in backend/daemon/main.cpp, `service.renderRestored();` moved above
//       `manager.recordStartingVideos();` -> "FAIL: a death in a restored video's playback names
//       that video" ("there is no readable playing record") and "FAIL: and the signal it died of".
//       Observed 2026-09-22 on NVIDIA. Reverted.
// COVERAGE-ASSERT: the daemon must have died of the staged fault, its line present, or NOT VERIFIED.
void checkTheRestoredVideoIsPostedAfterTheRecordOpens(const std::string& validationEnv, const std::string& clip,
                                                      const Check& check) {
    constexpr const char* kGoal = "S13.197 (a restored video's renders are posted after the playing record is open, "
                                  "so a death in its playback names it)";
    constexpr int kMonitor = 0;
    std::error_code ec;
    if (!std::filesystem::exists(clip, ec)) {
        notVerified(kGoal, "the video fixture " + clip + " was not generated, so no restored video can play");
        return;
    }
    ScratchDir home("restoreorder");
    if (!home.ok()) {
        throw ScratchDirectoryLost("a scratch HOME could not be created");
    }
    const std::filesystem::path configHome = home.path() / "config";
    const std::filesystem::path stateHome = home.path() / "state";
    std::filesystem::create_directories(configHome / "wallpaperx", ec);
    std::filesystem::create_directories(stateHome, ec);
    {
        // The saved choice this daemon restores, written as the daemon writes it.
        nlohmann::json monitors = nlohmann::json::array();
        monitors.push_back({{"monitor_id", kMonitor}, {"type", "video"}, {"path", clip}});
        std::ofstream out(configHome / "wallpaperx" / "config.json", std::ios::trunc);
        out << nlohmann::json{{"version", 1}, {"monitors", std::move(monitors)}}.dump();
        if (!out) {
            notVerified(kGoal, "the saved wallpaper could not be written");
            return;
        }
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
    std::vector<std::string> env = wedgedDaemonEnv(validationEnv, bus.address(), home.path());
    env.erase(std::remove(env.begin(), env.end(), std::string("WALLPAPERX_TEST_WEDGE_FREE=1")), env.end());
    env.push_back("XDG_CONFIG_HOME=" + configHome.string());
    env.push_back("XDG_STATE_HOME=" + stateHome.string());
    env.push_back("WALLPAPERX_TEST_DELAY_PLAYING_OPEN_MS=3000");
    env.push_back("WALLPAPERX_TEST_CRASH_IN_PLAYBACK=1");

    SpawnedDaemon daemon(env);
    const int exitCode = daemon.waitForExit();
    const std::string output = daemon.output();
    const bool faulted = output.find("wedge_shim: faulting") != std::string::npos;
    const bool diedOfTheFault = faulted && !daemon.timedOut() && !daemon.exitedNormally() &&
                                daemon.termSignal() == SIGSEGV;
    if (!diedOfTheFault) {
        notVerified(kGoal, faulted ? "the daemon did not die of the staged fault (exit code " +
                                         std::to_string(exitCode) + ", signal " +
                                         std::to_string(daemon.termSignal()) + ")"
                                   : "the staged fault never fired: this host never decoded the restored video");
        return;
    }
    // GOAL: X.27 -- the staged SIGSEGV is not handed to the host's crash collector (which on a
    //       desktop also raises a crash dialog): the daemon dies with the wait status's core bit clear.
    // WHY-THIS-SHAPE: the core bit, not the core limit: a piped core_pattern calls the collector even
    //       at RLIMIT_CORE 0. A file pattern has no collector, so that branch answers "not reached"
    //       on stdout and cannot go red by design.
    // CONTROL: via control_batch 2026-10-04, reverted: suppressCoreDump() deleted from
    //       backend/tests/wedge_shim.cpp's fault -> "FAIL: X.27: the daemon's staged SIGSEGV is not
    //       handed to the core collector", one new coredumpctl entry. Test-side: coreGoesToAProgram()
    //       (renderer/tests/support/forked_child.h) returning false -> the stdout line "X.27: this
    //       host's core_pattern writes a file ..." and Passed (seen in crash_log_flush, same helper).
    {
        std::string why;
        const std::optional<bool> collected = wallpaperx::testing::stagedCrashReachedCollector(daemon, why);
        if (!collected) {
            notVerified("X.27 (a deliberate daemon crash does not reach the core collector)", why);
        } else {
            check(!*collected, "X.27: the daemon's staged SIGSEGV is not handed to the core collector (no core bit in "
                               "its wait status)");
        }
    }
    const std::optional<wallpaperx::PlayingHistory> history = wallpaperx::readPlayingRecord(stateHome / "wallpaperx");
    const bool named = history && history->died && history->died->monitorId == kMonitor && history->died->path == clip;
    check(named, "a death in a restored video's playback names that video");
    if (!named) {
        std::fprintf(stderr, "  %s\n", !history            ? "there is no readable playing record"
                                       : !history->died    ? "the record names no video"
                                                           : ("the record names monitor " +
                                                              std::to_string(history->died->monitorId) + " '" +
                                                              history->died->path + "'")
                                                                 .c_str());
    }
    check(history && history->signal == SIGSEGV, "and the signal it died of");
    std::printf("daemon_system_test: a restored video's playback was faulted at its second picture, and the record "
                "%s\n",
                named ? "named it" : "did not");
}

// GetStatus's config_server object, or nullptr: operator[] on a const json missing the key is UB.
const nlohmann::json* configServerReport(const StatusReply& status) {
    const nlohmann::json& json = status.json();
    if (!json.is_object() || !json.contains("config_server")) return nullptr;
    const nlohmann::json& report = json["config_server"];
    return report.is_object() ? &report : nullptr;
}

// The daemon's verdict on its config port ("ok", "fail", "pending"), or empty with `why`.
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
        // is_string() first: value() throws on a present non-string (X.4).
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

// The daemon's bind wordings (http_server.cpp), duplicated since a spawned program's literals
// cannot be read; each is cross-checked against the live GetStatus detail, so a reword is caught.
std::string bindFailureMarker(int port) {
    return std::string("could not bind ") + wallpaperx::HttpServer::kLoopbackHost + ":" + std::to_string(port);
}

std::string bindSuccessMarker(int port) {
    return std::string("config server on http://") + wallpaperx::HttpServer::kLoopbackHost + ":" +
           std::to_string(port);
}

std::string listeningDetail(int port) {
    return std::string("listening on ") + wallpaperx::HttpServer::kLoopbackHost + ":" + std::to_string(port);
}

// A loopback port the kernel picks and that is let go at once, so no phase competes for 18462.
// Losing the race for it is a bind failure awaitConfigServerBind reports as a skip. -1: none.
int unusedLoopbackPort() {
    wallpaperx::testing::HeldPort probe;
    if (!probe.open(INADDR_LOOPBACK, 0)) return -1;
    return probe.port();
}

std::vector<std::string> configPortArgs(int port) { return {"--config-port", std::to_string(port)}; }

// GOAL: X.3 both ways, for phases needing the spawned daemon to be the one listening on `port`: a
//       reworded bind-success line or a daemon that quit is a named FAIL; a daemon the machine
//       killed, or a port really held by another process (asserted only where the daemon's own
//       bind-failure line says so), is a skip. Control (f) is also S13.210's evidence.
// WHY-THIS-SHAPE: one function, not a copy per phase. Each positive signal (the failure line; the
//       process gone, its status from waitid, not the capture pipe's EOF; the daemon's own "ok")
//       is consulted before anything is concluded from an absent line. The drain re-wait expires
//       into the same FAIL (X.1).
// CONTROL: six, all reverted; (a), (b), (f) 2026-09-23 with all three callers, (c)-(e) earlier.
//       (a) the success printf in backend/daemon/http_server.cpp reworded -> "FAIL: the
//       bind-success wording the connector-name phase waits for is still the daemon's" (and the
//       HTTP lifecycle's). (b) the HttpServer construction in backend/daemon/main.cpp commented out
//       -> NOT VERIFIED "... printed neither the bind-success nor the bind-failure line ...", no
//       FAIL. (c) it replaced by a thread that `std::_Exit(7)`s two seconds in -> "FAIL: the daemon
//       of the connector-name phase is still running when its config port is asked about". (d) the
//       same with SIGKILL -> NOT VERIFIED "... killed by signal 9 by something outside this test
//       ...", no FAIL. (e) readLoop() returning at once (a live daemon behind a lost pipe) ->
//       "HARNESS FAULT -- the connector-name phase could not be set up: the daemon's output pipe
//       failed while it was still running". (f) a bare socket holding 18462 -> from the
//       default-port phase only, the port-conflict skip with the daemon's own detail.
bool awaitConfigServerBind(const SpawnedDaemon& daemon, int port, const char* goals, const char* phase,
                           const Check& check) {
    const std::string bindOk = bindSuccessMarker(port);
    const std::string bindFail = bindFailureMarker(port);
    // Both markers: they are one `if` in HttpServer's constructor, so a taken port answers at once.
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

    // First: the daemon's own words, true even if it later died. config_server "fail" alone is
    // as true of EACCES or EADDRNOTAVAIL, so only this line claims another process holds the port.
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

// A /health body's `checks` object, or nullptr; asked before read, since operator[] on a const
// json missing the key asserts. CONTROL: `{"checks", checksJson}` dropped from the /health body in
// backend/daemon/http_server.cpp -> four named FAILs from "the real daemon's /health names shaders,
// gpu and wallpapers as separate checks", run finished (read-then-ask aborted). Reverted.
const nlohmann::json* healthChecks(const nlohmann::json& body) {
    if (!body.is_object() || !body.contains("checks")) return nullptr;
    const nlohmann::json& checks = body["checks"];
    return checks.is_object() ? &checks : nullptr;
}

// The checks reading `pending`, sorted: "ok overall" alone is as true with no pending state left.
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


// ---------------------------------------------------------------------------- //
// Phase 10 -- the HTTP config server, in the shipped binary: the command line that picks the
// port, main.cpp wiring it up, nothing listening after exit (S7.8), a poll during shutdown
// (S7.11). Every phase but the default-port one picks its own port.
// ---------------------------------------------------------------------------- //

void awaitNameRelease(NameWatcher& names, const char* nextUp);

// The words every refusal of the command line ends with, restated from main.cpp's usage text.
constexpr const char* kUsageLine = "usage: wallpaperx-daemon [--config-port <port>]";

// GOAL: S13.207 -- the daemon's command line is strict. Each refusal (an unknown option, the
//       `--config-port=<n>` form, a missing value, the option twice, a non-number, a number
//       outside 0-65535) exits 2 before doing anything, naming what was wrong and printing
//       the usage; --help and -h print the usage and exit 0. False if any of them starts a
//       daemon, exits with another code, or refuses without saying why.
// WHY-THIS-SHAPE: the real binary, main() being the only parser. "Started nothing" is the bus name
//       never taken; the reason is matched per case, since a parser refusing everything prints the
//       same usage.
// CONTROL: each in parseArguments (backend/daemon/main.cpp), each reverted.
//       (a) `portGiven ||` deleted -> "FAIL: --config-port 5 --config-port 6 is refused",
//       printing "exit -1 (still running after 5 s, took the bus name: yes)". (b) `|| port >
//       65535` deleted -> "FAIL: --config-port 65536 is refused", printing "exit -1 ...".
//       (c) `end != value.data() + value.size() ||` deleted -> "FAIL: --config-port 1x is
//       refused". (d) the unknown-argument branch made to `continue` -> "FAIL: --bogus is
//       refused" and "FAIL: --config-port=5 is refused". Observed 2026-09-23.
void checkCommandLine(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    struct Case {
        std::vector<std::string> args;
        const char* reason;  // what the refusal must say, beside the usage line
    };
    const Case kRefused[] = {
        {{"--bogus"}, "unknown argument '--bogus'"},
        {{"--config-port=5"}, "unknown argument '--config-port=5'"},
        {{"--config-port"}, "--config-port takes one port, given once"},
        {{"--config-port", "5", "--config-port", "6"}, "--config-port takes one port, given once"},
        {{"--config-port", "abc"}, "--config-port needs a port from 0 to 65535, got 'abc'"},
        {{"--config-port", "1x"}, "--config-port needs a port from 0 to 65535, got '1x'"},
        {{"--config-port", "-1"}, "--config-port needs a port from 0 to 65535, got '-1'"},
        {{"--config-port", "65536"}, "--config-port needs a port from 0 to 65535, got '65536'"},
        {{"--config-port", ""}, "--config-port needs a port from 0 to 65535, got ''"},
    };
    const auto run = [&](const std::vector<std::string>& args, int& exitCode, std::string& output, bool& tookName) {
        // Counted, not asked: a name taken and let go again would read as never taken.
        const uint64_t eventsBefore = names.events();
        SpawnedDaemon daemon({validationEnv}, SpawnedDaemon::Stdout::Captured, {}, 0, args);
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!daemon.exited() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool ranOn = !daemon.exited();
        exitCode = ranOn ? daemon.shutdown(SIGTERM) : daemon.waitForExit();
        if (ranOn) exitCode = -1;
        output = daemon.output();
        // The name signal travels the bus, slower than the exit.
        names.waitForEventsPast(eventsBefore, std::chrono::milliseconds(200));
        tookName = names.events() != eventsBefore;
        if (tookName) awaitNameRelease(names, "the next command-line case");
        return ranOn;
    };
    std::string joined;
    for (const Case& c : kRefused) {
        joined.clear();
        for (const std::string& a : c.args) joined += (joined.empty() ? "" : " ") + a;
        int exitCode = 0;
        std::string output;
        bool tookName = false;
        const bool ranOn = run(c.args, exitCode, output, tookName);
        const bool refused = !ranOn && !tookName && exitCode == 2 && output.find(c.reason) != std::string::npos &&
                             output.find(kUsageLine) != std::string::npos;
        check(refused, (joined + " is refused").c_str());
        if (!refused) {
            std::fprintf(stderr, "  exit %d (%s, took the bus name: %s)\n  output: %s\n", exitCode,
                         ranOn ? "still running after 5 s" : "exited", tookName ? "yes" : "no", output.c_str());
        }
    }
    for (const char* help : {"--help", "-h"}) {
        int exitCode = 0;
        std::string output;
        bool tookName = false;
        const bool ranOn = run({help}, exitCode, output, tookName);
        const bool shown = !ranOn && !tookName && exitCode == 0 && output.find(kUsageLine) != std::string::npos;
        check(shown, (std::string(help) + " prints the usage and exits 0").c_str());
        if (!shown) std::fprintf(stderr, "  exit %d, output: %s\n", exitCode, output.c_str());
    }
}

// How one run of the daemon binary with its stdout and stderr on a pipe whose reader was closed
// before it started ended: `wallpaperx-daemon --help | true` once true has exited.
struct ReaderGoneRun {
    bool started = false;
    bool ended = false;  // false: still running after 10 s, and killed
    int status = 0;
};

ReaderGoneRun runWithOutputReaderGone(const std::vector<std::string>& args, const std::vector<std::string>& extraEnv) {
    ReaderGoneRun run;
    int ends[2];
    if (pipe2(ends, O_CLOEXEC) != 0) return run;
    close(ends[0]);
    std::vector<std::string> env = wallpaperx::testing::mergedEnvironment(extraEnv);
    std::vector<char*> envp;
    for (std::string& entry : env) envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::string binary = WALLPAPERX_DAEMON_BINARY;
    std::vector<std::string> argStorage = args;
    std::vector<char*> argv{binary.data()};
    for (std::string& arg : argStorage) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid < 0) {
        close(ends[1]);
        return run;
    }
    if (pid == 0) {
        if (dup2(ends[1], STDOUT_FILENO) == -1 || dup2(ends[1], STDERR_FILENO) == -1) {
            _exit(wallpaperx::testing::kChildDup2FailedExit);
        }
        // As a shell starts it: SIGPIPE at its default action, and deliverable.
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &dfl, nullptr);
        sigset_t pipeSignal;
        sigemptyset(&pipeSignal);
        sigaddset(&pipeSignal, SIGPIPE);
        sigprocmask(SIG_UNBLOCK, &pipeSignal, nullptr);
        execChildOrDie(parent, binary.c_str(), argv.data(), envp.data());
    }
    close(ends[1]);
    run.started = true;
    for (const auto deadline = Clock::now() + std::chrono::seconds(10); !run.ended && Clock::now() < deadline;) {
        run.ended = waitpid(pid, &run.status, WNOHANG) == pid;
        if (!run.ended) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!run.ended) {
        kill(pid, SIGKILL);
        waitpid(pid, &run.status, 0);
    }
    return run;
}

// GOAL: S14.35 -- `--help` and a refused argument, with stdout and stderr on a pipe whose reader has
//       already gone, exit 0 and 2 as they do with a reader; neither dies of SIGPIPE. False if
//       SIGPIPE is ignored only once serving begins: the usage text reaches the log writer
//       (backend/daemon/log_pump.cpp) before that, and its failed write kills the process.
// WHY-THIS-SHAPE: forked here rather than through SpawnedDaemon, whose reader thread is the very
//       thing that must be absent; the reader is closed before the fork, so no write can find it.
//       The child resets SIGPIPE to its default, as a shell does, since this process's own
//       disposition would otherwise be inherited through exec.
// CONTROL: in backend/daemon/main.cpp, main()'s SIGPIPE ignore moved into serve(), after the
//       usage has been printed -> both lines red, alone. Observed
//       2026-09-25 via control_run.sh on NVIDIA; reverted.
void checkUsageWithOutputReaderGone(const std::string& validationEnv, const Check& check) {
    ScratchDir home("readergone");
    if (!home.ok()) {
        throw ScratchDirectoryLost("a scratch HOME could not be created");
    }
    const std::vector<std::string> env{validationEnv, "HOME=" + home.path().string(),
                                       "XDG_CONFIG_HOME=" + (home.path() / "config").string(),
                                       "XDG_STATE_HOME=" + (home.path() / "state").string()};
    for (const auto& [arg, expected] : {std::pair<const char*, int>{"--help", 0}, {"--bogus", 2}}) {
        const ReaderGoneRun run = runWithOutputReaderGone({arg}, env);
        if (!run.started) {
            wallpaperx::testing::harnessFailure(std::string(arg) + " with its output's reader gone: fork failed");
            continue;
        }
        if (run.ended && WIFEXITED(run.status)) {
            if (const char* fault = wallpaperx::testing::forkPreambleFault(WEXITSTATUS(run.status))) {
                wallpaperx::testing::harnessFailure(std::string(arg) + " with its output's reader gone: " + fault);
                continue;
            }
        }
        const bool held = run.ended && WIFEXITED(run.status) && WEXITSTATUS(run.status) == expected;
        check(held, (std::string(arg) + " exits " + std::to_string(expected) +
                     " with its output's reader gone, rather than dying of SIGPIPE")
                        .c_str());
        if (!held) {
            std::fprintf(stderr, "  %s: %s\n", arg,
                         !run.ended                 ? "still running after 10 s"
                         : WIFSIGNALED(run.status) ? ("signal " + std::to_string(WTERMSIG(run.status))).c_str()
                                                   : ("exit " + std::to_string(WEXITSTATUS(run.status))).c_str());
        }
    }
}

// GOAL: S13.208 -- a daemon given no port serves on 18462, the one the desktop plugin asks.
//       False if the default moved, or a daemon started bare served nowhere.
// WHY-THIS-SHAPE: the one phase on the shipped port; skips through the shared classifier when
//       18462 is taken. Written out, not kDefaultPort: the claim is about the number main.qml has.
// CONTROL: `int configPort = wallpaperx::HttpServer::kDefaultPort;` in backend/daemon/main.cpp
//       made `18463`, with 18462 free -> "FAIL: a daemon given no port serves on 18462",
//       printing "config_server ok / listening on 127.0.0.1:18463", and "FAIL: a daemon given
//       no port answers /health on 18462". With 18462 held by another process and no mutation,
//       the phase is NOT VERIFIED through awaitConfigServerBind's port-conflict arm. Both
//       observed 2026-09-23. Reverted.
void checkDefaultConfigPort(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr int kShippedPort = 18462;
    constexpr const char* kGoal = "S13.208 (a daemon given no port serves on 18462)";
    SpawnedDaemon daemon({validationEnv});
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(daemon, kGoal)) notVerified(kGoal, "the daemon never acquired the bus name");
        return;
    }
    // Asked first: the detail names whatever port was bound. Anything but ok goes to the classifier.
    std::string detail;
    std::string why;
    const std::string status = askConfigServerStatus(detail, why);
    if (status == "ok") {
        check(detail == listeningDetail(kShippedPort), "a daemon given no port serves on 18462");
        if (detail != listeningDetail(kShippedPort)) std::fprintf(stderr, "  config_server ok / %s\n", detail.c_str());
        httplib::Client cli = wallpaperx::testing::makeClient(kShippedPort);
        check(wallpaperx::testing::get(cli, "/health").ok(), "a daemon given no port answers /health on 18462");
    } else {
        (void)awaitConfigServerBind(daemon, kShippedPort, kGoal, "the default-port phase", check);
    }
    daemon.shutdown(SIGTERM);
    awaitNameRelease(names, "the port-0 phase");
}

// GOAL: S13.209 -- `--config-port 0` lets the kernel choose, and the daemon names the port it
//       got in the startup line and in GetStatus's config_server detail, the two agreeing, and
//       /health answers on it. False if either names 0, or they disagree, or nothing listens.
// WHY-THIS-SHAPE: agreement with a port nobody chose in advance, read from the startup line; the
//       detail compared whole keeps listeningDetail() honest.
// CONTROL: in HttpServer's constructor (backend/daemon/http_server.cpp), the success record made
//       to name `m_port` and moved above `m_port = bound;`, so it names the port asked for ->
//       "FAIL: GetStatus names the port the startup line does", alone, printing "startup line
//       port 44623; config_server ok, detail [listening on 127.0.0.1:0]". And the startup printf
//       made to print 0 -> "FAIL: a daemon told port 0 names the port it bound in its startup
//       line" ("bound 0"), beside the two bind-success-wording failures of the phases that wait
//       for that line. Observed 2026-09-23. Both reverted.
void checkChosenConfigPort(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr const char* kGoal = "S13.209 (port 0 is the kernel's choice, and the daemon names it)";
    SpawnedDaemon daemon({validationEnv}, SpawnedDaemon::Stdout::Captured, {}, 0, configPortArgs(0));
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(daemon, kGoal)) notVerified(kGoal, "the daemon never acquired the bus name");
        return;
    }
    const std::string prefix = bindSuccessMarker(0).substr(0, bindSuccessMarker(0).size() - 1);
    const bool printed = daemon.waitForOutput(prefix, std::chrono::seconds(10));
    int bound = -1;
    if (printed) {
        const std::string output = daemon.output();
        const size_t at = output.find(prefix) + prefix.size();
        size_t end = at;
        while (end < output.size() && std::isdigit(static_cast<unsigned char>(output[end]))) ++end;
        if (end > at && end - at <= 5) bound = std::stoi(output.substr(at, end - at));
    }
    check(printed && bound > 0, "a daemon told port 0 names the port it bound in its startup line");
    if (!printed || bound <= 0) {
        std::fprintf(stderr, "  bound %d; output: %s\n", bound, daemon.output().c_str());
    } else {
        std::string detail;
        std::string why;
        const std::string status = askConfigServerStatus(detail, why);
        check(status == "ok" && detail == listeningDetail(bound), "GetStatus names the port the startup line does");
        if (status != "ok" || detail != listeningDetail(bound)) {
            std::fprintf(stderr, "  startup line port %d; config_server %s, detail [%s]%s\n", bound, status.c_str(),
                         detail.c_str(), why.empty() ? "" : (" -- " + why).c_str());
        }
        httplib::Client cli = wallpaperx::testing::makeClient(bound);
        check(wallpaperx::testing::get(cli, "/health").ok(), "/health answers on the port the daemon named");
    }
    daemon.shutdown(SIGTERM);
    awaitNameRelease(names, "the HTTP lifecycle phase");
}

// GOAL: S15.398 -- the config port answers /config the moment the daemon's bus name appears, and the
//       daemon sends no ConfigChanged at startup. False if a plugin reading /config at the name's
//       arrival can find the port closed (it shows "not running" until some later change), or if a
//       startup announcement makes every plugin read /config a second time for nothing.
// WHY-THIS-SHAPE: tests/wedge_shim.cpp delays the playing record's creation by 1.5 s, a step taken
//       after the name is claimed and before the event loop, so a port bound after the claim is still
//       closed when the name appears for certain, not for microseconds. "None at startup" is read as
//       exactly one ConfigChanged, 1 s after a barrier SetMonitorOutput's own: a startup one leaves the
//       same connection before the barrier's, so it cannot arrive after it, and a correct daemon can
//       never reach two. The listener matches ConfigChanged from any sender, as the plugin subscribes,
//       and is in place before the daemon starts: a proxy addressed to the well-known name hears
//       nothing sent before the name is claimed. Only the daemon sends ConfigChanged on this bus.
// CONTROL: via control_run.sh 2026-10-08, --phase configChangedAtStartup, each reverted. In serve()
//       (backend/daemon/main.cpp) the HttpServer and armBeforeHttp moved to just after the claim block
//       -> "the config port answers /config the moment the daemon's bus name appears" alone, the
//       barrier taking 3001 ms. `announceConfigChanged();` added at the end of
//       DbusService::renderRestored (backend/daemon/dbus_service.cpp) -> "the daemon sends no
//       ConfigChanged at startup" alone, seen 2 times. The same added to DbusService::restoreSaved,
//       which runs before the claim -> the same line alone, seen 2 times.
// COVERAGE-ASSERT: the barrier, sent right after /config answered, must take at least 750 ms to be
//       answered -- it cannot be served before the event loop runs -- or the read may have come after
//       the delay and the phase is not verified.
void checkConfigChangedAtStartup(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr const char* kGoal = "S15.398 (/config answers when the name appears; no ConfigChanged at startup)";
    constexpr auto kInsideDelay = std::chrono::milliseconds(750);
    const int port = unusedLoopbackPort();
    if (port < 0) {
        notVerified(kGoal, "no free loopback port could be found for this phase's daemon");
        return;
    }

    // Written on the listener's dispatch thread; declared before the listener, so it outlives it.
    struct Seen {
        std::mutex mutex;
        std::condition_variable arrived;
        int count = 0;
    } seen;
    // Its event-loop thread stops when the connection is destroyed, before `seen` is.
    auto listener = sdbus::createSessionBusConnection();
    listener->addMatch(std::string("type='signal',path='") + DbusService::kObjectPath + "',interface='" +
                           DbusService::kInterface + "',member='ConfigChanged'",
                       [&seen](sdbus::Message) {
                           {
                               std::lock_guard<std::mutex> lock(seen.mutex);
                               ++seen.count;
                           }
                           seen.arrived.notify_all();
                       });
    listener->enterEventLoopAsync();
    const auto waitForCount = [&seen](int atLeast, std::chrono::seconds patience) {
        std::unique_lock<std::mutex> lock(seen.mutex);
        return seen.arrived.wait_for(lock, patience, [&] { return seen.count >= atLeast; });
    };
    const auto count = [&seen] {
        std::lock_guard<std::mutex> lock(seen.mutex);
        return seen.count;
    };

    SpawnedDaemon daemon(
        {validationEnv, preloading(WALLPAPERX_WEDGE_SHIM), "WALLPAPERX_TEST_DELAY_PLAYING_OPEN_MS=1500"},
        SpawnedDaemon::Stdout::Captured, {}, 0, configPortArgs(port));
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(daemon, kGoal)) notVerified(kGoal, "the daemon never acquired the bus name");
        return;
    }
    httplib::Client cli = wallpaperx::testing::makeClient(port);
    const bool answered = wallpaperx::testing::get(cli, "/config?output=DP-STARTUP").ok();
    const auto answeredAt = Clock::now();

    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    const std::string named =
        wallpaperx::testing::errorNameFrom([&] { wallpaperx::testing::setMonitorOutput(*proxy, 0, "DP-STARTUP"); });
    const auto barrierTook = Clock::now() - answeredAt;
    if (barrierTook < kInsideDelay) {
        notVerified(kGoal, "the barrier SetMonitorOutput was answered " +
                               std::to_string(static_cast<long long>(millis(barrierTook))) +
                               " ms after /config, so the read may have come after the staged delay rather than "
                               "inside it");
    } else {
        check(answered, "the config port answers /config the moment the daemon's bus name appears");
        if (!named.empty() || !waitForCount(1, std::chrono::seconds(5))) {
            notVerified("S15.398's no-startup-announcement clause",
                        "the barrier SetMonitorOutput answered '" + named + "' or was not announced within 5 s");
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            check(count() == 1, "the daemon sends no ConfigChanged at startup");
        }
        std::fprintf(stderr, "  /config %s at the name; the barrier took %.0f ms; ConfigChanged seen %d time(s)\n",
                     answered ? "answered" : "did not answer", millis(barrierTook), count());
    }
    daemon.shutdown(SIGTERM);
    awaitNameRelease(names, "the startup-announcement phase");
}

void checkHttpLifecycle(NameWatcher& names, const std::string& bluePath, const std::string& validationEnv,
                        const Check& check) {
    using wallpaperx::testing::ConnectResult;
    using wallpaperx::testing::describe;
    using wallpaperx::testing::HttpReply;
    using wallpaperx::testing::Poller;
    using wallpaperx::testing::tryConnect;

    constexpr int32_t kMonitorHttp = 26;

    const int kPort = unusedLoopbackPort();
    if (kPort < 0) {
        notVerified("S7.8/S7.11 (the HTTP server's process lifecycle)",
                    "no free loopback port could be found for this phase's daemon");
        return;
    }
    SpawnedDaemon daemon({validationEnv}, SpawnedDaemon::Stdout::Captured, {}, 0, configPortArgs(kPort));
    if (!names.waitFor(true)) {
        if (reportedStartupFault(daemon, "S7.8/S7.11 (the HTTP server's process lifecycle)")) return;
        notVerified("S7.8/S7.11 (the HTTP server's process lifecycle)",
                    "the daemon for this phase never acquired the bus name");
        return;
    }

    if (!awaitConfigServerBind(daemon, kPort, "S7.8/S7.11 (the HTTP server's process lifecycle)",
                               "the HTTP lifecycle phase", check)) {
        daemon.shutdown(SIGTERM);
        return;
    }

    // GOAL: S7.10/S7.10b end to end -- a fresh real daemon's /health has `shaders` and `gpu`
    //       `pending` while the aggregate, counting failures only, reads `ok`.
    // WHY-THIS-SHAPE: the in-process tier pins the JSON; only this tier shows the shipped binary
    //       reaches the state at all. The pending set is asserted, since `ok` alone is as true of a
    //       daemon reporting ok from birth.
    // CONTROL: the HttpServer construction removed from backend/daemon/main.cpp -> NOT VERIFIED
    //       here (the port-0 phase and phase 11 grade it), 2026-09-23. In
    //       backend/daemon/http_server.cpp: the Pending-and-not-Fail aggregation arm restored ->
    //       "FAIL: the real daemon reports ok overall before it has used anything, with the
    //       dependencies it has not used pending" ("aggregate: pending"); HealthCheck's default made
    //       Ok (backend/daemon/wallpaper_manager.h) -> the same line, "(none)", beside the GPU one;
    //       the gpu entry removed -> "FAIL: the real daemon's /health names shaders, gpu and
    //       wallpapers as separate checks" and the GPU-untried line. All reverted.
    httplib::Client cli = wallpaperx::testing::makeClient(kPort);
    const HttpReply fresh = wallpaperx::testing::get(cli, "/health");
    check(fresh.ok(), "the real daemon answers GET /health on its config port");
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
                         joined(untried).c_str(), aggregate.c_str(), fresh.body.c_str());
        }
        std::printf("daemon_system_test: /health on port %d before any wallpaper: %s\n", kPort, fresh.body.c_str());
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
        // GOAL: S7.10/S7.10b's other direction -- after a render, those checks read `ok`, and the
        //       GPU one names the device GetStatus reports.
        // WHY-THIS-SHAPE: the pending checks above mean nothing unless `ok` is reachable. Compared
        //       with GetStatus's name and tier, not a literal: the tier varies by host.
        // CONTROL: (a) `recordHealth(&DaemonHealth::gpu, HealthStatus::Ok, ...)` deleted from
        //       ensureGpu -> "FAIL: the real daemon's /health names the GPU GetStatus reports";
        //       with renderPending's record deleted too -> also "... reports the GPU as ok after a
        //       render" and "... reports the aggregate as ok after a successful render". (b) the
        //       detail made "gpu" -> only the names-the-GPU line. All reverted.
        const HttpReply afterRender = wallpaperx::testing::get(cli, "/health");
        // An unanswered GetStatus leaves both empty, which the check below names.
        const StatusReply status = getStatusOrExplain(*proxy);
        const nlohmann::json& statusJson = status.json();
        const std::string dbusTier = statusJson.contains("gpu") && statusJson["gpu"].is_object()
                                         ? statusJson["gpu"].value("descriptor_tier", std::string())
                                         : std::string();
        const std::string dbusName = statusJson.contains("gpu") && statusJson["gpu"].is_object()
                                         ? statusJson["gpu"].value("name", std::string())
                                         : std::string();
        const nlohmann::json* afterChecks = healthChecks(afterRender.json);
        // ok() stays: a verdict read from a non-200 body would pass without the endpoint working.
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
    // WHY-THIS-SHAPE: polling ACROSS the SIGTERM, so a poll lands during teardown; graded are
    //       what a broken ordering destroys (exit 0, "stopped cleanly", no malformed reply), never
    //       that a particular request got a 200 (X.2: when serving stops is shutdown's choice).
    // CONTROL: backend/daemon/main.cpp's HttpServer leaked with `new`, so it outlives the manager
    //       -> SIGSEGV in shutdown, "FAIL: the daemon exits 0 on SIGTERM with HTTP polls in flight"
    //       and "FAIL: the daemon still runs its destructor chain with HTTP polls in flight". Reverted.
    // COVERAGE-ASSERT: the polls collected before the signal are counted, or the malformed check is
    //       trivially true.
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

    if (sanitizerReportedIn(output)) {
        check(false, "the HTTP-lifecycle daemon's sanitizer reported no warnings (its exit code is the "
                     "sanitizer's here, so the exit-code check cannot run)");
    } else {
        check(exitCode == 0, "the daemon exits 0 on SIGTERM with HTTP polls in flight");
    }
    check(!daemon.timedOut(), "the daemon exits rather than hanging when its serving thread has to be joined");
    check(output.find("stopped cleanly") != std::string::npos,
          "the daemon still runs its destructor chain with HTTP polls in flight");
    check(names.waitFor(false), "the bus name is released after the HTTP-lifecycle daemon exits");

    // S7.8, after the reap: an open socket now is one the exited process left behind.
    const ConnectResult afterExit = tryConnect("127.0.0.1", kPort, 1000);
    check(afterExit == ConnectResult::Refused, "nothing is listening on the config port once the daemon has exited");
    if (afterExit != ConnectResult::Refused) {
        std::fprintf(stderr, "  connect to 127.0.0.1:%d %s (expected refused)\n", kPort, describe(afterExit));
    }
}

// ---------------------------------------------------------------------------- //
// Phase 11 -- S7.14: the daemon says on DBus whether it got its own config port. Against the real
// binary: main.cpp alone hands the bind result to the manager.
// ---------------------------------------------------------------------------- //

// Who holds 127.0.0.1:`port`, from /proc, which the daemon whose report is graded does not write.
enum class Listener { Daemon, Other, Nobody, Unknown };

// The listening socket's inode, 0 for none; false only when /proc/net/tcp is unreadable.
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
        // A descriptor closed under the walk is evidence of nothing.
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

// A barrier between daemon-spawning phases, not a check: a reaped process's name is released
// whatever the daemon did, so nothing could make it red (the SIGTERM phase grades release).
void awaitNameRelease(NameWatcher& names, const char* nextUp) {
    if (!names.waitFor(false)) {
        notVerified(nextUp, "the bus name was still owned after this phase's daemon had been reaped");
    }
}

// Direction 1: the port is free, so the daemon is the process listening on it.
void checkConfigServerOwnsPort(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    const int kPort = unusedLoopbackPort();
    if (kPort < 0) {
        notVerified("S7.14 (the ok direction)", "no free loopback port could be found for this phase's daemon");
        return;
    }
    SpawnedDaemon daemon({validationEnv}, SpawnedDaemon::Stdout::Captured, {}, 0, configPortArgs(kPort));
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
    //       process listening on the port it was told, not merely whenever the daemon is up.
    // WHY-THIS-SHAPE: /proc says who holds the socket; `status == "ok"` alone passes for a
    //       hardcoded string. Sampled after a GetStatus reply, which comes only once the event loop
    //       runs, after the HttpServer recorded its bind (the bus name is taken earlier).
    // CONTROL: (a) backend/daemon/main.cpp's HttpServer on `options->configPort + 1` -> "FAIL:
    //       config_server reads ok exactly when this daemon is the process holding its port" and
    //       "FAIL: the config_server report names the port it is listening on" ("... held by
    //       nobody"); (b) `status["config_server"] = ...` deleted from DbusService::getStatus ->
    //       "FAIL: GetStatus carries a config_server object" (not re-run 2026-09-23); (c) the success
    //       detail made empty in backend/daemon/http_server.cpp -> the names-the-port line. (a), (c)
    //       2026-09-23. All reverted.
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
                  "config_server reads ok exactly when this daemon is the process holding its port");
            const bool namesPort = reportStatus != "ok" || reportDetail == listeningDetail(kPort);
            check(!reportDetail.empty() && namesPort, "the config_server report names the port it is listening on");
            if ((reportStatus == "ok") != (who == Listener::Daemon) || reportDetail.empty() || !namesPort) {
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
    // WHY-THIS-SHAPE: the two channels differing -- absence alone is true of an empty /health.
    // CONTROL: `{"config_server", &snap.health.configServer},` added to http_server.cpp's
    //       checks array (sized 4) -> "FAIL: the bind result is on DBus only -- /health
    //       names its three checks and not config_server". Reverted.
    httplib::Client cli = wallpaperx::testing::makeClient(kPort);
    const wallpaperx::testing::HttpReply health = wallpaperx::testing::get(cli, "/health");
    const nlohmann::json* checks = healthChecks(health.json);
    const bool healthUsable = health.ok() && checks != nullptr;
    if (!healthUsable) {
        // Graded by the HTTP-lifecycle phase; one fault, one name.
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

// Direction 2: this test holds the port first. `hostRendered` (from earlier phases, never this
// daemon) separates "the conflict crippled the daemon" from "no working Vulkan".
void checkConfigServerLostPort(NameWatcher& names, const std::string& bluePath, const std::string& validationEnv,
                               bool hostRendered, const Check& check) {
    // The skip is decided on THIS process's bind(), never on config_server, the field under test.
    wallpaperx::testing::HeldPort impostor;
    if (!impostor.open(INADDR_LOOPBACK, 0)) {
        notVerified("S7.14 (the fail direction)",
                    "this test could not take a loopback port itself, so the port conflict could not be created");
        return;
    }
    const int kPort = impostor.port();

    SpawnedDaemon daemon({validationEnv}, SpawnedDaemon::Stdout::Captured, {}, 0, configPortArgs(kPort));
    if (!names.waitFor(true)) {
        if (reportedStartupFault(daemon, "S7.14 (the fail direction)")) return;
        const std::string startupOutput = daemon.output();
        if (startupOutput.find(bindFailureMarker(kPort)) != std::string::npos) {
            // A daemon dying of the conflict is a finding, not a skip. UNCONTROLLED: a throwing
            // bind reaches here or the GetStatus check below, by a race with the name request.
            check(false, "the daemon still reaches the bus when its config port is already taken");
        } else {
            notVerified("S7.14 (the fail direction)",
                        "the daemon for this phase never acquired the bus name and never reached its bind attempt");
        }
        return;
    }

    // Before the proxy, so it outlives the dispatch thread that writes to it (a measured TSan race).
    FrameReadyLog signals;
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

    const StatusReply status = getStatus(*proxy);

    // COVERAGE-ASSERT for everything below: the conflict was in force (HeldPort never sets
    // SO_REUSEPORT), read after the GetStatus reply, past the bind attempt.
    // CONTROL: SOCK_CLOEXEC dropped from HeldPort (backend/tests/client/http_probe.h), so the
    //       forked daemon inherits this socket -> "FAIL: the port conflict is real: the daemon
    //       is not the process listening on its port", alone. Observed 2026-09-23. Restored.
    std::string listenerWhy;
    const Listener who = whoListens(kPort, daemon.pid(), listenerWhy);
    if (who == Listener::Unknown) {
        std::printf("daemon_system_test: could not confirm from /proc who holds 127.0.0.1:%d (%s); the conflict is "
                    "still real -- this test holds the socket -- so the checks below stand.\n",
                    kPort, listenerWhy.c_str());
    } else {
        check(who != Listener::Daemon,
              "the port conflict is real: the daemon is not the process listening on its port");
    }

    // GOAL: S7.14's substantive half -- a daemon that lost its config port says so on DBus,
    //       with a cause an operator can act on, while still serving DBus at all.
    // WHY-THIS-SHAPE: the conflict is certain, and "still serving" is the same reply that carries
    //       the report; the stderr line says only that something was printed in passing.
    // CONTROL: in HttpServer's constructor (backend/daemon/http_server.cpp), 2026-09-23, reverted:
    //       (a) `m_manager.recordConfigServer(false, bindError)` deleted -> "FAIL: a daemon that
    //       could not bind its config port reports config_server fail", the names-the-port and
    //       wording lines; (b) bindError reworded -> the wording check below alone; (c) the bind
    //       branch throwing -> "FAIL: a daemon that could not bind its config port still answers
    //       GetStatus" and the two shutdown checks below.
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
        // The port is the one fact an operator needs to find what took it.
        check(reportDetail.find(hostPort) != std::string::npos,
              "the failed bind report names the port it could not take");

        // GOAL: X.3 -- the bind-failure wording two fail-or-skip decisions here match is still the
        //       daemon's, so a reword is a named failure, not two quiet skips.
        // WHY-THIS-SHAPE: against the DBus detail, recorded from the same `bindError` the daemon
        //       prints; the names-the-port check survives any rewording around the host:port.
        // CONTROL: bindError (backend/daemon/http_server.cpp) made "could not bind (host:port)" ->
        //       "FAIL: the bind-failure wording this file matches on in the daemon's stderr is still
        //       the daemon's", the run's only failure. Observed 2026-09-23. Reverted.
        const std::string marker = bindFailureMarker(kPort);
        check(reportDetail.find(marker) != std::string::npos,
              "the bind-failure wording this file matches on in the daemon's stderr is still the daemon's");
        if (reportDetail.find(marker) == std::string::npos) {
            std::fprintf(stderr, "  bindFailureMarker(%d) = \"%s\", but the daemon reports \"%s\"\n", kPort,
                         marker.c_str(), reportDetail.c_str());
        }

        // GOAL: S13.211 -- the failure names the port and guesses no cause. The daemon cannot
        //       know why a bind failed (httplib does not keep errno), so a guess such as "is
        //       another process using it?" misleads whenever the real cause is anything else.
        // WHY-THIS-SHAPE: equality, where the check above is a substring match on purpose, so
        //       that it survives a rewording around the marker. Only equality notices text
        //       added after it.
        // CONTROL: " (is another process using it?)" appended to bindError in HttpServer's
        //       constructor (backend/daemon/http_server.cpp) -> "FAIL: the failed bind report
        //       guesses no cause", alone. Observed 2026-09-23. Reverted.
        check(reportDetail == marker, "the failed bind report guesses no cause");
        if (reportDetail != marker) std::fprintf(stderr, "  the daemon reports \"%s\"\n", reportDetail.c_str());

        if (reportStatus != "fail" || reportDetail.find(hostPort) == std::string::npos) {
            std::fprintf(stderr, "  config_server: %s / %s, while %s is held by %s\n",
                         report ? reportStatus.c_str() : "(absent)",
                         reportDetail.empty() ? "(empty)" : reportDetail.c_str(), hostPort.c_str(),
                         describeListener(who));
        }
    }

    // GOAL: S7.14 -- a daemon that lost its port still renders.
    // WHY-THIS-SHAPE: a render, not another GetStatus; `hostRendered` decides between a failure
    //       and an unexercised goal, so neither a host with no ICD is accused nor a crippled daemon
    //       passed as a skip. SetWallpaper inside the try, so a dead daemon cannot end the phase.
    // CONTROL: (a) `throw InvalidRequest("control")` atop WallpaperManager::requestWallpaper ->
    //       "FAIL: a daemon that could not bind its config port still accepts SetWallpaper"; (b)
    //       FrameReady suppressed for this monitor only in backend/daemon/dbus_service.cpp -> "FAIL:
    //       a daemon that could not bind its config port renders a wallpaper anyway". Reverted.
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

    // The conflict must not change how the daemon goes down. CONTROL: (c) above -> "FAIL: a daemon
    // that could not bind its config port still exits 0 on SIGTERM" and "... still runs its
    // destructor chain". Observed 2026-09-23. Reverted.
    const int exitCode = daemon.shutdown(SIGTERM);
    const std::string output = daemon.output();
    if (sanitizerReportedIn(output)) {
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
// Phase 12 -- Step 8: config persistence, in the shipped binary: a restart is two processes and a
// file, and a config that aborts the daemon would take an in-process test down with it. Each phase
// owns its XDG_CONFIG_HOME; SpawnedDaemon gives every other spawn a fresh one.
// ---------------------------------------------------------------------------- //

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
    std::string output;  // empty: no connector name, the ordinary case
};

// The file as written, parsed here rather than by ConfigStore::load(), which shares the writer's
// field names and would agree with it when both were wrong.
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
        // Optional: a row without it is still compared.
        std::string output;
        if (entry.contains("output") && entry["output"].is_string()) output = entry["output"].get<std::string>();
        entries[entry["monitor_id"].get<int>()] =
            PersistedEntry{entry["type"].get<std::string>(), entry["path"].get<std::string>(), std::move(output)};
    }
    return entries;
}

// Every file under `dir`; false with a reason when the walk failed, which is not "empty".
// increment(ec), so a mode-0000 directory the daemon made cannot throw out of the phase.
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

// The daemon's /health body on `port`; false with a reason when it says it is not serving there.
bool fetchHealth(const StatusReply& status, int port, nlohmann::json& out, std::string& why, const Check& check) {
    const nlohmann::json* report = configServerReport(status);
    const std::string reportStatus = report ? report->value("status", std::string()) : std::string();
    if (reportStatus != "ok") {
        why = "the daemon reports config_server " + (reportStatus.empty() ? std::string("(absent)") : reportStatus) +
              ", so port " + std::to_string(port) + " is not its own to answer on";
        return false;
    }
    httplib::Client cli = wallpaperx::testing::makeClient(port);
    const wallpaperx::testing::HttpReply reply = wallpaperx::testing::get(cli, "/health");
    if (!reply.ok()) {
        // A failure, not a skip (it says it owns the port); `why` left empty marks "already red".
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

void checkPropertiesSavedOnStop(NameWatcher& names, const std::string& validationEnv, const std::string& bluePath,
                                const Check& check) {
    constexpr int32_t kMonitor = 5;
    constexpr const char* kGoals = "S15.651 (a property change just before SIGTERM is saved)";
    ScratchDir configHome("propstop");
    if (!configHome.ok()) {
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
    }
    SpawnedDaemon daemon({validationEnv, configHome.env()});
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(daemon, kGoals)) notVerified(kGoals, "the daemon never acquired the bus name");
        return;
    }
    {
        auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
        setWallpaper(*proxy, "static", bluePath, kMonitor);
        wallpaperx::testing::setWallpaperProperties(*proxy, kMonitor,
                                                    wallpaperx::testing::WireProperties{{"glow", sdbus::Variant(true)}});
    }

    // GOAL: S15.651 -- a SetWallpaperProperties answered just before SIGTERM is in the config file
    //       after the stop, though its save waits out a coalescing delay the stop cuts short.
    // WHY-THIS-SHAPE: SIGTERM follows the reply at once, well inside the 250 ms delay, so the value
    //       is on disk only if the stop path writes what is still waiting.
    // CONTROL: backend/daemon/main.cpp `manager.flushConfig();` and ~ConfigWriter()'s `waitForQueued();`
    //       (backend/daemon/config_writer.cpp) both deleted, with kCoalesce made 10 s (control_batch) ->
    //       red, "FAIL: a property change made just before SIGTERM is in the config file after the stop".
    //       At 250 ms the same deletions stay green -- the stop's teardown outlasts the delay, so the
    //       worker writes anyway -- and either flush alone also stays green: neither is graded alone.
    const int exitCode = daemon.shutdown(SIGTERM);
    awaitNameRelease(names, "the next phase");
    const nlohmann::json root = nlohmann::json::parse(readTextFile(configHome.configFile()), nullptr, false);
    bool saved = false;
    if (root.is_object() && root.contains("monitors") && root["monitors"].is_array()) {
        for (const nlohmann::json& row : root["monitors"]) {
            if (row.is_object() && row.value("monitor_id", -1) == kMonitor && row.contains("properties") &&
                row["properties"].is_object() && row["properties"].value("glow", false)) {
                saved = true;
            }
        }
    }
    check(saved, "a property change made just before SIGTERM is in the config file after the stop");
    if (!saved) {
        std::fprintf(stderr, "  exit %d, config: %s\n", exitCode, readTextFile(configHome.configFile()).substr(0, 400).c_str());
    }
}

void checkConfigRestart(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr int32_t kMonitorRestore = 18;
    constexpr const char* kGoals = "S8.1/S8.3/S8.11 (a wallpaper survives a restart, byte for byte)";

    ScratchDir configHome("restart");
    if (!configHome.ok()) {
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
    }

    // Every byte class sanitizeForLog rewrites (U+202E, DEL, newline): persisted must be the bytes.
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
        // A port of its own, so the /health half runs whoever holds 18462.
        const int firstPort = unusedLoopbackPort();
        if (firstPort < 0) {
            notVerified(kGoals, "no free loopback port could be found for this phase's daemon");
            return;
        }
        SpawnedDaemon first({validationEnv, configHome.env()}, SpawnedDaemon::Stdout::Captured, {}, 0,
                            configPortArgs(firstPort));
        if (!names.waitFor(true)) {
            if (!reportedStartupFault(first, kGoals)) {
                notVerified(kGoals, "the first daemon of this phase never acquired the bus name");
            }
            return;
        }

        FrameReadyLog signals;  // before the proxy, which writes to it
        auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                        sdbus::ObjectPath{DbusService::kObjectPath});
        proxy->uponSignal("FrameReady")
            .onInterface(DbusService::kInterface)
            .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

        // GOAL: S8.11, first half -- HealthStatus::Pending still means "no dependency has
        //       been tried yet", and after Step 8 it is reachable exactly when there is no
        //       config file. False if a daemon that restored nothing already reports a GPU.
        // WHY-THIS-SHAPE: the XDG_CONFIG_HOME was made empty by this phase, and this is asked
        //       before its SetWallpaper; the /health wording only where the daemon owns its port.
        // CONTROL: this block moved below the SetWallpaper and FrameReady wait -> "FAIL: a daemon
        //       with no config file has created no GPU device at startup" and "FAIL: /health reports
        //       the GPU as pending on a daemon that restored nothing". Moved back. (A populated config
        //       here would race the restore's render; that direction is graded below.)
        const StatusReply beforeAnything = getStatusOrExplain(*proxy);
        if (beforeAnything.ok()) {
            check(!beforeAnything.json().value("gpu", nlohmann::json::object()).value("initialized", true),
                  "a daemon with no config file has created no GPU device at startup");
            nlohmann::json health;
            std::string why;
            if (!fetchHealth(beforeAnything, firstPort, health, why, check)) {
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

    // GOAL: S8.3 -- the daemon persists the path it was given, byte for byte, not the sanitized
    //       form /config and GetStatus publish (which would restore a different file).
    // WHY-THIS-SHAPE: the file's bytes and the published sanitized form, as a pair: either alone
    //       passes for one of the two ways to get it wrong.
    // CONTROL: ConfigStore::save() writing `sanitizeForLog(entry.path, 4096, LogFraming::Bare)` ->
    //       "the daemon persists the path it was given, byte for byte", "GetStatus publishes the
    //       restored path in its sanitized form" and "the restored wallpaper renders in the new
    //       process without any client asking" red. Reverted.
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

    // Sampled with no daemon running, for the S8.5 check at the end.
    std::error_code timeEc;
    const std::string bytesBeforeRestart = readTextFile(configHome.configFile());
    const auto mtimeBeforeRestart = std::filesystem::last_write_time(configHome.configFile(), timeEc);

    // Subscribed BEFORE the restart: the name is taken before the restore render is queued, so a
    // later subscription can miss it. A well-known-sender match rule resolves at delivery.
    FrameReadyLog restored;
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&restored](int32_t monitorId, uint64_t generation) { restored.record(monitorId, generation); });

    // The restart; no client call precedes the assertions, which is the property.
    const int secondPort = unusedLoopbackPort();
    if (secondPort < 0) {
        notVerified(kGoals, "no free loopback port could be found for the restarted daemon");
        return;
    }
    SpawnedDaemon second({validationEnv, configHome.env()}, SpawnedDaemon::Stdout::Captured, {}, 0,
                         configPortArgs(secondPort));
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(second, kGoals)) {
            notVerified(kGoals, "the restarted daemon never acquired the bus name");
        }
        return;
    }

    // GOAL: S8.1 -- a wallpaper set before a restart is on the same monitor after it, no client
    //       acting.
    // WHY-THIS-SHAPE: the STATE unconditionally, the FRAME only if the first daemon rendered: a
    //       frame alone would blame a host without Vulkan, a state alone would pass a restored path
    //       that cannot open.
    // CONTROL: `service.restoreSaved();` disabled in backend/daemon/main.cpp -> sixteen FAILs, led by
    //       "the wallpaper set before the restart is on that monitor after it, with no client
    //       asking", via control_run.sh on NVIDIA, 2026-09-24; `service.renderRestored();` deleted ->
    //       "FAIL: the restored wallpaper renders in the new process without any client asking",
    //       the two S8.11 lines and the missing-file line, the state check green (2026-09-21).
    //       Reverted.
    const StatusReply afterRestart = getStatusOrExplain(*proxy);
    const nlohmann::json* entry = wallpaperStatus(afterRestart, std::to_string(kMonitorRestore));
    check(entry != nullptr,
          "the wallpaper set before the restart is on that monitor after it, with no client asking");
    if (entry != nullptr) {
        check(entry->value("type", std::string()) == "static", "the restored monitor carries the type it was set with");
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
        // Generation 1: the counter lives in the process, not the file.
        check(restored.wait(kMonitorRestore, 1),
              "the restored wallpaper renders in the new process without any client asking");

        // GOAL: S8.11, second half -- Pending is NOT what a daemon that restored a
        //       wallpaper reports, because it resolved the GPU at startup on its own.
        // WHY-THIS-SHAPE: read after the FrameReady, never by polling until the flag flips.
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
            if (!fetchHealth(afterRender, secondPort, health, healthWhy, check)) {
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

    // GOAL: S8.5 end to end -- a daemon restoring entries that need no repair does not write the
    //       config file (the only tier left that answers for it: the in-process twin's read beats
    //       the now asynchronous repair write).
    // WHY-THIS-SHAPE: the MTIME, sampled with no daemon running: a second save reproduces the
    //       first daemon's file byte for byte (measured: a byte comparison stayed green).
    // CONTROL: the `if (repaired)` guard on `m_repair = ...` at the end of
    //       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp) deleted ->
    //       "FAIL: a daemon that restores entries needing no repair leaves the config file exactly
    //       as it found it", "the file's mtime moved ...", six unreadable-config lines beside it.
    //       Via control_run.sh, 2026-09-29. Reverted.
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
// Step 13 -- a wallpaper the last run died applying, refused by the shipped binary: only this tier
// shows the binary finds the record ($XDG_STATE_HOME, resolved at startup). The record is planted:
// a real crash inside the window would be a race.
// ---------------------------------------------------------------------------- //
void checkCrashRefusal(NameWatcher& names, const std::string& validationEnv, const std::string& bluePath,
                       const Check& check) {
    constexpr int32_t kMonitorRefused = 18;
    constexpr const char* kGoals =
        "S13.13/S13.16 through the shipped binary (a wallpaper the last run died applying is refused, and an "
        "explicit set is the way out)";

    ScratchDir configHome("crashrefusal");
    if (!configHome.ok()) {
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
    }
    // Both env entries passed below: SpawnedDaemon derives one only for a config home it owns.
    const std::filesystem::path stateDir = configHome.preservedDir();
    const std::filesystem::path record = wallpaperx::crashRecordPath(stateDir, kMonitorRefused);

    const nlohmann::json config{
        {"version", 1},
        {"monitors", nlohmann::json::array({{{"monitor_id", kMonitorRefused},
                                             {"type", "static"},
                                             {"path", bluePath}}})}};
    const nlohmann::json recordLine{
        {"monitor_id", kMonitorRefused}, {"type", "static"}, {"path", bluePath}};
    if (!writeTextFile(configHome.configFile(), config.dump()) ||
        !writeTextFile(record, recordLine.dump() + "\nsignal 11\n")) {
        notVerified(kGoals, "the config file and crash record could not be written into the scratch directory");
        return;
    }

    FrameReadyLog signals;
    auto proxy =
        sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

    SpawnedDaemon daemon({validationEnv, configHome.env(), configHome.stateEnv()});
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(daemon, kGoals)) {
            notVerified(kGoals, "the daemon of the crash-refusal phase never acquired the bus name");
        }
        return;
    }

    // GOAL: S13.13, through the real binary -- a daemon started over a state directory
    //       holding a record that matches its saved wallpaper refuses that wallpaper, says
    //       which signal the last run died of, and answers GetFrame with RenderFailed.
    //       False if the shipped binary looks for its records somewhere else, or does not
    //       look before it applies the config.
    // WHY-THIS-SHAPE: no client call first; the reason from GetStatus, where a frontend reads it.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, restoreSaved()'s
    //       `state.quarantined = crash && ...` replaced with `= false` -> five FAIL lines,
    //       beginning "the shipped daemon refuses a wallpaper its state directory says the
    //       last run died applying" with an empty last_error printed, and "GetFrame on a
    //       refused monitor answers RenderFailed on the shipped daemon -- GetFrame
    //       answered: a frame". Reverted.
    const StatusReply refused = getStatusOrExplain(*proxy);
    const std::string key = std::to_string(kMonitorRefused);
    const nlohmann::json* entry = refused.ok() ? wallpaperStatus(refused, key) : nullptr;
    if (entry == nullptr) {
        check(false, "the shipped daemon restored the monitor whose wallpaper it refused");
    } else {
        const std::string lastError = entry->value("last_error", std::string());
        check(lastError.find("SIGSEGV") != std::string::npos,
              "the shipped daemon refuses a wallpaper its state directory says the last run died applying");
        check(!entry->value("has_frame", true), "the refused monitor has no frame in the shipped daemon");
        if (lastError.find("SIGSEGV") == std::string::npos) {
            std::fprintf(stderr, "  last_error: %s\n", lastError.c_str());
        }
        std::printf("daemon_system_test: the shipped daemon reports monitor %d as -- %s\n", kMonitorRefused,
                    lastError.c_str());
    }

    const std::string frameError =
        wallpaperx::testing::errorNameFrom([&] { (void)wallpaperx::testing::getFrame(*proxy, kMonitorRefused); });
    check(frameError == "io.wallpaperx.Daemon.Error.RenderFailed",
          "GetFrame on a refused monitor answers RenderFailed on the shipped daemon");
    if (frameError != "io.wallpaperx.Daemon.Error.RenderFailed") {
        std::fprintf(stderr, "  GetFrame answered: %s\n", frameError.empty() ? "a frame" : frameError.c_str());
    }

    // GOAL: S13.16, through the real binary -- setting the same wallpaper again is the way
    //       out, and the record does not survive the render that follows. False if a user
    //       whose daemon refused their wallpaper cannot get it back from the app, or if the
    //       retry works and the NEXT start refuses it all over again.
    // WHY-THIS-SHAPE: the record's absence is read off the disk, not off the bus, because
    //       that file is what the next start reads and nothing publishes it.
    // CONTROL: in backend/daemon/crash_record.cpp, the `::unlink(m_file.c_str())` in
    //       ~ApplyingRecord deleted -> "FAIL: the record is gone from the state directory
    //       once the retry has rendered". Reverted.
    check(std::filesystem::exists(record), "the record is still on disk while the wallpaper is refused");
    setWallpaper(*proxy, "static", bluePath, kMonitorRefused);
    if (!signals.wait(kMonitorRefused, 1)) {
        notVerified("S13.16's render half through the shipped binary",
                    "the retry never produced a FrameReady on this host, so what a completed render does to "
                    "the record could not be observed");
        return;
    }
    const StatusReply retried = getStatusOrExplain(*proxy);
    const nlohmann::json* after = retried.ok() ? wallpaperStatus(retried, key) : nullptr;
    if (after == nullptr) {
        check(false, "the shipped daemon still reports the monitor after the retry");
    } else {
        check(after->value("has_frame", false), "an explicit set renders the wallpaper the daemon had refused");
        // Absence of the key: GetStatus omits last_error when there is none.
        check(!after->contains("last_error"), "the refusal is cleared once the wallpaper has been set again");
        if (after->contains("last_error")) {
            std::fprintf(stderr, "  last_error after the retry: %s\n",
                         after->value("last_error", std::string()).c_str());
        }
    }
    check(!std::filesystem::exists(record), "the record is gone from the state directory once the retry has rendered");

    daemon.shutdown(SIGTERM);
    awaitNameRelease(names, "any phase after the crash-refusal one");
}

// ---------------------------------------------------------------------------- //
// Step 10 -- the connector name a plugin identifies itself by, across a restart (S10.5): it must
// come back from the file alone, in a fresh daemon; only the app could re-tell it.
// ---------------------------------------------------------------------------- //
void checkOutputRestart(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    using wallpaperx::testing::boundMonitor;
    using wallpaperx::testing::HttpReply;
    using wallpaperx::testing::setMonitorOutput;

    const int kPort = unusedLoopbackPort();
    constexpr int32_t kMonitorNamedA = 18;
    constexpr int32_t kMonitorNamedB = 19;
    constexpr const char* kGoals = "S10.5 (a connector name a plugin can still identify itself by after a restart)";

    const std::string nameA = "DP-3";
    const std::string nameB = "HDMI-A-1";
    const std::string neverAssigned = "DVI-I-99-never-told";

    ScratchDir configHome("outputrestart");
    if (!configHome.ok()) {
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
    }
    if (kPort < 0) {
        notVerified(kGoals, "no free loopback port could be found for the restarted daemon");
        return;
    }

    // COVERAGE-ASSERT: the two screens need two files, pinned at build time (literals, which no
    // runtime check could ever turn red).
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

    // Name and path both, as the checks after the restart compare both.
    std::string configWhy;
    const auto rowsOnDisk = [&] {
        const std::map<int, PersistedEntry> saved = configEntries(configHome.configFile(), configWhy);
        const auto rowPersisted = [&saved](int32_t monitorId, const std::string& name, const std::string& path) {
            const auto it = saved.find(monitorId);
            return it != saved.end() && it->second.output == name && it->second.path == path;
        };
        return rowPersisted(kMonitorNamedA, nameA, pathA) && rowPersisted(kMonitorNamedB, nameB, pathB);
    };

    // The first daemon's persistence verdict, read only if the rows are missing.
    std::string persistStatus;
    std::string persistDetail;
    {
        SpawnedDaemon first({validationEnv, configHome.env()});
        if (!names.waitFor(true)) {
            if (!reportedStartupFault(first, kGoals)) {
                notVerified(kGoals, "the first daemon of this phase never acquired the bus name");
            }
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
            // Bounded wait for the write: a returned call is no evidence of one, as the write worker
            // may be marked stalled. Reading mid-run is safe: save() renames over the file.
            constexpr auto kPersistBound = std::chrono::seconds(10);
            const auto persistDeadline = Clock::now() + kPersistBound;
            bool landed = false;
            while (true) {
                landed = rowsOnDisk();
                if (landed || Clock::now() >= persistDeadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
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
            // X.3 the other way: a name the daemon chose (its own, or InvalidArgs, which only the
            // callee generates) is the daemon refusing ordinary arguments -- a FAIL; anything else
            // (the transport's, org.sdbuscpp.Error) is unattributable and skips.
            // CONTROL: DbusService::setMonitorOutput (backend/daemon/dbus_service.cpp) throwing
            //       kErrorInvalidArgs -> "FAIL: the first daemon records a connector name for a valid
            //       screen id and name" (daemonErrorName() alone skipped it); throwing
            //       std::runtime_error -> the skip, naming org.sdbuscpp.Error. Both reverted.
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
            notVerified(kGoals,
                        std::string("the first daemon would not record the two screen names this phase restarts "
                                    "with: ") +
                            e.what());
            first.shutdown(SIGTERM);
            awaitNameRelease(names, "the phase after the connector-name restart");
            return;
        }
        // No FrameReady wait: stored identity is graded, not pixels.
        first.shutdown(SIGTERM);
        awaitNameRelease(names, "the restarted daemon of the connector-name phase");
    }

    // GOAL: the precondition every check below rests on -- the first daemon put both rows,
    //       connector name and wallpaper path, in $XDG_CONFIG_HOME/wallpaperx/config.json.
    //       False if SetMonitorOutput returns and the file does not hold them.
    // WHY-THIS-SHAPE: graded once the daemon exited; both fields, as the restore checks read both.
    //       X.3: persistence "ok" with the rows missing is the daemon's defect, a persistence
    //       failure the host's refusal.
    // CONTROL: in ConfigStore::save() (backend/daemon/config_store.cpp), all reverted: (a) the
    //       name never written -> "FAIL: the daemon writes the connector name and the wallpaper it
    //       was given into $XDG_CONFIG_HOME/wallpaperx/config.json", no restore check run; (b) a
    //       wrong path for named rows -> the same failure (a name-only gate blamed the restore);
    //       (c) save() failing -> no FAIL, "NOT VERIFIED -- ... could not persist the two screen
    //       rows (it reports persistence fail: ...)".
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

    // The restart; no client call precedes the reads, which are the point.
    SpawnedDaemon second({validationEnv, configHome.env()}, SpawnedDaemon::Stdout::Captured, {}, 0,
                         configPortArgs(kPort));
    if (!names.waitFor(true)) {
        if (!reportedStartupFault(second, kGoals)) {
            notVerified(kGoals, "the restarted daemon never acquired the bus name");
        }
        // The name's release, not the reap, is what the next phase needs.
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
    // CONTROL: the whole `if (bindOutputLocked(entry.monitorId, entry.output) ==
    //       BindResult::Displaced) ++displacedNames;` statement in WallpaperManager::restoreSaved()
    //       (backend/daemon/wallpaper_manager.cpp) deleted -- the bind is a side effect of
    //       the CONDITION, so removing only the `++displacedNames;` body disables nothing
    //       and leaves every check here green. Observed 2026-09-14. With the statement gone: "FAIL: both
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

    // Whether this daemon is the process on its port decides whether the HTTP half can run at
    // all: losing a race for the port is a skip, a restarted daemon killed by
    // the machine is a skip, a restarted daemon that quit of its own accord is a FAIL, and a
    // drifted log literal is a FAIL. They are told apart in awaitConfigServerBind, shared
    // with the HTTP lifecycle phase so the two cannot drift.
    if (awaitConfigServerBind(second, kPort, "S10.5's GET /config?output= half", "the connector-name phase", check)) {
        // GOAL: S10.5 on the surface the plugin actually uses -- GET /config?output=<name>
        //       answers "known": true with THAT screen's wallpaper, in a daemon that has
        //       never been told the name. False if it answers known=false, which is the
        //       state GOALS.md names, and equally false if it answers another screen's
        //       wallpaper.
        // WHY-THIS-SHAPE: beside the GetStatus check, since only this is what a plugin reads, over a
        //       real socket; the never-assigned name in the same breath, or "known": true passes for
        //       a daemon saying yes to everything; built by pluginPoll(), cache-buster and all.
        // CONTROL: all reverted. The restoreSaved() bind deleted -> "FAIL: a screen named before the
        //       restart is known to the restarted daemon over HTTP (DP-3)" and the own-wallpaper
        //       line, both screens, the never-assigned line green. In the /config handler
        //       (backend/daemon/http_server.cpp): `known` forced true -> "FAIL: a name the restarted
        //       daemon was never given is still reported as not known" alone; the lookup ignoring the
        //       name -> the own-wallpaper and "never answered with the other screen's wallpaper"
        //       lines (HDMI-A-1); the output arm throwing -> all seven red; `out["known"] = nullptr`
        //       -> five red, run finished; parseIdentity() refusing any other parameter -> all seven
        //       red ("answered 400"), where a bare `output=<name>` stayed green.
        // COVERAGE-ASSERT: two static_asserts (the two wallpapers, the table's size).
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
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
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
    //       report as satisfying the check, which would print a data race in the
    //       config-load path as a pass. The corpus leads with the shapes that reach a
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
        // S15.649: fields load() carries as text, which it would dump() recursively if not bounded.
        // 400 000, not 50 000: the daemon's stack takes dump() at 50 000 levels without the guard.
        // CONTROL: backend/daemon/config_store.cpp carriedText()'s nestsWithin test made `false &&`
        //       (control_batch) -> red, both cases' "the daemon starts and serves DBus", "exits through
        //       main() rather than aborting" and "exits 0 on SIGTERM"; at 50 000 levels it stayed green.
        {"a row's properties nested 400 000 deep",
         "{\"version\":1,\"monitors\":[{\"monitor_id\":0,\"type\":\"static\",\"path\":\"/a.png\",\"properties\":" +
             std::string(400000, '[') + std::string(400000, ']') + "}]}"},
        {"a top-level sound nested 400 000 deep",
         "{\"version\":1,\"sound\":" + std::string(400000, '[') + std::string(400000, ']') + ",\"monitors\":[]}"},
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
        const bool sanitizerReported = sanitizerReportedIn(output);
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
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
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
        //       whether the start has a repair to write at all. Observed 2026-08-19 against
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
        throw ScratchDirectoryLost("the scratch HOME this phase inspects could not be created");
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

// Every <config.json>.bad[.N] the daemon may have written, sorted, so a phase can say how many
// copies exist and which ones rather than only whether one does. Both the state directory the
// copies go to and the config directory beside the file are scanned: "no copy was made" has
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
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
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
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
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
    //       single FAIL. Both observed 2026-08-16. Both reverted.
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
    //       refused without the daemon ever blocking on it (opened O_NONBLOCK, inspected through
    //       the descriptor): it reaches the bus, answers, and exits on SIGTERM. Before the guard a
    //       FIFO parked it unreachable and immune to SIGTERM.
    // WHY-THIS-SHAPE: STOPPED, not merely started: the name appears, GetStatus answers, and it
    //       leaves through main() without timedOut() (a SIGKILLed hang still reports a code).
    //       KNOWN GAP: a config on a hung mount blocks inside the read, which nothing here can stage
    //       (it needs a FUSE fixture); measured out of suite, the daemon then exits at the teardown
    //       deadline after SIGTERM, useless but stoppable.
    // CONTROL: both reverted. (a) `O_NONBLOCK` dropped from load()'s open
    //       (backend/daemon/config_store.cpp) -> "FAIL: with a named pipe at config.json, the
    //       daemon says why it refused the file" (the 2 s load deadline still frees it). (b) that
    //       plus `kLoadDeadline` 3600 s (backend/daemon/wallpaper_manager.cpp) -> "FAIL: ... the
    //       daemon reaches the bus" and "FAIL: ... the daemon exits 0" ("exit code 1"): the
    //       teardown deadline in backend/daemon/main.cpp still ends it, so stoppability rests on
    //       three mechanisms and no single mutation reddens the "without being killed" checks.
    // COVERAGE-ASSERT: every case exercised or reported unstageable, against the table's size.
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
            throw ScratchDirectoryLost(std::string("the scratch XDG_CONFIG_HOME for the '") + c.what +
                                       "' case could not be created");
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
        if (daemon.sanitizerReported()) {
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
//       still holding it.
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
// CONTROL: the `if (!claimed)` arm around the startup message in backend/daemon/main.cpp
//       collapsed to the unconditional `printf("... serving %s at %s")` -> "FAIL: a start
//       stopped before the bus name was requested does not announce that name", and nothing
//       else red. Observed 2026-09-24. Reverted.
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
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
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

// For a stopped daemon: why it cannot be shown that its log pump (backend/daemon/log_pump.cpp) holds
// none of what the daemon wrote, or empty when it can. Shown means the pipe on the daemon's stdout is
// empty, and the pump thread -- the one thread polling that pipe's read end -- is blocked in the poll
// that starts a pass, with no write pending, so everything it had read has gone on to the capture.
// Reading another process's syscall and memory needs ptrace access; refused, it is not shown.
std::string logPumpUnproven(pid_t pid) {
    const std::string proc = "/proc/" + std::to_string(pid);
    std::error_code ec;
    const std::string pipe = std::filesystem::read_symlink(proc + "/fd/1", ec).string();
    if (ec || pipe.rfind("pipe:", 0) != 0) return "its stdout is not a pipe /proc names";
    const int reader = ::open((proc + "/fd/1").c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (reader < 0) return "its stdout pipe could not be opened through /proc";
    int queued = -1;
    const int asked = ::ioctl(reader, FIONREAD, &queued);
    ::close(reader);
    if (asked != 0) return "its stdout pipe's fill could not be read";
    if (queued != 0) return "its stdout pipe still held " + std::to_string(queued) + " byte(s)";

    const int mem = ::open((proc + "/mem").c_str(), O_RDONLY | O_CLOEXEC);
    if (mem < 0) return "its memory could not be read";
    int pumps = 0;
    bool writing = false;
    for (const auto& task : std::filesystem::directory_iterator(proc + "/task", ec)) {
        std::ifstream in(task.path() / "syscall");
        std::string number, fdsArg, countArg;
        if (!(in >> number >> fdsArg >> countArg)) continue;  // "running", or blocked outside a call
        const long call = std::strtol(number.c_str(), nullptr, 10);
        bool polling = call == SYS_ppoll;
#ifdef SYS_poll
        polling = polling || call == SYS_poll;
#endif
        if (!polling) continue;
        const unsigned long long fdsAt = std::strtoull(fdsArg.c_str(), nullptr, 16);
        const unsigned long long count = std::strtoull(countArg.c_str(), nullptr, 16);
        if (count == 0 || count > 16) continue;
        std::vector<pollfd> fds(count);
        const auto bytes = static_cast<ssize_t>(count * sizeof(pollfd));
        if (::pread(mem, fds.data(), static_cast<size_t>(bytes), static_cast<off_t>(fdsAt)) != bytes) continue;
        bool readsThePipe = false;
        bool writes = false;
        for (const pollfd& p : fds) {
            std::error_code linkEc;
            const std::string target = std::filesystem::read_symlink(proc + "/fd/" + std::to_string(p.fd), linkEc).string();
            if (!linkEc && target == pipe && (p.events & POLLIN) != 0) readsThePipe = true;
            if ((p.events & POLLOUT) != 0) writes = true;
        }
        if (!readsThePipe) continue;
        ++pumps;
        writing = writes;
    }
    ::close(mem);
    if (ec) return "its threads could not be listed";
    if (pumps != 1) return "no thread was blocked between passes polling its stdout pipe (the pump was mid-pass)";
    if (writing) return "the pump was waiting to write output it held";
    return {};
}

void checkShutdownDuringStartup(NameWatcher& names, const std::string& validationEnv, const Check& check) {
    constexpr const char* kGoal =
        "S8.20 (a shutdown signal that arrives before the event loop starts still stops the daemon)";
    constexpr int kAttempts = 12;
    constexpr std::size_t kConfigBytes = 1000000;
    // Printed and flushed after the startup steps that follow the claim, and before the loop gate.
    const std::string servingLine = std::string("serving ") + DbusService::kBusName + " at";
    // A port of this phase's own, so its daemon never reaches for the default one.
    const int kPort = unusedLoopbackPort();
    if (kPort < 0) {
        notVerified(kGoal, "no free loopback port could be found for this phase's daemon");
        return;
    }

    ScratchDir configHome("earlyterm");
    if (!configHome.ok()) {
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
    }
    std::error_code ec;
    std::filesystem::create_directories(configHome.configFile().parent_path(), ec);
    if (ec || !writeTextFile(configHome.configFile(), rejectedEntriesNearCap(kConfigBytes))) {
        notVerified(kGoal, "the large config that widens this phase's startup window could not be written");
        return;
    }

    // GOAL: S8.20 -- a SIGTERM delivered after the daemon owns the bus name but before it enters its
    //       event loop still terminates it, cleanly and through main(). False if such a
    //       daemon runs on holding io.wallpaperx.Daemon and its config port after being told to
    //       stop -- the user's `systemctl stop` returns, the name stays taken, and the next
    //       start fails with FileExists.
    // WHY-THIS-SHAPE: it asserts the OUTCOME, not the mechanism. What saves such a daemon is
    //       main.cpp's loop gate: the watcher records the stop under the gate's mutex and calls
    //       leaveEventLoop() only for a loop already entered, so main must see the recorded stop
    //       and not enter the loop at all.
    //       The window is entered by force rather than by luck. SIGSTOP freezes the daemon
    //       where it stands; a marker is then written into its capture pipe from this side and
    //       read back (syncCapture), so everything that reached the capture is in hand -- and
    //       logPumpUnproven() must show the daemon's log pump held nothing back, or the attempt
    //       is inconclusive, never in the window. No "serving ... at" line then means it stopped
    //       before the loop gate. Only then is
    //       SIGTERM queued and SIGCONT sent. The port cannot be the witness: it is bound before
    //       the name is claimed. The obvious simpler version -- signal as soon as the name
    //       appears and hope -- cannot tell which side of the loop it landed on, so it would
    //       grade an ordinary shutdown and report it as this one.
    //       The fixture is what makes that freeze land often enough to be usable, and it has
    //       to be a config whose cost falls AFTER the name: the config read and the restore
    //       both run before requestName(), so a merely large valid config lengthens the
    //       wrong side and buys nothing. A file
    //       whose entries are all rejected is copied aside by preserveUnreadableConfig(),
    //       which main() calls immediately after requestName(), so its size lands squarely in
    //       the graded window. Measured on this host, eight runs each: with rejected entries,
    //       caught on attempt 1 of 12 every time; with the same file made valid, attempts 1-3
    //       (mean 1.8) -- and with the phase as it stood before this change, attempts 1-6
    //       (mean 2.75), which is a 7% chance of no attempt landing at all and the goal
    //       silently going unexercised.
    // CONTROL: the gate's check deleted in backend/daemon/main.cpp -- `enter =
    //       g_stopReason.load() == StopReason::None` made `enter = true`, so main enters the loop
    //       whatever the watcher recorded -> "FAIL: a daemon signalled before its event loop began
    //       exits 0" and "FAIL: ... still runs its destructor chain"; "exits without having to be
    //       killed" stays green, since the teardown deadline ends it with exit 1. The same break
    //       reddens the SIGINT and SIGHUP phases' exit and destructor lines, which signal as soon
    //       as the name appears and land in this window by accident, and the config-repair phase's
    //       exit; the SIGTERM phase, which does work first, stays green. Observed 2026-09-24 via
    //       control_run.sh, those seven lines. Reverted. Re-run with the serving-line witness
    //       2026-10-08 via control_run.sh, --phase shutdownDuringStartup: "exits 0" (exit code 1)
    //       and "still runs its destructor chain" red, caught on attempt 1 of 12. Reverted.
    // CONTROL: test-side, by hand 2026-10-08: logPumpUnproven() made to return a reason always ->
    //       every attempt inconclusive, "NOT VERIFIED -- ... (the last: its log writer may still have
    //       held output: ...)", no FAIL line and nothing graded. Restored. A real held state, via
    //       control_run.sh: `::usleep(300000);` before writeAll() in Pump::pass
    //       (backend/daemon/log_pump.cpp) -> NOT VERIFIED, the last of 12 attempts "the pump was
    //       mid-pass" with no serving line captured: one the serving line alone would have graded.
    // COVERAGE-ASSERT: an attempt counts only when the capture was read back up to the freeze,
    //       the log pump held nothing, and no serving line was seen. If no attempt in twelve lands in the window -- this host
    //       starts the daemon faster than the harness can freeze it -- the phase reports NOT
    //       VERIFIED and grades nothing, rather than passing on shutdowns that all happened from
    //       inside the loop.
    int inWindow = 0;
    int attemptsUsed = 0;
    int pumpUnproven = 0;
    std::string lastWitness = "not probed";
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
        SpawnedDaemon daemon({validationEnv, configHome.env(), configHome.stateEnv()}, SpawnedDaemon::Stdout::Captured,
                             {}, 0, configPortArgs(kPort));
        if (!names.waitFor(true)) {
            if (reportedStartupFault(daemon, kGoal)) return;
            notVerified(kGoal, "the daemon for this phase never acquired the bus name");
            return;
        }

        const pid_t pid = daemon.pid();
        const bool frozen = pid > 0 && ::kill(pid, SIGSTOP) == 0;
        const bool readBack =
            frozen && daemon.syncCapture("daemon_system_test: capture marker, attempt " + std::to_string(attempt + 1),
                                         std::chrono::seconds(5));
        const bool serving = readBack && daemon.output().find(servingLine) != std::string::npos;
        const std::string pumpHeld = readBack && !serving ? logPumpUnproven(pid) : std::string();
        if (!pumpHeld.empty()) ++pumpUnproven;
        lastWitness = !frozen            ? "the daemon could not be stopped"
                      : !readBack         ? "its output could not be read back up to the freeze"
                      : serving           ? "it had already printed its serving line"
                      : !pumpHeld.empty() ? "its log writer may still have held output: " + pumpHeld
                                          : "frozen before its serving line";
        const bool beforeLoop = readBack && !serving && pumpHeld.empty();

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
        ++inWindow;

        const bool stopped = daemon.exitedNormally() && !daemon.timedOut();
        check(stopped, "the daemon signalled before its event loop began exits without having to be killed");
        if (!stopped) {
            std::fprintf(stderr, "  exited normally %d, timed out %d, signal %d\n",
                         static_cast<int>(daemon.exitedNormally()), static_cast<int>(daemon.timedOut()),
                         daemon.termSignal());
        }
        if (sanitizerReportedIn(output)) {
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
        std::printf("daemon_system_test: the daemon was frozen inside its startup window, after the name and "
                    "before its serving line, on attempt %d of %d (%d inconclusive for its log writer)\n",
                    attemptsUsed, kAttempts, pumpUnproven);
        std::fflush(stdout);
    } else {
        notVerified(kGoal, std::string("no attempt in ") + std::to_string(kAttempts) +
                               " froze the daemon before its serving line (the last: " + lastWitness +
                               "), so nothing was signalled inside the startup window");
    }
}

void checkFileSizeLimit(NameWatcher& names, const std::string& validationEnv, const std::string& bluePath,
                        const Check& check) {
    constexpr int32_t kMonitorFileLimit = 19;
    constexpr const char* kGoals = "S8.17 (a file-size limit hit during a config write does not kill the daemon)";

    ScratchDir configHome("fsize");
    if (!configHome.ok()) {
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
    }

    // GOAL: S8.17 -- a config write that exceeds RLIMIT_FSIZE does not kill the daemon. The
    //       kernel's default disposition for SIGXFSZ is to terminate, so before this was
    //       ignored the first SetWallpaper under a `LimitFSIZE=` unit killed the daemon
    //       outright, with a core dump, no log line and no clean shutdown -- after the
    //       client's call had already returned success. Every monitor goes blank at once.
    // WHY-THIS-SHAPE: the wallpaper is a `shader`, which requestWallpaper records and
    //       persists but renderPending refuses before touching the GPU -- and which stays
    //       undrawable after Step 13 makes `video` renderable. A `static` would
    //       create a Vulkan device under the same 1-byte file limit, and a driver that
    //       writes a shader cache would then die for a reason that has nothing to do with
    //       the config -- a failure this phase would report against the daemon. The limit is
    //       applied in the forked child rather than to this process, because a limit here
    //       would apply to the harness's own writes too.
    // CONTROL: main()'s `std::signal(SIGXFSZ, SIG_IGN)` in backend/daemon/main.cpp disabled -> four
    //       FAILs, observed 2026-09-26 via control_run.sh on NVIDIA: the
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
        setWallpaper(*proxy, "shader", bluePath, kMonitorFileLimit);
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
    if (daemon.sanitizerReported()) {
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
        throw ScratchDirectoryLost("a scratch XDG_CONFIG_HOME could not be created under the temp directory");
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
    // `shader`, not `static`, throughout: requestWallpaper records and persists it and
    // renderPending refuses it before touching the GPU, so nothing here creates a Vulkan
    // device. What is being measured is a handler's latency, and a driver's first-use cost
    // inside the same call would swamp it -- and would make this phase unanswerable on a host
    // with no working ICD, for a question that has nothing to do with the GPU.
    std::string baselineError;
    try {
        setWallpaper(*proxy, "shader", bluePath, kMonitorStalledFirst);
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
    // CONTROL: `m_store.save(entries);` added at the top of WallpaperManager::queuePersist()
    //       (backend/daemon/wallpaper_manager.cpp), i.e. the write put back on the bus thread, via
    //       control_batch.sh, --phase '^stalledConfigWrite$', 2026-09-29 -> "FAIL: the daemon
    //       answers a SetWallpaper whose config write has stopped coming back", and seven more: the
    //       deadline check here, printing "SetWallpaper took 15035 ms against a 2000 ms deadline",
    //       both GetStatus checks, the second-save check, the stall-report check and both shutdown
    //       checks -- eight FAIL lines in all. Reverted.
    const auto firstStart = Clock::now();
    std::string stalledError;
    try {
        setWallpaper(*proxy, "shader", bluePath, kMonitorStalledSecond, std::chrono::seconds(15));
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
    //       is why the write is off the bus thread: on it, one SetWallpaper onto a dead mount
    //       stops the daemon answering ANY method until its own teardown watchdog kills it, so
    //       every client -- the panel, a status poller, the plugin -- hangs with it.
    // WHY-THIS-SHAPE: asked after the stalled SetWallpaper has returned, not concurrently
    //       with it. The write is provably still outstanding at this point -- nothing in this
    //       phase attaches a reader to the fifo, so it cannot have completed -- while a
    //       racing GetStatus issued from a second thread might legally be answered before the
    //       handler ever reached the write, which is a check graded on the interleaving.
    //       The latency is bounded as well as the answer required: "it replied eventually" is
    //       also true of a daemon that answers status only once the deadline it should not be
    //       paying expires.
    // CONTROL: the same `m_store.save(entries);` at the top of queuePersist(), the same run,
    //       2026-09-29 -> "FAIL: the daemon answers GetStatus while a config write of its own is
    //       stalled", printing "GetStatus failed with org.freedesktop.DBus.Error.Timeout, which is
    //       not one of the daemon's own error names -- so it is the bus, the connection, or the
    //       daemon failing outside its own taxonomy: Connection timed out". Reverted.
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
    //       Observed 2026-08-18. Reverted.
    // CONTROL: the `!worker->stalled &&` short-circuit dropped from ConfigWriter::waitForQueued()
    //       (backend/daemon/config_writer.cpp), via control_batch.sh, --phase
    //       '^stalledConfigWrite$', 2026-09-30 -> "FAIL: a second SetWallpaper during the stall does
    //       not wait for the deadline all over again", printing "the second call took 2000 ms against
    //       a 2000 ms deadline". Also red: "a daemon with a config write it can never finish still exits
    //       0 on SIGTERM" and "it leaves through main() rather than through the teardown watchdog", since
    //       the stop's own wait for the stuck write is then paid again too. Reverted.
    const auto secondStart = Clock::now();
    std::string secondError;
    try {
        setWallpaper(*proxy, "shader", bluePath, kMonitorStalledFirst, std::chrono::seconds(15));
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
    //       baseline save's detail: with the write off the bus thread nothing else is slow, so
    //       a stale ok is the only thing an operator would ever see while nothing has reached
    //       the disk since the mount died.
    // WHY-THIS-SHAPE: read over the bus rather than from /health, so it does not depend on
    //       this daemon owning a port at all. Polled to a deadline with a failure branch, and the verdict is
    //       the LAST observation: the stall becomes reportable at almost the same instant the
    //       first caller is released, so a single reply taken here would land on either side
    //       of that by luck. Both the status and the wording are asserted, because a `fail`
    //       carrying the previous save's detail sends an operator to a disk error that does
    //       not exist.
    // CONTROL: `pastDeadline` in ConfigWriter::stateLocked() (backend/daemon/config_writer.cpp)
    //       forced to false, so a write that has stopped coming back reads as merely outstanding, via
    //       control_batch.sh, --phase '^stalledConfigWrite$', 2026-09-30 -> "FAIL: GetStatus reports the
    //       write that has not come back, rather than the last save that did", printing "persistence
    //       reads pending (<file>: a write has not come back, the choice may not be on disk yet)".
    //       Reverted. An outstanding write reports Pending, so what this block uniquely supplies
    //       is the STATUS being fail and the wording, against a mutation that leaves it pending.
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
    //       to be forced out at the teardown deadline.
    // CONTROL: `.detach()` in ConfigWriter::startLocked() (backend/daemon/config_writer.cpp)
    //       replaced by storing the thread in the writer and joining it in ~ConfigWriter ->
    //       "FAIL: a daemon with a config write it can never finish still exits 0 on SIGTERM"
    //       ("exit code 1") and "FAIL: it leaves through main() rather than through the teardown
    //       watchdog". The worker loops forever by design, so that mutation blocks EVERY
    //       teardown: ten other shutdown checks in this file went red with it, which is the sense
    //       in which abandoning this thread is load-bearing rather than incidental. Note what
    //       stayed green: "still exits on SIGTERM" above, because the forced path is still a
    //       normal exit -- which is why the exit CODE and the watchdog's line are asserted
    //       separately. Observed 2026-08-18. Reverted.
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
    if (sanitizerReportedIn(output)) {
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

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == kReaderMode) return readBlueFrameChild(argc, argv);
    wallpaperx::testing::g_testName = "daemon_system_test";

    // S14.9, graded in name_watcher.h. First, before the scratch-directory probe and the config
    // witness, so a refused run spawns nothing and touches no file; one guard rather than a check
    // per phase, since every phase's name wait is satisfied by a foreign owner.
    // CONTROL: the call below deleted, a scratch daemon owning the name -> the phases ran against
    //       it, printing FAILs about a daemon they never started, until `timeout 45` stopped the
    //       run. Observed 2026-09-24, inside dbus-run-session. Reverted.
    if (const std::optional<int> refused = wallpaperx::testing::refuseUnlessPrivateBus()) return *refused;

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
    //       False if the binary run with `TMPDIR=/nonexistent` dies on SIGABRT,
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
    // CONTROL: run as `dbus-run-session -- env TMPDIR=/nonexistent/nowhere
    //       ./wallpaperx-daemon-system-test` (the bus guard above refuses a bare run). Before
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
    //       elsewhere in this file (the default-port phase skips on it) and it
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
    // False only under WALLPAPERX_TEST_PHASE: the first daemon was not selected, or none of the
    // phases that make it draw was, so the checks below that read its output have nothing to read.
    bool primaryDaemonRan = true;
    bool primaryDaemonDrew = true;
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

        // sdbus-c++ builds every Variant on a connection of its own, opened on first use and kept for
        // the process: opened here, so the baseline holds it and a phase sending a{sv} is not a leak.
        { const sdbus::Variant prime(true); }
        const size_t fdsBefore = openFdCount();

        const std::string validationEnv = std::string("WALLPAPERX_VALIDATION=") + (wantValidation ? "1" : "0");

        // The first daemon: its own startup, second-instance and SIGTERM checks, and the three
        // phases driven against it.
        // GOAL: configServerLostPort run alone still fails a daemon that stops rendering after
        //       losing its port, rather than skipping.
        // WHY-THIS-SHAPE: its "this host renders at all" evidence is the first daemon's frame, so
        //       both are declared its prerequisites; alone it would read no frame anywhere and skip.
        // CONTROL: FrameReady suppressed for its monitor in backend/daemon/dbus_service.cpp, via
        //       control_run.sh --phase '^configServerLostPort$', 2026-09-29 -> "FAIL: a daemon that
        //       could not bind its config port renders a wallpaper anyway", held. With the
        //       prerequisites undeclared -> NOT VERIFIED S7.14, ctest Skipped, "NOT RED". Reverted.
        using wallpaperx::testing::phaseSelected;
        using wallpaperx::testing::phaseSelectedFor;
        const bool primaryDaemon =
            phaseSelectedFor("primaryDaemon", {"firstFrameUnderStatusLoad", "responsivenessUnderLoad",
                                               "concurrentClients", "handoverRelease", "configServerLostPort"});
        const bool firstFramePhase = phaseSelectedFor("firstFrameUnderStatusLoad", {"configServerLostPort"});
        const bool responsivenessPhase = phaseSelected("responsivenessUnderLoad");
        const bool concurrentClientsPhase = phaseSelected("concurrentClients");
        const bool handoverReleasePhase = phaseSelected("handoverRelease");
        primaryDaemonRan = primaryDaemon;
        primaryDaemonDrew = firstFramePhase || responsivenessPhase || concurrentClientsPhase || handoverReleasePhase;

        std::optional<SpawnedDaemon> daemon;
        bool primaryScratchLost = false;
        if (primaryDaemon) {
            try {
                daemon.emplace(std::vector<std::string>{validationEnv});
            } catch (const ScratchDirectoryLost& e) {
                phaseSetupFault("the first daemon's phases", e.what());
                primaryScratchLost = true;
            }
        }
        const bool cameUp = daemon.has_value() && names.waitFor(true);
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
            primaryDaemon && !cameUp &&
            (primaryScratchLost || reportedStartupFault(*daemon, "the bus-name, rendering and SIGTERM checks"));
        if (primaryDaemon && !startupWasHarnessFault) {
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
            const std::string coreLimit = softCoreLimit(daemon->pid());
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
            hostGuarded("the second-instance checks (S6.28)", [&] {
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
                // CONTROL: in backend/daemon/main.cpp, a failed requestName rethrown as
                //       `std::runtime_error("no")`, so a second instance still dies with a
                //       fatal line but for no stated reason -> "FAIL: the second instance is
                //       refused with org.freedesktop.DBus.Error.FileExists", alone: "says why it
                //       exited" stayed green. Observed 2026-09-26 via control_run.sh. Reverted.
                //       (A throw placed before the bus connection cannot serve as this control:
                //       the first instance dies of it too.)
                check(secondOutput.find("org.freedesktop.DBus.Error.FileExists") != std::string::npos,
                      "the second instance is refused with org.freedesktop.DBus.Error.FileExists");
            }
            });

            auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                            sdbus::ObjectPath{DbusService::kObjectPath});
            proxy->uponSignal("FrameReady")
                .onInterface(DbusService::kInterface)
                .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

            // Each guarded on its own, so one that throws ends none of the others or the SIGTERM
            // grading below.
            // GOAL: S15.328's sibling for the phases sharing the first daemon.
            // CONTROL: test-side: checkHandoverRelease made to throw at its start, --phase
            //       '^(handoverRelease|hangupShutdown)$' -> "FAIL: handoverRelease: unexpected exception:
            //       control", with the SIGTERM exit lines and hangupShutdown's phase still run.
            if (firstFramePhase) {
                hostGuarded("firstFrameUnderStatusLoad",
                            [&] { checkFirstFrameUnderStatusLoad(*proxy, signals, bluePath, check); });
            }
            if (responsivenessPhase) {
                hostGuarded("responsivenessUnderLoad",
                            [&] { checkResponsivenessUnderLoad(*proxy, signals, bluePath, check); });
            }
            if (concurrentClientsPhase) {
                hostGuarded("concurrentClients",
                            [&] { checkConcurrentClients(*proxy, signals, bluePath, redPath, corruptPath, check); });
            }
            if (handoverReleasePhase) {
                hostGuarded("handoverRelease", [&] { checkHandoverRelease(*proxy, signals, bluePath, check); });
            }
        }

        // SIGTERM, which is what systemd sends (Step 14) and what a `kill` defaults to.
        // Safe to send now and not earlier: the daemon blocks both shutdown signals
        // before it creates any thread, and it does that before requesting the bus name
        // -- so a name that is on the bus is a mask that is already installed. Sent
        // before that point, the default disposition would kill it outright.
        const int exitCode = daemon ? daemon->shutdown(SIGTERM) : 0;
        if (daemon) {
            daemonOutput = daemon->output();
            daemonOutputRead = true;
        }
        if (primaryDaemon && !cameUp) {
            // The daemon never reached the bus, which is already reported above. Signalling
            // it anyway is right -- it must not outlive the test -- but grading its exit
            // would add four more FAIL lines blaming main.cpp's shutdown path for a fault
            // that happened before the shutdown path was ever reached.
            std::printf("daemon_system_test: shutdown checks skipped -- %s, so its exit says nothing about the "
                        "shutdown path.\n",
                        startupWasHarnessFault ? "that daemon was never started (the harness fault above)"
                                               : "the daemon never acquired the bus name");
        } else if (primaryDaemon) {
            // A sanitizer that reported anything replaces the process's exit code with its
            // own, so under a sanitized build "did the daemon exit 0" stops being a question
            // about the shutdown path. Reported as what it actually is instead: a
            // non-zero exit that reads "the shutdown path is broken" would send the next
            // reader to main.cpp, which is not where the problem is.
            const bool sanitizerReported = sanitizerReportedIn(daemonOutput);
            // GOAL: S14.2 -- a signal stop exits 0 while a lost device exits 75, so the service
            //       manager's Restart=on-failure does not bring back a daemon a user stopped.
            //       And the signal the daemon wakes its own signal thread with is not a stop
            //       signal, so the wake never lands in the second-signal handler a stop arms.
            // CONTROL: in backend/daemon/main.cpp, via control_run.sh on NVIDIA, reverted. main's
            //       final status set on `!= StopReason::None` instead of `== StopReason::DeviceLost`
            //       -> this line, the SIGINT and SIGHUP exit-0 lines and every other phase's exit-0
            //       and destructor-chain lines red (30 FAILs, observed 2026-09-26, the first 20 of
            //       them shown by the tool). Observed 2026-09-24:
            //       ~ShutdownWatch's wake sent as SIGTERM instead of kWakeSignal -> 18 FAILs of
            //       the same kind, this line first: the wake lands in the handler the stop armed and
            //       exits 1. kWakeSignal itself made SIGTERM -> the user's SIGTERM is then a wake
            //       with no reason recorded and is ignored, so every SIGTERM stop hangs: 28 FAILs,
            //       this line and the other exit-0 and destructor-chain lines, before ctest's 600 s
            //       timeout ended the run.
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
            //       main.cpp's three-second budget, so it leaves through main() rather than
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
            //       out", with every other phase's exit-0 check beside them. A zero deadline is also
            //       alarm(0), which arms nothing, so the wedged connection-loss phase's "still ends"
            //       line goes red with them (39 FAILs, observed 2026-09-26 via control_run.sh, the
            //       first 20 shown by the tool). Reverted.
            const bool leftThroughMain = daemonOutput.find("shutdown blocked") == std::string::npos;
            check(leftThroughMain, "the daemon leaves through main() rather than through the teardown watchdog");
            check(names.waitFor(false), "the bus name is released once the daemon exits");
        }

        // The other signal in the same sigset. Cheap -- it never renders -- and without
        // it a typo that dropped SIGINT from the mask would be invisible, since SIGTERM
        // alone exercises one sigaddset call out of two.
        if (phaseSelected("sigintShutdown")) hostGuarded("the SIGINT shutdown checks", [&] {
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
                if (sanitizerReportedIn(interruptOutput)) {
                    check(false, "the SIGINT daemon's sanitizer reported no warnings (its exit code is the "
                                 "sanitizer's here, so the SIGINT exit-code check cannot run)");
                } else {
                    check(interruptExit == 0, "the daemon exits 0 on SIGINT");
                }
                check(interruptOutput.find("stopped cleanly") != std::string::npos,
                      "the daemon shuts down cleanly on SIGINT as well as SIGTERM");
                check(names.waitFor(false), "the bus name is released after the SIGINT shutdown");
            }
        });

        // Each phase below spawns daemons of its own and shuts them down, so they run after the
        // first daemon has released the name, and before the descriptor accounting at the end: their
        // proxies and httplib clients close their sockets at scope exit. The wedging phases come
        // last, on buses of their own: the daemon each leaves behind cannot be stopped cleanly.
        const auto phase = [](const char* name, auto&& body) {
            if (wallpaperx::testing::phaseSelected(name)) hostGuarded(name, body);
        };
        const std::string wedgeClip = std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_bands.mp4";
        phase("hangupShutdown", [&] { checkHangupShutdown(names, check); });
        phase("outputVisibleLog", [&] { checkOutputVisibleLog(names, validationEnv, check); });
        phase("frameAllocationFailure", [&] { checkFrameAllocationFailure(names, validationEnv, bluePath, check); });
        phase("closedStdout", [&] { checkClosedStdout(names, bluePath, validationEnv, wantValidation, check); });
        phase("nestedBusPrivate", [&] { checkNestedBusPrivate(check); });
        phase("busLoss", [&] { checkBusLoss(check); });
        phase("commandLine", [&] { checkCommandLine(names, validationEnv, check); });
        phase("usageWithOutputReaderGone", [&] { checkUsageWithOutputReaderGone(validationEnv, check); });
        phase("defaultConfigPort", [&] { checkDefaultConfigPort(names, validationEnv, check); });
        phase("chosenConfigPort", [&] { checkChosenConfigPort(names, validationEnv, check); });
        phase("httpLifecycle", [&] { checkHttpLifecycle(names, bluePath, validationEnv, check); });
        phase("configChangedAtStartup", [&] { checkConfigChangedAtStartup(names, validationEnv, check); });
        phase("configServerOwnsPort", [&] { checkConfigServerOwnsPort(names, validationEnv, check); });
        // The run's whole FrameReady history so far is the proof this host renders, which lets this
        // phase grade a missing frame rather than skip on it.
        phase("configServerLostPort", [&] {
            checkConfigServerLostPort(names, bluePath, validationEnv, signals.total() > 0, check);
        });
        phase("configRestart", [&] { checkConfigRestart(names, validationEnv, check); });
        phase("propertiesSavedOnStop", [&] { checkPropertiesSavedOnStop(names, validationEnv, bluePath, check); });
        phase("crashRefusal", [&] { checkCrashRefusal(names, validationEnv, bluePath, check); });
        phase("outputRestart", [&] { checkOutputRestart(names, validationEnv, check); });
        phase("startsOnAnyConfig", [&] { checkStartsOnAnyConfig(names, validationEnv, check); });
        phase("stoppableOnAnyConfigFile", [&] { checkStoppableOnAnyConfigFile(names, validationEnv, check); });
        phase("configPreservationOwnership", [&] { checkConfigPreservationOwnership(names, validationEnv, check); });
        phase("repairWrittenAfterPreservation", [&] { checkRepairWrittenAfterPreservation(names, validationEnv, check); });
        phase("shutdownDuringStartup", [&] { checkShutdownDuringStartup(names, validationEnv, check); });
        phase("stopBeforeClaiming", [&] { checkStopBeforeClaiming(names, validationEnv, check); });
        phase("unreadableRestore", [&] { checkUnreadableRestore(names, validationEnv, bluePath, check); });
        phase("configLocation", [&] { checkConfigLocation(names, validationEnv, bluePath, check); });
        phase("fileSizeLimit", [&] { checkFileSizeLimit(names, validationEnv, bluePath, check); });
        phase("stalledConfigWrite", [&] { checkStalledConfigWrite(names, validationEnv, bluePath, check); });
        phase("stuckTeardownIsBounded", [&] { checkStuckTeardownIsBounded(validationEnv, wedgeClip, check); });
        phase("secondSignalEndsAStuckStop", [&] { checkSecondSignalEndsAStuckStop(validationEnv, wedgeClip, check); });
        phase("watchdogEndsAStuckStopWritingNothing",
              [&] { checkWatchdogEndsAStuckStopWritingNothing(validationEnv, check); });
        phase("alarmDeliversQueuedLines", [&] { checkAlarmDeliversQueuedLines(validationEnv, wedgeClip, check); });
        phase("lossDuringFailedStart", [&] { checkLossDuringFailedStart(validationEnv, bluePath, check); });
        phase("ffmpegLogInstalled", [&] { checkFfmpegLogInstalled(validationEnv, wedgeClip, check); });
        phase("restoredVideoIsPostedAfterTheRecordOpens", [&] {
            checkTheRestoredVideoIsPostedAfterTheRecordOpens(validationEnv, wedgeClip, check);
        });

        // Every Frame's UnixFd closes at scope exit, and the daemon is gone, so this
        // process must be back where it started. A descriptor leaked per GetFrame is
        // an eventual EMFILE in a polling consumer, not a one-off.
        const size_t fdsAfter = openFdCount();
        check(fdsAfter <= fdsBefore, "the client leaks no descriptors across the whole run");
        if (fdsAfter > fdsBefore) {
            std::fprintf(stderr, "  %zu open fds before, %zu after\n", fdsBefore, fdsAfter);        }
    } catch (const ScratchDirectoryLost& e) {
        phaseSetupFault("the phase running when the host lost its scratch directory", e.what());
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
    // CONTROL: the GPU printf deleted from WallpaperManager::ensureGpu (backend/daemon/wallpaper_manager.cpp)
    //       via control_batch.sh, --phase '^firstFrameUnderStatusLoad$' (primaryDaemon runs as its
    //       prerequisite), validation wanted, NVIDIA, 2026-09-29 -> "FAIL: the daemon reports its GPU and
    //       descriptor tier on one line once the device exists", the only FAIL line; the validation check
    //       below printed its not-graded line instead. Reverted.
    // Read by the validation check below, so the one fact is graded once.
    bool gpuTierGraded = false;
    bool gpuTierReported = false;
    if (!primaryDaemonRan || !primaryDaemonDrew) {
        std::printf("daemon_system_test: GPU/tier report check not graded -- no phase selected by "
                    "WALLPAPERX_TEST_PHASE made the first daemon draw.\n");
    } else if (!daemonOutputRead || !daemonCameUp) {
        std::printf("daemon_system_test: GPU/tier report check skipped -- %s, and the failure that caused "
                    "that is reported above.\n",
                    daemonCameUp ? "the daemon's output was never collected" : "the daemon never started");
    } else {
        // The GPU name itself stays out of the assertion because it is host-specific.
        const bool reported = reportsGpuAndTier(daemonOutput);
        gpuTierGraded = true;
        gpuTierReported = reported;
        check(reported, "the daemon reports its GPU and descriptor tier on one line once the device exists");
        if (!reported) {
            std::fprintf(stderr, "  no single 'GPU: ... | Descriptor tier: ...' line in the daemon's output\n");
        }
    }

    // The daemon's own Vulkan usage, judged from its output. Only meaningful when the
    // layer is installed; the warning at the top of main says so when it is not.
    // GOAL: the first daemon's "no validation warnings" is graded only on a daemon that did Vulkan
    //       work. False if a run that started it and never drew -- WALLPAPERX_TEST_PHASE selecting
    //       primaryDaemon without a drawing phase -- reports the GPU-side oracle clean.
    // WHY-THIS-SHAPE: gated on primaryDaemonDrew like the GPU/tier check above, and, once graded,
    //       an empty warning list must also find the device line: "serving" proves only that the
    //       daemon started, and a gate removed by a later edit would otherwise turn back into a silent
    //       pass. Warnings found are listed first, whatever else the capture lacks: they are evidence
    //       on their own, and a missing log line must not hide them behind a device-creation message.
    //       When the GPU/tier check above already failed on the same line, this prints not-graded
    //       rather than a second FAIL for one cause.
    // CONTROL: `|| !primaryDaemonDrew` deleted from this gate, --phase '^primaryDaemon$' (a daemon
    //       that never draws) via control_batch.sh, NVIDIA, 2026-09-29 -> "FAIL: the first daemon
    //       created its Vulkan device, so its silence about validation means something", beside the
    //       GPU/tier check's not-graded line. Reverted.
    // CONTROL: in WallpaperManager::ensureGpu (backend/daemon/wallpaper_manager.cpp) the GPU printf
    //       replaced by one printing "[vulkan] CONTROL validation warning", via control_batch.sh,
    //       --phase '^firstFrameUnderStatusLoad$', NVIDIA, 2026-09-29 -> "FAIL: the daemon produced no
    //       Vulkan validation warnings across render, export and teardown" with that line listed,
    //       beside the GPU/tier FAIL above: the warning is reported although the device line is
    //       missing. Reverted.
    if (wantValidation && (!primaryDaemonRan || !primaryDaemonDrew)) {
        std::printf("daemon_system_test: the first daemon's validation check not graded -- no phase selected by "
                    "WALLPAPERX_TEST_PHASE made the first daemon draw.\n");
    } else if (wantValidation && (!daemonOutputRead || !daemonCameUp)) {
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
        const std::vector<std::string> warnings = validationLines(daemonOutput);
        if (!warnings.empty()) {
            check(false, "the daemon produced no Vulkan validation warnings across render, export and teardown");
            for (const std::string& line : warnings) {
                std::fprintf(stderr, "  %s\n", line.c_str());
            }
        } else if (!daemonSpoke) {
            // One failure, not two. The daemon never got far enough to use Vulkan, which
            // is already reported above; grading its silence as well would be a second
            // FAIL line for a cause the reader has already been told about.
            check(false, "the daemon's output was captured, so its silence about validation means something");
        } else if (!(gpuTierGraded ? gpuTierReported : reportsGpuAndTier(daemonOutput))) {
            if (gpuTierGraded) {
                std::printf("daemon_system_test: the first daemon's validation check not graded -- it never printed "
                            "its device line, which the GPU/tier check above reports.\n");
            } else {
                check(false,
                      "the first daemon created its Vulkan device, so its silence about validation means something");
            }
        } else {
            // GOAL: S15.323 -- zero [vulkan] lines count as clean only from a daemon that says its validation
            //       layer is active. False if a daemon asked for validation whose layer did not load (this
            //       process found it loadable, so the child should have too) is graded clean.
            // WHY-THIS-SHAPE: the daemon's own line, which it prints only when its debug messenger exists,
            //       rather than this process's layer query alone: the child's environment can lose the
            //       layer when this process still sees it.
            // CONTROL: in WallpaperManager::ensureGpu (backend/daemon/wallpaper_manager.cpp) the condition
            //       made `!m_device->validationLayerActive()`, via control_batch.sh, --phase
            //       '^firstFrameUnderStatusLoad$', 2026-10-05 -> "FAIL: the first daemon says its Vulkan
            //       validation layer is active, so its silence about validation is graded". Test-side, the
            //       first daemon spawned with VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation -> the same
            //       line. Both reverted.
            const bool active = hasLine(daemonOutput, "wallpaperx-daemon: Vulkan validation layer active");
            check(active, "the first daemon says its Vulkan validation layer is active, so its silence about "
                          "validation is graded");
            if (!active) {
                std::fprintf(stderr, "  %s\n",
                             daemonOutput.find("the layer is not active") != std::string::npos
                                 ? "it said validation was asked for but the layer is not active"
                                 : "it said nothing about its validation layer");
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
            // This suite already tolerates the developer's own daemon running (the
            // default-port phase skips on it), and that daemon rewrites the config on
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
