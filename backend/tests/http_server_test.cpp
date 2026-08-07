// Step 7's HTTP config server (backend/daemon/http_server.cpp), in process, against a
// real WallpaperManager and a real DbusService on a private session bus.
//
// Why both channels are here rather than in a pure HTTP test: half of what Step 7 must
// establish is that /config and GetStatus *agree* (S7.3, S7.4). Agreement is not a
// property of either channel alone, so the test that checks it has to hold both, which
// means a bus connection and a DbusService -- GetStatus is private to DbusService and
// reachable only over the wire.
//
// Port discipline, and it is not incidental. Nothing here uses HttpServer::kDefaultPort.
// A developer running ctest while their own wallpaperx-daemon is up would otherwise get
// failures that are a port conflict wearing a defect's clothes, and two ctest entries
// running concurrently would collide with each other. Every server this file starts is
// bound to a port the OS just told us was free (see freePort()), and HttpServer::port()
// reports it back -- the constructor's port parameter is the whole reason this component
// is testable without touching production.
//
// Rules inherited from the other two backend tiers:
//
//   - Nothing waits on a sleep for a render. SetWallpaper returns before the render
//     happens, so "the frame exists" is knowable only from FrameReady.
//   - A phase that cannot be set up on this host prints why and is reported to ctest as
//     a SKIP (exit 77), never as a pass and never as a failure of the daemon.
//   - Every HttpServer created here serves at least one request before it is destroyed.
//     That is deliberate: httplib's Server::stop() is a no-op until the serving thread
//     has set is_running_, so a construct-then-immediately-destroy sequence can hang in
//     the join. See the CONCERN note above runShutdownChecks.
//
// No Vulkan validation oracle is claimed by this file. It renders only because two goals
// (S7.10e, S7.12) need a device to exist, and the daemon ships with validation off;
// dbus_service_test and daemon_system_test own that oracle.

#include "daemon/dbus_service.h"
#include "daemon/http_server.h"
#include "daemon/wallpaper_manager.h"

// Driven directly, not through the manager, by the submit-split measurement that S7.10e's
// aim depends on -- see measureSubmitSplit().
#include "renderers/static_renderer.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"

#include "tests/client/daemon_client.h"
#include "tests/client/http_probe.h"

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <vulkan/vulkan.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using wallpaperx::DbusService;
using wallpaperx::Device;
using wallpaperx::HttpServer;
using wallpaperx::OffscreenTarget;
using wallpaperx::OffscreenTargetManager;
using wallpaperx::StaticRenderer;
using wallpaperx::WallpaperManager;
using wallpaperx::testing::Check;
using wallpaperx::testing::ConnectResult;
using wallpaperx::testing::describe;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::get;
using wallpaperx::testing::getStatus;
using wallpaperx::testing::HeldPort;
using wallpaperx::testing::HttpReply;
using wallpaperx::testing::makeClient;
using wallpaperx::testing::Poller;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::tryConnect;
using wallpaperx::testing::wallpaperStatus;

// ---------------------------------------------------------------------------- //
// Failing one vkQueueSubmit, from the test binary
// ---------------------------------------------------------------------------- //
//
// S7.10e is about a GPU that stops working *after* it has worked, and no input to
// SetWallpaper reaches a failing submit -- an undecodable image fails in stb, long
// before the queue. The lever is the driver call itself: Device holds no function
// pointers, so vkQueueSubmit is an undefined symbol in this executable resolved through
// the Vulkan loader, and a definition here is what device.cpp's call binds to.
//
// This is NOT the test-only hook in a production file that X.6 forbids. Nothing is added
// to, or compiled differently in, renderer/ or backend/daemon/ -- the fault lives
// entirely in the test binary, which is the same technique renderer/tests/
// image_bounds_probe.cpp uses on malloc and fopen.
//
// The plan is thread_local rather than global, so no other thread in this process can
// see it: the manager's HTTP serving thread, the DBus dispatch thread and any render
// worker submit nothing that this counter can observe, which is what keeps the counts
// below from being an assertion on the scheduler (X.2).
namespace {

struct SubmitFaultPlan {
    int observed = 0;  // submits made on the arming thread while armed
    int failAt = 0;    // 1-based index of the submit to fail; 0 counts and fails nothing
    int injected = 0;  // faults actually returned, so a bypassed interposer is visible
};

thread_local SubmitFaultPlan* t_submitFault = nullptr;

// Arms the interposer for the current thread only, and disarms on every exit path
// including an exception (X.4) -- a plan left armed would inject into an unrelated phase.
class SubmitFault {
public:
    explicit SubmitFault(int failAt) : m_previous(t_submitFault) {
        m_plan.failAt = failAt;
        t_submitFault = &m_plan;
    }
    ~SubmitFault() { disarm(); }
    SubmitFault(const SubmitFault&) = delete;
    SubmitFault& operator=(const SubmitFault&) = delete;

    // Restores whatever was armed before rather than clearing, so a nested fault cannot
    // silently disarm an outer one still in scope. Idempotent: after an explicit disarm()
    // the destructor's own call is a no-op.
    void disarm() {
        if (t_submitFault == &m_plan) t_submitFault = m_previous;
    }

    int observed() const { return m_plan.observed; }
    int injected() const { return m_plan.injected; }

private:
    SubmitFaultPlan m_plan;
    SubmitFaultPlan* m_previous;
};

}  // namespace

// VK_ERROR_DEVICE_LOST because that is what a real stalled submit ends in (S7.16), and
// production only tests the code against VK_SUCCESS. Nothing is actually lost: the call
// never reaches the driver, so immediateSubmit's cleanup destroys a fence and command
// buffer that were never submitted -- legal, and what makes the recovery half testable.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                                                        const VkSubmitInfo* pSubmits, VkFence fence) {
    // RTLD_NEXT skips this executable and lands on the loader's own entry point. Idempotent,
    // so the relaxed racing store can only ever write the same value twice.
    static std::atomic<void*> realSubmit{nullptr};
    void* real = realSubmit.load(std::memory_order_relaxed);
    if (real == nullptr) {
        real = ::dlsym(RTLD_NEXT, "vkQueueSubmit");
        realSubmit.store(real, std::memory_order_relaxed);
    }

    if (SubmitFaultPlan* plan = t_submitFault; plan != nullptr) {
        ++plan->observed;
        if (plan->failAt != 0 && plan->observed == plan->failAt) {
            ++plan->injected;
            return VK_ERROR_DEVICE_LOST;
        }
    }

    auto* realFn = reinterpret_cast<PFN_vkQueueSubmit>(real);
    if (realFn == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    return realFn(queue, submitCount, pSubmits, fence);
}

namespace {

using Clock = std::chrono::steady_clock;

// Small on purpose: this file renders only to make a Device exist. The one place a
// render needs to be *slow* (runGpuLockChecks) gets there through a large source image,
// not through a large target.
constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 48;

// Ids are partitioned per phase and never reused: an assertion about "a monitor nothing
// has been set on" changes meaning the moment another phase touches that id.
constexpr int kMonitorParam = 0;      // recorded, never rendered -- the not-clamped check
constexpr int kMonitorTorn = 1;       // flipped by the writer thread in runTornReadChecks
constexpr int kMonitorAgree = 2;      // the hostile path, rendered
constexpr int kMonitorGpuLock = 3;    // the slow render in runGpuLockChecks
constexpr int kMonitorNeverSet = 31;  // also the upper in-range boundary for S7.5

// Goals this run could not exercise, as opposed to goals it exercised and that held.
// Reported to ctest as exit 77 (SKIP_RETURN_CODE in backend/CMakeLists.txt) so an
// unexercised goal can never read as a pass. Not a check() failure: X.3 forbids
// reporting a fault of the host or the harness as a fault of the daemon.
int g_notVerified = 0;
constexpr int kNotVerifiedExitCode = 77;

void notVerified(const char* goals, const std::string& reason) {
    std::printf("http_server_test: NOT VERIFIED -- %s could not be exercised: %s. This run establishes "
                "nothing about them, and is reported to ctest as a skip rather than as a pass.\n",
                goals, reason.c_str());
    std::fflush(stdout);
    ++g_notVerified;
}

double millis(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(d).count();
}

std::string testAssetDir() { return std::string(WALLPAPERX_TEST_IMAGE_DIR); }

// ---------------------------------------------------------------------------- //
// Sockets: free ports, held ports, and a connect that reports *why* it failed.
// ---------------------------------------------------------------------------- //

// A loopback port nothing is listening on right now: bind :0, read the number back,
// close. Racy in principle -- another process could take it between the close and
// HttpServer's bind -- which is why every caller goes through startServer() below and
// retries rather than treating one bind failure as a defect.
int freePort() {
    HeldPort probe;
    if (!probe.open(INADDR_LOOPBACK)) return -1;
    const int port = probe.port();
    probe.release();
    return port;
}

// One attempt to bind and listen on 127.0.0.1:`port`, keeping *why* it failed. A bool
// would grade "the port is held by somebody else" the same as "this process could not
// make a socket at all", and only the first says anything about the server under test.
struct BindAttempt {
    enum class Stage { Bound, SocketFailed, BindFailed, ListenFailed };
    Stage stage = Stage::SocketFailed;
    int err = 0;

    bool bound() const { return stage == Stage::Bound; }
    bool refusedAsInUse() const { return stage == Stage::BindFailed && err == EADDRINUSE; }

    std::string describe() const {
        switch (stage) {
            case Stage::Bound: return "bound and listened";
            case Stage::SocketFailed: return std::string("could not create a socket: ") + std::strerror(err);
            case Stage::BindFailed: return std::string("was refused at bind: ") + std::strerror(err);
            case Stage::ListenFailed: return std::string("bound but failed at listen: ") + std::strerror(err);
        }
        return "?";
    }
};

// `reuseOption` is SO_REUSEPORT, SO_REUSEADDR, or 0 for neither. The socket is closed on
// return either way: this is asking the kernel a question, not holding anything.
BindAttempt tryBindLoopback(int port, int reuseOption) {
    BindAttempt out;
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        out.err = errno;
        return out;
    }
    struct FdGuard {
        int fd;
        ~FdGuard() { ::close(fd); }
    } guard{fd};

    if (reuseOption != 0) {
        int on = 1;
        ::setsockopt(fd, SOL_SOCKET, reuseOption, &on, static_cast<socklen_t>(sizeof(on)));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        out.stage = BindAttempt::Stage::BindFailed;
        out.err = errno;
        return out;
    }
    if (::listen(fd, 4) != 0) {
        out.stage = BindAttempt::Stage::ListenFailed;
        out.err = errno;
        return out;
    }
    out.stage = BindAttempt::Stage::Bound;
    return out;
}

// The positive control for the exclusivity check: the same option, against a port nothing
// holds. Retried because freePort() is racy by construction -- one lost race is not a
// statement about whether this process can bind at all.
BindAttempt bindSomeFreePort(int reuseOption) {
    BindAttempt last;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int port = freePort();
        if (port < 0) continue;
        last = tryBindLoopback(port, reuseOption);
        if (last.bound()) break;
    }
    return last;
}

// The first non-loopback IPv4 address on an up interface, or "" if this host has none.
std::string nonLoopbackAddress() {
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return {};
    std::string found;
    for (ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) continue;
        if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0) continue;
        char buf[INET_ADDRSTRLEN] = {};
        const auto* in4 = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if (::inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof(buf)) == nullptr) continue;
        found = buf;
        break;
    }
    ::freeifaddrs(list);
    return found;
}

// Starts an HttpServer on a port the OS says is free, retrying if something took the
// port in between. Returns null with `reason` filled when no port could be had -- a
// harness failure, never graded as a daemon failure.
std::unique_ptr<HttpServer> startServer(WallpaperManager& manager, std::string& reason) {
    std::string lastError = "no port could be probed";
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int port = freePort();
        if (port < 0) continue;
        auto server = std::make_unique<HttpServer>(manager, port);
        if (server->listening()) return server;
        lastError = server->lastError();
    }
    reason = "could not start an HttpServer on any ephemeral port (last error: " + lastError + ")";
    return nullptr;
}

// ---------------------------------------------------------------------------- //
// Test fixtures on disk
// ---------------------------------------------------------------------------- //

std::string writeSolidImage(const std::string& name, unsigned char r, unsigned char g, unsigned char b) {
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

// A source image big enough that decoding it dominates the render, so runGpuLockChecks
// has a window to poll across. The *target* stays 64x48 -- what has to be slow is the
// decode, which WallpaperManager::renderPending performs holding m_gpuMutex and NOT
// m_stateMutex. That asymmetry is exactly what S7.12 claims, so the fixture has to make
// the decode the expensive half rather than the submit.
std::string writeLargeImage(const std::string& name, int width, int height) {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/" + name;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            // A gradient rather than noise: stb's PNG writer is slow on incompressible
            // data, and this fixture is written on every run. Decode cost is driven by
            // the pixel count, which is unchanged either way.
            pixels[i + 0] = static_cast<unsigned char>(x & 0xFF);
            pixels[i + 1] = static_cast<unsigned char>(y & 0xFF);
            pixels[i + 2] = static_cast<unsigned char>((x + y) & 0xFF);
            pixels[i + 3] = 255;
        }
    }
    if (stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) == 0) {
        throw std::runtime_error("could not write " + path);
    }
    return path;
}

// The bytes that make the hostile filename hostile, named once so the fixture and the
// assertions cannot drift apart.
constexpr const char* kBidiOverride = "\xE2\x80\xAE";  // U+202E RIGHT-TO-LEFT OVERRIDE
constexpr char kDel = '\x7F';

// A real, readable, decodable PNG whose *name* carries a bidi override, a DEL and a
// newline. All three have to survive onto disk: requestWallpaper rejects a path that is
// not a readable regular file before anything is recorded, so a fixture that failed to
// create would turn S7.4 into a check on the rejection path instead.
std::string writeHostileNamedImage() {
    const std::string name = std::string("wpx_hostile_") + kBidiOverride + kDel + "\nname.png";
    return writeSolidImage(name, 0, 255, 0);
}

// A regular, readable file that is not a decodable image. requestWallpaper accepts it --
// it checks only that the path is a readable regular file -- and the render fails on the
// worker afterwards, which is the one way to reach "wallpapers failed while shaders and
// gpu are both fine". A path that did not exist would be rejected on the request instead
// and never reach the render at all.
std::string writeUndecodableFile(const std::string& name) {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/" + name;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) throw std::runtime_error("could not write " + path);
    static const char kJunk[] = "wallpaperx: deliberately not an image";
    const size_t written = std::fwrite(kJunk, 1, sizeof(kJunk) - 1, f);
    const bool closed = std::fclose(f) == 0;
    if (written != sizeof(kJunk) - 1 || !closed) throw std::runtime_error("could not write " + path);
    return path;
}

