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
// (S7.10, S7.12) need a device to exist, and the daemon ships with validation off;
// dbus_service_test and daemon_system_test own that oracle.

#include "daemon/dbus_service.h"
#include "daemon/http_server.h"
#include "daemon/wallpaper_manager.h"

#include "tests/client/daemon_client.h"
#include "tests/client/http_probe.h"

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <sdbus-c++/sdbus-c++.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using wallpaperx::DbusService;
using wallpaperx::HttpServer;
using wallpaperx::WallpaperManager;
using wallpaperx::testing::Check;
using wallpaperx::testing::ConnectResult;
using wallpaperx::testing::describe;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::get;
using wallpaperx::testing::getStatus;
using wallpaperx::testing::HttpReply;
using wallpaperx::testing::makeClient;
using wallpaperx::testing::Poller;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::tryConnect;
using wallpaperx::testing::wallpaperStatus;

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

// A listening socket the test owns, kept open so the port it holds is genuinely taken.
class HeldPort {
public:
    HeldPort() = default;
    ~HeldPort() { release(); }
    HeldPort(const HeldPort&) = delete;
    HeldPort& operator=(const HeldPort&) = delete;

    // Binds `host`:0 and listens. Returns false (with fd untouched) on any failure.
    bool open(uint32_t hostOrder) {
        release();
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(hostOrder);
        addr.sin_port = 0;
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(m_fd, 4) != 0) {
            release();
            return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            release();
            return false;
        }
        m_port = ntohs(addr.sin_port);
        return true;
    }

    void release() {
        if (m_fd >= 0) ::close(m_fd);
        m_fd = -1;
        m_port = -1;
    }

    int port() const { return m_port; }

