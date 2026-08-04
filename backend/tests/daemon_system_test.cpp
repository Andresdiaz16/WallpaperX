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
#include <thread>
#include <vector>

#include <fcntl.h>
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
constexpr int32_t kStormMonitors = 6;
constexpr int32_t kMonitorBarrierPre = 30;
constexpr int32_t kMonitorBarrierPost = 31;

constexpr int kStormThreads = 4;
constexpr int kStormIterations = 12;

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
    // `extraEnv` entries are "KEY=VALUE"; each replaces any inherited entry for that key.
    explicit SpawnedDaemon(const std::vector<std::string>& extraEnv = {}) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) throw std::runtime_error("pipe2 failed");

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
            if (dup2(fds[1], STDOUT_FILENO) == -1 || dup2(fds[1], STDERR_FILENO) == -1) {
                _exit(126);
            }
            execve(binary.c_str(), argv, envp.data());
            _exit(127);  // only reached if execve failed
        }

        close(fds[1]);
        m_readFd = fds[0];

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
    std::string output() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_output;
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
            if (r < 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!exited) {
            kill(m_pid, SIGKILL);
            waitpid(m_pid, &status, 0);
            m_exitCode = -1;
            m_timedOut = true;
        } else {
            m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
    int m_exitCode = -1;
    bool m_timedOut = false;
    std::thread m_reader;
    mutable std::mutex m_mutex;
    std::string m_output;
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
                    m_owned = !newOwner.empty();
                }
                (void)oldOwner;
                m_changed.notify_all();
            });
    }

    // True if the daemon currently holds the name. Asked of the bus rather than the
    // cached signal state, so it is also correct before any signal has arrived.
    //
    // The answer is written back into the cached flag, and that is load-bearing rather
    // than an optimisation. waitFor() returns early on this call without waiting for the
    // matching NameOwnerChanged, so without the write-back the flag keeps the *previous*
    // daemon's value: waitFor(false) after a shutdown would leave it stale at true, and
    // the next waitFor(true) would be satisfied instantly by that stale value before the
    // new daemon had acquired anything. The test would then signal a daemon that has not
    // yet installed its signal mask, and the default disposition would kill it -- a
    // spurious failure of the shutdown checks, on a correct daemon.
    bool ownedNow() {
        bool has = false;
        m_proxy->callMethod("NameHasOwner")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string(DbusService::kBusName))
            .storeResultsTo(has);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_owned = has;
        }
        return has;
    }

    // Waits until the name is (or is not) owned. Returns false on timeout.
    bool waitFor(bool owned) {
        if (ownedNow() == owned) return true;
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, kProcessTimeout, [&] { return m_owned == owned; });
    }

private:
    std::unique_ptr<sdbus::IProxy> m_proxy;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_owned = false;
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
                    std::strerror(errno));
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

    bool allArrived = true;
    for (int i = 0; i < loadCount; ++i) {
        if (!signals.wait(kMonitorLoadBase + i, 1)) allArrived = false;
    }
    const auto drain = Clock::now() - loadStart;
    check(allArrived, "every queued render completes and announces itself");

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
    for (int i = 0; i < loadCount; ++i) {
        if (!signals.wait(kMonitorLoadBase + i, 2)) allArrived = false;
    }
    const auto drain2 = Clock::now() - secondStart;
    check(allArrived, "the second measuring burst also completes");

    // Ratios rather than raw intervals, since the two rounds may drain at different
    // speeds; the smaller ratio is the round less disturbed by the host.
    const double ratio1 = millis(drain) > 0 ? millis(issueTime) / millis(drain) : 1.0;
    const double ratio2 = millis(drain2) > 0 ? millis(issueTime2) / millis(drain2) : 1.0;
    const bool useSecond = ratio2 < ratio1;
    const auto bestDrain = useSecond ? drain2 : drain;
    std::printf("daemon_system_test: issue/drain ratio %.4f then %.4f (threshold 0.25, grading the %s round)\n",
                ratio1, ratio2, useSecond ? "second" : "first");

    // The control on the check below: it compares an interval against the render queue's
    // own duration, so if the queue costs no measurable time then a daemon that rendered
    // inline would look identical to one that posts, and passing would prove nothing.
    // Reported as inconclusive rather than counted -- a host fast enough to erase the
    // difference must not be allowed to manufacture evidence.
    if (bestDrain < kMinConclusiveDrain) {
        std::printf("daemon_system_test: responsiveness check INCONCLUSIVE on this host -- the queue drained in "
                    "%.1f ms, under the %lld ms needed for a blocking handler to be distinguishable.\n",
                    millis(bestDrain), static_cast<long long>(kMinConclusiveDrain.count()));
        return;
    }

    check(std::min(ratio1, ratio2) * 4 < 1.0,
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
    uint64_t& highest = marks[monitorId];
    if (generation < highest) {
        ++result.generationRegressions;
        return;
    }
    highest = generation;
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
    std::printf("daemon_system_test: %d concurrent calls across %d clients completed\n", result.calls.load(),
                kStormThreads);

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

        SpawnedDaemon daemon({std::string("WALLPAPERX_VALIDATION=") + (wantValidation ? "1" : "0")});
        const bool cameUp = names.waitFor(true);
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
            check(!second.timedOut(), "the second daemon instance exits on its own rather than having to be killed");
            check(!second.timedOut() && secondExit != 0,
                  "a second daemon instance refuses to start while the first owns the name");
            check(names.ownedNow(), "the first daemon still owns the name after the second one exits");
            const std::string secondOutput = second.output();
            check(secondOutput.find("fatal:") != std::string::npos,
                  "the second instance says why it exited rather than dying silently");

            auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                            sdbus::ObjectPath{DbusService::kObjectPath});
            proxy->uponSignal("FrameReady")
                .onInterface(DbusService::kInterface)
                .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

            checkFirstFrame(*proxy, signals, bluePath, check);
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

    // The daemon's own Vulkan usage, judged from its output. Only meaningful when the
    // layer is installed; the warning at the top of main says so when it is not.
    if (wantValidation) {
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