// An empty directory under the test's asset tree, removed first so a previous run cannot
// leave it populated.
std::string makeEmptyDir(const std::string& name) {
    const std::string dir = testAssetDir() + "/" + name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// ---------------------------------------------------------------------------- //
// /health: reading one, and building the states it has to tell apart.
// ---------------------------------------------------------------------------- //

std::string healthCheckStatus(const nlohmann::json& health, const char* name) {
    if (!health.is_object() || !health.contains("checks") || !health["checks"].is_object()) return {};
    const nlohmann::json& checks = health["checks"];
    if (!checks.contains(name) || !checks[name].is_object()) return {};
    return checks[name].value("status", std::string());
}

std::string healthCheckDetail(const nlohmann::json& health, const char* name) {
    if (!health.is_object() || !health.contains("checks") || !health["checks"].is_object()) return {};
    const nlohmann::json& checks = health["checks"];
    if (!checks.contains(name) || !checks[name].is_object()) return {};
    return checks[name].value("detail", std::string());
}

// A WallpaperManager and an HttpServer that belong to nobody else.
//
// The health states below cannot be built on the run's shared manager. A manager's shader
// directory is fixed at construction, so "the shaders are missing" is not a state the
// shared one can be put into at all; and a failed render is permanent per-monitor state,
// so building one there would change what every later phase reads. Each scenario
// therefore gets its own pair, driven through requestWallpaper/renderPending directly --
// the same route runTornReadChecks uses -- so no render worker exists and the Device, when
// one gets created, is created, used and destroyed on this one thread.
class IsolatedDaemon {
public:
    explicit IsolatedDaemon(WallpaperManager::Config config) : m_manager(std::move(config)) {
        m_server = startServer(m_manager, m_reason);
    }

    bool ready() const { return m_server != nullptr; }
    const std::string& reason() const { return m_reason; }
    WallpaperManager& manager() { return m_manager; }

    HttpReply health() {
        httplib::Client cli = makeClient(m_server->port());
        return get(cli, "/health");
    }

private:
    WallpaperManager m_manager;
    std::string m_reason;
    // Declared after the manager so it is destroyed first: it holds a reference to it.
    // That is the ordering main.cpp gets from declaring the HttpServer last.
    std::unique_ptr<HttpServer> m_server;
};

// ---------------------------------------------------------------------------- //
// Phase 1 -- where the socket is, and what happens when it cannot be had.
// ---------------------------------------------------------------------------- //
void runBindChecks(WallpaperManager& manager, sdbus::IProxy& proxy, HttpServer& server, const Check& check) {
    httplib::Client cli = makeClient(server.port());
    const HttpReply health = get(cli, "/health");
    check(health.reached && health.status == 200, "the server answers /health on 127.0.0.1");

    // GOAL: S7.1 -- the config server is reachable from this machine and no other. It
    //       binds 127.0.0.1, not INADDR_ANY; false if the listening socket answers on a
    //       non-loopback address, which would put every monitor's wallpaper path on the
    //       local network.
    // WHY-THIS-SHAPE: the obvious check -- "connecting to the LAN address fails" -- is
    //       satisfied on any host whose firewall drops inbound traffic to its own
    //       address, and is then equally true of a server bound to 0.0.0.0. So the host
    //       is asked the question first, with a socket this test binds to INADDR_ANY on
    //       another port: if *that* is not reachable at the same address, the host cannot
    //       distinguish the two bindings at all and the phase skips instead of passing.
    //       Refusal and timeout are also kept apart (tryConnect), because a DROP shows up
    //       as a timeout and only a refusal means "nothing is listening here".
    // CONTROL: HttpServer::kLoopbackHost changed to "0.0.0.0" -> "FAIL: the config
    //       server does not answer on this host's non-loopback address", with
    //       "connect to 192.168.1.186:36605 connected (expected refused)". Reverted.
    const std::string lanAddress = nonLoopbackAddress();
    if (lanAddress.empty()) {
        notVerified("S7.1 (loopback-only bind)", "this host has no non-loopback IPv4 interface to test against");
    } else {
        HeldPort anyPort;
        const bool anyOpened = anyPort.open(INADDR_ANY);
        const ConnectResult reachesAny =
            anyOpened ? tryConnect(lanAddress, anyPort.port(), 1000) : ConnectResult::Failed;
        if (reachesAny != ConnectResult::Connected) {
            notVerified("S7.1 (loopback-only bind)",
                        std::string("a control socket bound to 0.0.0.0 is itself not reachable at ") + lanAddress +
                            " (" + describe(reachesAny) + "), so this host cannot tell a loopback bind from an "
                            "INADDR_ANY one");
        } else {
            const ConnectResult reachesServer = tryConnect(lanAddress, server.port(), 1000);
            check(reachesServer == ConnectResult::Refused,
                  "the config server does not answer on this host's non-loopback address");
            if (reachesServer != ConnectResult::Refused) {
                std::fprintf(stderr, "  connect to %s:%d %s (expected refused)\n", lanAddress.c_str(), server.port(),
                             describe(reachesServer));
            }
        }
    }

    // GOAL: S7.15 -- while the daemon is serving, nothing else on this machine can bind
    //       and listen on its port. The plugin identifies the daemon by port alone, so a
    //       second listener the kernel load-balances to decides what the desktop displays
    //       for whatever share of the polls it receives: measured at 14 of 30 /config
    //       responses under httplib's SO_REUSEPORT default. False if any of the three
    //       attempts below is allowed to listen.
    // WHY-THIS-SHAPE: "the second bind failed" is the obvious check and it cannot be
    //       trusted -- it is equally what a run with no server, a failed socket() or a
    //       sandbox with no networking produces, so it would pass while establishing
    //       nothing. Three things narrow it: the refusal must be EADDRINUSE from bind()
    //       specifically (BindAttempt keeps the stage and errno apart), the same option
    //       is first proved to bind a port that IS free, and /health is read afterwards
    //       so the port that refused is known to be the daemon's and still served.
    //       SO_REUSEADDR and a plain bind are attempted alongside SO_REUSEPORT because
    //       the goal is "nothing can bind", not "the one option httplib defaulted to".
    // CONTROL: two, because the three variants are not refused for the same reason.
    //       (a) production: HttpServer's set_socket_options call deleted, restoring
    //       httplib's SO_REUSEPORT default -> "FAIL: no second listener can take a share
    //       of the daemon's port (SO_REUSEPORT)" with "SO_REUSEPORT bind+listen on
    //       127.0.0.1:33643 bound and listened while the server was serving there", the
    //       other two staying green -- that asymmetry is exactly what the fix turns on.
    //       (b) harness: server.port() replaced with freePort() as the target -> all
    //       three FAIL, which is what proves the SO_REUSEADDR and plain variants are
    //       capable of failing at all. Both reverted.
    // COVERAGE-ASSERT: each variant's positive control must bind a free port before its
    //       refusal is graded; a variant whose control cannot bind is reported as not
    //       verified (exit 77) rather than counted, so a host where sockets are
    //       unavailable skips instead of passing three checks it never performed.
    struct ReuseVariant {
        int option;
        const char* name;
    };
    const ReuseVariant kReuseVariants[] = {
        {SO_REUSEPORT, "SO_REUSEPORT"},
        {SO_REUSEADDR, "SO_REUSEADDR"},
        {0, "no reuse option"},
    };
    for (const ReuseVariant& variant : kReuseVariants) {
        const BindAttempt control = bindSomeFreePort(variant.option);
        if (!control.bound()) {
            const std::string goal = std::string("S7.15 (port exclusivity, ") + variant.name + ")";
            notVerified(goal.c_str(),
                        std::string("a ") + variant.name + " bind could not take even a free loopback port (" +
                            control.describe() + "), so a refusal on the server's port would mean nothing");
            continue;
        }
        const BindAttempt hijack = tryBindLoopback(server.port(), variant.option);
        const std::string message =
            std::string("no second listener can take a share of the daemon's port (") + variant.name + ")";
        check(hijack.refusedAsInUse(), message.c_str());
        if (!hijack.refusedAsInUse()) {
            std::fprintf(stderr, "  %s bind+listen on 127.0.0.1:%d %s while the server was serving there\n",
                         variant.name, server.port(), hijack.describe().c_str());
        }
    }
    const HttpReply stillServing = get(cli, "/health");
    check(stillServing.reached && stillServing.status == 200,
          "the port those binds were refused from is the one the daemon is still serving");

    // GOAL: S7.7 -- a bind failure does not take the daemon down and does not leave it
    //       looking healthy: the constructor returns, listening() is false, lastError()
    //       says what happened, and DBus keeps working.
    //       UNRATIFIED: PLAN.md does not settle whether a port conflict should be fatal.
    //       This pins what cd28790 does, so a change of mind is a visible decision rather
    //       than a silent one; it is not a claim that surviving is correct.
    // WHY-THIS-SHAPE: the port is held by a socket this test owns for the duration,
    //       rather than by a second HttpServer, so the conflict is real and the failing
    //       constructor is the only HttpServer involved -- with two servers a wrong
    //       result could belong to either.
    // CONTROL: two, because "does not throw" and "reports the failure" are separate
    //       claims. (a) `throw std::runtime_error(m_bindError);` in place of the return
    //       in the bind-failure branch -> "FAIL: a port conflict does not throw out of
    //       the HttpServer constructor". (b) the `return` deleted from that branch, so
    //       m_listening is set anyway -> "FAIL: a server that could not bind reports
    //       listening() == false". Both reverted.
    HeldPort taken;
    if (!taken.open(INADDR_LOOPBACK)) {
        notVerified("S7.7 (bind failure is survivable)", "could not hold a loopback port to create the conflict");
        return;
    }

    bool threw = false;
    bool listening = true;
    std::string bindError;
    int reportedPort = -1;
    try {
        HttpServer conflicted(manager, taken.port());
        listening = conflicted.listening();
        bindError = conflicted.lastError();
        reportedPort = conflicted.port();
        // Destroyed here, on a server that never served a request. Safe in this one case
        // precisely because the serving thread was never started -- see the CONCERN note
        // above runShutdownChecks for why that matters everywhere else.
    } catch (const std::exception&) {
        threw = true;
    }
    check(!threw, "a port conflict does not throw out of the HttpServer constructor");
    // Gated rather than each written as `!threw && ...`: a constructor that threw has
    // nothing to report through, so grading its accessors too would turn one fault into
    // five FAIL lines pointing at four properties that were never observed.
    if (!threw) {
        check(!listening, "a server that could not bind reports listening() == false");
        check(!bindError.empty(), "a server that could not bind reports why in lastError()");
        check(bindError.find(std::to_string(taken.port())) != std::string::npos,
              "the bind error names the port that was taken");
        check(reportedPort == taken.port(), "port() reports the port that was asked for even after a failure");
    }

    // The other half of S7.7, and the half that says the daemon is still a daemon: the
    // bus surface has to be unaffected by the HTTP server failing to come up.
    const nlohmann::json status = getStatus(proxy);
    check(!status.is_discarded() && status.value("running", false),
          "DBus still answers GetStatus after an HTTP bind failure");
}

// ---------------------------------------------------------------------------- //
// Phase 2 -- /health before any device exists.
// ---------------------------------------------------------------------------- //
void runHealthBeforeDeviceChecks(httplib::Client& cli, const Check& check) {
    const HttpReply health = get(cli, "/health");
    if (!health.reached || health.json.is_discarded()) {
        check(false, "/health answers with parseable JSON before any wallpaper is set");
        return;
    }

    check(health.status == 200, "/health answers 200 before any wallpaper is set");

    // Compared against the same project(... VERSION) the daemon library compiles in, not
    // against a literal: what this pins is that /health still *derives* its version from
    // the build, since a hardcoded string is how a user comes to report a bug against a
    // version this binary stopped being two releases ago.
    // CONTROL: `{"version", WALLPAPERX_VERSION}` replaced with `{"version", "9.9.9"}` ->
    //       "FAIL: /health reports the version CMake built this daemon as". Reverted.
    check(health.json.value("version", std::string()) == std::string(WALLPAPERX_EXPECTED_VERSION),
          "/health reports the version CMake built this daemon as");

    // GOAL: S7.10a -- each dependency is reported separately and by name, so a failure is
    //       attributable. `shaders`, `gpu` and `wallpapers` are three distinct checks,
    //       each carrying its own status and its own cause.
    // WHY-THIS-SHAPE: the schema is asserted here, on a daemon where all three are in the
    //       same state, rather than only where they differ. Checking names only in the
    //       failure phases would leave a /health that dropped a check entirely -- so that
    //       a reader could no longer see it at all -- passing everywhere the remaining two
    //       happened to be enough to decide the aggregate.
    // CONTROL: `{"wallpapers", &wallpapers},` removed from the checks array in
    //       http_server.cpp -> "FAIL: /health names shaders, gpu and wallpapers as three
    //       separate checks" (observed: checks = {"gpu":..., "shaders":...}). Reverted.
    const bool shape = health.json.contains("checks") && health.json["checks"].is_object() &&
                       health.json["checks"].size() == 3;
    check(shape, "/health carries a `checks` object with exactly the three dependencies in it");
    check(!healthCheckStatus(health.json, "shaders").empty() && !healthCheckStatus(health.json, "gpu").empty() &&
              !healthCheckStatus(health.json, "wallpapers").empty(),
          "/health names shaders, gpu and wallpapers as three separate checks");
    if (!shape) {
        std::fprintf(stderr, "  checks: %s\n",
                     health.json.contains("checks") ? health.json["checks"].dump().c_str() : "(absent)");
    }

    // GOAL: S7.10b -- `pending` is a distinct state from `ok` and it reaches the
    //       aggregate. Both the shader directory and the device are touched only on the
    //       first render, so on a daemon that has not rendered, nothing has been tried and
    //       nothing is known. False if this reports "ok", which is health on the strength
    //       of checks nobody has run -- the defect S7.10 exists to remove, and the
    //       aggregate is the field a watchdog reads.
    // WHY-THIS-SHAPE: the per-check statuses and the top-level one are asserted together.
    //       Asserting only the three sub-checks would pass for an aggregate hardcoded to
    //       "ok", which is exactly the old endpoint; asserting only the aggregate would
    //       pass for a daemon that reported everything pending forever, so scenarios (a)
    //       and (c) of runHealthStateChecks assert that the same three fields and the
    //       aggregate all read "ok" once a render has happened. Neither half establishes
    //       anything alone.
    // CONTROL: two, because the aggregate and the three checks it is computed from fail
    //       independently. (a) the `else if (check->status == HealthStatus::Pending ...)`
    //       arm deleted from the aggregation in http_server.cpp, so pending counts as ok
    //       -> "FAIL: /health reports the aggregate as pending before anything has been
    //       tried" with "aggregate before any render: ok", the three per-check assertions
    //       staying green. (b) `HealthStatus status = HealthStatus::Ok;` as HealthCheck's
    //       default in wallpaper_manager.h -- which is the pre-rebuild endpoint restored,
    //       health asserted rather than established -> all four go red, with
    //       "{"gpu":{"status":"ok"},"shaders":{"status":"ok"},...},"status":"ok"" on a
    //       daemon that had done nothing at all. Both reverted.
    check(healthCheckStatus(health.json, "shaders") == "pending",
          "/health reports the shader directory as pending before anything has looked at it");
    check(healthCheckStatus(health.json, "gpu") == "pending",
          "/health reports the GPU as pending before a device has been created");
    check(healthCheckStatus(health.json, "wallpapers") == "pending",
          "/health reports wallpapers as pending before any wallpaper is set");
    const std::string aggregate = health.json.value("status", std::string());
    check(aggregate == "pending", "/health reports the aggregate as pending before anything has been tried");
    if (aggregate != "pending") {
        std::fprintf(stderr, "  aggregate before any render: %s (body: %s)\n", aggregate.c_str(),
                     health.body.c_str());
    }
}

// Defined below Phase 2b but called from its tail, where the state it needs -- a daemon
// that has already rendered successfully -- is the subject.
void runGpuFailureChecks(const std::string& bluePath, const WallpaperManager::Config& base, const Check& check);

// ---------------------------------------------------------------------------- //
// Phase 2b -- the states /health has to be able to tell apart.
// ---------------------------------------------------------------------------- //
void runHealthStateChecks(const std::string& bluePath, const std::string& videoPath, const std::string& junkPath,
                          const Check& check) {
    WallpaperManager::Config base;
    base.width = kTargetWidth;
    base.height = kTargetHeight;
    base.enableValidation = false;

    // Kept for the attribution comparison at the end of this phase: what makes a failure
    // attributable is that two different faults produce two different bodies, which is not
    // a property either scenario has on its own.
    nlohmann::json shaderFailHealth;
    nlohmann::json imageFailHealth;

    // ------------------------------------------------------------------ //
    // (a) a monitor that has only been asked for is not one that is showing, and the
    //     pending that produces reaches the aggregate past two checks that are already ok.
    // ------------------------------------------------------------------ //
    {
        IsolatedDaemon daemon(base);
        if (!daemon.ready()) {
            notVerified("S7.10b (pending propagates to the aggregate), S7.10f (in flight is not ok)",
                        daemon.reason());
        } else {
            // Monitor 0 is rendered, which is the only route to shaders=ok and gpu=ok --
            // both are touched exclusively by a real render. Monitor 1 is requested and
            // deliberately never rendered; IsolatedDaemon runs no render worker, so
            // "requested, no frame" is a state this thread creates and nothing else can
            // leave, rather than a window that has to be caught (X.2).
            daemon.manager().requestWallpaper("static", bluePath, 0);
            daemon.manager().renderPending(0);
            daemon.manager().requestWallpaper("static", bluePath, 1);

            // Read off the manager, never off /health: gating on the endpoint under test
            // turns a broken endpoint into a skip. Scenario (c) records that being observed.
            const WallpaperManager::Snapshot mixed = daemon.manager().snapshot();
            std::size_t mixedInFlight = 0;
            for (const auto& [id, monitor] : mixed.monitors) {
                if (monitor.lastError.empty() && !monitor.hasFrame) ++mixedInFlight;
            }
            const auto shown = mixed.monitors.find(0);
            const auto asked = mixed.monitors.find(1);
            const bool built = shown != mixed.monitors.end() && shown->second.hasFrame &&
                               asked != mixed.monitors.end() && !asked->second.hasFrame &&
                               asked->second.lastError.empty() && mixedInFlight == 1;

            const HttpReply health = daemon.health();
            if (!built) {
                // X.3: a host that cannot render is not a defect in /health. Without a
                // frame on monitor 0 there is no ok check for the pending to propagate
                // past, so the state this scenario grades does not exist.
                notVerified("S7.10b (pending propagates to the aggregate), S7.10f (in flight is not ok)",
                            "the mixed state could not be built on this host (monitor 0 error: " +
                                (shown != mixed.monitors.end() ? shown->second.lastError : "no such monitor") + ")");
            } else if (!health.ok()) {
                check(false, "the mixed-state daemon answered /health with a parseable 200");
            } else {
                // GOAL: S7.10b -- `pending` propagates to the aggregate rather than being
                //       rounded off to `ok`; and S7.10f -- a monitor that has been
                //       requested but has not yet rendered is not a monitor showing a
                //       picture. False here if /health answers "ok" for a daemon with one
                //       blank screen, which is a watchdog being told a half-black desktop
                //       is healthy.
                // WHY-THIS-SHAPE: runHealthBeforeDeviceChecks already pins an all-pending
                //       daemon, and an aggregation with the pending arm deleted fails
                //       there too -- but only because *nothing* was ok. An implementation
                //       taking worst(ok, pending) = ok and worst(pending) = pending passes
                //       there and fails here. So the state is deliberately mixed, and the
                //       ok-ness of the ok checks is asserted rather than assumed, or the
                //       aggregate assertion below is the all-pending case under another
                //       name. The mix is built from an in-flight monitor rather than from
                //       an unrenderable type, which is what it used to use: that state now
                //       reads `fail`, and `{wallpapers ok, shaders pending, gpu pending}`
                //       is unreachable by construction, since wallpapers=ok requires a
                //       render and a render is what makes the other two ok.
                // CONTROL: the `else if (inFlight > 0)` arm deleted from the wallpapers
                //       derivation in http_server.cpp, so an in-flight monitor falls into
                //       the ok branch -> "FAIL: a monitor that has been requested but not
                //       rendered leaves wallpapers pending, not ok", "FAIL: an untried
                //       dependency keeps the aggregate at pending even when another check
                //       is ok" (with "mixed state {wallpapers ok, shaders ok, gpu ok}
                //       aggregated to: ok") and the pending-detail check below, which has
                //       no branch left to read. Reverted.
                check(healthCheckStatus(health.json, "shaders") == "ok" &&
                          healthCheckStatus(health.json, "gpu") == "ok",
                      "the mixed-state daemon really does have two checks reading ok");
                check(healthCheckStatus(health.json, "wallpapers") == "pending",
                      "a monitor that has been requested but not rendered leaves wallpapers pending, not ok");
                const std::string aggregate = health.json.value("status", std::string());
                check(aggregate == "pending",
                      "an untried dependency keeps the aggregate at pending even when another check is ok");
                if (aggregate != "pending") {
                    std::fprintf(stderr, "  mixed state {wallpapers %s, shaders %s, gpu %s} aggregated to: %s\n",
                                 healthCheckStatus(health.json, "wallpapers").c_str(),
                                 healthCheckStatus(health.json, "shaders").c_str(),
                                 healthCheckStatus(health.json, "gpu").c_str(), aggregate.c_str());
                }

                // The count in the pending detail is compared against the manager's own
                // in-flight tally, not against "1": a detail that reported the monitor
                // total would read "2 of 2" here and satisfy any check that only looked
                // for the words.
                // CONTROL: the pending detail's first term changed from `inFlight` to
                //       `snap.monitors.size()` -> "FAIL: the pending detail counts the
                //       monitors that have not rendered" with "  detail: 2 of 2 monitors
                //       have not rendered yet (expected 1 of 2)". Reverted.
                const std::string expectPending =
                    std::to_string(mixedInFlight) + " of " + std::to_string(mixed.monitors.size());
                const std::string pendingDetail = healthCheckDetail(health.json, "wallpapers");
                check(pendingDetail.find(expectPending) != std::string::npos,
                      "the pending detail counts the monitors that have not rendered");
                if (pendingDetail.find(expectPending) == std::string::npos) {
                    std::fprintf(stderr, "  detail: %s (expected %s)\n", pendingDetail.c_str(),
                                 expectPending.c_str());
                }

                // GOAL: S7.10f's other half -- the `ok` detail's count comes from the
                //       monitors that have a frame. Rendering the second monitor is also
                //       what makes the pending above a *transient* verdict rather than a
                //       state this daemon can never leave.
                // WHY-THIS-SHAPE: two monitors, not one, and the expected count is read
                //       off the manager. With a single monitor "1" satisfies a detail
                //       counting frames and one counting map entries alike, so the old
                //       "N monitor(s) rendered" bug would still pass. On the ok branch
                //       `showing` and `monitors.size()` are necessarily equal -- that is
                //       what ok means -- so what separates the two derivations is the
                //       check above, where a non-showing monitor must keep the branch out
                //       of ok at all; this one pins the number itself.
                // CONTROL: `std::to_string(showing)` in the ok detail replaced with
                //       `std::to_string(showing + 1)` -> "FAIL: the ok detail counts the
                //       monitors that have a frame" with "  detail: 3 monitor(s) showing a
                //       frame (expected 2)". Reverted.
                daemon.manager().renderPending(1);
                const WallpaperManager::Snapshot done = daemon.manager().snapshot();
                std::size_t showing = 0;
                for (const auto& [id, monitor] : done.monitors) {
                    if (monitor.hasFrame) ++showing;
                }
                const HttpReply full = daemon.health();
                if (showing != done.monitors.size()) {
                    notVerified("S7.10f (the ok detail counts frames)",
                                "the second render did not produce a frame, so this daemon never reached the "
                                "state whose detail is being graded");
                } else if (!full.ok()) {
                    check(false, "the fully rendered daemon answered /health with a parseable 200");
                } else {
                    check(healthCheckStatus(full.json, "wallpapers") == "ok",
                          "wallpapers reads ok once every monitor has a frame");
                    check(full.json.value("status", std::string()) == "ok",
                          "the aggregate leaves pending once the in-flight monitor has rendered");
                    const std::string okDetail = healthCheckDetail(full.json, "wallpapers");
                    check(showing == 2 && okDetail.find(std::to_string(showing) + " monitor") != std::string::npos,
                          "the ok detail counts the monitors that have a frame");
                    if (okDetail.find(std::to_string(showing) + " monitor") == std::string::npos) {
                        std::fprintf(stderr, "  detail: %s (expected %zu)\n", okDetail.c_str(), showing);
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------ //
    // (a2) a type this build cannot draw is a failure, not silence.
    // ------------------------------------------------------------------ //
    {
        IsolatedDaemon daemon(base);
        if (!daemon.ready()) {
            notVerified("S7.10g (an unrenderable type is a failure)", daemon.reason());
        } else {
            // renderPending returns early for a type this build cannot draw, before
            // ensureGpu, so this scenario needs no working Vulkan -- which is the point of
            // it being separate from the after-device phase that also covers the rule.
            daemon.manager().requestWallpaper("video", videoPath, 0);
            daemon.manager().renderPending(0);

            const WallpaperManager::Snapshot snap = daemon.manager().snapshot();
            const auto it = snap.monitors.find(0);
            const HttpReply health = daemon.health();
            if (it == snap.monitors.end()) {
                check(false, "the video request was recorded on the unsupported-type daemon");
            } else if (!health.ok()) {
                check(false, "the unsupported-type daemon answered /health with a parseable 200");
            } else {
                // GOAL: S7.10g -- a monitor set to a type this build cannot render is
                //       reported as a failure, named with the monitor and the type. False
                //       if a daemon whose only wallpaper is a `video` reads ok: that
                //       screen is provably blank forever and no amount of retrying changes
                //       it, so `ok` is the one answer that cannot be repaired by waiting.
                // WHY-THIS-SHAPE: the shaders and gpu checks are asserted to be *pending*
                //       alongside. The obvious check -- wallpapers == "fail" alone -- is
                //       also satisfied by a handler that failed the wallpapers check
                //       because the render blew up, which is a different fact with the same
                //       verdict; pending on both dependencies is what pins that nothing was
                //       ever tried and the fail is a verdict on the *type*.
                // CONTROL: the `!isRenderableType(monitor.type)` arm deleted from the
                //       classification loop in http_server.cpp, so an unrenderable monitor
                //       falls through to the in-flight count -> "FAIL: a wallpaper type
                //       this build cannot render is reported as a failure", "FAIL: an
                //       unrenderable type makes the aggregate fail" and "FAIL: the
                //       unsupported detail names the monitor and the type it is set to",
                //       with wallpapers reading {pending, "1 of 1 monitors have not
                //       rendered yet"}. Reverted.
                // COVERAGE-ASSERT: the monitor is asserted to carry no lastError. That is
                //       what makes this scenario the old defect rather than a restatement
                //       of scenario (c): the pre-fix handler derived the check purely from
                //       lastError, so a monitor with none read ok. If a future
                //       requestWallpaper started recording an error for `video`, this would
                //       silently become a second copy of the failed-render check.
                check(it->second.lastError.empty(),
                      "an unrenderable type leaves no error on the monitor, so fail cannot come from one");
                check(healthCheckStatus(health.json, "wallpapers") == "fail",
                      "a wallpaper type this build cannot render is reported as a failure");
                check(healthCheckStatus(health.json, "shaders") == "pending" &&
                          healthCheckStatus(health.json, "gpu") == "pending",
                      "an unrenderable type does not implicate the shaders or the GPU");
                check(health.json.value("status", std::string()) == "fail",
                      "an unrenderable type makes the aggregate fail");

                // The detail has to say which monitor and which type, or a reader is back
                // to calling GetStatus. Both substrings are built from the recorded state
                // rather than spelled out, so a rename of the type reaching /health without
                // reaching the monitor record cannot pass.
                // CONTROL: `monitor.type` replaced with `"static"` in the firstUnsupported
                //       string -> "FAIL: the unsupported detail names the monitor and the
                //       type it is set to" with "detail: 1 of 1 monitors show nothing:
                //       monitor 0 is set to a 'static' wallpaper, which this build cannot
                //       render (expected 'monitor 0' and 'video')". Reverted.
                const std::string detail = healthCheckDetail(health.json, "wallpapers");
                const bool named = detail.find("monitor 0") != std::string::npos &&
                                   detail.find("'" + it->second.type + "'") != std::string::npos;
                check(named, "the unsupported detail names the monitor and the type it is set to");
                if (!named) {
                    std::fprintf(stderr, "  detail: %s (expected 'monitor 0' and '%s')\n", detail.c_str(),
                                 it->second.type.c_str());
                }
                std::printf("http_server_test: unrenderable type -> %s\n", health.body.c_str());
                std::fflush(stdout);
            }
        }
    }

    // ------------------------------------------------------------------ //
    // (b) a missing shader directory is attributed to the shaders.
    // ------------------------------------------------------------------ //
    {
        WallpaperManager::Config config = base;
        config.shaderDir = testAssetDir() + "/wpx_no_such_shader_dir";
        std::filesystem::remove_all(config.shaderDir);

        IsolatedDaemon daemon(config);
        if (!daemon.ready()) {
            notVerified("S7.10a (a shader failure is attributed to the shaders)", daemon.reason());
        } else {
            daemon.manager().requestWallpaper("static", bluePath, 0);
            daemon.manager().renderPending(0);

            const HttpReply health = daemon.health();
            if (!health.ok()) {
                check(false, "the missing-shader-directory daemon answered /health with a parseable 200");
            } else {
                shaderFailHealth = health.json;

                // GOAL: S7.10a -- a failure is attributed to the dependency that failed.
                //       A missing shader directory is the shaders' fault; the GPU is not
                //       implicated, because nothing ever asked it for anything.
                // WHY-THIS-SHAPE: `gpu == "pending"` is asserted, not merely "gpu is not
                //       fail". Pending is the true answer here -- ensureGpu resolves the
                //       shader directory before it creates a Device, so the device is
                //       never attempted -- and an implementation that recorded a blanket
                //       failure across every check, or that marked the GPU ok because the
                //       render "got that far", both satisfy a not-fail check and neither
                //       tells a reader where to look. S7.10c rides along: the detail has
                //       to name the directory, or the reader is back to guessing.
                // CONTROL: ensureGpu's shader catch changed to record into
                //       `&DaemonHealth::gpu` instead of `&DaemonHealth::shaders` -> three
                //       red: "a missing shader directory is reported as a shader failure",
                //       "... does not implicate the GPU", and "the shader check's detail
                //       names the directory that could not be found", the body showing
                //       shaders={pending,""} and the directory named under gpu instead.
                //       Reverted.
                check(healthCheckStatus(health.json, "shaders") == "fail",
                      "a missing shader directory is reported as a shader failure");
                check(healthCheckStatus(health.json, "gpu") == "pending",
                      "a missing shader directory does not implicate the GPU");
                check(healthCheckDetail(health.json, "shaders").find("wpx_no_such_shader_dir") != std::string::npos,
                      "the shader check's detail names the directory that could not be found");
                check(health.json.value("status", std::string()) == "fail",
                      "a failing check makes the aggregate fail");
                std::printf("http_server_test: missing shader directory -> %s\n", health.body.c_str());
                std::fflush(stdout);
            }
        }
    }

    // ------------------------------------------------------------------ //
    // (c) an undecodable image is attributed to the wallpapers, and carries its cause.
    // (d) and a good render afterwards puts the aggregate back to ok.
    // ------------------------------------------------------------------ //
    {
        IsolatedDaemon daemon(base);
        if (!daemon.ready()) {
            notVerified("S7.10a/S7.10c (an image failure is attributed to the wallpapers)", daemon.reason());
        } else {
            daemon.manager().requestWallpaper("static", junkPath, 0);
            daemon.manager().renderPending(0);

            const HttpReply health = daemon.health();
            // X.3: no working Vulkan on this host is not a defect in /health. Without a
            // device the render fails for the wrong reason, so the attribution this
            // scenario exists to establish cannot be observed at all.
            //
            // Asked of the manager, NOT of /health's own gpu check. A gate that reads the
            // field under test converts a broken endpoint into a skip: with the gpu check
            // wired to the wrong source, this phase reported NOT VERIFIED instead of
            // failing, and the attribution checks below never ran at all. Observed while
            // controlling them -- it is why the gate moved.
            const bool deviceExists = daemon.manager().snapshot().gpu.initialized;
            if (!health.ok()) {
                check(false, "the undecodable-image daemon answered /health with a parseable 200");
            } else if (!deviceExists) {
                notVerified("S7.10a/S7.10c (an image failure is attributed to the wallpapers)",
                            "no Vulkan device could be created on this host, so the render failed for a reason "
                            "other than the image (/health said gpu=" + healthCheckStatus(health.json, "gpu") + ": " +
                                healthCheckDetail(health.json, "gpu") + ")");
            } else {
                imageFailHealth = health.json;

                // GOAL: S7.10c -- a failing check carries its cause. `wallpapers` reports
                //       the first monitor's actual error text, so a reader does not have
                //       to go and call GetStatus to learn why -- that is the call /health
                //       exists to save them. S7.10a rides along: shaders and gpu both read
                //       ok, so the fault is pinned on the one dependency that broke.
                // WHY-THIS-SHAPE: the detail is compared against the monitor's own
                //       lastError read back off the manager, not against a substring this
                //       file chose. A `find("not a decodable image")` check would pass for
                //       a handler that pasted a fixed sentence in, and would have to be
                //       edited every time the renderer reworded a message -- an assertion
                //       that has to be maintained in lockstep with the string it checks is
                //       one that gets loosened.
                // CONTROL: `+ first` dropped from the wallpapers detail in
                //       http_server.cpp, leaving the count -> "FAIL: the wallpapers check
                //       carries the monitor's own error text" with "  detail: 1 of 1
                //       monitors failed  lastError: ... is not a decodable image ...".
                //       Reverted.
                // COVERAGE-ASSERT: the monitor's lastError is asserted non-trivial first.
                //       If the render failed with an empty or one-word error, "the detail
                //       contains it" would be satisfied by any detail at all.
                const WallpaperManager::Snapshot snap = daemon.manager().snapshot();
                const auto it = snap.monitors.find(0);
                const std::string lastError = it != snap.monitors.end() ? it->second.lastError : std::string();
                check(lastError.size() > 20, "the failed render left a non-trivial error on the monitor");
                check(healthCheckStatus(health.json, "wallpapers") == "fail",
                      "an undecodable image is reported as a wallpaper failure");
                check(healthCheckStatus(health.json, "shaders") == "ok",
                      "an undecodable image does not implicate the shaders");
                check(healthCheckStatus(health.json, "gpu") == "ok",
                      "an undecodable image does not implicate the GPU");
                check(!lastError.empty() &&
                          healthCheckDetail(health.json, "wallpapers").find(lastError) != std::string::npos,
                      "the wallpapers check carries the monitor's own error text");
                check(health.json.value("status", std::string()) == "fail",
                      "an undecodable image makes the aggregate fail");
                if (healthCheckDetail(health.json, "wallpapers").find(lastError) == std::string::npos) {
                    std::fprintf(stderr, "  detail: %s\n  lastError: %s\n",
                                 healthCheckDetail(health.json, "wallpapers").c_str(), lastError.c_str());
                }
                std::printf("http_server_test: undecodable image -> %s\n", health.body.c_str());
                std::fflush(stdout);

                // GOAL: S7.10 -- /health reports what is *currently* failing. A daemon
                //       that has recovered must say so; a failure that sticks is a
                //       different kind of lie from one that is never reported, and it
                //       trains a reader to ignore the endpoint.
                // WHY-THIS-SHAPE: this is also what keeps every "not ok" assertion in this
                //       phase from being vacuous. If /health could never answer "ok" on
                //       this host -- a missing GPU, a build with no shaders -- then
                //       "pending", "fail" and "fail" above would all be satisfied by an
                //       endpoint that was simply stuck, and the whole phase would
                //       establish nothing. One reachable "ok" from the same manager, after
                //       the same monitor is given a file that does decode, is what rules
                //       that out.
                // CONTROL: the `else` arm of the wallpapers derivation in http_server.cpp
                //       changed to `wallpapers.status = HealthStatus::Fail;` -> "FAIL: a
                //       later successful render clears the wallpaper failure" and "FAIL: a
                //       daemon with nothing wrong reports the aggregate as ok", the body
                //       showing wallpapers={fail,"1 monitor(s) rendered"}. The two failure
                //       scenarios above stayed green under it, which is the point -- it is
                //       the ok verdict, not the fail verdicts, that this pair pins.
                //       Reverted.
                daemon.manager().requestWallpaper("static", bluePath, 0);
                daemon.manager().renderPending(0);
                const HttpReply recovered = daemon.health();
                if (!recovered.ok()) {
                    check(false, "the recovered daemon answered /health with a parseable 200");
                } else {
                    check(healthCheckStatus(recovered.json, "wallpapers") == "ok",
                          "a later successful render clears the wallpaper failure");
                    check(recovered.json.value("status", std::string()) == "ok",
                          "a daemon with nothing wrong reports the aggregate as ok");
                    if (recovered.json.value("status", std::string()) != "ok") {
                        std::fprintf(stderr, "  after a good render: %s\n", recovered.body.c_str());
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------ //
    // (e) health is recorded as the daemon uses a dependency, not probed on demand.
    // ------------------------------------------------------------------ //
    {
        const std::string shaderDir = makeEmptyDir("wpx_health_shader_dir");
        WallpaperManager::Config config = base;
        config.shaderDir = shaderDir;

        IsolatedDaemon daemon(config);
        if (!daemon.ready()) {
            notVerified("S7.10d (health is recorded, never probed)", daemon.reason());
        } else {
            // resolveShaderDir only requires the path to be a directory, so an empty one
            // records shaders=ok. What happens after that -- device creation, and a render
            // that then cannot find any .spv -- is irrelevant here and deliberately not
            // asserted on: this scenario is about the shaders check alone.
            daemon.manager().requestWallpaper("static", bluePath, 0);
            daemon.manager().renderPending(0);

            // The precondition is that the daemon RECORDED the directory as ok, which is
            // read off the manager rather than off /health -- what this scenario grades is
            // how the handler renders that recording after the directory is gone, so a
            // gate that read the handler's own answer could turn a handler bug into a skip.
            const bool recordedOk =
                daemon.manager().snapshot().health.shaders.status == wallpaperx::HealthStatus::Ok;
            const HttpReply before = daemon.health();
            if (!recordedOk) {
                notVerified("S7.10d (health is recorded, never probed)",
                            "the shader directory was never recorded as ok in the first place, so removing it "
                            "afterwards would prove nothing");
            } else if (!before.ok()) {
                check(false, "the recorded-not-probed daemon answered /health with a parseable 200");
            } else {
                // GOAL: S7.10d -- health is recorded as the daemon uses its dependencies
                //       and never probed on demand. False if a handler performs I/O:
                //       stat()ing the shader directory inside the health handler would put
                //       a filesystem call on the HTTP thread, and a hung network mount
                //       would then wedge the one endpoint still worth asking when things
                //       are bad.
                // WHY-THIS-SHAPE: asserting the absence of a syscall is not directly
                //       possible from here, so the dependency is destroyed *after* it was
                //       recorded ok and the recorded answer is required to survive. A
                //       handler that re-derived the check would now report fail -- the
                //       directory genuinely is gone -- so the two implementations are
                //       distinguishable by their output. The obvious alternative, timing a
                //       request against a slow filesystem, is an assertion on the
                //       scheduler and needs a hangable mount no test host is guaranteed
                //       to have. The detail is compared byte for byte with the earlier
                //       reply as well, so a handler that re-derived a *passing* answer
                //       some other way still has to reproduce the recorded string.
                // CONTROL: two, one per assertion, both patching the /health handler to
                //       re-derive the check it was handed and using `&shadersProbed` in
                //       the checks array. (a) status re-derived --
                //           if (shadersProbed.status == HealthStatus::Ok &&
                //               !std::filesystem::is_directory(shadersProbed.detail))
                //               shadersProbed.status = HealthStatus::Fail;
                //       -> "FAIL: /health still reports the shader directory it recorded,
                //       after that directory has been deleted" with "shaders after
                //       deleting the directory: fail". (b) detail re-derived --
                //           if (::access(shadersProbed.detail.c_str(), R_OK) != 0)
                //               shadersProbed.detail += " (missing)";
                //       -> "FAIL: the recorded shader detail is unchanged by the directory
                //       disappearing". Two controls because (a) leaves the detail check
                //       green and (b) leaves the status check green -- they are not one
                //       assertion written twice. Both reverted.
                // COVERAGE-ASSERT: the directory is asserted to be really gone at the
                //       moment of the second request. A remove_all that silently failed
                //       would leave this checking that /health still answers ok about a
                //       directory that still exists, which is no check at all.
                std::error_code ec;
                std::filesystem::remove_all(shaderDir, ec);
                check(!std::filesystem::exists(shaderDir),
                      "the shader directory really was removed before /health was asked again");

                const HttpReply after = daemon.health();
                if (!after.ok()) {
                    check(false, "/health still answers after its shader directory is deleted");
                } else {
                    check(healthCheckStatus(after.json, "shaders") == "ok",
                          "/health still reports the shader directory it recorded, after that directory has been "
                          "deleted");
                    check(healthCheckDetail(after.json, "shaders") == healthCheckDetail(before.json, "shaders"),
                          "the recorded shader detail is unchanged by the directory disappearing");
                    if (healthCheckStatus(after.json, "shaders") != "ok") {
                        std::fprintf(stderr, "  shaders after deleting the directory: %s (%s)\n",
                                     healthCheckStatus(after.json, "shaders").c_str(),
                                     healthCheckDetail(after.json, "shaders").c_str());
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------ //
    // (f) the two faults do not look alike.
    // ------------------------------------------------------------------ //
    //
    // GOAL: S7.10a, stated the way the goal states it -- "false if a missing shader
    //       directory and an undecodable image produce the same output", because those
    //       need different responses from whoever is reading. One is a broken
    //       installation, the other is one bad file.
    // WHY-THIS-SHAPE: the two scenarios are compared against each other rather than each
    //       against a literal. Both end with the aggregate at "fail" and both end with
    //       `wallpapers` failing -- a render that cannot find its shaders also fails the
    //       monitor -- so a per-scenario assertion on the aggregate, or on wallpapers,
    //       would be satisfied by an endpoint that reported one undifferentiated failure
    //       for both. What has to differ is the per-dependency breakdown, which needs both
    //       bodies held at once. The comparison is on the two named verdicts rather than
    //       on the whole body: the details embed the renderer's own error strings, which
    //       differ between any two faults whatever the endpoint does with them, so
    //       "the bodies are not byte-identical" would be true of a /health that reported
    //       one undifferentiated failure and pasted the message in.
    // CONTROL: the `shaders` and `gpu` entries in http_server.cpp's checks array both
    //       pointed at `&wallpapers`, so the breakdown stops being a breakdown -> "FAIL: a
    //       missing shader directory and an undecodable image get different shader
    //       verdicts" and "FAIL: ... different GPU verdicts", both scenarios reading
    //       fail/fail. Reverted.
    if (shaderFailHealth.is_object() && imageFailHealth.is_object()) {
        check(healthCheckStatus(shaderFailHealth, "shaders") != healthCheckStatus(imageFailHealth, "shaders"),
              "a missing shader directory and an undecodable image get different shader verdicts");
        check(healthCheckStatus(shaderFailHealth, "gpu") != healthCheckStatus(imageFailHealth, "gpu"),
              "a missing shader directory and an undecodable image get different GPU verdicts");
        if (healthCheckStatus(shaderFailHealth, "shaders") == healthCheckStatus(imageFailHealth, "shaders")) {
            std::fprintf(stderr, "  shader fault: %s\n  image fault:  %s\n", shaderFailHealth["checks"].dump().c_str(),
                         imageFailHealth["checks"].dump().c_str());
        }
    } else {
        notVerified("S7.10a (two different faults are distinguishable)",
                    "one of the two fault scenarios could not be constructed on this host, so there was nothing "
                    "to compare against");
    }

    // ------------------------------------------------------------------ //
    // (g) a GPU that breaks after it has already worked, and the recovery from it.
    // ------------------------------------------------------------------ //
    runGpuFailureChecks(bluePath, base, check);

    // WHAT REMAINS UNCOVERED OF S7.10e, stated rather than left to be inferred from an
    // absence. The goal itself records the limit: GpuFailure is thrown from
    // Device::immediateSubmit and nowhere else, so a device-level fault in
    // OffscreenTargetManager's image or render-pass creation, or in pipeline creation,
    // still leaves /health blaming `wallpapers`. Scenario (g) below fails a submit, which
    // reaches every GpuFailure site there is; it cannot reach the ones that do not exist.
    //
    // Also uncovered: the other two throw sites inside immediateSubmit --
    // vkAllocateCommandBuffers and vkWaitForFences. Both are interposable exactly as
    // vkQueueSubmit is, and both were deliberately left alone. A failed wait is the one
    // fault that is not survivable: work is genuinely in flight, so immediateSubmit's
    // cleanup would destroy a fence and command buffer the GPU still owns, and the
    // recovery half of (g) would be measuring a process that is already undefined.
}

// ---------------------------------------------------------------------------- //
// Phase 2c -- S7.10e, by failing one real driver call.
// ---------------------------------------------------------------------------- //

// What a manager render is *made of*, in vkQueueSubmit calls: the three stages
// renderPending performs in order -- ensureGpu + acquireTarget, the texture upload inside
// StaticRenderer's constructor, and OffscreenTargetManager::render.
//
// Measured by driving those same three production pieces directly, on this thread, with
// the interposer armed around each stage separately. Nothing here is a stand-in: they are
// the classes renderPending itself constructs and calls.
struct SubmitSplit {
    int setup = 0;   // Device + OffscreenTargetManager + target creation
    int upload = 0;  // StaticRenderer's constructor
    int render = 0;  // OffscreenTargetManager::render
    bool measured = false;
    std::string reason;

    int total() const { return setup + upload + render; }
};

SubmitSplit measureSubmitSplit(const std::string& imagePath, const std::string& shaderDir, uint32_t width,
                               uint32_t height, bool enableValidation) {
    SubmitSplit split;
    if (shaderDir.empty()) {
        split.reason = "the daemon published no resolved shader directory, so the same StaticRenderer it built "
                       "cannot be built here";
        return split;
    }
    try {
        // Declared in the order they must be destroyed in reverse: the renderer holds
        // images and a pipeline on the Device, and the targets hold images on it too.
        std::optional<Device> device;
        std::optional<OffscreenTargetManager> targets;
        const OffscreenTarget* target = nullptr;
        std::optional<StaticRenderer> renderer;
        {
            // Both constructions are inside the window, because ensureGpu() runs both
            // inside the manager's -- a submit in either would otherwise be missing from
            // the split and show up as a permanent mismatch.
            SubmitFault setupCount(/*failAt=*/0);
            device.emplace(enableValidation);
            targets.emplace(*device);
            target = &targets->create(0, width, height);
            split.setup = setupCount.observed();
        }
        {
            SubmitFault uploadCount(/*failAt=*/0);
            renderer.emplace(*device, targets->renderPass(), imagePath, shaderDir);
            split.upload = uploadCount.observed();
        }
        {
            SubmitFault renderCount(/*failAt=*/0);
            targets->render(*renderer, *target);
            split.render = renderCount.observed();
        }
        split.measured = true;
    } catch (const std::exception& e) {
        // X.3: this is the harness failing to build its own instrument. It is reported as
        // the reason the phase could not run, never as a verdict on /health.
        split.reason = std::string("the submit split could not be measured: ") + e.what();
    }
    return split;
}

void runGpuFailureChecks(const std::string& bluePath, const WallpaperManager::Config& base, const Check& check) {
    // Pass 1 -- count. The interposer observes and fails nothing, so `submitsPerRender`
    // is what a *successful* first render of a fresh daemon costs. Nothing is asserted
    // about the number: it is measured here precisely so that pass 2 does not have to
    // name it.
    int submitsPerRender = 0;
    bool countPassRendered = false;
    std::string resolvedShaderDir;
    std::string countPassReason = "no frame could be rendered on this host, so there is no working GPU to break";
    {
        IsolatedDaemon daemon(base);
        if (!daemon.ready()) {
            countPassReason = daemon.reason();
        } else {
            SubmitFault counting(/*failAt=*/0);
            daemon.manager().requestWallpaper("static", bluePath, 0);
            daemon.manager().renderPending(0);
            counting.disarm();
            submitsPerRender = counting.observed();
            const WallpaperManager::Snapshot snap = daemon.manager().snapshot();
            const auto it = snap.monitors.find(0);
            countPassRendered = it != snap.monitors.end() && it->second.hasFrame;
            // Taken from the daemon rather than resolved again here: the split below has to
            // build the same StaticRenderer this manager built, and base.shaderDir is empty
            // (resolve-at-first-use), so re-deriving it would be a second implementation.
            const HttpReply health = daemon.health();
            if (health.ok() && healthCheckStatus(health.json, "shaders") == "ok") {
                resolvedShaderDir = healthCheckDetail(health.json, "shaders");
            }
        }
    }

    if (!countPassRendered || submitsPerRender < 2) {
        // X.3: a host that cannot render, or a render whose shape leaves nothing to
        // spare, is not a defect in /health -- and grading it would be grading a
        // scenario that was never built.
        notVerified("S7.10e (a GPU that fails after working reads fail, and a commit clears it)",
                    countPassRendered
                        ? "one render costs " + std::to_string(submitsPerRender) +
                              " vkQueueSubmit call(s), so there is no earlier submit to let through and the "
                              "texture upload's own pre-existing catch would be the one that recorded the failure"
                        : countPassReason);
        return;
    }

    // GOAL: the fault about to be injected lands inside OffscreenTargetManager::render, not
    //       inside the texture upload. Everything below is about the `catch (GpuFailure&)`
    //       around `m_targets->render(...)`; a fault that landed on the upload is caught by
    //       the pre-existing arm around ensureGpu/emplace, records gpu=fail through a
    //       different code path, and produces a byte-identical /health body -- so every
    //       assertion below passes while the arm under test never runs.
    // WHY-THIS-SHAPE: the manager's render is decomposed into the three stages
    //       renderPending performs, each measured by driving the same production class
    //       directly, and the aim is trusted only if they sum to what the manager cost and
    //       the render stage contributed at least one submit. The simpler alternative --
    //       "observed == submitsPerRender", which is what stood here -- cannot do this: the
    //       aim IS submitsPerRender, so injected == 1 already implies the count reached it,
    //       and the equality adds only "no further submit followed". What the split cannot
    //       measure is the *order* of the three stages; renderPending fixes that by
    //       construction, since it cannot render before the renderer it renders with exists.
    // CONTROL: two edits to offscreen.cpp made the upload the last submit -- a dummy
    //       immediateSubmit added to OffscreenTargetManager's constructor, and the one in
    //       its render() skipped. Without this guard that world passed entirely green, the
    //       four checks below reading exactly as on a healthy run while the render's catch
    //       was never entered. With it: exit 77, "OffscreenTargetManager::render made 0
    //       vkQueueSubmit call(s), the texture upload 1 and device/target setup 1, which is
    //       2 against the 2 a whole manager render costs". Both edits reverted.
    // COVERAGE-ASSERT: `split.render >= 1` is deliberately not folded into the sum. Under
    //       that control the sum balanced (1 + 1 + 0 == 2) and only the render stage's own
    //       count fell short. The sum has its own control -- the upload stage's measurement
    //       dropped, giving "the texture upload 0 ... which is 1 against the 2" -- so
    //       neither clause is carried by the other.
    const SubmitSplit split =
        measureSubmitSplit(bluePath, resolvedShaderDir, base.width, base.height, base.enableValidation);
    if (!split.measured || split.render < 1 || split.total() != submitsPerRender) {
        notVerified("S7.10e (a GPU that fails after working reads fail, and a commit clears it)",
                    !split.measured
                        ? split.reason
                        : "the aimed submit cannot be attributed to the render: "
                          "OffscreenTargetManager::render made " +
                              std::to_string(split.render) + " vkQueueSubmit call(s), the texture upload " +
                              std::to_string(split.upload) + " and device/target setup " +
                              std::to_string(split.setup) + ", which is " + std::to_string(split.total()) +
                              " against the " + std::to_string(submitsPerRender) +
                              " a whole manager render costs -- so submit " + std::to_string(submitsPerRender) +
                              " is not the render's, and failing it would exercise the upload's catch instead");
        return;
    }

    IsolatedDaemon daemon(base);
    if (!daemon.ready()) {
        notVerified("S7.10e (a GPU that fails after working reads fail, and a commit clears it)", daemon.reason());
        return;
    }

    // GOAL: S7.10e -- `gpu: ok` means the GPU worked recently, not that a device was once
    //       created. A GpuFailure out of the render's submit re-records the check as
    //       failed. False if a device that dies after the first successful render still
    //       reads ok, which is a watchdog being told a blank desktop is healthy.
    // WHY-THIS-SHAPE: the fault is aimed by a two-pass count, not at a literal submit
    //       index. Aiming at "submit 2" would couple this to production's internal call
    //       sequence, and a refactor merging the texture upload into the render would
    //       silently re-aim it while the check stayed green. Failing *every* submit --
    //       the obvious alternative -- proves nothing at all: StaticRenderer's
    //       constructor uploads its texture first, so it throws before m_targets->render()
    //       is ever reached, and the pre-existing catch around ensureGpu/emplace records
    //       gpu=fail identically. Both variants then print the same body, and the catch
    //       this scenario exists for never runs.
    // CONTROL: the `catch (const GpuFailure& e)` added around `m_targets->render(...)` in
    //       wallpaper_manager.cpp deleted, so the failure falls through to the plain
    //       std::exception arm -> "FAIL: a GPU that fails a submit after the device was
    //       created reads fail, not ok" and "FAIL: the gpu detail names the driver call
    //       that failed", with gpu reading {ok, "NVIDIA GeForce RTX 3080 Ti (Heap)"}
    //       while every render had failed. Reverted.
    // COVERAGE-ASSERT: `injected == 1` is the load-bearing one -- if dispatch ever moves to
    //       volk or vkGetDeviceProcAddr the interposer is bypassed, and without it the
    //       phase would inject nothing and grade a successful render. `observed ==
    //       submitsPerRender` is NOT independent of it and does not pin the fault to the
    //       render: the aim is submitsPerRender, so injection implies the count reached it.
    //       What it adds is only that no submit followed the failed one -- a silent retry
    //       inside renderPending would clear the record before /health is asked. The
    //       attribution to the render is established by the split guard above, not here.
    SubmitFault fault(/*failAt=*/submitsPerRender);
    daemon.manager().requestWallpaper("static", bluePath, 0);
    daemon.manager().renderPending(0);
    fault.disarm();

    if (fault.injected() != 1 || fault.observed() != submitsPerRender) {
        // Deliberately a skip and not a check(): with nothing injected the assertions
        // below would be grading a *successful* render and would go red naming the
        // daemon, which is X.3's exact prohibition -- the fault would be in this file's
        // aim or in the loader's dispatch, not in /health.
        notVerified("S7.10e (a GPU that fails after working reads fail, and a commit clears it)",
                    "the interposed vkQueueSubmit did not land where it was aimed -- " +
                        std::to_string(fault.observed()) + " submit(s) observed and " +
                        std::to_string(fault.injected()) + " fault(s) injected, against " +
                        std::to_string(submitsPerRender) +
                        " counted for the same render; either the render's submit sequence changed or this "
                        "executable's vkQueueSubmit is no longer what production calls");
        return;
    }

    const WallpaperManager::Snapshot broken = daemon.manager().snapshot();
    const auto brokenMonitor = broken.monitors.find(0);
    const HttpReply failed = daemon.health();
    if (!failed.ok()) {
        check(false, "the failed-submit daemon answered /health with a parseable 200");
        return;
    }

    check(brokenMonitor != broken.monitors.end() && !brokenMonitor->second.hasFrame,
          "the render whose submit was failed committed no frame");
    check(healthCheckStatus(failed.json, "gpu") == "fail",
          "a GPU that fails a submit after the device was created reads fail, not ok");
    check(healthCheckDetail(failed.json, "gpu").find("vkQueueSubmit") != std::string::npos,
          "the gpu detail names the driver call that failed");
    // Attribution, and it is what separates "gpu recorded the failure" from "everything
    // was blanket-failed": the shader directory resolved fine and is untouched by this.
    check(healthCheckStatus(failed.json, "shaders") == "ok",
          "a failing submit does not implicate the shaders");
    if (healthCheckStatus(failed.json, "gpu") != "fail") {
        std::fprintf(stderr, "  after failing submit %d of %d: gpu %s (%s)\n", submitsPerRender, submitsPerRender,
                     healthCheckStatus(failed.json, "gpu").c_str(), healthCheckDetail(failed.json, "gpu").c_str());
    }
    std::printf("http_server_test: submit %d of %d failed (setup %d + upload %d + render %d) -> %s\n",
                submitsPerRender, submitsPerRender, split.setup, split.upload, split.render, failed.body.c_str());
    std::fflush(stdout);

    // GOAL: S7.10e's other direction -- a successful commit clears the recorded failure,
    //       and clears the *text* with it. False if a recovered GPU reports status ok
    //       alongside detail "Device::immediateSubmit: vkQueueSubmit failed", which is a
    //       reader being handed two contradictory facts and no way to date either.
    // WHY-THIS-SHAPE: the detail is compared byte for byte against the string the manager
    //       itself would build from its published GpuStatus -- the same expression
    //       ensureGpu records on success. The obvious alternative, asserting the detail no
    //       longer contains "vkQueueSubmit", is satisfied by a commit that cleared the
    //       detail to the empty string or replaced it with any other wording, so it would
    //       not have caught the bare `m_health.gpu.status = Ok` this replaced -- that
    //       version left the failure text in place, but so would a dozen wrong repairs.
    // CONTROL: the recovery's `recordHealthLocked(&DaemonHealth::gpu, Ok, ...)` in
    //       wallpaper_manager.cpp put back to `m_health.gpu.status = HealthStatus::Ok;`
    //       -> "FAIL: a recovered GPU carries the detail ensureGpu writes, not the text of
    //       the failure it recovered from" with "  gpu after recovery: ok
    //       (Device::immediateSubmit: vkQueueSubmit failed) (expected NVIDIA GeForce RTX
    //       3080 Ti (Heap))". The status check above it stayed green, which is the point:
    //       the two are not one assertion written twice. Reverted.
    // COVERAGE-ASSERT: the expected string is required to be non-trivial before it is
    //       compared. On a manager reporting no GPU name it would be " ()", and an
    //       equality check against that establishes nothing.
    daemon.manager().requestWallpaper("static", bluePath, 0);
    daemon.manager().renderPending(0);
    const WallpaperManager::Snapshot recoveredSnap = daemon.manager().snapshot();
    const auto recoveredMonitor = recoveredSnap.monitors.find(0);
    const bool committed = recoveredMonitor != recoveredSnap.monitors.end() && recoveredMonitor->second.hasFrame;
    const std::string expectGpuDetail =
        recoveredSnap.gpu.name + " (" + recoveredSnap.gpu.descriptorTier + ")";
    if (!committed || recoveredSnap.gpu.name.empty()) {
        notVerified("S7.10e (a commit clears the recorded GPU failure)",
                    committed ? "the manager publishes no GPU name, so \"the detail ensureGpu writes\" is an "
                                "empty string and comparing against it would establish nothing"
                              : "the render after the injected fault committed no frame, so there was no "
                                "successful commit to clear anything");
        return;
    }

    const HttpReply recovered = daemon.health();
    if (!recovered.ok()) {
        check(false, "the recovered-GPU daemon answered /health with a parseable 200");
        return;
    }
    check(healthCheckStatus(recovered.json, "gpu") == "ok",
          "a later successful commit clears the recorded GPU failure");
    check(healthCheckDetail(recovered.json, "gpu") == expectGpuDetail,
          "a recovered GPU carries the detail ensureGpu writes, not the text of the failure it recovered from");
    if (healthCheckStatus(recovered.json, "gpu") != "ok" ||
        healthCheckDetail(recovered.json, "gpu") != expectGpuDetail) {
        std::fprintf(stderr, "  gpu after recovery: %s (%s) (expected %s)\n",
                     healthCheckStatus(recovered.json, "gpu").c_str(),
                     healthCheckDetail(recovered.json, "gpu").c_str(), expectGpuDetail.c_str());
    }

    // GOAL: S7.10e reaches the export too -- exportCurrentFrame is GPU work (it submits an
    //       ownership-release barrier), so a GpuFailure there is the GPU going wrong and
    //       must be recorded as such. False if a daemon whose every GetFrameFd dies in the
    //       driver still reports gpu ok.
    // WHY-THIS-SHAPE: this runs immediately after the two checks above have established
    //       gpu == ok on this same daemon, so the fail below cannot be left over from the
    //       render fault -- the ok is the barrier that makes it attributable to the
    //       export. Aiming at the first submit after arming is exact rather than
    //       index-coupled here: exportFrame issues exactly one, and if it ever issues none
    //       the injected count catches it.
    // CONTROL: the `try`/`catch (const GpuFailure&)` around wallpaperx::exportFrame in
    //       WallpaperManager::exportCurrentFrame reduced to the bare call it replaced ->
    //       "FAIL: a frame export that fails in the driver is recorded against the GPU",
    //       gpu still reading {ok, "NVIDIA GeForce RTX 3080 Ti (Heap)"} after the export
    //       had thrown. Reverted.
    {
        SubmitFault exportFault(/*failAt=*/1);
        std::string exportError;
        bool threw = false;
        try {
            wallpaperx::FrameExport exported = daemon.manager().exportCurrentFrame(0);
            // Only reached if the fault missed. The caller owns the fd (exportFrame's
            // contract), and leaking one out of a phase that then keeps running is X.4's
            // business, not something to leave to process exit.
            if (exported.handle && exported.handle->frame.fd >= 0) ::close(exported.handle->frame.fd);
        } catch (const std::exception& e) {
            threw = true;
            exportError = e.what();
        }
        exportFault.disarm();

        if (exportFault.injected() != 1) {
            notVerified("S7.10e (a failed frame export is attributed to the GPU)",
                        "exportCurrentFrame made " + std::to_string(exportFault.observed()) +
                            " vkQueueSubmit call(s) with the interposer armed on the first, so no fault reached "
                            "the export path on this host" +
                            (threw ? " (it threw for another reason: " + exportError + ")" : ""));
        } else {
            const HttpReply afterExport = daemon.health();
            if (!afterExport.ok()) {
                check(false, "the failed-export daemon answered /health with a parseable 200");
            } else {
                check(threw && exportError.find("vkQueueSubmit") != std::string::npos,
                      "exportCurrentFrame propagates the driver failure instead of returning a frame");
                check(healthCheckStatus(afterExport.json, "gpu") == "fail",
                      "a frame export that fails in the driver is recorded against the GPU");
                if (healthCheckStatus(afterExport.json, "gpu") != "fail") {
                    std::fprintf(stderr, "  gpu after a failed export: %s (%s); export threw: %s\n",
                                 healthCheckStatus(afterExport.json, "gpu").c_str(),
                                 healthCheckDetail(afterExport.json, "gpu").c_str(),
                                 threw ? exportError.c_str() : "no");
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------- //
// Phase 3 -- the monitor query parameter.
// ---------------------------------------------------------------------------- //
void runMonitorParamChecks(httplib::Client& cli, const std::string& recordedPath, const Check& check) {
    // The documented single-argument form, which must keep working: PLAN.md documents
    // /config?t=<ts> with no way to ask for a monitor, and `monitor` was added as an
    // optional extra defaulting to 0.
    const HttpReply documented = get(cli, "/config?t=1234567890");
    check(documented.reached && documented.status == 200, "/config?t=<ts> answers 200 with no monitor parameter");
    check(!documented.json.is_discarded() && documented.json.value("monitor_id", -1) == 0,
          "/config with no monitor parameter answers for monitor 0");
    check(!documented.json.is_discarded() && documented.json.value("path", std::string()) == recordedPath,
          "/config with no monitor parameter reports monitor 0's recorded path");

    // In-range ids, including both boundaries.
    //
    // The echoed monitor_id and the empty-monitor answer are checked together because one
    // mutation breaks both, and it is the plausible one: a handler that looked up a fixed
    // monitor instead of the requested one.
    // CONTROL: `outMonitorId = static_cast<int>(parsed);` replaced with
    //       `outMonitorId = 0;` -> "FAIL: /config echoes back the monitor id it was asked
    //       about" (twice), "FAIL: a monitor nothing has been set on answers with empty
    //       type and path", "FAIL: /config reports no type for a monitor GetStatus has no
    //       entry for", and the torn-read coverage assert, which then sees only one state.
    //       Reverted.
    for (const int id : {0, 1, 31}) {
        const HttpReply res = get(cli, "/config?monitor=" + std::to_string(id));
        check(res.reached && res.status == 200,
              "an in-range monitor id is answered with 200");
        check(!res.json.is_discarded() && res.json.value("monitor_id", -1) == id,
              "/config echoes back the monitor id it was asked about");
        if (!(res.reached && res.status == 200)) {
            std::fprintf(stderr, "  monitor=%d answered %d\n", id, res.status);
        }
    }

    // A monitor nothing has been set on: empty strings and 200, not 404. The plugin polls
    // this once a second from session start, and a fresh monitor is the ordinary state.
    const HttpReply unset = get(cli, "/config?monitor=" + std::to_string(kMonitorNeverSet));
    check(!unset.json.is_discarded() && unset.json.value("type", std::string("x")).empty() &&
              unset.json.value("path", std::string("x")).empty(),
          "a monitor nothing has been set on answers with empty type and path");

    // GOAL: S7.5 -- an id outside [0, kMaxMonitors) is rejected, not clamped and not
    //       silently answered as monitor 0. A plugin asking for monitor 5 on a
    //       two-monitor machine has a bug; showing it monitor 0's wallpaper puts the
    //       wrong image on a screen the user is looking at with nothing saying why.
    // WHY-THIS-SHAPE: "status == 400" alone does not separate rejected from clamped --
    //       an implementation that answered 400 *and* filled in monitor 0's wallpaper
    //       would satisfy it. So the body is checked too: monitor 0 has a distinctive
    //       recorded path by this point, and it must not appear in any rejection.
    //       COVERAGE-ASSERT below pins that the path really is distinctive, or the
    //       body check would be trivially true.
    // CONTROL: two. (a) `parsed >= kMaxMonitors` weakened to `parsed > kMaxMonitors` ->
    //       "FAIL: an out-of-range monitor id is rejected with 400" with "monitor=32 (one
    //       past the last valid id) answered 200", while every in-range case stayed
    //       green. (b) the rejection replaced with `outMonitorId = 0; return true;`, i.e.
    //       clamping -> both that FAIL and "FAIL: a rejected monitor id is not answered
    //       with monitor 0's wallpaper", the body showing monitor 0's recorded path for
    //       monitor=-1. Both reverted.
    check(!recordedPath.empty() && recordedPath.size() > 8,
          "the recorded monitor-0 path is distinctive enough for the not-clamped check to mean something");
    struct OutOfRange {
        const char* value;
        const char* why;
    };
    constexpr OutOfRange kOutOfRange[] = {
        {"-1", "negative"},
        {"32", "one past the last valid id"},
        {"33", "past the end"},
        {"2147483648", "past int range but inside long"},
        {"99999999999999999999", "past long range, so stol throws rather than returning"},
    };
    for (const OutOfRange& bad : kOutOfRange) {
        const HttpReply res = get(cli, std::string("/config?monitor=") + bad.value);
        check(res.reached && res.status == 400, "an out-of-range monitor id is rejected with 400");
        check(res.reached && res.body.find(recordedPath) == std::string::npos,
              "a rejected monitor id is not answered with monitor 0's wallpaper");
        if (!(res.reached && res.status == 400)) {
            std::fprintf(stderr, "  monitor=%s (%s) answered %d: %s\n", bad.value, bad.why, res.status,
                         res.body.c_str());
        }
    }

    // GOAL: S7.6 -- a `monitor` value that is not wholly an integer is rejected. The C
    //       library's parsers stop at the first non-digit and report success, so leniency
    //       here is the default and has to be actively prevented.
    // WHY-THIS-SHAPE: each value is a separate case rather than one representative,
    //       because they are rejected by different halves of the parser -- "0abc", "1 "
    //       and "0x10" parse a number and leave bytes over, so only the end-of-input
    //       check catches them, while "", "abc" and "--1" are rejected by from_chars
    //       itself. A single case would leave one half untested.
    // CONTROL: the `end != raw.data() + raw.size()` half of the guard disabled -> seven
    //       "FAIL: a monitor value that is not wholly an integer is rejected with 400"
    //       lines, one each for "0abc", "1%20", "1%09", "1.0", "0x10", "1e0" and "0%00"
    //       (0abc, 0x10 and 0%00 came back as monitor 0, carrying its recorded path),
    //       while "", "abc" and "--1" stayed green. Reverted.
    constexpr const char* kNotIntegers[] = {
        "0abc", "1%20", "1%09", "", "1.0", "0x10", "abc", "--1", "1e0", "0%00",
    };
    for (const char* value : kNotIntegers) {
        const HttpReply res = get(cli, std::string("/config?monitor=") + value);
        check(res.reached && res.status == 400, "a monitor value that is not wholly an integer is rejected with 400");
        if (!(res.reached && res.status == 400)) {
            std::fprintf(stderr, "  monitor=%s answered %d: %s\n", value, res.status, res.body.c_str());
        }
    }

    // GOAL: S7.6 again, on the half a did-it-consume-everything guard cannot reach: a
    //       value whose non-integer part comes *first* is rejected too. These reach the
    //       parser as " 1" (%20, and a bare '+', which httplib decodes to a space in a
    //       query value), "\t1" and "+1" (%2B) -- none wholly an integer, so a 200 makes
    //       the goal false.
    // WHY-THIS-SHAPE: kept as its own loop rather than folded into the values above,
    //       because these four fail through a path the others never touch. A parser built
    //       on the C library's stol reports the whitespace and sign it skipped as
    //       *consumed*, so a length guard sees a full-length parse and admits " 1" while
    //       still rejecting "1 " -- leading-vs-trailing asymmetry, not rejection count,
    //       is what this block pins, and merging it would hide which side was tested.
    //       "%2B1" is spelled encoded because a raw '+' arrives as a space and would test
    //       the whitespace case a second time instead of the leading-sign one.
    // CONTROL: parseMonitorParam's std::from_chars replaced with the std::stol form it
    //       superseded (parse in a try/catch, then `consumed != raw.size()`) -> four
    //       "FAIL: a monitor value whose leading characters are not digits is rejected
    //       with 400" lines: `monitor=%201 answered 200: {"monitor_id":1,...}` and the
    //       same for "+1", "%2B1" and "%091", with every value in the loop above still
    //       green. Reverted.
    for (const char* value : {"%201", "+1", "%2B1", "%091"}) {
        const HttpReply res = get(cli, std::string("/config?monitor=") + value);
        check(res.reached && res.status == 400,
              "a monitor value whose leading characters are not digits is rejected with 400");
        if (!(res.reached && res.status == 400)) {
            std::fprintf(stderr, "  monitor=%s answered %d: %s\n", value, res.status, res.body.c_str());
        }
    }
}

// ---------------------------------------------------------------------------- //
// Phase 4 -- /config cannot report a torn view of one monitor.
// ---------------------------------------------------------------------------- //
void runTornReadChecks(WallpaperManager& manager, httplib::Client& cli, const std::string& staticPath,
                       const std::string& videoPath, const Check& check) {
    // GOAL: S7.3 -- /config and GetStatus cannot disagree about a monitor, which is
    //       false if either reads the same state through separate lock acquisitions that
    //       another writer can commit between. This half establishes the *reader* side:
    //       a single /config response can never mix the type of one request with the path
    //       of another.
    // WHY-THIS-SHAPE: reading both channels once and comparing them -- the obvious check,
    //       and the one in runAgreementChecks -- passes for two independent reads that
    //       happen to agree, and cannot see a torn read at all. So a writer thread flips
    //       one monitor between two states that differ in BOTH fields, and every response
    //       is classified: a response is valid only if it is wholly one state or wholly
    //       the other. ("static", videoPath) is unreachable under any interleaving of a
    //       correct implementation, because requestWallpaper writes both fields under one
    //       m_stateMutex acquisition -- so this is an invariant, not a claim about
    //       scheduling (X.2). The writer drives WallpaperManager directly rather than
    //       SetWallpaper, so no render is queued and no GPU work is involved.
    // CONTROL: the /config handler split into two snapshots --
    //           const auto typeSnap = m_manager.snapshot();   // type read from this one
    //           const auto pathSnap = m_manager.snapshot();   // path read from this one
    //       -> "FAIL: no /config response ever mixes the type of one request with the
    //       path of another" with "280 of 400 responses were torn, e.g. type=video
    //       path=.../http_blue.png". Reverted.
    // COVERAGE-ASSERT: the two counters below. If the writer never ran, or the flip was
    //       too slow to overlap the reads, every response is one state and the check
    //       would be vacuously green -- so both states must actually be observed, and the
    //       number of responses is asserted rather than left to whatever the loop
    //       managed.
    constexpr int kReads = 400;

    // Recorded once BEFORE the writer starts, and it is load-bearing rather than tidy: a
    // monitor nothing has been set on answers with empty strings, which is neither of the
    // two states below and was therefore counted as torn. The first read of the loop can
    // legitimately beat the writer's first request, so without this the check failed
    // roughly one run in eight -- on the harness's own startup order, not on anything the
    // daemon did. Pre-recording makes "" unreachable for this monitor, which is what lets
    // a non-matching pair mean a genuine mix.
    manager.requestWallpaper("static", staticPath, kMonitorTorn);

    std::atomic<bool> writerFailed{false};
    std::string writerError;
    {
        std::jthread writer([&](std::stop_token stop) {
            try {
                while (!stop.stop_requested()) {
                    manager.requestWallpaper("static", staticPath, kMonitorTorn);
                    manager.requestWallpaper("video", videoPath, kMonitorTorn);
                }
            } catch (const std::exception& e) {
                writerError = e.what();
                writerFailed.store(true);
            } catch (...) {
                writerError = "unknown exception";
                writerFailed.store(true);
            }
        });

        int sawStatic = 0;
        int sawVideo = 0;
        int torn = 0;
        int answered = 0;
        std::string firstTorn;
        const std::string path = "/config?monitor=" + std::to_string(kMonitorTorn);
        for (int i = 0; i < kReads; ++i) {
            const HttpReply res = get(cli, path);
            if (!res.reached || res.status != 200 || res.json.is_discarded()) continue;
            ++answered;
            const std::string type = res.json.value("type", std::string());
            const std::string reported = res.json.value("path", std::string());
            if (type == "static" && reported == staticPath) {
                ++sawStatic;
            } else if (type == "video" && reported == videoPath) {
                ++sawVideo;
            } else {
                ++torn;
                if (firstTorn.empty()) firstTorn = "type=" + type + " path=" + reported;
            }
        }
        writer.request_stop();
        writer.join();

        check(!writerFailed.load(), "the writer driving the torn-read check ran without throwing");
        if (writerFailed.load()) std::fprintf(stderr, "  writer error: %s\n", writerError.c_str());

        check(torn == 0, "no /config response ever mixes the type of one request with the path of another");
        if (torn != 0) {
            std::fprintf(stderr, "  %d of %d responses were torn, e.g. %s\n", torn, answered, firstTorn.c_str());
        }
        check(answered >= kReads / 2, "at least half the torn-read probes got a parseable 200");
        check(sawStatic > 0 && sawVideo > 0,
              "both of the writer's states were actually observed, so the torn-read check had something to tear");
        std::printf("http_server_test: torn-read phase: %d responses, %d static, %d video, %d torn\n", answered,
                    sawStatic, sawVideo, torn);
        std::fflush(stdout);
    }
}

// ---------------------------------------------------------------------------- //
// Phase 5 -- the two channels, on a monitor whose path is hostile.
// ---------------------------------------------------------------------------- //
void runAgreementChecks(httplib::Client& cli, sdbus::IProxy& proxy, const std::string& hostilePath,
                        const Check& check) {
    // The fixture has to be hostile, or both checks below are trivially true of a daemon
    // that escapes nothing. Asserted rather than assumed: the filename is built from
    // constants, but a filesystem that refused one of these bytes would leave the phase
    // silently testing an ordinary path.
    check(hostilePath.find('\n') != std::string::npos, "the fixture path really contains a newline");
    check(hostilePath.find(kDel) != std::string::npos, "the fixture path really contains DEL");
    check(hostilePath.find(kBidiOverride) != std::string::npos, "the fixture path really contains U+202E");

    const HttpReply config = get(cli, "/config?monitor=" + std::to_string(kMonitorAgree));
    const nlohmann::json status = getStatus(proxy);
    const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorAgree));

    if (!config.reached || config.json.is_discarded() || entry == nullptr) {
        check(false, "both channels answered for the monitor whose path is hostile");
        return;
    }

    const std::string httpType = config.json.value("type", std::string());
    const std::string httpPath = config.json.value("path", std::string());
    const std::string dbusType = entry->value("type", std::string());
    const std::string dbusPath = entry->value("path", std::string());

    // GOAL: S7.3/S7.4 -- the same client-supplied string reaching two consumers must
    //       arrive the same way. A value escaped on one channel and raw on the other is
    //       precisely the defect aded2e5 fixed for GetStatus.
    // WHY-THIS-SHAPE: the comparison is done on a path containing U+202E, DEL and a
    //       newline rather than on an ordinary one, because two channels that both
    //       return the raw string also agree -- an ordinary path makes the equality
    //       check unfalsifiable. The second assertion is what keeps the first honest: it
    //       pins that the agreed-on value is the *escaped* one, so "both raw" cannot
    //       satisfy the pair. Read after a quiesced render (its FrameReady has arrived
    //       and nothing else is queued), so a difference cannot be two channels honestly
    //       observing two different moments.
    // CONTROL: two, one per assertion. (a) sanitizeForLog dropped from the /config
    //       handler's path (`out["path"] = it->second.path;`) -> both "FAIL: /config and
    //       GetStatus report the same path for the same monitor" and "FAIL: /config's
    //       path is escaped, not the raw client string", the http side printing the raw
    //       U+202E. (b) the sanitize cap in http_server.cpp changed from 4096 to 8, which
    //       keeps the escaping but truncates -> the agreement check went red and the
    //       escaping check stayed green, so the two are not one check twice. Both
    //       reverted.
    check(httpType == dbusType, "/config and GetStatus report the same type for the same monitor");
    check(httpPath == dbusPath, "/config and GetStatus report the same path for the same monitor");
    check(httpPath.find('\n') == std::string::npos && httpPath.find(kDel) == std::string::npos &&
              httpPath.find(kBidiOverride) == std::string::npos,
          "/config's path is escaped, not the raw client string");
    if (httpPath != dbusPath) {
        std::fprintf(stderr, "  http: %s\n  dbus: %s\n", httpPath.c_str(), dbusPath.c_str());
    }

    // A monitor neither channel has a record of must also agree: /config answers with
    // empty strings, GetStatus has no entry at all. Two different shapes for the same
    // fact, which is fine -- what would not be is /config inventing a type.
    const HttpReply unset = get(cli, "/config?monitor=" + std::to_string(kMonitorNeverSet));
    check(!unset.json.is_discarded() && unset.json.value("type", std::string("x")).empty(),
          "/config reports no type for a monitor GetStatus has no entry for");
    check(wallpaperStatus(status, std::to_string(kMonitorNeverSet)) == nullptr,
          "GetStatus has no entry for the monitor /config reports as empty");
}

// ---------------------------------------------------------------------------- //
// Phase 6 -- /health once a device exists.
// ---------------------------------------------------------------------------- //
void runHealthAfterDeviceChecks(httplib::Client& cli, sdbus::IProxy& proxy, const Check& check) {
    const HttpReply health = get(cli, "/health");
    const nlohmann::json status = getStatus(proxy);
    if (!health.reached || health.json.is_discarded() || status.is_discarded()) {
        check(false, "/health and GetStatus both answered after the first render");
        return;
    }

    // GOAL: S7.10b's other direction -- the two dependency checks that read `pending` on a
    //       daemon that had done nothing read `ok` once the daemon has actually used them.
    //       Without this, every `!= "ok"` assertion earlier in the run is satisfied by an
    //       endpoint that can only ever answer `pending`. (The third check, `wallpapers`,
    //       is not ok on this manager and is graded below; runHealthStateChecks scenarios
    //       (a) and (c) are where a reachable `wallpapers: ok` and a reachable `ok`
    //       aggregate are established.)
    // WHY-THIS-SHAPE: the GPU verdict is cross-checked against GetStatus rather than
    //       asserted alone. `gpu.initialized` and `checks.gpu` are two views of one fact
    //       recorded at one place in ensureGpu, and the failure worth catching is them
    //       disagreeing -- a client told by one channel that the graphics stack is up and
    //       by the other that nothing has been tried has no way to decide which to
    //       believe. Nothing here asserts a tier *name*: NVIDIA resolves Heap, Lavapipe
    //       Buffer and WALLPAPERX_FORCE_TIER=classic makes it Classic, so a literal would
    //       be a check on the host.
    // CONTROL: the GPU is recorded ok in TWO places -- ensureGpu, and renderPending's
    //       commit under the lock it already holds -- so deleting either alone leaves this
    //       green, and that was observed before it was written down: with only ensureGpu's
    //       record removed, gpu still read ok here (the commit had set it) and only the
    //       detail check below went red. Both removed together -> "FAIL: /health reports
    //       the GPU as ok once a device exists" with "GetStatus: initialized=true
    //       /health: gpu=pending". The aggregate is `fail` on this manager whichever way
    //       the mutation goes, so it moves only the gpu line. The shaders half is separate:
    //       `recordHealth(&DaemonHealth::shaders, HealthStatus::Ok, ...)` deleted ->
    //       "FAIL: /health reports the shader directory as ok once a render has resolved
    //       it". All reverted.
    const bool gpuUp = status.contains("gpu") && status["gpu"].is_object() &&
                       status["gpu"].value("initialized", false);
    check(gpuUp, "GetStatus reports an initialised GPU after the first render");
    check(healthCheckStatus(health.json, "gpu") == "ok", "/health reports the GPU as ok once a device exists");
    if (gpuUp != (healthCheckStatus(health.json, "gpu") == "ok")) {
        std::fprintf(stderr, "  GetStatus: initialized=%s  /health: gpu=%s\n", gpuUp ? "true" : "false",
                     healthCheckStatus(health.json, "gpu").c_str());
    }
    if (!gpuUp) return;

    check(healthCheckStatus(health.json, "shaders") == "ok",
          "/health reports the shader directory as ok once a render has resolved it");

    // GOAL: S7.10g, on the run's shared manager -- one monitor showing nothing is enough
    //       to fail the endpoint even though both dependencies are up and another monitor
    //       genuinely has a frame on it. False if the working monitors outvote the blank
    //       one, which is how a user with one dead screen gets told the daemon is healthy.
    // WHY-THIS-SHAPE: the expected verdict is derived from the GetStatus reply taken
    //       alongside, not written as a literal. What this manager holds by now is the sum
    //       of every phase above -- a rendered monitor, one the torn-read writer left on a
    //       `video`, one only ever recorded -- and a hand-written expectation would have to
    //       be re-derived whenever a phase above touches an id; the version written on that
    //       day is the one that agrees with whatever /health happened to answer. Deriving
    //       it also makes the two channels answer the same question: GetStatus's
    //       `unsupported` key and this check are both isRenderableType, so a monitor one
    //       channel calls unrenderable is the monitor the other must name. Read at the same
    //       quiesced moment as /health (the last FrameReady has arrived, nothing is
    //       queued), so a disagreement cannot be two honest views of two moments.
    // CONTROL: two, because the verdict and the cause fail independently. (a)
    //       `firstUnsupported` dropped from the wallpapers detail (`failed > 0 ?
    //       firstFailure : std::string()`) -> only "FAIL: the wallpapers detail names the
    //       monitor GetStatus reports as unrenderable", with "detail: 1 of 3 monitors show
    //       nothing:" and nothing after the colon; both status checks stayed green. (b) the
    //       `unsupported > 0` term dropped from the fail condition -> "FAIL: a monitor set
    //       to a type this build cannot render fails the wallpapers check", "FAIL: one
    //       blank monitor fails the aggregate even with every dependency ok" and the naming
    //       check, /health answering {pending, "1 of 3 monitors have not rendered yet"}.
    //       The count check survives both, since "1 of 3" also appears in that pending
    //       detail; it was seen red under a third -- the whole `!isRenderableType` arm
    //       deleted, giving "2 of 3 monitors have not rendered yet". All reverted.
    // COVERAGE-ASSERT: the mix is required before anything is graded. If this run left the
    //       manager with no unrenderable monitor, or with none showing a frame, the
    //       "one blank monitor outvotes the working ones" claim has nothing in it and the
    //       phase reports NOT VERIFIED rather than passing on a state it did not build.
    std::size_t failedCount = 0, unsupportedCount = 0, showingCount = 0, monitorCount = 0;
    int firstUnsupportedId = -1;
    std::string firstUnsupportedType;
    if (status.contains("wallpapers") && status["wallpapers"].is_object()) {
        for (const auto& [key, entry] : status["wallpapers"].items()) {
            if (!entry.is_object()) continue;
            ++monitorCount;
            const int id = std::atoi(key.c_str());
            if (entry.contains("last_error")) {
                ++failedCount;
            } else if (entry.value("unsupported", false)) {
                ++unsupportedCount;
                if (firstUnsupportedId < 0 || id < firstUnsupportedId) {
                    firstUnsupportedId = id;
                    firstUnsupportedType = entry.value("type", std::string());
                }
            } else if (entry.value("has_frame", false)) {
                ++showingCount;
            }
        }
    }

    if (failedCount != 0 || unsupportedCount == 0 || showingCount == 0) {
        notVerified("S7.10g (a blank monitor fails the endpoint next to a working one)",
                    "this run left the shared manager without the mix the check needs (" +
                        std::to_string(monitorCount) + " monitors: " + std::to_string(showingCount) +
                        " showing, " + std::to_string(unsupportedCount) + " unrenderable, " +
                        std::to_string(failedCount) + " failed)");
    } else {
        check(healthCheckStatus(health.json, "wallpapers") == "fail",
              "a monitor set to a type this build cannot render fails the wallpapers check");
        check(health.json.value("status", std::string()) == "fail",
              "one blank monitor fails the aggregate even with every dependency ok");

        const std::string detail = healthCheckDetail(health.json, "wallpapers");
        const std::string expectCount =
            std::to_string(failedCount + unsupportedCount) + " of " + std::to_string(monitorCount);
        check(detail.find(expectCount) != std::string::npos,
              "the wallpapers detail counts the blank monitors against the monitor total");
        const bool named = detail.find("monitor " + std::to_string(firstUnsupportedId)) != std::string::npos &&
                           detail.find("'" + firstUnsupportedType + "'") != std::string::npos;
        check(named, "the wallpapers detail names the monitor GetStatus reports as unrenderable");
        if (!named || detail.find(expectCount) == std::string::npos) {
            std::fprintf(stderr, "  detail: %s\n  expected '%s', 'monitor %d' and '%s'\n", detail.c_str(),
                         expectCount.c_str(), firstUnsupportedId, firstUnsupportedType.c_str());
        }
    }

    // The GPU detail has to be about *this* GPU, not a fixed word. Compared against the
    // tier GetStatus reports rather than against a literal, for the reason above; the
    // detail is what a reader gets instead of the descriptor_tier key this endpoint used
    // to carry, so a detail that named neither the device nor the tier would quietly drop
    // the only GPU identification /health has.
    // CONTROL: the detail replaced with a literal (`recordHealth(&DaemonHealth::gpu,
    //       HealthStatus::Ok, "gpu")`) -> "FAIL: the GPU check's detail names the device
    //       GetStatus reports" with "detail: gpu  dbus: NVIDIA GeForce RTX 3080 Ti /
    //       Heap", and nothing else red -- the status checks above stayed green, so this
    //       is not one of them restated. Reverted.
    const std::string dbusTier = status["gpu"].value("descriptor_tier", std::string());
    const std::string dbusName = status["gpu"].value("name", std::string());
    const std::string gpuDetail = healthCheckDetail(health.json, "gpu");
    check(!dbusTier.empty() && !dbusName.empty(),
          "GetStatus reports a GPU name and tier for the detail check to be compared against");
    check(!dbusTier.empty() && gpuDetail.find(dbusTier) != std::string::npos && !dbusName.empty() &&
              gpuDetail.find(dbusName) != std::string::npos,
          "the GPU check's detail names the device GetStatus reports");
    if (gpuDetail.find(dbusTier) == std::string::npos || gpuDetail.find(dbusName) == std::string::npos) {
        std::fprintf(stderr, "  detail: %s  dbus: %s / %s\n", gpuDetail.c_str(), dbusName.c_str(), dbusTier.c_str());
    }
    std::printf("http_server_test: /health after the first render: %s\n", health.body.c_str());
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------- //
// Phase 7 -- nothing served here changes anything.
// ---------------------------------------------------------------------------- //
void runReadOnlyChecks(httplib::Client& cli, sdbus::IProxy& proxy, const Check& check) {
    // GOAL: S7.2 -- nothing served here mutates daemon state. Any process on the machine
    //       can reach a loopback port with no authentication at all, where the DBus
    //       surface is at least bounded by the session bus, so a route that writes would
    //       be an unauthenticated way to change what every monitor displays.
    // WHY-THIS-SHAPE: two halves, because either alone is weak. Asserting only that
    //       POST/PUT/DELETE are not routed would pass for a GET handler that mutated;
    //       asserting only that state is unchanged would pass for a mutating POST route
    //       whose write happened to be idempotent. The state comparison is a full
    //       GetStatus reply compared byte for byte, taken when the daemon is quiesced
    //       (the last render's FrameReady has arrived and nothing else is queued), so a
    //       difference cannot be an in-flight render.
    // CONTROL: a mutating route added to registerRoutes --
    //           m_server->Post("/config", [this](const auto& req, auto& res) {
    //               m_manager.requestWallpaper("video", req.body, 0); res.status = 200; });
    //       -> "FAIL: POST /config is not a route this server serves" and "FAIL: no
    //       sequence of HTTP requests changed what GetStatus reports". Reverted.
    const std::string before = getStatus(proxy).dump();

    struct MethodCase {
        const char* name;
        int status;
    };
    std::vector<MethodCase> methods;
    {
        const httplib::Result post = cli.Post("/config", "type=video&path=/etc/passwd", "text/plain");
        methods.push_back({"POST /config", post ? post->status : 0});
        const httplib::Result put = cli.Put("/config", "{}", "application/json");
        methods.push_back({"PUT /config", put ? put->status : 0});
        const httplib::Result del = cli.Delete("/config");
        methods.push_back({"DELETE /config", del ? del->status : 0});
        const httplib::Result postHealth = cli.Post("/health", "", "text/plain");
        methods.push_back({"POST /health", postHealth ? postHealth->status : 0});
    }
    for (const MethodCase& m : methods) {
        // 404 (httplib's answer for an unrouted method+path) and 405 are both refusals.
        // What must not happen is a 2xx, which would mean the route exists.
        check(m.status == 404 || m.status == 405, "a non-GET method is not a route this server serves");
        if (!(m.status == 404 || m.status == 405)) {
            std::fprintf(stderr, "  %s answered %d\n", m.name, m.status);
        }
    }

    // A battery of ordinary and awkward GETs on top, so the state comparison covers the
    // routes that do exist as well as the methods that do not.
    for (const char* path : {"/config", "/config?t=1", "/config?monitor=0", "/config?monitor=99", "/health",
                             "/", "/config/../health", "/config?monitor=0&monitor=1"}) {
        (void)get(cli, path);
    }

    const std::string after = getStatus(proxy).dump();
    check(before == after, "no sequence of HTTP requests changed what GetStatus reports");
    if (before != after) {
        std::fprintf(stderr, "  before: %s\n  after:  %s\n", before.c_str(), after.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 8 -- a poll never waits on a render.
// ---------------------------------------------------------------------------- //
void runGpuLockChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, HttpServer& server, const std::string& largePath,
                      const Check& check) {
    // GOAL: S7.12 -- polling at the plugin's cadence never waits on a render. The lock
    //       split exists for this: snapshot() takes m_stateMutex only, while a render
    //       holds m_gpuMutex for its whole duration (decode included, and the decode is
    //       the long part). False the moment anything in the handler path touches the GPU
    //       lock.
    // WHY-THIS-SHAPE: the tempting version -- "a /config request completes in under N
    //       milliseconds" -- is an assertion on the scheduler and goes red on a loaded
    //       machine (X.2). What is asserted instead is that polls kept completing WHILE a
    //       render was in flight, and specifically in the second half of that window: a
    //       handler that took m_gpuMutex would block on its first request until the
    //       render finished, so its completions would all fall before the render started.
    //       The window is bounded by real events at both ends (the SetWallpaper call and
    //       that request's own FrameReady), never by a sleep.
    //       If the render is too fast on this host to be conclusive, the phase says so
    //       and is not graded -- a fast render must not be graded as a passing poll.
    // CONTROL: `(void)m_manager.validationCounter();` added at the top of the /config
    //       handler -- one line, and it takes m_gpuMutex exactly as an accidental GPU
    //       touch would -> "FAIL: polls kept completing while a render was in flight"
    //       and "FAIL: polls kept completing in the second half of the render window",
    //       with "0 of 1 polls completed inside the 63.8 ms render window" -- the
    //       poller's single request spent the whole render blocked on m_gpuMutex.
    //       Reverted.
    // COVERAGE-ASSERT: the completion counts are asserted, not just their existence, and
    //       the window length is printed, so a future edit that shortens the render or
    //       stops the poller cannot leave this quietly green.
    constexpr auto kMinConclusiveWindow = std::chrono::milliseconds(20);
    constexpr int kMinPollsInWindow = 5;
    constexpr int kMinPollsInSecondHalf = 3;

    uint64_t generation = 0;
    {
        const nlohmann::json status = getStatus(proxy);
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorGpuLock));
        generation = entry != nullptr ? entry->value("generation", uint64_t{0}) : 0;
    }

    Poller poller(server.port(), "/config?monitor=" + std::to_string(kMonitorGpuLock));

    const Clock::time_point start = Clock::now();
    setWallpaper(proxy, "static", largePath, kMonitorGpuLock);
    const bool arrived = signals.wait(kMonitorGpuLock, generation + 1);
    const Clock::time_point end = Clock::now();
    poller.stop();

    if (!arrived) {
        // X.1: the wait expiring is not data. Nothing below runs on it.
        notVerified("S7.12 (a poll never waits on a render)",
                    "the large-image render never signalled FrameReady, so there was no render window to poll across");
        return;
    }
    check(poller.error().empty(), "the /config poller ran without throwing");
    if (!poller.error().empty()) std::fprintf(stderr, "  poller error: %s\n", poller.error().c_str());

    const auto window = end - start;
    if (window < kMinConclusiveWindow) {
        notVerified("S7.12 (a poll never waits on a render)",
                    "the render window was only " + std::to_string(millis(window)) +
                        " ms, too short for a blocked poll to be distinguishable from a slow one");
        return;
    }

    const Clock::time_point midpoint = start + window / 2;
    int inWindow = 0;
    int inSecondHalf = 0;
    int malformed = 0;
    for (const Poller::Sample& s : poller.samples()) {
        if (s.completed < start || s.completed > end) continue;
        ++inWindow;
        if (s.completed > midpoint) ++inSecondHalf;
        if (!s.reached || s.status != 200 || !s.bodyParsed) ++malformed;
    }

    std::printf("http_server_test: render window %.1f ms, %d polls completed inside it (%d in its second half)\n",
                millis(window), inWindow, inSecondHalf);
    std::fflush(stdout);

    check(inWindow >= kMinPollsInWindow, "polls kept completing while a render was in flight");
    check(inSecondHalf >= kMinPollsInSecondHalf, "polls kept completing in the second half of the render window");
    check(malformed == 0, "every poll answered during a render was a parseable 200");
    if (inWindow < kMinPollsInWindow) {
        std::fprintf(stderr, "  %d of %zu polls completed inside the %.1f ms render window\n", inWindow,
                     poller.samples().size(), millis(window));
    }
}

// ---------------------------------------------------------------------------- //
// Phase 9 -- destruction stops serving.
// ---------------------------------------------------------------------------- //
//
// CONCERN, recorded here because this phase is where it would bite and it is a finding
// about production, not about the test: httplib's Server::stop() is a no-op while
// is_running_ is still false, and listen_internal() sets that flag only once the serving
// thread reaches it. ~HttpServer calls stop() and then joins unconditionally, so a
// server destroyed within microseconds of construction -- before its thread is scheduled
// -- can leave stop() doing nothing and the join waiting forever. httplib exposes
// wait_until_ready() for exactly this. Every server this file destroys has served at
// least one request first, which is what makes the join safe here; no shipped check
// depends on the race, and none provokes it.
void runShutdownChecks(WallpaperManager& manager, const Check& check) {
    std::string reason;
    std::unique_ptr<HttpServer> server = startServer(manager, reason);
    if (!server) {
        notVerified("S7.8/S7.11 (the serving thread stops before the daemon does)", reason);
        return;
    }
    const int port = server->port();

    // GOAL: S7.11 -- a request in flight cannot read state that is being torn down:
    //       shutdown stops serving before anything the handlers read is destroyed. The
    //       in-process half of it -- ~HttpServer stops the listener and joins the thread,
    //       so by the time it returns nothing is being served and the manager it reads is
    //       still alive.
    // WHY-THIS-SHAPE: "destroy it and assert nothing crashed" is not falsifiable -- a
    //       server that kept serving would also not crash, because the manager outlives
    //       it here. So the port is probed afterwards and must be REFUSED, which
    //       distinguishes "stopped" from "still running". Requests are in flight across
    //       the destruction (the poller) rather than before it, because a quiesced
    //       shutdown is the easy case and the one that would pass anyway.
    // CONTROL: ~HttpServer's body replaced with `m_server.release(); m_thread.detach();`
    //       -- the guard dropped, and the server leaked rather than freed so the failure
    //       is the property under test and not a dangling pointer -> "FAIL: the port
    //       stops accepting connections once the server is destroyed", with "connect to
    //       127.0.0.1:51731 connected (expected refused)". Dropping the guard WITHOUT
    //       the leak (plain detach) instead ends the run at SIGSEGV, since the detached
    //       thread runs on inside a destroyed httplib::Server -- also red, and worth
    //       knowing, but it names the wrong fault. Removing only the stop() hangs in the
    //       join instead of going red, which is why the control drops both halves.
    //       Reverted.
    // NOTE on the join: if a future change makes ~HttpServer hang, this phase hangs and
    //       ctest reports a timeout. That is not a pass, but it is not a FAIL line
    //       either; the CONCERN above is the known way that could happen.
    {
        httplib::Client cli = makeClient(port);
        const HttpReply health = get(cli, "/health");
        check(health.reached && health.status == 200, "the server about to be destroyed was serving first");
        if (!health.reached) {
            notVerified("S7.8/S7.11 (the serving thread stops before the daemon does)",
                        "the freshly started server never answered, so its destruction proves nothing");
            return;
        }
    }

    Poller poller(port, "/config?monitor=0");
    // Give the poller a chance to get requests in flight before the destructor runs. A
    // wait, not a barrier, and it is only about making the phase interesting -- every
    // assertion below is graded on what the poller actually recorded, and the sample
    // count is asserted, so a poller that never started fails rather than passes.
    // recorded(), not samples().size(): the poller thread is pushing to that vector right
    // now, and reading its size concurrently is a data race with undefined behaviour --
    // in a harness whose whole job is to survive its own error paths and report.
    for (int i = 0; i < 400 && poller.recorded() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    server.reset();  // ~HttpServer: stop() then join()

    poller.stop();
    check(poller.error().empty(), "the poller across the destruction ran without throwing");

    // Counted over the poller's whole life. Attempts made after the destruction come back
    // unreached (the port is gone) and are skipped rather than counted as failures --
    // being refused once the server is destroyed is the property two checks below, not a
    // fault. What is asserted here is that the poller really was being answered, so the
    // malformed count below is a statement about served responses and not about zero of
    // them.
    int answered = 0;
    int malformed = 0;
    for (const Poller::Sample& s : poller.samples()) {
        if (!s.reached) continue;
        ++answered;
        if (s.status != 200 || !s.bodyParsed) ++malformed;
    }
    check(answered >= 3, "the poller really was being answered up to the moment of destruction");
    check(malformed == 0, "every request the server answered while being torn down was a parseable 200");
    if (malformed != 0) {
        std::fprintf(stderr, "  %d of %d responses were not a parseable 200\n", malformed, answered);
    }

    const ConnectResult after = tryConnect("127.0.0.1", port, 1000);
    check(after == ConnectResult::Refused, "the port stops accepting connections once the server is destroyed");
    if (after != ConnectResult::Refused) {
        std::fprintf(stderr, "  connect to 127.0.0.1:%d %s (expected refused)\n", port, describe(after));
    }
}

// ---------------------------------------------------------------------------- //
// Phase 10 -- an idle client cannot hold the shutdown open.
// ---------------------------------------------------------------------------- //

// One complete HTTP/1.1 response off `fd`, bounded by `timeoutMs`. False on timeout, on
// EOF before the response finished, or on a reply with no Content-Length -- never a
// partial read reported as a whole one, because the caller uses "a second response
// arrived" as its proof that the connection was reused.
bool readOneResponse(int fd, int timeoutMs, std::string& out) {
    out.clear();
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    size_t headerEnd = std::string::npos;
    long contentLength = -1;
    for (;;) {
        if (headerEnd != std::string::npos && contentLength >= 0 &&
            out.size() >= headerEnd + 4 + static_cast<size_t>(contentLength)) {
            return true;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) return false;

        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) return false;
        char buf[1024];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<size_t>(n));

        if (headerEnd == std::string::npos) {
            headerEnd = out.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                static constexpr const char* kHeader = "Content-Length: ";
                const size_t at = out.find(kHeader);
                if (at == std::string::npos || at > headerEnd) return false;
                contentLength = std::strtol(out.c_str() + at + std::strlen(kHeader), nullptr, 10);
            }
        }
    }
}

void runKeepAliveShutdownChecks(WallpaperManager& manager, const Check& check) {
    std::string reason;
    std::unique_ptr<HttpServer> server = startServer(manager, reason);
    if (!server) {
        notVerified("S7.17 (an idle keep-alive client cannot hold shutdown open)", reason);
        return;
    }
    const int port = server->port();

    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        notVerified("S7.17 (an idle keep-alive client cannot hold shutdown open)", "could not create a socket");
        return;
    }
    // Closed on every path including the early returns below (X.4). Declared before the
    // measurement so the descriptor cannot outlive the phase that opened it.
    struct FdGuard {
        int fd;
        ~FdGuard() {
            if (fd >= 0) ::close(fd);
        }
    } guard{fd};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        notVerified("S7.17 (an idle keep-alive client cannot hold shutdown open)",
                    "could not connect to the server this phase just started");
        return;
    }

    const std::string request = "GET /health HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                "\r\nConnection: keep-alive\r\n\r\n";
    std::string response;
    bool exchanged = true;
    for (int i = 0; i < 2 && exchanged; ++i) {
        exchanged = ::send(fd, request.data(), request.size(), MSG_NOSIGNAL) ==
                        static_cast<ssize_t>(request.size()) &&
                    readOneResponse(fd, 5000, response) && response.find(" 200 ") != std::string::npos;
    }
    if (!exchanged) {
        // X.3, and it matters here more than usual: without a live idle connection the
        // measurement below would be timing an ordinary shutdown with nothing to hold it
        // open, which is exactly the state that cannot fail.
        notVerified("S7.17 (an idle keep-alive client cannot hold shutdown open)",
                    "the two requests needed to prove the connection was kept alive did not both complete, so "
                    "no idle keep-alive connection was ever established");
        return;
    }

    // GOAL: S7.17's fast row -- a client holding an idle keep-alive connection cannot make
    //       the daemon take materially longer to stop. False if ~HttpServer's join waits on
    //       a worker parked in httplib's keep_alive() poll: shutdown would go from instant
    //       to seconds per connection, and nothing else in this suite would notice.
    // WHY-THIS-SHAPE: httplib is vendored, so this pins a *dependency's* behaviour. 0.50.1
    //       checks svr_sock == INVALID_SOCKET inside keep_alive()'s loop (httplib.h:5738),
    //       which is upstream issue #1959; a version bump that lost it would regress this
    //       silently. Two requests, not one, and on a raw socket rather than an
    //       httplib::Client: a Client transparently reconnects when the server closed the
    //       connection, so a second successful Get through one would prove nothing about
    //       keep-alive. A second response on the same fd is positive proof the connection
    //       was reused, which is what makes the connection being *open* at the moment of
    //       destruction a fact rather than an assumption.
    //       Deliberately does NOT cover the dribbling client, which S7.17 records as a
    //       decided non-goal at 199.52 s and rising -- and which, per that goal, a test
    //       must never leave open across a teardown.
    // CONTROL: the `if (svr_sock == INVALID_SOCKET) { break; }` arm removed from
    //       keep_alive() in backend/third_party/httplib.h, reproducing upstream #1959 ->
    //       "FAIL: an idle keep-alive connection does not hold the server's destruction
    //       open" with "  ~HttpServer took 5006.43 ms with one idle keep-alive connection
    //       open (budget 1000 ms)", against 10.24 ms unmutated. Reverted.
    // Threshold: the healthy cost is one 10 ms keep-alive poll slice
    // (CPPHTTPLIB_KEEPALIVE_TIMEOUT_CHECK_INTERVAL_USECOND) plus a thread join, and the
    // regression is bounded below by CPPHTTPLIB_KEEPALIVE_TIMEOUT_SECOND = 5 s, which
    // http_server.cpp does not override. 1000 ms sits 100x above the first and 5x below
    // the second, so neither a slow host nor the scheduler can move it across (X.2): which
    // side of the destructor the worker reaches keep_alive() on changes the answer by at
    // most one 10 ms slice.
    constexpr double kShutdownBudgetMs = 1000.0;
    const Clock::time_point before = Clock::now();
    server.reset();  // ~HttpServer: stop() then join()
    const double elapsedMs = millis(Clock::now() - before);

    check(elapsedMs < kShutdownBudgetMs,
          "an idle keep-alive connection does not hold the server's destruction open");
    if (elapsedMs >= kShutdownBudgetMs) {
        std::fprintf(stderr, "  ~HttpServer took %.2f ms with one idle keep-alive connection open (budget %.0f ms)\n",
                     elapsedMs, kShutdownBudgetMs);
    }
    std::printf("http_server_test: shutdown with one idle keep-alive connection open: %.2f ms\n", elapsedMs);
    std::fflush(stdout);
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

    try {
        const std::string bluePath = writeSolidImage("http_blue.png", 0, 0, 255);
        const std::string videoPath = writeSolidImage("http_video.mp4", 255, 0, 0);
        const std::string hostilePath = writeHostileNamedImage();
        const std::string junkPath = writeUndecodableFile("http_not_an_image.png");
        // 2400x1800. Large enough that stb's decode dominates the render on a fast GPU,
        // small enough that writing it costs about a second. If it is still too fast to
        // be conclusive, runGpuLockChecks says so rather than grading it.
        const std::string largePath = writeLargeImage("http_large.png", 2400, 1800);

        // Declared before the proxy and the service so it outlives both: the FrameReady
        // callback runs on the proxy's dispatch thread, and a log destroyed first would
        // be locked by a signal still in flight during teardown.
        FrameReadyLog signals;

        WallpaperManager::Config config;
        config.width = kTargetWidth;
        config.height = kTargetHeight;
        // Off, matching the shipped daemon. This file claims no GPU-side oracle -- see
        // the header comment -- so turning validation on would cost time and buy a
        // warning count nobody here grades.
        config.enableValidation = false;
        WallpaperManager manager(config);

        auto serviceConn = sdbus::createSessionBusConnection(sdbus::ServiceName{DbusService::kBusName});
        DbusService service(*serviceConn, manager);
        serviceConn->enterEventLoopAsync();

        auto proxy =
            sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
        proxy->uponSignal("FrameReady")
            .onInterface(DbusService::kInterface)
            .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

        std::string reason;
        std::unique_ptr<HttpServer> server = startServer(manager, reason);
        if (!server) {
            // A harness failure, not a daemon failure (X.3): every check below needs a
            // listening server, so none of them runs and none of them is graded.
            notVerified("every Step 7 goal", reason);
        } else {
            std::printf("http_server_test: serving on 127.0.0.1:%d (never the shipped 18462, so a running daemon "
                        "and a parallel ctest entry cannot collide with this run)\n",
                        server->port());
            std::fflush(stdout);

            httplib::Client cli = makeClient(server->port());

            runBindChecks(manager, *proxy, *server, check);

            // Before anything renders: the whole point of the S7.10b pre-device check is
            // that nothing has been tried yet on this manager, so `pending` is the true
            // answer for all three checks.
            runHealthBeforeDeviceChecks(cli, check);

            // Runs on managers of its own, so it neither needs nor disturbs the state
            // above -- and so it can be reached on a host with no Vulkan at all, where the
            // main render below never happens.
            runHealthStateChecks(bluePath, videoPath, junkPath, check);

            // Recorded through the manager rather than through SetWallpaper, so no render
            // is queued and the "before any device" state above survives into this phase.
            manager.requestWallpaper("static", bluePath, kMonitorParam);
            runMonitorParamChecks(cli, bluePath, check);

            runTornReadChecks(manager, cli, bluePath, videoPath, check);

            // First render of the run. Everything after it needs a Device to exist.
            setWallpaper(*proxy, "static", hostilePath, kMonitorAgree);
            const bool rendered = signals.wait(kMonitorAgree, 1);
            if (!rendered) {
                // X.3 again: on a host with no working Vulkan this is not a fault of the
                // HTTP server, and grading the four phases below would produce a page of
                // FAIL lines pointing at the wrong component.
                const nlohmann::json status = getStatus(*proxy);
                const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorAgree));
                notVerified("S7.3 (channel agreement), S7.4 (escaping), S7.2 (read-only), "
                            "S7.10b (/health reads ok once the daemon has used its dependencies), "
                            "S7.12 (poll never waits on a render)",
                            "no frame was ever rendered on this host" +
                                (entry != nullptr && entry->contains("last_error")
                                     ? " -- last_error: " + entry->value("last_error", std::string())
                                     : std::string()));
            } else {
                runAgreementChecks(cli, *proxy, hostilePath, check);
                runHealthAfterDeviceChecks(cli, *proxy, check);
                runReadOnlyChecks(cli, *proxy, check);
                runGpuLockChecks(*proxy, signals, *server, largePath, check);
            }

            // Destroyed before the manager it reads, which is the ordering main.cpp gets
            // from declaring the HttpServer last.
            server.reset();
        }

        runShutdownChecks(manager, check);
        runKeepAliveShutdownChecks(manager, check);
    } catch (const std::exception& e) {
        // Without this an escaping sdbus::Error -- what a host with no session bus
        // produces -- would abort the process, so ctest would report an opaque SIGABRT
        // instead of the named failures above.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++failures;
    }

    if (failures > 0) {
        // Asked before the not-verified count, so a run that is both broken and
        // incomplete reports as broken rather than hiding behind ctest's "Skipped".
        std::fprintf(stderr, "http_server_test: %d check(s) failed\n", failures);
        return 1;
    }
    if (g_notVerified > 0) {
        std::printf("http_server_test: every check that ran passed, but %d goal group(s) could not be exercised "
                    "on this host (listed above) -- reported to ctest as a skip, not a pass.\n",
                    g_notVerified);
        return kNotVerifiedExitCode;
    }
    std::printf("http_server_test: all checks passed\n");
    return 0;
}