private:
    int m_fd = -1;
    int m_port = -1;
};

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

    // GOAL: S7.1's other half, and this block deliberately PRINTS rather than asserting.
    //       "Reachable from this machine and no other" assumes the daemon *owns* the
    //       port. It does not: httplib's default_socket_options sets SO_REUSEPORT on
    //       Linux, so any process running as the same user can bind the very port the
    //       daemon is serving on, and the kernel then load-balances incoming connections
    //       between them. Measured against a real daemon on 18462: a second listener
    //       answered 14 of 30 /config polls with a wallpaper path of its own choosing.
    //       The plugin has no way to tell which answered, so this is an unprivileged
    //       local process deciding what the desktop displays -- the exact threat model
    //       S7.2 cites for keeping every route read-only.
    // WHY-THIS-SHAPE: asserting it would ship a permanently red suite for a defect this
    //       agent may not fix, and it is a finding against http_server.cpp rather than a
    //       property this test can restore. Printed with its own label so it cannot read
    //       as a forgotten assertion, and written so that the day HttpServer passes its
    //       own socket options (SO_REUSEADDR only, via Server::set_socket_options) this
    //       print flips to the "now exclusive" branch and should become an assertion.
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        bool hijacked = false;
        if (fd >= 0) {
            int on = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(static_cast<uint16_t>(server.port()));
            hijacked = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(fd, 4) == 0;
            ::close(fd);
        }
        if (hijacked) {
            std::printf("http_server_test: KNOWN DIVERGENCE from S7.1 (defect in http_server.cpp, not in this "
                        "test) -- a second process bound and listened on 127.0.0.1:%d while the daemon was "
                        "serving there. httplib sets SO_REUSEPORT, so the port is not exclusive and an "
                        "unprivileged local process can answer a share of the plugin's polls.\n",
                        server.port());
        } else {
            std::printf("http_server_test: 127.0.0.1:%d is now exclusive -- the SO_REUSEPORT divergence noted "
                        "here has been fixed; turn this print into an assertion.\n",
                        server.port());
        }
        std::fflush(stdout);
    }

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
    check(health.json.value("status", std::string()) == "ok", "/health reports status \"ok\"");

    // Compared against the same project(... VERSION) the daemon library compiles in, not
    // against a literal: what this pins is that /health still *derives* its version from
    // the build, since a hardcoded string is how a user comes to report a bug against a
    // version this binary stopped being two releases ago.
    // CONTROL: `{"version", WALLPAPERX_VERSION}` replaced with `{"version", "9.9.9"}` ->
    //       "FAIL: /health reports the version CMake built this daemon as". Reverted.
    check(health.json.value("version", std::string()) == std::string(WALLPAPERX_EXPECTED_VERSION),
          "/health reports the version CMake built this daemon as");

    // GOAL: S7.10 -- /health reports a descriptor tier exactly when a device exists, and
    //       never invents one. The Device is created lazily on the first SetWallpaper, so
    //       a freshly started daemon has no tier to report; the key is present and null
    //       rather than absent, so a client learns of it from the schema and not by
    //       hitting its absence.
    // WHY-THIS-SHAPE: asserting only "descriptor_tier is present" would pass for a daemon
    //       that guessed a tier before creating a device, which is the failure this goal
    //       exists to prevent -- so the *value* is asserted null, and the paired check
    //       after the first render asserts it is a non-empty string. Neither half is
    //       worth anything alone.
    //       UNRATIFIED, and it CONFLICTS with PLAN.md's Definition of Done #2, which
    //       expects /health to return {"descriptor_tier":"heap"} -- true only after a
    //       wallpaper has been set, and spelled lowercase where the daemon emits "Heap"
    //       from the same toString() GetStatus uses. This asserts what cd28790 does. Do
    //       not read it as a ruling that PLAN.md is wrong.
    // CONTROL: the null branch removed, so the key always carries
    //       snap.gpu.descriptorTier -> "FAIL: /health reports no descriptor tier before a
    //       device exists" (observed: the key came back as ""). Reverted.
    check(health.json.contains("descriptor_tier"), "/health always carries a descriptor_tier key");
    check(health.json.contains("descriptor_tier") && health.json["descriptor_tier"].is_null(),
          "/health reports no descriptor tier before a device exists");
    check(health.json.contains("gpu_initialized") && health.json["gpu_initialized"].is_boolean() &&
              !health.json["gpu_initialized"].get<bool>(),
          "/health reports gpu_initialized=false before a device exists");
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
    //       because they fail through different paths -- "0abc" and "1 " return from
    //       stol successfully with a short `consumed`, "" and "x" throw invalid_argument,
    //       and "0x10" parses as 0 with three bytes left over. A single case would leave
    //       two of the three paths untested.
    // CONTROL: the `consumed != raw.size()` guard disabled -> seven "FAIL: a monitor
    //       value that is not wholly an integer is rejected with 400" lines, one each for
    //       "0abc", "1%20", "1%09", "1.0", "0x10", "1e0" and "0%00" (0abc and 0x10 came
    //       back as monitor 0, carrying its recorded path), while the three cases that
    //       reach stol's invalid_argument -- "", "abc", "--1" -- stayed green. Reverted.
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

    // GOAL: S7.6 again, and this block deliberately PRINTS rather than asserting.
    //       std::stol skips leading whitespace and accepts a leading '+', and httplib
    //       decodes both %20 and '+' in a query value -- so `monitor=%201` and
    //       `monitor=+1` arrive as " 1", which stol consumes entirely and the daemon
    //       accepts as monitor 1. By S7.6's wording ("not wholly an integer is
    //       rejected") that is a divergence from the goal, in the code under test and
    //       not in this file.
    // WHY-THIS-SHAPE: asserting it would ship a permanently red suite for a defect this
    //       agent is not permitted to fix, and quietly widening S7.6 to exclude
    //       whitespace would be closing a gap by weakening the check. Printed with its
    //       own label instead, so it is impossible to mistake for a forgotten assertion,
    //       and reported as a finding against http_server.cpp. If parseMonitorParam
    //       grows a leading-character guard, these become assertions.
    // Leading whitespace and a leading sign only -- TRAILING whitespace is rejected
    // (stol consumes the digits and stops, leaving `consumed` short) and is asserted
    // above as "1%20"/"1%09". The asymmetry is the whole shape of the divergence.
    for (const char* value : {"%201", "+1", "%091"}) {
        const HttpReply res = get(cli, std::string("/config?monitor=") + value);
        if (res.reached && res.status == 200) {
            std::printf("http_server_test: KNOWN DIVERGENCE from S7.6 (defect in http_server.cpp, not in this "
                        "test) -- monitor=%s answered %d: %s. std::stol skips leading whitespace and a leading "
                        "'+', so a value that is not wholly an integer is accepted.\n",
                        value, res.status, res.body.c_str());
        } else {
            std::printf("http_server_test: monitor=%s is now rejected (%d) -- the S7.6 divergence noted here "
                        "has been fixed; turn this print into an assertion.\n",
                        value, res.status);
        }
    }
    std::fflush(stdout);
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

    // GOAL: S7.10's second half -- once a device exists, /health reports its tier, and
    //       reports the SAME tier GetStatus does. Two spellings of one fact would be
    //       S7.3's divergence in another place.
    // WHY-THIS-SHAPE: compared against GetStatus rather than against a literal "Heap".
    //       A literal would pin the check to whichever GPU the machine has -- NVIDIA
    //       resolves Heap, Lavapipe Buffer, and WALLPAPERX_FORCE_TIER=classic makes it
    //       Classic -- so it would have to be relaxed to a non-empty check to run
    //       anywhere, and a non-empty check passes for a daemon that invents a tier.
    //       Equality with the other channel is falsifiable on every host.
    // CONTROL: /health's tier lowercased (`toLower(snap.gpu.descriptorTier)`, which is
    //       what DoD #2's "heap" would need) -> "FAIL: /health reports the same
    //       descriptor tier GetStatus does" with "  http: heap  dbus: Heap". Reverted.
    // NOTE, and it is a real conflict rather than a test convenience: PLAN.md's
    //       Definition of Done #2 expects /health to return {"descriptor_tier":"heap"},
    //       lowercase, on a daemon that has just started. Neither half is true of
    //       cd28790 -- the value is "Heap" and it is null until the first render. This
    //       records what the daemon does; PLAN.md and the daemon disagree and only a
    //       human can settle which moves.
    const bool gpuUp = status.contains("gpu") && status["gpu"].is_object() &&
                       status["gpu"].value("initialized", false);
    check(gpuUp, "GetStatus reports an initialised GPU after the first render");
    check(health.json.value("gpu_initialized", false), "/health reports gpu_initialized=true after the first render");
    if (!gpuUp) return;

    const std::string dbusTier = status["gpu"].value("descriptor_tier", std::string());
    check(!health.json["descriptor_tier"].is_null(), "/health reports a descriptor tier once a device exists");
    const std::string httpTier =
        health.json["descriptor_tier"].is_string() ? health.json["descriptor_tier"].get<std::string>() : std::string();
    check(!httpTier.empty() && httpTier == dbusTier, "/health reports the same descriptor tier GetStatus does");
    if (httpTier != dbusTier) {
        std::fprintf(stderr, "  http: %s  dbus: %s\n", httpTier.c_str(), dbusTier.c_str());
    }
    std::printf("http_server_test: /health descriptor_tier = \"%s\" (PLAN.md's Definition of Done #2 asks for "
                "\"heap\", lowercase, on a freshly started daemon -- this daemon reports the GetStatus spelling "
                "and reports null until the first render; see S7.10)\n",
                httpTier.c_str());
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

            // Before anything renders: the whole point of the S7.10 pre-device check is
            // that no Device exists yet.
            runHealthBeforeDeviceChecks(cli, check);

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
                notVerified("S7.3 (channel agreement), S7.4 (escaping), S7.2 (read-only), S7.10 (tier), "
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
