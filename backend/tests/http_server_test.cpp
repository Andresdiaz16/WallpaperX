// SPDX-License-Identifier: GPL-2.0-or-later

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

#include "daemon/crash_record.h"
#include "daemon/dbus_service.h"
#include "daemon/http_server.h"
#include "daemon/log_escape.h"
#include "daemon/wallpaper_manager.h"

// Driven directly, not through the manager, by the submit-split measurement that S7.10e's
// aim depends on -- see measureSubmitSplit().
#include "renderers/static_renderer.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"

#include "tests/client/daemon_client.h"
#include "tests/client/http_probe.h"
#include "tests/client/scratch_dir.h"
#include "tests/manager_output.h"
#include "tests/test_report.h"

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <vulkan/vulkan.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
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
#include <new>
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
#include <pthread.h>
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
using wallpaperx::testing::getStatusOrExplain;
using wallpaperx::testing::HeldPort;
using wallpaperx::testing::HttpReply;
using wallpaperx::testing::makeClient;
using wallpaperx::testing::nameScreen;
using wallpaperx::testing::Poller;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::StatusReply;
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

// One armed driver call. Three entry points are interposed in this file and the bookkeeping
// is identical for all three, so it is written once: three hand-copied versions are three
// things that can drift, and the one that quietly loses its exception-safe disarm leaves a
// plan armed into an unrelated phase with nothing to say so.
struct VkCallFaultPlan {
    int observed = 0;  // calls made on the arming thread while armed
    int failAt = 0;    // 1-based index of the call to fail; 0 counts and fails nothing
    int injected = 0;  // faults actually returned, so a bypassed interposer is visible
    VkResult result = VK_ERROR_UNKNOWN;  // what the failed call hands back
};

// Returns the result to inject, or VK_SUCCESS for "let this one through" -- which is why no
// interposer here can be asked to inject VK_SUCCESS, and none needs to. Called from the
// interposer bodies, the only things that know their own entry point's signature.
VkResult armedFault(VkCallFaultPlan* plan) {
    if (plan == nullptr) return VK_SUCCESS;
    ++plan->observed;
    if (plan->failAt == 0 || plan->observed != plan->failAt) return VK_SUCCESS;
    ++plan->injected;
    return plan->result;
}

// Arms one interposer for the current thread only, and disarms on every exit path including
// an exception (X.4) -- a plan left armed would inject into an unrelated phase. `slot` is
// the interposer's own thread_local pointer; taking its address is safe because this is
// always a stack object, so the arm and the disarm happen on one thread.
class VkCallFault {
public:
    VkCallFault(VkCallFaultPlan** slot, int failAt, VkResult result) : m_slot(slot), m_previous(*slot) {
        m_plan.failAt = failAt;
        m_plan.result = result;
        *slot = &m_plan;
    }
    ~VkCallFault() { disarm(); }
    VkCallFault(const VkCallFault&) = delete;
    VkCallFault& operator=(const VkCallFault&) = delete;

    // Restores whatever was armed before rather than clearing, so a nested fault cannot
    // silently disarm an outer one still in scope. Idempotent: after an explicit disarm()
    // the destructor's own call is a no-op.
    void disarm() {
        if (*m_slot == &m_plan) *m_slot = m_previous;
    }

    int observed() const { return m_plan.observed; }
    int injected() const { return m_plan.injected; }

private:
    VkCallFaultPlan** m_slot;
    VkCallFaultPlan m_plan;
    VkCallFaultPlan* m_previous;
};

thread_local VkCallFaultPlan* t_submitFault = nullptr;

// Named wrapper rather than a bare VkCallFault at each site: the result is fixed for this
// entry point (see the interposer below), and four of the five uses arm only to count, where
// naming a result would read as though one were going to be injected.
class SubmitFault {
public:
    explicit SubmitFault(int failAt) : m_fault(&t_submitFault, failAt, VK_ERROR_DEVICE_LOST) {}

    void disarm() { m_fault.disarm(); }
    int observed() const { return m_fault.observed(); }
    int injected() const { return m_fault.injected(); }

private:
    VkCallFault m_fault;
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

    if (const VkResult injected = armedFault(t_submitFault); injected != VK_SUCCESS) return injected;

    auto* realFn = reinterpret_cast<PFN_vkQueueSubmit>(real);
    if (realFn == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    return realFn(queue, submitCount, pSubmits, fence);
}

// ---------------------------------------------------------------------------- //
// Failing the frame export's driver call, from the test binary
// ---------------------------------------------------------------------------- //
//
// Publishing a rendered frame is the render's job, so exportFrame submits nothing and the
// lever above cannot reach it. What an export does do is ask the driver for a file
// descriptor onto the image's memory, and that call failing is the driver going wrong --
// which the daemon must record against the GPU rather than swallow.
//
// The lever is vkGetDeviceProcAddr, not vkGetMemoryFdKHR. Defining vkGetMemoryFdKHR in this
// binary would never be reached: renderer/vulkan/frame_export.cpp calls it through the
// pointer the loader hands back, not through the dynamic symbol. vkGetDeviceProcAddr *is*
// called as an undefined symbol from that file, so interposing it and returning a stub is
// what puts a fault where production actually looks.
//
// Nothing in backend/daemon or renderer/ changes, so this is not the test-only hook in a
// production file that X.6 forbids -- it is the technique this file already uses on
// vkQueueSubmit. thread_local for the same reason: no other thread in this process can see
// the plan, so the counts below are not an assertion on the scheduler (X.2).
namespace {

// The result is selectable per arming because the export classifies what it gets, on the
// result itself rather than on a per-call list of permitted results: anything
// isResourceExhaustion() accepts must not touch the GPU's recorded health, and everything
// else is the driver going wrong and must.
thread_local VkCallFaultPlan* t_getMemoryFdFault = nullptr;

PFN_vkVoidFunction realDeviceProcAddr(VkDevice device, const char* name) {
    // RTLD_NEXT skips this executable and lands on the loader's own entry point. Idempotent,
    // so the relaxed racing store can only ever write the same value twice.
    static std::atomic<void*> real{nullptr};
    void* p = real.load(std::memory_order_relaxed);
    if (p == nullptr) {
        p = ::dlsym(RTLD_NEXT, "vkGetDeviceProcAddr");
        real.store(p, std::memory_order_relaxed);
    }
    auto* fn = reinterpret_cast<PFN_vkGetDeviceProcAddr>(p);
    return fn == nullptr ? nullptr : fn(device, name);
}

// *pFd is left untouched: exportFrame initialises it to -1 and rejects a non-VK_SUCCESS
// result anyway.
VKAPI_ATTR VkResult VKAPI_CALL faultingGetMemoryFd(VkDevice device, const VkMemoryGetFdInfoKHR* info, int* pFd) {
    if (const VkResult injected = armedFault(t_getMemoryFdFault); injected != VK_SUCCESS) return injected;

    auto* realFn = reinterpret_cast<PFN_vkGetMemoryFdKHR>(realDeviceProcAddr(device, "vkGetMemoryFdKHR"));
    if (realFn == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    return realFn(device, info, pFd);
}

class GetMemoryFdFault {
public:
    GetMemoryFdFault(int failAt, VkResult result) : m_fault(&t_getMemoryFdFault, failAt, result) {}

    void disarm() { m_fault.disarm(); }
    int observed() const { return m_fault.observed(); }
    int injected() const { return m_fault.injected(); }

private:
    VkCallFault m_fault;
};

}  // namespace

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    // Substituted only while a plan is armed on THIS thread, and only for this one name:
    // the Vulkan Memory Allocator and the descriptor tiers resolve their own entry points
    // through here, and handing any of them a stub would break the daemon rather than fail
    // one export.
    if (t_getMemoryFdFault != nullptr && pName != nullptr && std::strcmp(pName, "vkGetMemoryFdKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&faultingGetMemoryFd);
    }
    return realDeviceProcAddr(device, pName);
}

// ---------------------------------------------------------------------------- //
// Failing one vkCreateSampler, from the test binary
// ---------------------------------------------------------------------------- //
//
// The two levers above reach the render's submit and the export's file-descriptor query.
// Neither reaches the *object creation* a render does before it submits anything, and that
// is where renderer/vulkan/gpu_fault.cpp's throwVkFailure is called from most of its sites:
// allocator, buffer and image creates, render pass, image views, framebuffers, the three
// descriptor tiers and the static renderer's own objects. Whether a resource limit raised
// there stays off the GPU's recorded health is a different question from whether a failed
// submit reaches it, and only this lever can ask it.
//
// vkCreateSampler is the aim for two reasons. It has exactly one caller in this executable
// -- StaticRenderer::createSampler in renderer/renderers/static_renderer.cpp -- so a fault
// on the first call cannot land somewhere else and be graded as though it had landed here.
// And a static wallpaper builds a fresh StaticRenderer for every render rather than keeping
// one, so the aim stays live after the first render instead of being reachable once. It is
// called as an undefined symbol, resolved through the Vulkan loader, so a definition here is
// what production binds to.
//
// Nothing in backend/daemon or renderer/ changes, so this is not the test-only hook in a
// production file that X.6 forbids -- it is the technique this file already uses on
// vkQueueSubmit. thread_local for the same reason: no other thread in this process can see
// the plan, so the counts below are not an assertion on the scheduler (X.2).
namespace {
thread_local VkCallFaultPlan* t_createSamplerFault = nullptr;
}  // namespace

// *pSampler is left untouched: production checks the result before it reads the handle, and
// StaticRenderer's member is VK_NULL_HANDLE until this call assigns it.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo,
                                                          const VkAllocationCallbacks* pAllocator,
                                                          VkSampler* pSampler) {
    // RTLD_NEXT skips this executable and lands on the loader's own entry point. Idempotent,
    // so the relaxed racing store can only ever write the same value twice.
    static std::atomic<void*> real{nullptr};
    void* p = real.load(std::memory_order_relaxed);
    if (p == nullptr) {
        p = ::dlsym(RTLD_NEXT, "vkCreateSampler");
        real.store(p, std::memory_order_relaxed);
    }

    if (const VkResult injected = armedFault(t_createSamplerFault); injected != VK_SUCCESS) return injected;

    auto* realFn = reinterpret_cast<PFN_vkCreateSampler>(p);
    if (realFn == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    return realFn(device, pCreateInfo, pAllocator, pSampler);
}

namespace {

class CreateSamplerFault {
public:
    CreateSamplerFault(int failAt, VkResult result) : m_fault(&t_createSamplerFault, failAt, result) {}

    void disarm() { m_fault.disarm(); }
    int observed() const { return m_fault.observed(); }
    int injected() const { return m_fault.injected(); }

private:
    VkCallFault m_fault;
};

}  // namespace

// ---------------------------------------------------------------------------- //
// Failing one allocation inside an HTTP handler, from the test binary
// ---------------------------------------------------------------------------- //
//
// S7.9 needs an exception raised *inside* a route handler, and no client input produces
// one: `sanitizeForLog` guarantees valid UTF-8 for any byte sequence so json::dump()
// cannot throw, `from_chars` reports overflow through a value, and the wallpaper type is
// whitelisted before it is ever quoted into JSON. What both handlers do unavoidably is
// allocate -- snapshot() copies strings, sanitizeForLog builds one, dump() builds another
// -- so failing one allocation on the serving thread raises std::bad_alloc where nothing
// else can, and bad_alloc is a std::exception, which is what http_server.cpp's exception
// handler catches.
//
// Replacing ::operator new is the standard-sanctioned way to interpose on allocation.
// Nothing in backend/daemon or renderer/ changes, so this is not the test-only hook in a
// production file that X.6 forbids -- it is the technique this file already uses on
// vkQueueSubmit and renderer/tests/image_bounds_probe.cpp uses on malloc and fopen.
// operator delete is replaced alongside it so every pointer this binary hands out is a
// malloc/free pair; a lone operator new would leave malloc'd memory to be freed through
// the default delete's bookkeeping.
//
// A sanitized runtime cannot host it. The guard is CMake's -- backend/CMakeLists.txt --
// with the macro chain below as the fallback for a configure that does not set it.
// The tool owns operator new/delete: its versions carry the shadow-memory bookkeeping it
// reasons from, and this replacement removes it. Measured under -fsanitize=thread before
// the guard existed: 90 "data race in operator delete" reports against correctly
// synchronised threads, on a run whose every check passed. So under a sanitizer the
// instrument is not compiled, S7.9 reports itself unexercised, and the run exits 77 --
// inconclusive, never green.
#ifndef WALLPAPERX_ALLOCATION_INSTRUMENT
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_LEAK__) || \
    defined(__SANITIZE_MEMORY__) || defined(__SANITIZE_HWADDRESS__)
#define WALLPAPERX_ALLOCATION_INSTRUMENT 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) ||                        \
    __has_feature(leak_sanitizer) || __has_feature(memory_sanitizer) ||                           \
    __has_feature(hwaddress_sanitizer)
#define WALLPAPERX_ALLOCATION_INSTRUMENT 0
#else
#define WALLPAPERX_ALLOCATION_INSTRUMENT 1
#endif
#else
#define WALLPAPERX_ALLOCATION_INSTRUMENT 1
#endif
#endif  // WALLPAPERX_ALLOCATION_INSTRUMENT

namespace {

// One relaxed load is all an unarmed allocation pays. It has to be that cheap: with the
// probe compiled in, every allocation in this process -- the Vulkan loader's, the ICD's,
// sdbus's -- goes through here.
std::atomic<bool> g_allocProbeArmed{false};

// Calibration measures what ONE sanitizeForLog call asks for, so the size the injection
// keys on is produced by the same function the handler runs rather than by arithmetic
// over libstdc++'s growth policy.
thread_local bool t_calibrating = false;
thread_local std::size_t t_calibMax = 0;
thread_local int t_calibCount = 0;

// The main thread issues the request and allocates response buffers of its own, so
// without this the observation below could name the client as the handler's thread.
thread_local bool t_testOwnedThread = false;

constexpr std::size_t kBigAllocFloor = 1024;

std::atomic<std::size_t> g_watchSize{0};  // 0 = watch nothing
std::atomic<int> g_watchObserved{0};
std::atomic<unsigned long long> g_watchThread{0};   // last non-test thread to ask for it
std::atomic<unsigned long long> g_injectThread{0};  // 0 = inject nowhere
std::atomic<int> g_injected{0};

unsigned long long selfThreadId() { return static_cast<unsigned long long>(::pthread_self()); }

// Never inlined into operator new, so the unarmed path stays a load and a branch. Must
// not allocate: it runs inside the allocator.
[[gnu::noinline]] void noteAllocation(std::size_t bytes) {
    if (t_calibrating) {
        if (bytes >= kBigAllocFloor) {
            ++t_calibCount;
            if (bytes > t_calibMax) t_calibMax = bytes;
        }
        return;
    }

    const std::size_t watch = g_watchSize.load(std::memory_order_relaxed);
    if (watch == 0 || bytes != watch || t_testOwnedThread) return;

    const unsigned long long self = selfThreadId();
    g_watchThread.store(self, std::memory_order_relaxed);
    g_watchObserved.fetch_add(1, std::memory_order_relaxed);

    // One-shot, disarmed by the exchange before the throw, so exactly one allocation in
    // the process ever fails. The 500 path allocates too -- sanitizeForLog on what(),
    // json::dump(), set_content -- and every later request this worker serves would be
    // faulted as well, which would make g_injected a count of nothing in particular.
    if (g_injectThread.load(std::memory_order_relaxed) != self) return;
    if (g_injectThread.exchange(0, std::memory_order_relaxed) != self) return;
    g_injected.fetch_add(1, std::memory_order_relaxed);
    throw std::bad_alloc();
}

}  // namespace

#if WALLPAPERX_ALLOCATION_INSTRUMENT

void* operator new(std::size_t bytes) {
    if (g_allocProbeArmed.load(std::memory_order_relaxed)) noteAllocation(bytes);
    const std::size_t request = bytes == 0 ? 1 : bytes;
    void* p = std::malloc(request);
    while (p == nullptr) {
        // The replacement owes the same new_handler protocol the default one implements;
        // returning null instead would turn an out-of-memory condition into undefined
        // behaviour everywhere in this binary, not just in the phase under test.
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
        p = std::malloc(request);
    }
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#endif  // WALLPAPERX_ALLOCATION_INSTRUMENT

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
constexpr int kMonitorLiteral = 4;    // the path_literal round trip -- set over the bus, twice
constexpr int kMonitorNonUtf8 = 5;    // a path JSON cannot carry, recorded through the manager
// Step 10's connector-name ids. Two named monitors carry a wallpaper of their own, because
// the property is that a name resolves to ITS OWN monitor: with one named monitor every wrong
// answer is also the right one, so the phase could not tell a lookup from a constant.
constexpr int kMonitorNamedSet = 6;      // named, and a wallpaper of its own
constexpr int kMonitorNamedOther = 7;    // named, and a different wallpaper
constexpr int kMonitorNamedUnset = 8;    // named, nothing ever set on it
constexpr int kMonitorNamedHostile = 9;  // named with bytes a display surface must escape
constexpr int kMonitorNeverSet = 31;  // also the upper in-range boundary for S7.5

// Goals this run could not exercise, as opposed to goals it exercised and that held.
// Reported to ctest as exit 77 (SKIP_RETURN_CODE in backend/CMakeLists.txt) so an
// unexercised goal can never read as a pass. Not a check() failure: X.3 forbids
// reporting a fault of the host or the harness as a fault of the daemon. The channel
// itself lives in tests/test_report.h.
using wallpaperx::testing::notVerified;

// Unlike the other test binaries, this one must not say "on this host" in its skip summary:
// the reasons its phases print are a mix of host limits and preconditions this run could not
// build, and naming one cause for all of them points a reader at the wrong component.
constexpr const char* kSkipReasonMixed = "goal group(s) could not be exercised (each reason is printed above)";

// Three faults startServer() can meet that no single phase can grade, recorded here and
// answered once in main: a phase whose server is null can only say NOT VERIFIED, so without
// these the run would end at exit 77 -- a ctest skip -- with nothing naming the defect.
// Kept apart because two of them are the daemon's fault and one is the host's (X.3).
std::string g_serverRegistrationRefused;    // graded: the registration guard turned us away
std::string g_serverBoundButNeverServed;    // graded: it took a port and never answered
std::string g_serverConstructorHostFault;   // printed only: a limit this host imposed

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

// The wording http_server.cpp stamps into config_server health when bind_to_port fails.
// One definition, shared by startServer's rejection predicate below and by S7.7's
// discrimination check in runBindChecks: a reword then reddens S7.7 rather than quietly
// turning the rejection predicate into something that never fires.
constexpr const char* kBindRefusedMarker = "could not bind ";

// Starts an HttpServer on a port the OS says is free, retrying if something took the port
// in between. Returns null with `reason` filled whenever no server could be accepted; the
// callers all report that as NOT VERIFIED, and the globals above decide which of those
// reasons main additionally grades as a daemon defect.
//
// WHY-THIS-SHAPE: two predicates, and which one does which job is the whole point.
//       ACCEPTANCE is the socket: a server is handed back because it answered /health.
//       Gating acceptance on `snapshot().health.configServer` would make the harness read
//       the very value the graded checks grade -- a change that broke publication of a
//       *successful* bind would leave every attempt looking failed and end the process at
//       exit 77, a ctest SKIP, with no check ever naming the defect.
//       REJECTION is health, and only health: `/health` answering proves a listener exists,
//       never that the listener is ours, so losing the freePort() race to a stranger (a
//       second ctest entry is a real case -- see this file's header) would otherwise hand
//       back a server that never bound and let every later phase grade a foreign socket.
//       Our own constructor is the one witness that can settle it, and it settles only the
//       negative: a config_server detail carrying kBindRefusedMarker is this HttpServer
//       saying it did not get the port. Health is never used to accept, so a broken
//       publication still reaches the graded checks as FAIL rather than as a skip.
//       The FINAL attempt is handed back untested, before the probe: its answer cannot
//       change the outcome there and costs up to three seconds of timeout, and a daemon that
//       cannot serve on eight different free ports is a defect for the graded checks to
//       report, not a host fact to skip on. Falling out of the loop is a different case --
//       it happens only when that final attempt could not get a port at all, so whatever
//       `server` still holds is an object an EARLIER attempt rejected. Handing that back
//       (inferring "usable" from "non-null") would grade every later phase against a foreign
//       listener or a dead one, with `reason` left empty and the host-fact skip unreachable.
//       So the tail destroys it and answers from the recorded verdict instead: bound-and-
//       silent is the daemon's fault and is graded in main, a lost port race and a host with
//       no ephemeral ports are the host's and are only skips.
// CONTROL: five, reverted. (a) recordConfigServer(true, {}) changed to record a failure ->
//       "FAIL: the daemon that is serving reports config_server ok" and ctest Failed, not a
//       skip; it also reddens the shutdown record's names-the-port check, since a record
//       that was never Ok is one recordConfigServerShutdown deliberately leaves alone.
//       (b) the constructor made to return after a successful bind, so it publishes nothing
//       and serves nothing -> many FAIL lines and ctest Failed, but only in minutes unless
//       makeClient's timeouts are shortened with it. (c) freePort() made
//       to return -1 always -> the host-fact NOT VERIFIED path, ctest Skipped, no FAIL.
//       (d) the probe forced to fail and freePort() made to fail after the first call ->
//       "FAIL: every HttpServer that bound a port went on to answer /health", ctest Failed.
//       (e) the same, with the bind-refusal predicate forced true instead -> the lost-race
//       skip, ctest Skipped and no FAIL. The constructor-throw paths have their own controls,
//       on the checks in main that grade them.
std::unique_ptr<HttpServer> startServer(WallpaperManager& manager, std::string& reason) {
    constexpr int kAttempts = 8;
    std::unique_ptr<HttpServer> server;
    // Why the last constructed attempt was rejected, so the tail can name the fault rather
    // than infer one from a pointer. At most one is ever set: each is cleared where the
    // other is written.
    int silentPort = -1;  // >= 0: bound this port, then never answered /health
    std::string lostToStranger;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const int port = freePort();
        if (port < 0) continue;
        server.reset();  // one config server per manager: the previous attempt must be gone
        // Layered like S7.20's refusal check below, and for the same reason: HttpServer's
        // registration guard throws std::logic_error on a manager that already has a server,
        // and every caller here is inside a phase. Left to escape, it unwinds past the
        // remaining phases into main's handler, which prints one opaque line and abandons
        // them with no NOT VERIFIED row. Caught, the phase says what happened and the run
        // continues. Not retried: a claim that is held is held, so eight more attempts would
        // only be slower.
        try {
            server = std::make_unique<HttpServer>(manager, port);
        } catch (const std::logic_error& e) {
            if (g_serverRegistrationRefused.empty()) g_serverRegistrationRefused = e.what();
            reason = std::string("the HttpServer constructor threw: ") + e.what();
            return nullptr;
        } catch (const std::exception& e) {
            if (g_serverConstructorHostFault.empty()) g_serverConstructorHostFault = e.what();
            reason = std::string("the HttpServer constructor threw: ") + e.what();
            return nullptr;
        } catch (...) {
            if (g_serverConstructorHostFault.empty()) {
                g_serverConstructorHostFault = "something that is not a std::exception";
            }
            reason = "the HttpServer constructor threw something that is not a std::exception";
            return nullptr;
        }
        if (attempt == kAttempts - 1) return server;

        const wallpaperx::HealthCheck record = manager.snapshot().health.configServer;
        const bool selfReportedBindRefusal = record.status == wallpaperx::HealthStatus::Fail &&
                                             record.detail.find(kBindRefusedMarker) != std::string::npos;
        if (selfReportedBindRefusal) {
            lostToStranger = record.detail;
            silentPort = -1;
            continue;
        }

        httplib::Client cli = makeClient(port);
        // Both bounded well below makeClient's defaults. A loopback server that is alive
        // answers /health in microseconds, so these can only shorten the wait on a server
        // that is not; and a probe that expires costs one more attempt, not a verdict.
        cli.set_connection_timeout(std::chrono::seconds(2));
        cli.set_read_timeout(std::chrono::seconds(1));
        if (get(cli, "/health").reached) return server;
        silentPort = port;
        lostToStranger.clear();
    }
    server.reset();
    if (silentPort >= 0) {
        reason = "an HttpServer bound 127.0.0.1:" + std::to_string(silentPort) + " and then never answered /health";
        if (g_serverBoundButNeverServed.empty()) g_serverBoundButNeverServed = reason;
    } else if (!lostToStranger.empty()) {
        reason = "another process took every port this host handed out (" + lostToStranger + ")";
    } else {
        reason = "this host would not hand out an ephemeral loopback port on any of " + std::to_string(kAttempts) +
                 " attempts";
    }
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

// A decodable PNG whose *path* is long, and long on purpose.
//
// The /config handler sanitizes the recorded path with one reserve(), so the byte count of
// that single allocation request is what tells the handler's own work apart from every
// other allocation in a process that has several threads in it. A path of ordinary length
// would produce a request size that httplib's header parsing, sdbus and the client all ask
// for routinely; ~2 KiB is a size nothing else here requests, which is what makes a
// one-shot injection keyed on it land where it is aimed.
//
// Returns "" when the target length cannot be built (PATH_MAX, NAME_MAX, a read-only
// tree), which the caller reports as an unexercised goal rather than as a daemon fault.
std::string writeDeepPathImage(std::size_t targetLength) {
    constexpr std::size_t kComponent = 200;  // well under NAME_MAX
    constexpr std::size_t kSuffixLen = 4;    // ".png"
    std::error_code ec;
    std::string dir = testAssetDir() + "/s79";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};

    // Grow by whole components while a component plus a minimal leaf still fits.
    while (dir.size() + 1 + kComponent + 1 + kSuffixLen + 1 <= targetLength) {
        dir += "/" + std::string(kComponent, 'd');
        std::filesystem::create_directories(dir, ec);
        if (ec) return {};
    }

    if (targetLength < dir.size() + 1 + kSuffixLen + 1) return {};
    const std::size_t leafLen = targetLength - dir.size() - 1;
    if (leafLen <= kSuffixLen || leafLen > 255) return {};

    std::string leaf(leafLen - kSuffixLen, 'f');
    leaf += ".png";
    const std::string path = dir + "/" + leaf;
    if (path.size() != targetLength) return {};

    constexpr int kSide = 8;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(kSide) * kSide * 4, 0xFF);
    if (stbi_write_png(path.c_str(), kSide, kSide, 4, pixels.data(), kSide * 4) == 0) return {};
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

// The per-monitor breakdown the wallpapers check carries: monitor id -> {state, detail}.
// Read defensively -- `checks.wallpapers.monitors` on a const json is JSON_ASSERT plus a
// dereference if any level is missing, so a daemon that dropped the object would abort this
// process instead of failing the check that noticed.
const nlohmann::json* healthMonitors(const nlohmann::json& health) {
    if (!health.is_object() || !health.contains("checks") || !health["checks"].is_object()) return nullptr;
    const nlohmann::json& checks = health["checks"];
    if (!checks.contains("wallpapers") || !checks["wallpapers"].is_object()) return nullptr;
    const nlohmann::json& wallpapers = checks["wallpapers"];
    if (!wallpapers.contains("monitors") || !wallpapers["monitors"].is_object()) return nullptr;
    return &wallpapers["monitors"];
}

// The sorted key list of a JSON object, for the exact-set comparisons below: a set is what
// makes an unreviewed addition to a published surface visible, and sorting means a failure
// prints what actually arrived.
std::vector<std::string> sortedKeys(const nlohmann::json& object) {
    std::vector<std::string> keys;
    if (!object.is_object()) return keys;
    for (const auto& [key, value] : object.items()) {
        (void)value;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

// The names of the checks reading `pending`, sorted. The aggregate counts failures only, so
// every claim of the form "ok overall while a dependency is untried" has to name the untried
// dependencies in the same breath: `status == "ok"` on its own is equally true of an endpoint
// that has no `pending` state left to report.
std::vector<std::string> pendingCheckNames(const nlohmann::json& health) {
    std::vector<std::string> names;
    if (!health.is_object() || !health.contains("checks") || !health["checks"].is_object()) return names;
    for (const auto& [name, entry] : health["checks"].items()) {
        if (entry.is_object() && entry.value("status", std::string()) == "pending") names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out.empty() ? "(none)" : out;
}

// One monitor's reported state, and "" when this monitor is not in the breakdown at all --
// which a caller must tell apart from a state it did not expect, so every use below asserts
// the id is present first.
std::string monitorState(const nlohmann::json& health, int monitorId) {
    const nlohmann::json* monitors = healthMonitors(health);
    if (monitors == nullptr) return {};
    const std::string key = std::to_string(monitorId);
    if (!monitors->contains(key) || !(*monitors)[key].is_object()) return {};
    return (*monitors)[key].value("state", std::string());
}

std::string monitorDetail(const nlohmann::json& health, int monitorId) {
    const nlohmann::json* monitors = healthMonitors(health);
    if (monitors == nullptr) return {};
    const std::string key = std::to_string(monitorId);
    if (!monitors->contains(key) || !(*monitors)[key].is_object()) return {};
    return (*monitors)[key].value("detail", std::string());
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
    int port() const { return m_server->port(); }

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
// The run's shared manager is a parameter because S7.20's refusal half needs a manager that
// already has a server on it, and that is the only one there is. The S7.7 conflict case
// still builds its own: a *failing* second server on the shared manager is the damage S7.20
// forbids, not a fixture.
void runBindChecks(sdbus::IProxy& proxy, WallpaperManager& manager, HttpServer& server, const Check& check) {
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
    //       looking healthy: the constructor returns, the manager's config-server health
    //       reads fail with a detail naming the port, and DBus keeps working.
    //       UNRATIFIED: PLAN.md does not settle whether a port conflict should be fatal.
    //       This pins what cd28790 does, so a change of mind is a visible decision rather
    //       than a silent one; it is not a claim that surviving is correct.
    // WHY-THIS-SHAPE: the port is held by a socket this test owns, not by a second
    //       HttpServer, so the failing constructor is the only HttpServer involved. And it
    //       gets a manager of its own, because the shared one already has a server: aimed
    //       there it would be refused by the registration guard before it ever reached the
    //       bind, and this phase would be grading S7.20 while claiming to grade S7.7.
    //       The detail is read as stored: recordConfigServer runs it through
    //       sanitizeForLog(.., 1024, Bare), which leaves this ASCII string untouched.
    //       Two different writers stamp Fail with this port in the detail -- the
    //       bind-failure branch and the serving thread noticing accept died -- so the
    //       recorded detail is discriminated by which one wrote it, not merely graded as
    //       non-empty. Without that, control (b) below is decided by the scheduler.
    // CONTROL: four, reverted. (a) `throw std::runtime_error(bindError);` for the return
    //       in the bind-failure branch -> only the does-not-throw check red. (b) that
    //       return deleted, so the constructor records success and then starts a doomed
    //       serving thread -> red under either interleaving: not-healthy and names-the-port
    //       if the read lands on the success stamp, names-the-bind-refusal if the thread's
    //       "stopped serving" stamp got there first. (c) the port dropped from the
    //       bindError text -> only names-the-port red, which is what keeps it distinct from
    //       "something failed". (d) recordConfigServer(false, "") -> records-why,
    //       names-the-port and names-the-bind-refusal red.
    // COVERAGE-ASSERT: kBindRefusedMarker is production's own wording, and it is file-scope
    //       because startServer's rejection predicate keys on the same string. A reword
    //       reddens this check -- which is the intended failure, a detail nobody can
    //       attribute to a writer being the state this check exists to forbid -- rather than
    //       silently leaving that predicate matching nothing.

    HeldPort taken;
    if (!taken.open(INADDR_LOOPBACK)) {
        // Not an early return: the DBus half of S7.7 below needs no held port, and skipping
        // it too would let a host that is out of sockets take a gradeable check with it.
        notVerified("S7.7 (bind failure is survivable, HTTP half)",
                    "could not hold a loopback port to create the conflict");
    } else {
        bool threw = false;
        // Pending is what a manager nothing has written to reads back, so this seed cannot
        // be mistaken for a published verdict. It is never graded: the gate below skips
        // every check that reads it unless the constructor returned.
        wallpaperx::HealthStatus recordStatus = wallpaperx::HealthStatus::Pending;
        std::string recordDetail;
        wallpaperx::HealthStatus outlivedStatus = wallpaperx::HealthStatus::Pending;
        std::string outlivedDetail;
        int reportedPort = -1;
        {
            // Outlives the server below, which holds a reference to it -- the same ordering
            // discipline IsolatedDaemon and main.cpp use.
            WallpaperManager conflictedManager{WallpaperManager::Config{}};
            try {
                HttpServer conflicted(conflictedManager, taken.port());
                reportedPort = conflicted.port();
                // Read while the failed server is still alive: what is graded is what the
                // constructor published, with nothing destruction might add mixed into it.
                const wallpaperx::HealthCheck record = conflictedManager.snapshot().health.configServer;
                recordStatus = record.status;
                recordDetail = record.detail;
            } catch (const std::exception&) {
                threw = true;
            }
            // Read again with the failed server destroyed and its manager still alive: the
            // second reading is what a client asking GetStatus after the shutdown would get.
            const wallpaperx::HealthCheck outlived = conflictedManager.snapshot().health.configServer;
            outlivedStatus = outlived.status;
            outlivedDetail = outlived.detail;
        }
        check(!threw, "a port conflict does not throw out of the HttpServer constructor");
        // Gated rather than each written as `!threw && ...`: a constructor that threw has
        // nothing to report through, so grading its published state too would turn one fault
        // into five FAIL lines pointing at properties that were never observed.
        if (!threw) {
            check(recordStatus == wallpaperx::HealthStatus::Fail,
                  "a server that could not bind does not report itself healthy");
            check(!recordDetail.empty(), "a server that could not bind records why it could not");
            check(recordDetail.find(std::to_string(taken.port())) != std::string::npos,
                  "the recorded bind failure names the port that was taken");
            check(recordDetail.find(kBindRefusedMarker) != std::string::npos,
                  "the recorded failure is the bind being refused, not the accept loop dying afterwards");
            check(reportedPort == taken.port(), "port() reports the port that was asked for even after a failure");
            if (recordStatus != wallpaperx::HealthStatus::Fail || recordDetail.empty() ||
                recordDetail.find(std::to_string(taken.port())) == std::string::npos ||
                recordDetail.find(kBindRefusedMarker) == std::string::npos) {
                std::fprintf(stderr, "  config_server after a conflict on 127.0.0.1:%d: %s / %s\n", taken.port(),
                             wallpaperx::toString(recordStatus), recordDetail.c_str());
            }

            // GOAL: S7.7's diagnosis survives the failed server's destruction -- the reason a
            //       port conflict was reported is still readable once the HttpServer that
            //       reported it is gone. False if teardown overwrites it, and that string is
            //       the only place in the daemon a port conflict is named at all.
            // WHY-THIS-SHAPE: compared against what the live server published rather than
            //       asserted to be Fail, or to name the port. Both of those weaker forms pass
            //       under the mutation that matters: ~Registration's shutdown text is itself a
            //       Fail and itself contains the port, so only equality with the bind-refusal
            //       reading distinguishes "kept" from "replaced". Read here, inside the
            //       manager's scope, because the record dies with the manager.
            // CONTROL: the `status != Ok` early return dropped from
            //       WallpaperManager::recordConfigServerShutdown -> "FAIL: the reason a bind
            //       was refused is still readable after the failed server is destroyed".
            //       Reverted.
            const bool diagnosisKept = outlivedStatus == recordStatus && outlivedDetail == recordDetail;
            check(diagnosisKept, "the reason a bind was refused is still readable after the failed server is "
                                 "destroyed");
            if (!diagnosisKept) {
                std::fprintf(stderr, "  config_server went from %s / %s to %s / %s when the failed server was "
                                     "destroyed\n",
                             wallpaperx::toString(recordStatus), recordDetail.c_str(),
                             wallpaperx::toString(outlivedStatus), outlivedDetail.c_str());
            }
        }
    }

    // The other half of S7.7, and the half that says the daemon is still a daemon: the
    // bus surface has to be unaffected by the HTTP server failing to come up.
    // getStatus() reports a bus that dropped the call as a value rather than throwing (see
    // tests/client/daemon_client.h), which is what lets the check below be the claim it
    // reads as: a call that never came back is this check's red, not an unwind past every
    // phase that follows into main's handler.
    // CONTROL: DbusService::getStatus made to throw -> "FAIL: DBus still answers GetStatus
    //       after an HTTP bind failure", with the later phases still running. Reverted.
    const StatusReply status = getStatusOrExplain(proxy);
    const bool statusUsable = status.ok();
    check(statusUsable && status.json().value("running", false),
          "DBus still answers GetStatus after an HTTP bind failure");
    // A reply that never arrived is one fault, not two: the check below reads a field of
    // that reply, so grading it as well would blame config_server for a dead bus.
    //
    // Read over DBus rather than off the manager: config_server is not on /health at all
    // (see DaemonHealth::configServer), so the bus is the only channel a client can learn
    // the bind result from. This is also the field startServer never ACCEPTS a server on --
    // a harness that did could absorb a broken publication into a skip instead of a failure.
    bool servedReportPresent = false;
    std::string servedStatus;
    std::string servedDetail;
    if (statusUsable) {
        const nlohmann::json& statusJson = status.json();
        const nlohmann::json* servedReport =
            statusJson.contains("config_server") && statusJson["config_server"].is_object()
                ? &statusJson["config_server"]
                : nullptr;
        servedReportPresent = servedReport != nullptr;
        if (servedReport != nullptr) {
            servedStatus = servedReport->value("status", std::string());
            servedDetail = servedReport->value("detail", std::string());
        }
        check(servedStatus == "ok", "the daemon that is serving reports config_server ok");
        if (servedStatus != "ok") {
            std::fprintf(stderr, "  config_server: %s / %s, on a daemon serving %d\n", servedStatus.c_str(),
                         servedReportPresent ? servedDetail.c_str() : "(absent)", server.port());
        }
    }

    // GOAL: S7.20's refusal half -- HttpServer *enforces* at most one instance per
    //       WallpaperManager rather than trusting callers to observe the rule. False if a
    //       second server can construct on, or publish into, a manager that already has one.
    // WHY-THIS-SHAPE: outside the statusUsable gate, because the refusal itself needs no bus
    //       at all: nested inside it, an unusable GetStatus reply would drop the only check
    //       in the suite that establishes the one-server rule, and drop it with no
    //       NOT VERIFIED row -- a phase that could not run has to say so. Only the
    //       publish-nothing comparison needs the bus, and it is gated separately.
    //       The second server is aimed at a port this test holds, so with the guard gone it
    //       does not merely construct quietly -- it stamps a bind failure over a daemon that
    //       is serving, which is the damage the invariant exists to prevent. Aimed at a free
    //       port instead, both servers would record ok with an empty detail and no health
    //       read could tell them apart. The record is compared before against after rather
    //       than asserted to be "ok", so this states something about the refused server
    //       rather than restating the check above it.
    //       The catch is layered rather than narrow: std::logic_error is the contract and is
    //       what `refused` grades, but a bare `catch (const std::logic_error&)` lets any
    //       other type escape runBindChecks into main's handler, which abandons every
    //       remaining phase in this file behind one opaque line and no NOT VERIFIED row. So
    //       the broader handlers exist to keep the run alive and to name what was thrown;
    //       they do not make a wrong type pass.
    // CONTROL: two, reverted. (a) tryAttachConfigServer() made to always return true ->
    //       "FAIL: a second HttpServer on a manager that already has one is refused with
    //       std::logic_error" and "FAIL: a refused second server publishes nothing into the
    //       serving daemon's config_server health". (b) the guard's throw changed to
    //       std::runtime_error -> the refusal check red, with the thrown type printed, and
    //       the phases after this one still running.
    HeldPort blocked;
    if (!blocked.open(INADDR_LOOPBACK)) {
        notVerified("S7.20 (one config server per manager, refusal half)",
                    "could not hold a loopback port for a second server to fail to bind on");
    } else {
        bool refused = false;
        std::string wrongThrow;
        try {
            HttpServer second(manager, blocked.port());
            std::fprintf(stderr, "  a second HttpServer constructed on the run's manager, reporting port %d\n",
                         second.port());
        } catch (const std::logic_error&) {
            refused = true;
        } catch (const std::exception& e) {
            wrongThrow = std::string("a std::exception that is not a std::logic_error: ") + e.what();
        } catch (...) {
            wrongThrow = "something that is not a std::exception at all";
        }
        check(refused, "a second HttpServer on a manager that already has one is refused with std::logic_error");
        if (!wrongThrow.empty()) {
            std::fprintf(stderr, "  the second HttpServer threw %s\n", wrongThrow.c_str());
        }

        // Gated on the FIRST read having produced a config_server object: without that, an
        // absent config_server is graded twice -- once above as "not ok" and again here as
        // the refused server having changed it -- two named properties failing for one defect
        // that predates the refusal. The gate also decides whether the second call is made at
        // all, since a degraded bus is exactly the state that produced an unusable first
        // reply and there is nothing left for a second one to be compared against.
        //
        // The comparison is abandoned rather than graded: with no second reading there is
        // nothing to compare the first against, and a call that did not answer is not
        // evidence about what the refused server published. The unanswered call itself is
        // graded, because the DbusService is in this process -- a skip-only row would let
        // an in-process defect end the binary at exit 77, i.e. ctest Skipped.
        // CONTROL: DbusService::getStatus made to throw from its second call onward, so the
        //       before-reading still exists and this is the call that fails -> "FAIL: the
        //       in-process DbusService answers GetStatus" beside this skip row, ctest
        //       Failed, and the phases after it still running. Reverted.
        if (!servedReportPresent) {
            notVerified("S7.20 (one config server per manager, publish-nothing half)",
                        statusUsable ? "GetStatus carried no config_server object before the refusal, so no "
                                       "before-reading exists to compare against"
                                     : "the GetStatus reply before the refusal was not a usable object, which is "
                                       "graded above, so no before-reading exists to compare against");
        } else {
            const StatusReply after = getStatus(proxy);
            if (!after.ok()) {
                check(false, "the in-process DbusService answers GetStatus");
                notVerified("S7.20 (one config server per manager, publish-nothing half)", after.error());
            } else {
                const nlohmann::json& afterStatus = after.json();
                const bool afterPresent =
                    afterStatus.contains("config_server") && afterStatus["config_server"].is_object();
                const std::string afterStatusText =
                    afterPresent ? afterStatus["config_server"].value("status", std::string()) : "";
                const std::string afterDetail =
                    afterPresent ? afterStatus["config_server"].value("detail", std::string()) : "";
                const bool unchanged = afterPresent && afterStatusText == servedStatus && afterDetail == servedDetail;
                check(unchanged, "a refused second server publishes nothing into the serving daemon's config_server "
                                 "health");
                if (!unchanged) {
                    std::fprintf(stderr,
                                 "  config_server went from %s / %s to %s / %s across a refused second server\n",
                                 servedStatus.c_str(), servedDetail.c_str(),
                                 afterPresent ? afterStatusText.c_str() : "(absent)", afterDetail.c_str());
                }
            }
        }

        const HttpReply survived = get(cli, "/health");
        check(survived.reached && survived.status == 200,
              "the daemon kept serving across a second server being refused");
    }

    // GOAL: S7.20's release half -- a destroyed server releases its claim, so servers may
    //       attach and release in sequence. False if a destroyed server's claim outlives it,
    //       which would break the ordinary attach-release-attach shape startServer's retry
    //       and both shutdown phases depend on, not just a pathological one.
    // WHY-THIS-SHAPE: only the construction is graded, and only the construction is inside
    //       the try that feeds `reattachError`. Requiring the re-attached server to also
    //       answer would make a lost race for `reattachPort` -- freePort() is racy by
    //       construction -- print as a failure of the claim being released; leaving the
    //       client calls in the same try does the same thing more quietly, since any
    //       std::exception out of makeClient or get() would then read as "the manager failed
    //       to release its claim" and be indistinguishable from the CONTROL's red line. On
    //       its own manager, because the shared one has a live server on it and "a manager
    //       that had a server and no longer does" is not a state it can be put into.
    // CONTROL: ~Registration's detachConfigServer() call deleted -> "FAIL: a destroyed
    //       server releases its claim on the manager, so a later one can attach". Reverted.
    WallpaperManager sequentialManager{WallpaperManager::Config{}};
    std::string firstReason;
    bool firstStarted = false;
    {
        std::unique_ptr<HttpServer> first = startServer(sequentialManager, firstReason);
        firstStarted = first != nullptr;
    }
    const int reattachPort = firstStarted ? freePort() : -1;
    if (!firstStarted || reattachPort < 0) {
        notVerified("S7.20 (one config server per manager, release half)",
                    firstStarted ? "no ephemeral port could be probed for the second attach" : firstReason);
    } else {
        std::string reattachError;  // graded
        std::string probeError;     // printed only -- a client fault is not a claim leak
        try {
            HttpServer again(sequentialManager, reattachPort);
            // Served before it is destroyed, per this file's standing rule; the reply is
            // not graded, because a lost race for `reattachPort` would make it a statement
            // about the port and not about the claim being released. Its own try for the
            // same reason: nothing this client does may reach `reattachError`.
            try {
                httplib::Client c = makeClient(again.port());
                (void)get(c, "/health");
            } catch (const std::exception& e) {
                probeError = e.what();
            }
        } catch (const std::exception& e) {
            reattachError = e.what();
        }
        check(reattachError.empty(),
              "a destroyed server releases its claim on the manager, so a later one can attach");
        if (!reattachError.empty()) {
            std::fprintf(stderr, "  attaching a second server after the first was destroyed: %s\n",
                         reattachError.c_str());
        }
        if (!probeError.empty()) {
            std::fprintf(stderr, "  the re-attached server was built, but the harness could not probe it: %s\n",
                         probeError.c_str());
        }
    }
}

// ---------------------------------------------------------------------------- //
// Phase 2 -- /health before any device exists.
// ---------------------------------------------------------------------------- //
void runHealthBeforeDeviceChecks(httplib::Client& cli, const Check& check) {
    const HttpReply health = get(cli, "/health");
    if (!health.hasJsonObject()) {
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
    //       attributable. `shaders`, `gpu`, `wallpapers` and (since Step 8) `persistence`
    //       are four distinct checks, each carrying its own status and its own cause.
    // WHY-THIS-SHAPE: an EXACT set, not a subset. The set is a published surface -- a
    //       watchdog reads these names -- so the value of this check is that adding or
    //       dropping one cannot happen without a reviewer seeing it here. `contains the
    //       four we know about` is the weaker version, and it goes green for a /health that
    //       has grown a fifth check nobody reviewed. The names are compared as a sorted
    //       list rather than by four contains() calls so the failure prints what arrived.
    // CONTROL: `{"wallpapers", &wallpapers},` in http_server.cpp's checks array replaced
    //       with a second `{"persistence2", &snap.health.persistence}` -- one edit that
    //       drops a name and adds one -> "FAIL: /health carries a `checks` object naming
    //       exactly the daemon's dependencies", printing the body it got: gpu, persistence,
    //       persistence2, shaders. Reverted.
    const std::vector<std::string> names =
        health.json.contains("checks") ? sortedKeys(health.json["checks"]) : std::vector<std::string>{};
    const std::vector<std::string> expectedNames{"gpu", "persistence", "shaders", "wallpapers"};
    const bool shape = names == expectedNames;
    check(shape, "/health carries a `checks` object naming exactly the daemon's dependencies");
    if (!shape) {
        std::fprintf(stderr, "  checks: %s\n",
                     health.json.contains("checks") ? health.json["checks"].dump().c_str() : "(absent)");
    }

    // The same exactness one level down, on the check that grew a field in Step 8: the
    // wallpapers entry carries a `monitors` object beside its status and detail, and it is
    // present -- empty -- before any wallpaper exists. Asserting only that the key appears
    // once monitors exist would let a build that omits it here answer `{}` to a frontend
    // that has no way to tell "no monitors" from "this daemon does not report per monitor".
    // CONTROL: `checksJson["wallpapers"]["monitors"] = std::move(perMonitor);` deleted from
    //       http_server.cpp -> "FAIL: the wallpapers check carries exactly a status, a
    //       detail and a per-monitor breakdown", printing the entry it got. Reverted.
    const nlohmann::json* checksObject =
        health.json.contains("checks") && health.json["checks"].is_object() ? &health.json["checks"] : nullptr;
    const bool wallpapersShape =
        checksObject != nullptr && checksObject->contains("wallpapers") &&
        sortedKeys((*checksObject)["wallpapers"]) == std::vector<std::string>{"detail", "monitors", "status"};
    check(wallpapersShape, "the wallpapers check carries exactly a status, a detail and a per-monitor breakdown");
    if (!wallpapersShape && checksObject != nullptr && checksObject->contains("wallpapers")) {
        std::fprintf(stderr, "  wallpapers: %s\n", (*checksObject)["wallpapers"].dump().c_str());
    }
    const nlohmann::json* monitorsBefore = healthMonitors(health.json);
    check(monitorsBefore != nullptr && monitorsBefore->empty(),
          "the per-monitor breakdown is present and empty before any wallpaper is set");

    // GOAL: S8.6 / PLAN.md decision 4 -- a daemon that never opted into persistence reports
    //       that check as ok with the detail `disabled`, not as pending and not as fail.
    //       False if an opted-out daemon drags the worst-wins aggregate down: every user
    //       who has not configured persistence would then read as broken.
    // WHY-THIS-SHAPE: the detail is asserted too. Status alone cannot distinguish "no
    //       persistence was asked for" from "a save succeeded", and those are the two
    //       states an operator reading this endpoint has to tell apart -- one means the
    //       wallpaper will not survive a restart.
    // CONTROL: `HealthStatus::Ok, "disabled"` in WallpaperManager's constructor changed to
    //       `HealthStatus::Fail` -> "FAIL: /health reports persistence as ok with detail
    //       'disabled' on a daemon that never opted in" ("status 'fail', detail
    //       'disabled'"), and four more downstream of the aggregate, starting with "/health
    //       reports the aggregate as pending before anything has been tried" (aggregate:
    //       fail) -- which is the worst-wins damage decision 4 exists to avoid. Reverted.
    const bool persistenceShape = healthCheckStatus(health.json, "persistence") == "ok" &&
                                  healthCheckDetail(health.json, "persistence") == "disabled";
    check(persistenceShape,
          "/health reports persistence as ok with detail 'disabled' on a daemon that never opted in");
    if (!persistenceShape) {
        std::fprintf(stderr, "  persistence: status '%s', detail '%s'\n",
                     healthCheckStatus(health.json, "persistence").c_str(),
                     healthCheckDetail(health.json, "persistence").c_str());
    }

    // GOAL: S7.10b -- `pending` is a distinct state from `ok` in the entry that reports it,
    //       and the aggregate counts failures only. The shader directory and the device are
    //       touched only on the first render, so on a daemon that has not rendered, both
    //       read `pending` and neither is a fault. Two independent ways to be false: a
    //       dependency nobody has run reads `ok` (health asserted rather than established,
    //       the defect S7.10 exists to remove), or the aggregate reads `pending` (an idle,
    //       working daemon reporting itself unhealthy to whatever watchdog polls it).
    // WHY-THIS-SHAPE: the aggregate is asserted in the same check as the exact set of
    //       checks reading `pending`, not on its own. `aggregate == "ok"` alone is the
    //       vacuous shape this contract invites -- it is equally satisfied by an endpoint
    //       that has no `pending` state at all, which is the pre-rebuild endpoint -- and
    //       the set is what makes the claim "ok *in spite of* two untried dependencies".
    //       The set is compared by name rather than counted, so a daemon that started
    //       reporting one of these eagerly cannot hold the count at two with some other
    //       check going untried.
    // CONTROL: three. (a) the `else if (check->status == HealthStatus::Pending && overall
    //       != HealthStatus::Fail)` arm restored in http_server.cpp's aggregation -> "FAIL:
    //       /health reports ok overall while the dependencies nothing has used report
    //       pending" with "  pending checks: gpu, shaders; aggregate: pending". (b)
    //       `HealthStatus status = HealthStatus::Ok;` as HealthCheck's default in
    //       wallpaper_manager.h -> the same check red from the other direction ("pending
    //       checks: (none); aggregate: ok") plus the two per-check assertions. (c)
    //       `wallpapers.status = HealthStatus::Ok;` deleted from the empty-monitor arm ->
    //       "FAIL: /health reports wallpapers as ok with detail 'no wallpaper set' when no
    //       wallpaper has been set" and the pending-set half of the check above. All
    //       reverted.
    check(healthCheckStatus(health.json, "shaders") == "pending",
          "/health reports the shader directory as pending before anything has looked at it");
    check(healthCheckStatus(health.json, "gpu") == "pending",
          "/health reports the GPU as pending before a device has been created");

    // The detail, not just the status. `ok` alone cannot separate "nothing was asked for"
    // from "every monitor is showing a frame", and those are opposite facts to an operator
    // looking at a blank desktop -- the empty-monitor arm exists precisely to say which.
    // CONTROL: the empty-monitor detail changed to "0 monitor(s) showing a frame" -> "FAIL:
    //       /health reports wallpapers as ok with detail 'no wallpaper set' when no
    //       wallpaper has been set", printing the detail it got. Reverted.
    const bool idleWallpapers = healthCheckStatus(health.json, "wallpapers") == "ok" &&
                                healthCheckDetail(health.json, "wallpapers") == "no wallpaper set";
    check(idleWallpapers,
          "/health reports wallpapers as ok with detail 'no wallpaper set' when no wallpaper has been set");
    if (!idleWallpapers) {
        std::fprintf(stderr, "  wallpapers: status '%s', detail '%s'\n",
                     healthCheckStatus(health.json, "wallpapers").c_str(),
                     healthCheckDetail(health.json, "wallpapers").c_str());
    }

    const std::string aggregate = health.json.value("status", std::string());
    const std::vector<std::string> untried = pendingCheckNames(health.json);
    const bool idleIsHealthy = untried == std::vector<std::string>{"gpu", "shaders"} && aggregate == "ok";
    check(idleIsHealthy, "/health reports ok overall while the dependencies nothing has used report pending");
    if (!idleIsHealthy) {
        std::fprintf(stderr, "  pending checks: %s; aggregate: %s (body: %s)\n", joined(untried).c_str(),
                     aggregate.c_str(), health.body.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 1b -- S7.9, by failing one allocation inside a handler.
// ---------------------------------------------------------------------------- //
//
// On a daemon of its own (IsolatedDaemon), for two reasons: the fixture records a
// wallpaper that is never rendered, which would move the shared daemon's `wallpapers`
// check for every later phase; and the observation below counts allocation requests of one
// exact size process-wide, so it wants the quietest server it can get.
void runHandlerExceptionChecks(const Check& check) {
    constexpr const char* kGoal = "S7.9 (an exception escaping a handler becomes a 500, not a dead daemon)";
    // Odd, and ~2 KiB: see writeDeepPathImage.
    constexpr std::size_t kPathLength = 1993;
    constexpr int kMaxInjectionAttempts = 8;

    if (!WALLPAPERX_ALLOCATION_INSTRUMENT) {
        notVerified(kGoal, "this build replaces the allocator itself (a sanitizer), so the test binary cannot "
                           "interpose ::operator new and no exception can be placed inside a handler");
        return;
    }

    const std::string longPath = writeDeepPathImage(kPathLength);
    if (longPath.empty()) {
        notVerified(kGoal, "this host could not carry a " + std::to_string(kPathLength) +
                               "-byte image path under " + testAssetDir());
        return;
    }

    // Calibrate: what does ONE sanitizeForLog of this path ask the allocator for? Measured
    // rather than derived, so the injection keys on a size produced by the same function
    // the handler calls, whatever libstdc++'s reserve() does with it.
    std::size_t cookie = 0;
    int calibCount = 0;
    std::size_t sanitizedLength = 0;
    {
        t_calibMax = 0;
        t_calibCount = 0;
        t_calibrating = true;
        g_allocProbeArmed.store(true, std::memory_order_relaxed);
        const std::string sanitized = wallpaperx::sanitizeForLog(longPath, 4096, wallpaperx::LogFraming::Bare);
        g_allocProbeArmed.store(false, std::memory_order_relaxed);
        t_calibrating = false;
        cookie = t_calibMax;
        calibCount = t_calibCount;
        // Read, so the call cannot be optimised away as a dead store.
        sanitizedLength = sanitized.size();
    }

    if (calibCount != 1 || cookie < kBigAllocFloor || sanitizedLength != longPath.size()) {
        notVerified(kGoal, "sanitizeForLog made " + std::to_string(calibCount) + " allocation(s) of >= " +
                               std::to_string(kBigAllocFloor) + " bytes (largest " + std::to_string(cookie) +
                               ") for a " + std::to_string(longPath.size()) + "-byte path returning " +
                               std::to_string(sanitizedLength) +
                               " bytes, so no single request size identifies the handler's own work");
        return;
    }

    WallpaperManager::Config config;
    config.width = kTargetWidth;
    config.height = kTargetHeight;
    config.enableValidation = false;
    IsolatedDaemon daemon(config);
    if (!daemon.ready()) {
        notVerified(kGoal, daemon.reason());
        return;
    }
    try {
        daemon.manager().requestWallpaper("static", longPath, 0);
    } catch (const std::exception& e) {
        notVerified(kGoal, std::string("the long-path fixture was refused by requestWallpaper: ") + e.what());
        return;
    }

    httplib::Client cli = makeClient(daemon.port());
    const std::string route = "/config?monitor=0";

    g_watchObserved.store(0, std::memory_order_relaxed);
    g_watchThread.store(0, std::memory_order_relaxed);
    g_injectThread.store(0, std::memory_order_relaxed);
    g_injected.store(0, std::memory_order_relaxed);
    g_watchSize.store(cookie, std::memory_order_relaxed);
    g_allocProbeArmed.store(true, std::memory_order_relaxed);
    // A probe left armed past this phase would fault an unrelated one, and the exit paths
    // below are many. Disarms on every one of them, exceptions included (X.4).
    struct Disarm {
        ~Disarm() {
            g_injectThread.store(0, std::memory_order_relaxed);
            g_watchSize.store(0, std::memory_order_relaxed);
            g_allocProbeArmed.store(false, std::memory_order_relaxed);
        }
    } disarm;

    // Round 1, observe only: prove the handler really asks for `cookie` bytes, exactly
    // once, on a thread this test does not own. Without it the injection would be armed on
    // a guess, and "no 500 came back" could not be told from "nothing was ever injected".
    const HttpReply observed = get(cli, route);
    const int observedCount = g_watchObserved.load(std::memory_order_relaxed);
    unsigned long long handlerThread = g_watchThread.load(std::memory_order_relaxed);

    const bool observationUsable = observed.ok() && observed.json.value("path", std::string()) == longPath &&
                                   observedCount == 1 && handlerThread != 0;
    if (!observationUsable) {
        notVerified(kGoal, "a /config request for the long-path fixture answered status " +
                               std::to_string(observed.status) + " and made " + std::to_string(observedCount) +
                               " allocation request(s) of " + std::to_string(cookie) +
                               " bytes off the test's own threads (wanted exactly 1), so the handler's own "
                               "allocation could not be identified");
        return;
    }

    // Round 2: arm the one-shot for the thread that just served us and repeat the request.
    // httplib hands a keep-alive connection to one pool worker, so this normally fires on
    // the first attempt; the loop re-aims at whichever worker actually answered rather
    // than assuming, and gives up with a reason instead of falling through (X.1).
    HttpReply faulted;
    int attempts = 0;
    while (attempts < kMaxInjectionAttempts && g_injected.load(std::memory_order_relaxed) == 0) {
        ++attempts;
        g_injectThread.store(handlerThread, std::memory_order_relaxed);
        faulted = get(cli, route);
        handlerThread = g_watchThread.load(std::memory_order_relaxed);
    }
    // Ordering, not tidying: the survival requests below must not be faulted, and the
    // guard above only runs at scope exit. The watch stays on, because the survival check
    // reads its counter.
    g_injectThread.store(0, std::memory_order_relaxed);
    const int injected = g_injected.load(std::memory_order_relaxed);

    const int observedBeforeSurvival = g_watchObserved.load(std::memory_order_relaxed);
    const HttpReply afterConfig = get(cli, route);
    const HttpReply afterHealth = get(cli, "/health");
    const int observedAfterSurvival = g_watchObserved.load(std::memory_order_relaxed);

    if (injected != 1) {
        notVerified(kGoal, "no allocation failure could be placed inside a handler: " + std::to_string(attempts) +
                               " attempt(s) were all served by a worker thread other than the one observed");
        return;
    }

    std::printf("http_server_test: S7.9 -- failed one %zu-byte allocation inside the /config handler after %d "
                "attempt(s); it answered %d\n",
                cookie, attempts, faulted.status);
    std::fflush(stdout);

    // GOAL: S7.9 -- an exception raised inside a route handler becomes a 500 carrying the
    //       daemon's own JSON error, and the config server keeps serving afterwards. False
    //       if the handler's throw kills the process, closes the connection, or answers
    //       with something the daemon did not write.
    // WHY-THIS-SHAPE: the body, not just the status. httplib catches a handler exception
    //       whether or not set_exception_handler was called -- with no handler it still
    //       answers 500, with an empty body -- so a check that asserted only `status ==
    //       500` would stay green with http_server.cpp's exception handler deleted, which
    //       is precisely the code S7.9 exists to protect. What identifies the daemon's own
    //       handler is `{"error":"internal error"}` arriving as parseable JSON.
    // CONTROL: four, all reverted. (a) `m_server->set_exception_handler(...)` disabled in
    //       http_server.cpp -> "FAIL: a handler that threw answers with the daemon's own
    //       JSON error body", "  body: (empty)"; the 500 check stayed GREEN, which is the
    //       measurement this shape exists for. (b) httplib.h's `try { routed = routing(
    //       ...); } catch` replaced by a bare call, so nothing catches -> "terminate
    //       called after throwing an instance of 'std::bad_alloc'", ctest reporting
    //       http_config as Failed and none of the five checks below printing at all: the
    //       daemon really does die without a catch. (c) `res.status = 500` -> 200 in the
    //       exception handler -> "FAIL: a handler that threw answers 500", body still the
    //       daemon's JSON. (d) harness-side, for the survival pair: the injection made
    //       permanent and un-gated by thread -> "FAIL: the config server serves /config
    //       correctly again after a handler threw". It has to be un-gated because httplib
    //       drops the keep-alive connection after the 500, so the next request is served
    //       by a different worker (measured: 139749780092608 then 139749771699904).
    // COVERAGE-ASSERT: `injected == 1` above gates every check here, so a run where the
    //       fault never reached a handler skips instead of grading; and the survival check
    //       requires the cookie allocation to be observed AGAIN, so a 200 served from
    //       anywhere but that same handler path does not satisfy it.
    check(faulted.reached, "a handler that threw still answers, rather than dropping the connection");
    check(faulted.status == 500, "a handler that threw answers 500");
    check(faulted.hasJsonObject() && faulted.json.value("error", std::string()) == "internal error",
          "a handler that threw answers with the daemon's own JSON error body");
    if (faulted.status != 500 || !faulted.hasJsonObject()) {
        std::fprintf(stderr, "  body: %s\n", faulted.body.empty() ? "(empty)" : faulted.body.c_str());
    }

    check(afterConfig.ok() && afterConfig.json.value("path", std::string()) == longPath &&
              observedAfterSurvival > observedBeforeSurvival,
          "the config server serves /config correctly again after a handler threw");
    check(afterHealth.ok(), "the config server still answers /health after a handler threw");
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
                // GOAL: S7.10f -- a monitor that has been requested but has not yet
                //       rendered is not a monitor showing a picture, so `wallpapers` reads
                //       `pending`; and S7.10b-i -- that particular pending DOES reach the
                //       aggregate, as `pending` and never as `fail`. It is the daemon
                //       owing an answer it started, which is different from a dependency
                //       nobody has needed yet. Three independent ways to be false: an
                //       endpoint calling an unrendered monitor `ok` tells a frontend a
                //       blank screen has a picture on it; an aggregate of `ok` tells a
                //       watchdog a wedged render worker is a working daemon, which is the
                //       state this exists to make visible; and an aggregate of `fail`
                //       accuses the daemon of a failure when nothing has failed.
                // WHY-THIS-SHAPE: the aggregate is asserted to be exactly `pending`, on a
                //       state whose OTHER two checks are asserted to read `ok`. Asserting
                //       only `wallpapers == "pending"` says nothing about the aggregate,
                //       and asserting `aggregate != "ok"` would be satisfied by `fail`,
                //       which is the misattribution S8.16 forbids. The two ok checks are
                //       what make this state distinguishable from the all-pending daemon
                //       of runHealthBeforeDeviceChecks, where a `pending` aggregate is
                //       exactly what S7.10b says must NOT happen -- the same aggregate
                //       value is right here and wrong there, so the mix has to be pinned.
                // CONTROL: three, all reverted. (a) the `if (wallpapers.status ==
                //       HealthStatus::Pending && overall == HealthStatus::Ok)` arm deleted
                //       from the aggregation in http_server.cpp -> "FAIL: a render the
                //       daemon started and has not finished makes the aggregate pending"
                //       with "wallpapers pending, shaders ok, gpu ok -> aggregate ok".
                //       (b) `HealthStatus::Pending` in that arm replaced with
                //       `HealthStatus::Fail` -> the same check red with "-> aggregate
                //       fail". (c) the `else if (inFlight > 0)` arm deleted from the
                //       wallpapers derivation, so an in-flight monitor falls into the ok
                //       branch -> "FAIL: a monitor that has been requested but not
                //       rendered leaves wallpapers pending, not ok", the aggregate check,
                //       and the pending-detail check below, which has no branch left to
                //       read.
                //       A fourth mutation, run to place the boundary rather than to redden
                //       anything here: the blanket "any pending check propagates" rule
                //       restored in the aggregation loop. This scenario stayed GREEN under
                //       it -- the aggregate it asks for is what the blanket rule produces --
                //       and the untried-dependency checks went red instead ("pending checks:
                //       gpu, shaders; aggregate: pending", in runHealthBeforeDeviceChecks
                //       and in scenario (a2)). That pair of scenarios, not this one, is
                //       where S7.10b and S7.10b-i are told apart. Reverted.
                check(healthCheckStatus(health.json, "shaders") == "ok" &&
                          healthCheckStatus(health.json, "gpu") == "ok",
                      "the mixed-state daemon really does have two checks reading ok");
                check(healthCheckStatus(health.json, "wallpapers") == "pending",
                      "a monitor that has been requested but not rendered leaves wallpapers pending, not ok");
                const std::string aggregate = health.json.value("status", std::string());
                const bool inFlightIsNotAFault =
                    healthCheckStatus(health.json, "wallpapers") == "pending" && aggregate == "pending";
                check(inFlightIsNotAFault,
                      "a render the daemon started and has not finished makes the aggregate pending, not ok and "
                      "not fail");
                if (!inFlightIsNotAFault) {
                    std::fprintf(stderr, "  wallpapers %s, shaders %s, gpu %s -> aggregate %s\n",
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

                    // `pending` must be a state this daemon can leave, or every "ok overall
                    // while a check is pending" assertion in this file is equally satisfied
                    // by an endpoint stuck on pending forever. The aggregate cannot carry
                    // that any more -- it read `ok` in the mixed state too -- so what is
                    // asserted is that no check is left reading pending at all.
                    // CONTROL: `wallpapers.status = HealthStatus::Ok;` deleted from the
                    //       all-showing arm in http_server.cpp -> "FAIL: no check is left
                    //       reading pending once every dependency has been used" with
                    //       "still pending: wallpapers; aggregate: ok", beside the check
                    //       above and its equivalent in the recovery scenario. Reverted.
                    const std::vector<std::string> stillPending = pendingCheckNames(full.json);
                    const bool nothingUntried =
                        stillPending.empty() && full.json.value("status", std::string()) == "ok";
                    check(nothingUntried, "no check is left reading pending once every dependency has been used");
                    if (!nothingUntried) {
                        std::fprintf(stderr, "  still pending: %s; aggregate: %s\n", joined(stillPending).c_str(),
                                     full.json.value("status", std::string()).c_str());
                    }
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
                // GOAL: S8.16 -- a monitor set to a type this build cannot render is
                //       reported per monitor, as `unsupported` with a detail naming it, and
                //       does NOT fail the daemon aggregate. Both halves are independently
                //       false: reporting nothing per monitor leaves a frontend calling
                //       GetStatus to find out which screen is blank, and reporting a
                //       wallpaper's problem as a daemon failure makes a watchdog restart a
                //       daemon that is working exactly as designed. The daemon's own faults
                //       still fail the aggregate -- scenario (b) above is where a missing
                //       shader directory does precisely that.
                // WHY-THIS-SHAPE: the aggregate is asserted to be exactly `ok`, together
                //       with the exact set of checks reading `pending`. `!= "fail"` would
                //       be satisfied by an endpoint that answers `pending` to everything,
                //       and `== "ok"` on its own would be satisfied by one with no
                //       `pending` state left at all -- the pre-rebuild endpoint, which is
                //       what S7.10 exists to remove. The pending pair is also what pins
                //       that the verdict here is about the TYPE and not about a render
                //       that blew up: neither dependency was ever asked for anything.
                // CONTROL: three. (a) the `!isRenderableType(monitor.type)` arm deleted from
                //       the classification loop in http_server.cpp, so an unrenderable
                //       monitor falls through to the in-flight count -> "FAIL: a wallpaper
                //       type this build cannot render is not a failure of the daemon" (the
                //       check reads `pending`), "FAIL: an unrenderable type leaves the
                //       aggregate ok with the untried dependencies still pending" (pending
                //       checks: gpu, shaders, wallpapers), "FAIL: a monitor whose type this
                //       build cannot render is reported per monitor as unsupported" (state
                //       'rendering'), "FAIL: the unsupported monitor's own detail names it
                //       and the type it is set to", and the same in the after-device phase.
                //       (b) `firstUnsupported` in the detail replaced with `std::string()`
                //       -> only "FAIL: the unsupported detail names the monitor and the type
                //       it is set to", printing "detail: 1 of 1 monitors failed or cannot be
                //       rendered:  (expected 'monitor 0' and 'video')". (c) the `else if
                //       (check->status == HealthStatus::Pending && overall !=
                //       HealthStatus::Fail)` arm restored in the aggregation -> only the
                //       aggregate check red, "pending checks: gpu, shaders; aggregate:
                //       pending". All reverted.
                // COVERAGE-ASSERT: the monitor is asserted to carry no lastError, so this
                //       scenario cannot silently become a second copy of scenario (c) -- a
                //       future requestWallpaper that recorded an error for `video` would
                //       otherwise leave this grading the failed-render path instead.
                check(it->second.lastError.empty(),
                      "an unrenderable type leaves no error on the monitor, so `unsupported` cannot come from one");
                check(healthCheckStatus(health.json, "wallpapers") == "ok",
                      "a wallpaper type this build cannot render is not a failure of the daemon");
                check(healthCheckStatus(health.json, "shaders") == "pending" &&
                          healthCheckStatus(health.json, "gpu") == "pending",
                      "an unrenderable type does not implicate the shaders or the GPU");
                const std::string aggregate = health.json.value("status", std::string());
                const std::vector<std::string> untried = pendingCheckNames(health.json);
                const bool notTheDaemonsFault = untried == std::vector<std::string>{"gpu", "shaders"} &&
                                                aggregate == "ok";
                check(notTheDaemonsFault,
                      "an unrenderable type leaves the aggregate ok with the untried dependencies still pending");
                if (!notTheDaemonsFault) {
                    std::fprintf(stderr, "  pending checks: %s; aggregate: %s (body: %s)\n", joined(untried).c_str(),
                                 aggregate.c_str(), health.body.c_str());
                }

                // The per-monitor object, as an exact set on both levels: which monitors it
                // names, and which keys each entry carries. A `contains` check would go
                // green for a breakdown that had grown a field nobody reviewed, and this is
                // a published surface -- a frontend reads these names.
                const nlohmann::json* monitors = healthMonitors(health.json);
                check(monitors != nullptr, "the wallpapers check carries a per-monitor breakdown");
                if (monitors != nullptr) {
                    const bool named = sortedKeys(*monitors) == std::vector<std::string>{"0"};
                    check(named, "the breakdown names exactly the monitors the daemon has a record of");
                    // Indexed only once the key is known to be there: `(*monitors)["0"]` on a
                    // const json whose key is missing is a JSON_ASSERT, and this process
                    // aborting is how the FAIL lines this phase exists to print get lost.
                    check(named && sortedKeys((*monitors)["0"]) == std::vector<std::string>{"detail", "state"},
                          "each monitor's entry carries exactly a state and a detail");
                    check(monitorState(health.json, 0) == "unsupported",
                          "a monitor whose type this build cannot render is reported per monitor as unsupported");
                    const std::string perMonitor = monitorDetail(health.json, 0);
                    check(perMonitor.find("monitor 0") != std::string::npos &&
                              perMonitor.find("'" + it->second.type + "'") != std::string::npos,
                          "the unsupported monitor's own detail names it and the type it is set to");
                    if (monitorState(health.json, 0) != "unsupported" || perMonitor.empty()) {
                        std::fprintf(stderr, "  monitors: %s\n", monitors->dump().c_str());
                    }
                }

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
    // (a3) a wallpaper the last run died applying is refused, and that is the wallpaper's
    //      problem and not the daemon's.
    // ------------------------------------------------------------------ //
    {
        // Needs no GPU: the refusal is decided in restoreSaved(), and the render pass for a
        // refused monitor returns before ensureGpu(). That is why it sits in this phase rather
        // than in the after-device one -- a host with no working driver still grades it.
        const wallpaperx::testing::ScratchDir scratch("healthquarantine");
        const std::filesystem::path stateDir = scratch.path() / "state" / "wallpaperx";
        const std::filesystem::path configFile = scratch.path() / "config" / "config.json";
        bool planted = scratch.ok();
        if (planted) {
            try {
                std::filesystem::create_directories(configFile.parent_path());
                std::filesystem::create_directories(stateDir);
                const nlohmann::json config{
                    {"version", 1},
                    {"monitors", nlohmann::json::array({{{"monitor_id", 0},
                                                         {"type", "static"},
                                                         {"path", bluePath}}})}};
                std::ofstream(configFile) << config.dump();
                std::ofstream(wallpaperx::crashRecordPath(stateDir, 0))
                    << nlohmann::json{{"monitor_id", 0}, {"type", "static"}, {"path", bluePath}}.dump()
                    << "\nsignal 11\n";
            } catch (const std::exception& e) {
                planted = false;
                notVerified("S13.13 (the /health half)",
                            std::string("the config and crash record could not be planted: ") + e.what());
            }
        } else {
            notVerified("S13.13 (the /health half)", "no scratch directory for the config and state files");
        }

        if (planted) {
            WallpaperManager::Config config = base;
            config.configFile = configFile;
            config.stateDir = stateDir;

            IsolatedDaemon daemon(config);
            if (!daemon.ready()) {
                notVerified("S13.13 (the /health half)", daemon.reason());
            } else {
                for (int id : daemon.manager().restoreSaved()) daemon.manager().renderPending(id);
                const HttpReply health = daemon.health();
                if (!health.ok()) {
                    check(false, "the daemon that refused a wallpaper answered /health with a parseable 200");
                } else {
                    // GOAL: S13.13's /health clause -- a monitor whose wallpaper the last run
                    //       died applying reads `failed` with the reason, while the daemon
                    //       itself reads `ok`. Both halves are separately false: no per-monitor
                    //       entry leaves a frontend with a blank screen and nothing to show for
                    //       it, and a failed aggregate makes a service manager restart a daemon
                    //       that is working exactly as designed -- which is the crash loop
                    //       rebuilt out of the watchdog instead of the config.
                    // WHY-THIS-SHAPE: the aggregate is asserted to be exactly "ok", not merely
                    //       "not fail". `pending` would also be wrong here and is what a build
                    //       that refused the wallpaper but still owed a render would report.
                    // CONTROL: two, one per half, both in backend/daemon/wallpaper_manager.cpp.
                    //       `state.lastErrorIsDaemonFault = false` in restoreSaved()'s
                    //       quarantine branch changed to `= true` -> "FAIL: a refused wallpaper
                    //       does not make the daemon itself unhealthy", printing `aggregate:
                    //       fail; wallpapers: 1 of 1 monitors failed inside the daemon`. And
                    //       `state.quarantined = ...` replaced with `= false` -> "FAIL: /health
                    //       reports a monitor whose wallpaper was refused as failed" and the
                    //       detail check beside it. Both reverted.
                    check(monitorState(health.json, 0) == "failed",
                          "/health reports a monitor whose wallpaper was refused as failed");
                    check(monitorDetail(health.json, 0).find("SIGSEGV") != std::string::npos,
                          "/health's per-monitor detail says the daemon died applying it, and of what");
                    check(healthCheckStatus(health.json, "wallpapers") == "ok",
                          "a refused wallpaper is the wallpaper's problem: the wallpapers check stays ok");
                    check(health.json.value("status", std::string()) == "ok",
                          "a refused wallpaper does not make the daemon itself unhealthy");
                    if (health.json.value("status", std::string()) != "ok") {
                        std::fprintf(stderr, "  aggregate: %s; wallpapers: %s\n",
                                     health.json.value("status", std::string()).c_str(),
                                     healthCheckDetail(health.json, "wallpapers").c_str());
                    }
                }
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
                // GOAL: S7.10b's unchanged half -- a genuine failure of one of the daemon's
                //       own dependencies still reaches the aggregate. False if a daemon
                //       whose shader directory does not exist reports itself healthy, which
                //       is the whole endpoint gone: a failure counted the way `pending` now
                //       is would leave `status` a constant.
                // WHY-THIS-SHAPE: this is the only state in the suite where the aggregate is
                //       anything but `ok`, so it is load-bearing for every check that
                //       asserts `ok`: without it, "the aggregate is ok" is satisfied by a
                //       hardcoded string. Asserting the shaders check alone would not
                //       establish it -- the aggregation loop is a separate piece of code
                //       from the recording.
                // CONTROL: `if (check->status == HealthStatus::Fail) overall =
                //       HealthStatus::Fail;` deleted from the aggregation in
                //       http_server.cpp, leaving the aggregate a constant `ok` -> "FAIL: a
                //       failing check makes the aggregate fail" with "  aggregate: ok".
                //       Reverted.
                const std::string aggregate = health.json.value("status", std::string());
                check(aggregate == "fail", "a failing check makes the aggregate fail");
                if (aggregate != "fail") std::fprintf(stderr, "  aggregate: %s\n", aggregate.c_str());
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
                check(healthCheckStatus(health.json, "wallpapers") == "ok",
                      "an image the daemon cannot decode is not a failure of the daemon");
                check(healthCheckStatus(health.json, "shaders") == "ok",
                      "an undecodable image does not implicate the shaders");
                check(healthCheckStatus(health.json, "gpu") == "ok",
                      "an undecodable image does not implicate the GPU");
                check(!lastError.empty() &&
                          healthCheckDetail(health.json, "wallpapers").find(lastError) != std::string::npos,
                      "the wallpapers check carries the monitor's own error text");

                // GOAL: S8.16's second clause, in its sharpest form -- with every dependency
                //       of the daemon reporting ok, one wallpaper that cannot be drawn does
                //       not make the daemon report itself broken. False if the aggregate
                //       reads `fail` here: a watchdog restarts a daemon whose only problem
                //       is a picture the user deleted, and restarting will not bring it back.
                // WHY-THIS-SHAPE: asserted on this daemon rather than on the shared one
                //       because every other check here is ok, so `fail` could come from
                //       nowhere else -- on the shared manager the same assertion would also
                //       be satisfied by a gpu or shaders verdict. The per-monitor state is
                //       asserted in the same breath: an endpoint that reported ok and said
                //       nothing about the monitor would satisfy the aggregate half while
                //       leaving the operator with no way to find the blank screen, and that
                //       is the trade this contract change is only safe because it avoids.
                // CONTROL: `wallpapers.status = HealthStatus::Ok;` in http_server.cpp's
                //       failed-or-unsupported arm changed back to `HealthStatus::Fail` ->
                //       "FAIL: an image the daemon cannot decode is not a failure of the
                //       daemon" and "FAIL: a wallpaper that cannot be drawn leaves the
                //       aggregate ok when every dependency is ok" (aggregate: fail), and the
                //       two equivalents in the unrenderable-type and after-device phases.
                //       Reverted.
                check(health.json.value("status", std::string()) == "ok",
                      "a wallpaper that cannot be drawn leaves the aggregate ok when every dependency is ok");
                check(monitorState(health.json, 0) == "failed",
                      "the monitor whose image could not be decoded is reported per monitor as failed");
                check(monitorDetail(health.json, 0) == lastError,
                      "the failed monitor's own detail is the error the monitor recorded");
                if (healthCheckDetail(health.json, "wallpapers").find(lastError) == std::string::npos ||
                    monitorDetail(health.json, 0) != lastError ||
                    health.json.value("status", std::string()) != "ok") {
                    std::fprintf(stderr, "  detail: %s\n  per monitor: %s\n  lastError: %s\n  aggregate: %s\n",
                                 healthCheckDetail(health.json, "wallpapers").c_str(),
                                 monitorDetail(health.json, 0).c_str(), lastError.c_str(),
                                 health.json.value("status", std::string()).c_str());
                }
                std::printf("http_server_test: undecodable image -> %s\n", health.body.c_str());
                std::fflush(stdout);

                // GOAL: S7.10 with S8.16 -- /health reports what is *currently* wrong. A
                //       daemon that has recovered must say so; a failure that sticks is a
                //       different kind of lie from one that is never reported, and it trains
                //       a reader to ignore the endpoint.
                // WHY-THIS-SHAPE: graded on the PER-MONITOR state, not on the check's status
                //       or the aggregate. Since a wallpaper's problem no longer moves either
                //       of those, both read `ok` before and after this render, so a check
                //       written on them -- which is what this was -- would pass whether the
                //       daemon cleared the monitor's error or kept it forever. The detail is
                //       asserted to become empty for the same reason: a state that flipped
                //       to `showing` while still carrying the old error text would leave a
                //       frontend displaying a failure that no longer exists.
                // CONTROL: both `lastError.clear()` calls removed from
                //       wallpaper_manager.cpp -- the one in requestWallpaper and the one in
                //       renderPassLocked's commit -- i.e. a failure that never clears -> "FAIL:
                //       a later successful render clears the monitor's failed state" with
                //       "  monitor 0 after a good render: failed", and "FAIL: the recovered
                //       monitor's detail is empty, not the failure it no longer has".
                //       Removing either alone leaves the check green, since the other still
                //       clears it; that is a fact about the daemon, not a weakness here.
                //       Re-observed 2026-09-14. Reverted.
                daemon.manager().requestWallpaper("static", bluePath, 0);
                daemon.manager().renderPending(0);
                const HttpReply recovered = daemon.health();
                if (!recovered.ok()) {
                    check(false, "the recovered daemon answered /health with a parseable 200");
                } else {
                    check(monitorState(recovered.json, 0) == "showing",
                          "a later successful render clears the monitor's failed state");
                    check(monitorDetail(recovered.json, 0).empty(),
                          "the recovered monitor's detail is empty, not the failure it no longer has");
                    check(healthCheckStatus(recovered.json, "wallpapers") == "ok",
                          "the wallpapers check reads ok once the monitor is showing a frame");
                    check(recovered.json.value("status", std::string()) == "ok",
                          "a daemon with nothing wrong reports the aggregate as ok");
                    if (monitorState(recovered.json, 0) != "showing" ||
                        recovered.json.value("status", std::string()) != "ok") {
                        std::fprintf(stderr, "  monitor 0 after a good render: %s\n  body: %s\n",
                                     monitorState(recovered.json, 0).c_str(), recovered.body.c_str());
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------ //
    // (d2) who owns a failed render: the daemon, or the wallpaper.
    // ------------------------------------------------------------------ //
    {
        // A shader directory that EXISTS and is empty. resolveShaderDir only requires a
        // directory, so `shaders` records ok and a device is created -- and then the render
        // fails inside StaticRenderer with a plain std::runtime_error, which is the
        // unclassified case. A missing directory (scenario (b)) will not do: it fails
        // `shaders` as well, so the aggregate would read fail whatever the classification
        // did, and the check would establish nothing.
        WallpaperManager::Config config = base;
        config.shaderDir = makeEmptyDir("wpx_health_no_spv_dir");

        IsolatedDaemon daemon(config);
        std::string vanishing;
        try {
            vanishing = writeSolidImage("http_health_vanishing.png", 0, 128, 0);
        } catch (const std::exception& e) {
            vanishing.clear();
        }
        if (!daemon.ready()) {
            notVerified("S7.10h (a render failure the daemon owns fails the verdict; a wallpaper's does not)",
                        daemon.reason());
        } else if (vanishing.empty()) {
            notVerified("S7.10h (a render failure the daemon owns fails the verdict; a wallpaper's does not)",
                        "the image fixture this scenario deletes mid-flight could not be written");
        } else {
            // Three renders on ONE monitor of ONE daemon, in this order: an undecodable
            // image, then a good image that cannot find a shader, then an image deleted
            // between the request and the render.
            //
            // GOAL: S7.10h -- a render that failed for a reason the daemon owns makes
            //       /health report a failing verdict, while an image the user can replace
            //       leaves it ok. Both directions are independently false and both are
            //       damaging: a daemon whose renders all die of a bug inside it reporting
            //       `ok` is a blank desktop nothing will ever flag, and a deleted wallpaper
            //       reporting `fail` is a watchdog restarting a daemon that is working.
            // WHY-THIS-SHAPE: the three renders are made on the same monitor of the same
            //       manager, one after another, and the verdict is read between them. Three
            //       separate daemons would compare three configurations as much as three
            //       classifications; here the shader directory, the device, the monitor and
            //       every other check are identical across all three, so the ONLY thing that
            //       differs is which exception the render threw. `shaders` and `gpu` are
            //       asserted to stay `ok` through the failing one for the same reason: the
            //       fault has to land on `wallpapers` without accusing a dependency that is
            //       working, which is what S7.10a requires and what makes this different
            //       from scenario (b).
            //       The last render is what shows the flag is per-render rather than sticky.
            //       Without it, a build that latched the first daemon fault forever would
            //       pass every assertion above it.
            // CONTROL: three, all reverted. (a) `daemonFault = true;` deleted from both
            //       `catch (const std::exception&)` arms in wallpaper_manager.cpp's
            //       renderPassLocked (re-observed 2026-09-14) -> exactly three red: "a render that failed inside the
            //       daemon is recorded as the daemon's own fault", "after a render that
            //       failed inside the daemon, /health reports the verdict as fail", and "the
            //       failing wallpapers check says the failure was inside the daemon and
            //       carries the monitor's own error text". (b) both `catch (const
            //       WallpaperContentError&)` arms deleted, so a content rejection falls into
            //       the unclassified branch -> the two ok-side cases red on both of their
            //       checks ("an image that cannot be decoded is recorded as not the daemon's
            //       fault", "after ... /health reports the verdict as ok", and the same pair
            //       for the deleted file), plus scenario (c)'s two aggregate checks. (c)
            //       `daemonFaults > 0` in http_server.cpp's wallpapers derivation changed to
            //       `false` -> only the two fail-side checks red and the ok side untouched,
            //       which separates "the manager classified it" from "the endpoint acted on
            //       it": (a) and (c) redden overlapping but different sets.
            // COVERAGE-ASSERT: each of the three renders is required to have actually failed,
            //       and to have failed for its own reason (the shader case must name a
            //       shader, the deleted case must name the open), before its verdict is
            //       graded. A render that quietly succeeded -- a host that ships .spv files
            //       into every directory, say -- would otherwise leave three vacuous ok
            //       comparisons behind.
            //       CONTROL: `if (c.removeBeforeRender) continue;` added at the top of the
            //       loop -> "FAIL: every fault-ownership case reached the state it grades"
            //       with "  2 of 3 cases were graded", and nothing else. Reverted.
            struct FaultCase {
                const char* what;
                std::string path;
                // Deleted after the request and before the render, so the render meets a
                // file that was there when the daemon accepted it.
                bool removeBeforeRender;
                const char* errorFragment;
                bool expectDaemonFault;
                const char* expectAggregate;
                const char* expectWallpapers;
            };
            const std::vector<FaultCase> faults{
                {"an image that cannot be decoded", junkPath, false, "not a decodable image", false, "ok", "ok"},
                {"a render that failed inside the daemon", bluePath, false, "shader", true, "fail", "fail"},
                {"an image that was deleted before the render", vanishing, true, "cannot open image", false, "ok",
                 "ok"},
            };

            // One render before the table, only to find out whether this host can make a
            // device at all. Asked of the manager rather than of /health's own gpu check: a
            // gate that reads the field under test turns a broken endpoint into a skip.
            const bool deviceExists = [&] {
                daemon.manager().requestWallpaper("static", junkPath, 0);
                daemon.manager().renderPending(0);
                return daemon.manager().snapshot().gpu.initialized;
            }();
            int graded = 0;
            int skipped = 0;
            if (!deviceExists) {
                notVerified("S7.10h (a render failure the daemon owns fails the verdict; a wallpaper's does not)",
                            "no Vulkan device could be created on this host, so every render here fails for the "
                            "same reason and the classification cannot be observed");
            } else {
                for (const FaultCase& c : faults) {
                    daemon.manager().requestWallpaper("static", c.path, 0);
                    if (c.removeBeforeRender) {
                        std::error_code ec;
                        std::filesystem::remove(c.path, ec);
                        if (ec) {
                            ++skipped;
                            notVerified("S7.10h, one case",
                                        std::string("the fixture for '") + c.what + "' could not be deleted");
                            continue;
                        }
                    }
                    daemon.manager().renderPending(0);

                    const WallpaperManager::Snapshot snap = daemon.manager().snapshot();
                    const auto it = snap.monitors.find(0);
                    const std::string lastError = it != snap.monitors.end() ? it->second.lastError : std::string();
                    if (lastError.find(c.errorFragment) == std::string::npos) {
                        // X.3: the state this case grades was never reached, and grading the
                        // verdict anyway would report the endpoint for a property of the
                        // host's renderer.
                        ++skipped;
                        notVerified("S7.10h, one case",
                                    std::string("the '") + c.what + "' render did not fail the way this case needs " +
                                        "(monitor 0 recorded '" + lastError + "', expected it to name '" +
                                        c.errorFragment + "')");
                        continue;
                    }
                    const HttpReply health = daemon.health();
                    if (!health.ok()) {
                        // Graded, not skipped: an endpoint that stopped answering is the
                        // defect this file exists to find, and it has just been reported as
                        // one -- so this case is accounted for below rather than counted as
                        // coverage this host could not reach.
                        ++graded;
                        check(false, "the fault-ownership daemon answered /health with a parseable 200");
                        continue;
                    }
                    ++graded;

                    check(it->second.lastErrorIsDaemonFault == c.expectDaemonFault,
                          (std::string(c.what) + " is recorded as " +
                           (c.expectDaemonFault ? "the daemon's own fault" : "not the daemon's fault"))
                              .c_str());
                    const std::string aggregate = health.json.value("status", std::string());
                    const bool verdict = aggregate == c.expectAggregate &&
                                         healthCheckStatus(health.json, "wallpapers") == c.expectWallpapers;
                    check(verdict, (std::string("after ") + c.what + ", /health reports the verdict as " +
                                    c.expectAggregate)
                                       .c_str());
                    // The daemon's own dependencies are working, and the endpoint must keep
                    // saying so: pinning a render bug on the GPU or the shaders sends the
                    // reader to a component that is fine, which is the misattribution
                    // S7.10a exists to prevent.
                    check(healthCheckStatus(health.json, "shaders") == "ok" &&
                              healthCheckStatus(health.json, "gpu") == "ok",
                          (std::string("after ") + c.what + ", neither the shaders nor the GPU is implicated")
                              .c_str());
                    if (!verdict || healthCheckStatus(health.json, "gpu") != "ok") {
                        std::fprintf(stderr, "  %s -> %s\n", c.what, health.body.c_str());
                    }
                    if (c.expectDaemonFault) {
                        const std::string detail = healthCheckDetail(health.json, "wallpapers");
                        const bool named = detail.find("inside the daemon") != std::string::npos &&
                                           detail.find(lastError) != std::string::npos;
                        check(named, "the failing wallpapers check says the failure was inside the daemon and "
                                     "carries the monitor's own error text");
                        if (!named) std::fprintf(stderr, "  detail: %s\n  lastError: %s\n", detail.c_str(),
                                                 lastError.c_str());
                    }
                }
                // COVERAGE-ASSERT: every case in the table is accounted for -- graded, or
                //       reported through notVerified as one this host could not reach. What
                //       this must NOT do is assert `graded == faults.size()`: the two skip
                //       arms above are legitimate X.3 skips (a fixture this filesystem would
                //       not delete, a render that failed for a different reason on this
                //       driver), and counting them as a shortfall prints a named FAIL against
                //       the daemon for a property of the host, contradicting the notVerified
                //       the same iteration just issued. The counters are bumped on the exit
                //       paths rather than at the top of the loop, so the hollowing edit this
                //       guards against -- a `continue` inserted before any of them -- still
                //       goes red.
                // CONTROL: two, both reverted, and the pair is what establishes the shape.
                //       (a) a `continue` added as the first statement of the loop body, which
                //       is what hollowing this out looks like -> "FAIL: every fault-ownership
                //       case is either graded or reported as one this run could not reach",
                //       with "  0 graded, 0 skipped, of 3 cases", and nothing else red here.
                //       (b) the deleted-fixture arm forced (the `remove()` result replaced
                //       with a permission-denied error_code, a harness-input mutation) -> one
                //       NOT VERIFIED naming that case, no FAIL, and ctest Skipped. Run again
                //       with this line written as `graded == faults.size()`, the same
                //       mutation printed the NOT VERIFIED *and* "FAIL: ..." -- the daemon
                //       accused of a fault the harness had just reported as the host's.
                check(graded + skipped == static_cast<int>(faults.size()),
                      "every fault-ownership case is either graded or reported as one this run could not reach");
                if (graded + skipped != static_cast<int>(faults.size())) {
                    std::fprintf(stderr, "  %d graded, %d skipped, of %zu cases\n", graded, skipped, faults.size());
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

    // One armed export, reported as the pair the checks below need: whether the injected
    // result actually reached exportFrame, what it threw, and what /health said afterwards.
    // Written once because the two arms differ only in the result injected and in what they
    // expect of `gpu`, and a second hand-written copy of the arming, the fd hygiene and the
    // reached-the-driver guard is a second thing to get wrong.
    struct ExportOutcome {
        bool reached = false;  // the fault was returned to exportFrame exactly once
        int observed = 0;
        bool threw = false;
        std::string error;
        HttpReply health;
    };
    auto exportUnder = [&daemon](VkResult injected) {
        ExportOutcome outcome;
        {
            GetMemoryFdFault fault(/*failAt=*/1, injected);
            try {
                wallpaperx::FrameExport exported = daemon.manager().exportCurrentFrame(0);
                // Only reached if the fault missed. The caller owns the fd (exportFrame's
                // contract), and leaking one out of a phase that then keeps running is
                // X.4's business, not something to leave to process exit.
                if (exported.handle && exported.handle->frame.fd >= 0) ::close(exported.handle->frame.fd);
            } catch (const std::exception& e) {
                outcome.threw = true;
                outcome.error = e.what();
            }
            outcome.observed = fault.observed();
            outcome.reached = fault.injected() == 1;
        }
        outcome.health = daemon.health();
        return outcome;
    };

    // GOAL: S7.10a aimed at the export -- a frame export that fails because the *host* is
    //       out of file descriptors is not the GPU going wrong, and must leave `gpu`
    //       reading ok while GetFrameFd still fails. False if a consumer that leaks the
    //       descriptors GetFrameFd hands it can drive /health to blame the graphics driver,
    //       which would send an operator to their driver for a problem in RLIMIT_NOFILE.
    // WHY-THIS-SHAPE: VK_ERROR_TOO_MANY_OBJECTS is one of the two results the spec permits
    //       this call to return, and it is what fd exhaustion produces. Run before the
    //       unpermitted-result arm below, not after: that arm records a GPU failure that
    //       only a later successful render clears, so afterwards "gpu is ok" would be
    //       unreachable and this check could only ever be red or vacuous.
    // CONTROL: renderer/vulkan/frame_export.cpp -- in throwNoDescriptor, the
    //       `throw std::runtime_error(...)` under `result == VK_SUCCESS ||
    //       isResourceExhaustion(result)` changed to `throw GpuFailure(...)`, which is the
    //       classification this split replaced -> "FAIL: a frame export that fails on a host
    //       resource limit leaves the GPU's recorded health alone", printing "gpu after an
    //       fd-exhaustion export: fail (exportFrame: vkGetMemoryFdKHR produced no file
    //       descriptor (VK_ERROR_TOO_MANY_OBJECTS) -- a resource limit, not the graphics
    //       driver: ...)", and nothing else in the file red. Reverted.
    // COVERAGE-ASSERT: reached must be true. An interposer that stopped landing would leave
    //       a successful export and a gpu that reads ok for the ordinary reason, which is
    //       exactly this check's pass condition -- so it is skipped, not passed.
    {
        const ExportOutcome hostLimit = exportUnder(VK_ERROR_TOO_MANY_OBJECTS);
        if (!hostLimit.reached) {
            notVerified("S7.10a (an fd-exhaustion export is not blamed on the GPU)",
                        "exportCurrentFrame made " + std::to_string(hostLimit.observed) +
                            " vkGetMemoryFdKHR call(s) with the interposer armed on the first, so no fault reached "
                            "the export path on this host" +
                            (hostLimit.threw ? " (it threw for another reason: " + hostLimit.error + ")" : ""));
        } else if (!hostLimit.health.ok()) {
            check(false, "the fd-exhaustion daemon answered /health with a parseable 200");
        } else {
            check(hostLimit.threw && hostLimit.error.find("file descriptor") != std::string::npos,
                  "an export that runs out of file descriptors fails the caller rather than returning a frame");
            check(healthCheckStatus(hostLimit.health.json, "gpu") == "ok",
                  "a frame export that fails on a host resource limit leaves the GPU's recorded health alone");
            if (healthCheckStatus(hostLimit.health.json, "gpu") != "ok" || !hostLimit.threw) {
                std::fprintf(stderr, "  gpu after an fd-exhaustion export: %s (%s); export threw: %s\n",
                             healthCheckStatus(hostLimit.health.json, "gpu").c_str(),
                             healthCheckDetail(hostLimit.health.json, "gpu").c_str(),
                             hostLimit.threw ? hostLimit.error.c_str() : "no");
            }
        }
    }

    // GOAL: S7.10e reaches the export too -- when the fd query returns a result this call is
    //       not permitted to return, the driver is what went wrong and /health must record
    //       it against the GPU. False if a daemon whose every GetFrameFd dies in the driver
    //       still reports gpu ok.
    // WHY-THIS-SHAPE: the injected result is VK_ERROR_INITIALIZATION_FAILED, chosen because
    //       it is outside the set vkGetMemoryFdKHR may return. The obvious pick,
    //       VK_ERROR_TOO_MANY_OBJECTS, is now a host resource limit and deliberately does
    //       NOT reach the GPU's health -- it is the arm above. Aimed at the fd query rather
    //       than at a submit because the export no longer submits anything: publishing moved
    //       into the render, and the submit interposer observes zero calls here. This runs
    //       immediately after the arm above left gpu reading ok on this same daemon, so the
    //       fail below cannot be left over from the earlier render fault.
    // CONTROL: two, both reverted. (1) The `recordHealth(&DaemonHealth::gpu, ...)` line
    //       deleted from the `catch (const GpuFailure&)` in WallpaperManager::exportHandle,
    //       backend/daemon/wallpaper_manager.cpp -> "FAIL: a frame export whose driver
    //       returned a result it may not return is recorded against the GPU", printing "gpu
    //       after an unpermitted driver result: ok (NVIDIA GeForce RTX 3080 Ti (Heap))".
    //       (2) In throwNoDescriptor, renderer/vulkan/frame_export.cpp, the tail
    //       `throwVkFailure(context, result)` replaced by `throw std::runtime_error(context)`
    //       -> the same check red the same way, because an unclassified throw is not
    //       attributed to the GPU. Neither reddened the propagation check, so the two are
    //       independent.
    // COVERAGE-ASSERT: reached must be 1. With the name test in the vkGetDeviceProcAddr
    //       interposer disabled the phase reported "exportCurrentFrame made 0
    //       vkGetMemoryFdKHR call(s)" and ctest recorded a skip -- an interposer that stops
    //       landing cannot leave this looking green.
    {
        const ExportOutcome driverFault = exportUnder(VK_ERROR_INITIALIZATION_FAILED);
        if (!driverFault.reached) {
            notVerified("S7.10e (a failed frame export is attributed to the GPU)",
                        "exportCurrentFrame made " + std::to_string(driverFault.observed) +
                            " vkGetMemoryFdKHR call(s) with the interposer armed on the first, so no fault reached "
                            "the export path on this host" +
                            (driverFault.threw ? " (it threw for another reason: " + driverFault.error + ")" : ""));
        } else if (!driverFault.health.ok()) {
            check(false, "the failed-export daemon answered /health with a parseable 200");
        } else {
            check(driverFault.threw && driverFault.error.find("vkGetMemoryFdKHR") != std::string::npos,
                  "exportCurrentFrame propagates the driver failure instead of returning a frame");
            check(healthCheckStatus(driverFault.health.json, "gpu") == "fail",
                  "a frame export whose driver returned a result it may not return is recorded against the GPU");
            if (healthCheckStatus(driverFault.health.json, "gpu") != "fail") {
                std::fprintf(stderr, "  gpu after an unpermitted driver result: %s (%s); export threw: %s\n",
                             healthCheckStatus(driverFault.health.json, "gpu").c_str(),
                             healthCheckDetail(driverFault.health.json, "gpu").c_str(),
                             driverFault.threw ? driverFault.error.c_str() : "no");
            }
        }
    }

    // ------------------------------------------------------------------ //
    // What a failed export leaves behind in the daemon's claim ledger
    // ------------------------------------------------------------------ //
    //
    // Every arm above exports for no client at all, which records nothing, so none of them
    // reaches the bookkeeping a NAMED export does. exportCurrentFrame() charges the client with
    // holding the image before the descriptors exist, and undoes that charge if the export then
    // throws. That undo runs on no other path in either suite.
    //
    // The checks live in this file rather than beside the other hand-over checks in
    // dbus_service_test.cpp because the lever does not travel: making an export fail on demand
    // means interposing vkGetDeviceProcAddr, which is a process-global symbol defined by this
    // translation unit. The alternative lever -- a consumer reporting a DRM format modifier the
    // driver cannot produce -- only exists on a dma_buf host and would leave the goal ungraded
    // on the NVIDIA machine this daemon targets.

    // Whether the daemon still has a claim for `client` on `bufferId`, asked by ending it. The
    // hold list is private; a release is the daemon's own report of whether there was a claim
    // there, and it reports one either by naming a monitor to re-render or by re-announcing the
    // committed frame -- reportFreedImageLocked() always produces exactly one of the two.
    auto releaseFreedSomething = [&daemon](const std::string& client, uint64_t bufferId) {
        std::vector<wallpaperx::FrameAvailable> announced;
        const std::vector<int> toRender = daemon.manager().releaseBuffer(client, 0, bufferId, &announced);
        return !toRender.empty() || !announced.empty();
    };

    // One export for a NAMED client with the fd query failed. VK_ERROR_TOO_MANY_OBJECTS rather
    // than a driver fault, so this sub-phase leaves the recorded GPU health where it found it
    // and the sampler arms below still start from a state they set themselves.
    struct NamedExportFault {
        bool reached = false;
        bool threw = false;
        int observed = 0;
    };
    auto faultedExportFor = [&daemon](const std::string& client) {
        NamedExportFault outcome;
        GetMemoryFdFault fault(/*failAt=*/1, VK_ERROR_TOO_MANY_OBJECTS);
        try {
            wallpaperx::FrameExport exported = daemon.manager().exportCurrentFrame(0, client);
            if (exported.handle && exported.handle->frame.fd >= 0) ::close(exported.handle->frame.fd);
            if (exported.handle && exported.handle->acquireFd >= 0) ::close(exported.handle->acquireFd);
        } catch (const std::exception&) {
            outcome.threw = true;
        }
        outcome.observed = fault.observed();
        outcome.reached = fault.injected() == 1;
        return outcome;
    };

    {
        const std::string holder = ":test.rollback.holder";
        const std::string newcomer = ":test.rollback.newcomer";

        // The premise: one successful export, so `holder` genuinely has a claim to keep.
        uint64_t held = 0;
        std::string setupError;
        try {
            wallpaperx::FrameExport granted = daemon.manager().exportCurrentFrame(0, holder);
            if (granted.handle) {
                held = granted.handle->frame.bufferId;
                if (granted.handle->frame.fd >= 0) ::close(granted.handle->frame.fd);
                if (granted.handle->acquireFd >= 0) ::close(granted.handle->acquireFd);
            }
        } catch (const std::exception& e) {
            setupError = e.what();
        }
        const uint64_t generationBefore = [&daemon] {
            const WallpaperManager::Snapshot snap = daemon.manager().snapshot();
            const auto it = snap.monitors.find(0);
            return it == snap.monitors.end() ? 0 : it->second.generation;
        }();

        const NamedExportFault repeat = held != 0 ? faultedExportFor(holder) : NamedExportFault{};
        const bool holderStillHolds = repeat.reached && releaseFreedSomething(holder, held);
        const NamedExportFault fresh = held != 0 ? faultedExportFor(newcomer) : NamedExportFault{};
        const bool newcomerCharged = fresh.reached && releaseFreedSomething(newcomer, held);

        const uint64_t generationAfter = [&daemon] {
            const WallpaperManager::Snapshot snap = daemon.manager().snapshot();
            const auto it = snap.monitors.find(0);
            return it == snap.monitors.end() ? 0 : it->second.generation;
        }();

        if (held == 0 || !repeat.reached || !fresh.reached || generationAfter != generationBefore) {
            // A skip, not a check(): with no claim recorded, or with the fault landing
            // somewhere other than the export, the two assertions below would be describing a
            // state this phase never built and would name the daemon for a fault of the host.
            notVerified("the export rollback (a throwing export leaves the claim ledger as it was)",
                        held == 0 ? "the export that was to give a client a claim returned no frame" +
                                        (setupError.empty() ? std::string() : " (it threw: " + setupError + ")")
                        : generationAfter != generationBefore
                            ? "monitor 0 committed a new frame during the sub-phase, so the image the claims were "
                              "recorded against is no longer the one the exports were asked for"
                            : "the interposed vkGetMemoryFdKHR did not land on the named exports -- " +
                                  std::to_string(repeat.observed) + " and " + std::to_string(fresh.observed) +
                                  " call(s) observed with the interposer armed on the first of each");
        } else if (!repeat.threw || !fresh.threw) {
            // Asserted, not skipped: the fault reached the driver call and production went on to
            // return a frame anyway, which is the daemon's fault and not the host's.
            check(false, "an export whose file-descriptor query fails is failed to its named caller rather than "
                         "returning a frame");
            std::fprintf(stderr, "  the repeat export %s and the newcomer's %s\n",
                         repeat.threw ? "threw" : "returned normally", fresh.threw ? "threw" : "returned normally");
        } else {
            // GOAL: an export that throws for a client that did NOT already hold that image
            //       leaves the client charged with nothing. False if a consumer whose GetFrameFd
            //       failed is left holding an image it was never handed -- an id it cannot name
            //       to release, which keeps that image out of the producer's ring until the
            //       consumer leaves the bus.
            // WHY-THIS-SHAPE: graded by asking the daemon to end the claim and requiring it to
            //       report that there was none. Asserting on the exception alone establishes
            //       nothing: the throw is the same with and without the rollback, and the whole
            //       difference is in state this file cannot read directly. The claim is charged
            //       BEFORE the descriptors exist, so "never recorded" is not the daemon's
            //       behaviour here -- the undo is.
            // CONTROL: the `if (claimAdded)` rollback block in the catch of
            //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp)
            //       deleted -> "FAIL: a client whose export threw is left holding nothing, so no
            //       image is stranded in a claim it cannot name", printing "the prior holder
            //       kept its claim, the newcomer was left charged with one"; the only red line
            //       in the run, and the check below stayed green. Reverted.
            check(!newcomerCharged,
                  "a client whose export threw is left holding nothing, so no image is stranded in a claim it "
                  "cannot name");

            // GOAL: and the undo is limited to what that call added. A client that already held
            //       the image keeps its claim through a failed export, because it is still
            //       reading the image that claim is on.
            // WHY-THIS-SHAPE: the same client is exported to twice -- once successfully, once
            //       under the fault -- with no render in between, so the second export names the
            //       image the first one handed over. That is the only configuration in which the
            //       rollback and an unconditional undo disagree; a rollback test that used a
            //       fresh client every time would pass under both.
            // CONTROL: `if (claimAdded)` in the catch of WallpaperManager::exportCurrentFrame
            //       (backend/daemon/wallpaper_manager.cpp) changed to `if (true)`, which is the
            //       rollback undoing more than the call added -> "FAIL: a failed export does not
            //       take back the claim its client already had on that image", printing "the
            //       prior holder lost the claim it already had, the newcomer was charged with
            //       nothing"; the only red line, and the check above stayed green. The two
            //       controls redden one check each, so the pair is not one assertion written
            //       twice. Reverted.
            check(holderStillHolds,
                  "a failed export does not take back the claim its client already had on that image");
            if (!holderStillHolds || newcomerCharged) {
                std::fprintf(stderr, "  after two failed exports of image %llu: the prior holder %s, the newcomer "
                                     "%s\n",
                             static_cast<unsigned long long>(held),
                             holderStillHolds ? "kept its claim" : "lost the claim it already had",
                             newcomerCharged ? "was left charged with one" : "was charged with nothing");
            }
        }
    }

    // ------------------------------------------------------------------ //
    // The object creation a render does before it submits anything
    // ------------------------------------------------------------------ //
    //
    // The two levers used above reach exactly two driver calls: the render's submit and the
    // export's file-descriptor query. renderer/vulkan/gpu_fault.cpp's throwVkFailure is
    // called from well over a dozen other sites -- the allocator and its buffer and image
    // creates, the render pass, image views, framebuffers, the three descriptor tiers, the
    // static renderer's own objects -- and every one of them classifies a resource limit
    // apart from a driver fault. Deleting that classification outright leaves both suites
    // green without the two arms below, so what follows is the only thing in either suite
    // that grades an object create's classification reaching the daemon's health.
    //
    // Both arms drive one whole render with StaticRenderer's vkCreateSampler failed, and
    // differ only in the result injected and in what they then require of `gpu`. Each is
    // preceded by a successful render that puts the recorded health back to ok, so neither
    // inherits the other's verdict.
    struct SamplerFaultOutcome {
        bool reached = false;  // the fault was returned to production exactly once
        int observed = 0;
        bool committed = false;
        std::string lastError;
        HttpReply health;
    };
    auto renderWithSamplerFault = [&daemon, &bluePath](VkResult injected) {
        SamplerFaultOutcome outcome;
        {
            CreateSamplerFault fault(/*failAt=*/1, injected);
            daemon.manager().requestWallpaper("static", bluePath, 0);
            daemon.manager().renderPending(0);
            outcome.observed = fault.observed();
            outcome.reached = fault.injected() == 1;
        }
        const WallpaperManager::Snapshot snap = daemon.manager().snapshot();
        if (const auto it = snap.monitors.find(0); it != snap.monitors.end()) {
            outcome.committed = it->second.hasFrame;
            outcome.lastError = it->second.lastError;
        }
        outcome.health = daemon.health();
        return outcome;
    };

    // One successful render, whose only job is to leave `gpu` reading ok. Returns the detail
    // it left behind, which is the baseline "was left alone" is measured against; an empty
    // string means the daemon could not be put into that state, and the caller must skip
    // rather than grade. A daemon reporting no GPU name is refused for the reason the
    // recovery check above gives: the detail would be " ()", and comparing against that
    // establishes nothing.
    auto restoreHealthyGpu = [&daemon, &bluePath]() -> std::string {
        daemon.manager().requestWallpaper("static", bluePath, 0);
        daemon.manager().renderPending(0);
        const WallpaperManager::Snapshot snap = daemon.manager().snapshot();
        const auto it = snap.monitors.find(0);
        if (it == snap.monitors.end() || !it->second.hasFrame || snap.gpu.name.empty()) return {};
        const HttpReply health = daemon.health();
        if (!health.ok() || healthCheckStatus(health.json, "gpu") != "ok") return {};
        return healthCheckDetail(health.json, "gpu");
    };

    // GOAL: S7.10i -- a driver call that fails because a host or device resource ran out
    //       fails the render and tells the caller which call failed, and leaves the daemon's
    //       recorded GPU health exactly as it was. False if one out-of-host-memory inside a
    //       render makes /health report the graphics driver as broken, which sends an
    //       operator after their driver for a shortage of memory.
    // WHY-THIS-SHAPE: aimed at an object create (vkCreateSampler) rather than at the submit
    //       the phase above already fails. Injecting a limit at the submit is the obvious
    //       cheaper alternative and it grades one code path twice -- Device::submit's
    //       classification is the one already covered -- while leaving every create site,
    //       which is where throwVkFailure is actually called from, ungraded. The status AND
    //       the detail recorded before the fault are both compared afterwards: "left alone"
    //       is a claim about the whole record, and a status-only check passes on a daemon
    //       that reports ok while its detail holds the text of a failure.
    // CONTROL: the `isResourceExhaustion(result)` branch deleted from throwVkFailure in
    //       renderer/vulkan/gpu_fault.cpp, so every failure becomes a GpuFailure -> "FAIL: a
    //       render that fails on a resource limit leaves the GPU's recorded health alone"
    //       and "FAIL: a render that fails on a resource limit leaves the recorded GPU
    //       detail alone", printing "gpu after a resource-limit render: fail (StaticRenderer:
    //       vkCreateSampler failed with VK_ERROR_OUT_OF_HOST_MEMORY) (expected ok / NVIDIA
    //       GeForce RTX 3080 Ti (Heap))". Under that same mutation exactly three of the 74
    //       backend entries went red -- this one on both drivers, plus renderer/tests/
    //       gpu_fault_test.cpp -- and the other 71 stayed green, which is the measurement
    //       this arm exists for. The export arm above is among the 71, but not because it
    //       avoids throwVkFailure -- throwNoDescriptor in renderer/vulkan/frame_export.cpp
    //       calls it for every result that is not a limit. It stays green because a limit
    //       never gets there: throwNoDescriptor tests isResourceExhaustion() itself and
    //       throws its own unclassified error first. Reverted.
    // COVERAGE-ASSERT: `reached` must be true and the render must have failed. An interposer
    //       that stopped landing leaves a successful render and a gpu reading ok for the
    //       ordinary reason -- which is this arm's own pass condition -- so it skips.
    //       Observed with the armedFault() call deleted from this file's vkCreateSampler:
    //       "the render made 0 vkCreateSampler call(s) ... and still committed a frame", and
    //       ctest recorded a Skip rather than a pass.
    const std::string healthyDetail = restoreHealthyGpu();
    if (healthyDetail.empty()) {
        notVerified("S7.10i (a resource limit inside a render is not blamed on the GPU)",
                    "the daemon could not be returned to a state where gpu reads ok with a name, so there is no "
                    "baseline for \"the record was left alone\" to be compared against");
    } else {
        const SamplerFaultOutcome limit = renderWithSamplerFault(VK_ERROR_OUT_OF_HOST_MEMORY);
        if (!limit.reached || limit.committed) {
            notVerified("S7.10i (a resource limit inside a render is not blamed on the GPU)",
                        "the render made " + std::to_string(limit.observed) +
                            " vkCreateSampler call(s) with the interposer armed on the first, and " +
                            (limit.committed ? "still committed a frame" : "no fault reached production") +
                            " -- either StaticRenderer no longer creates a sampler, or this executable's "
                            "vkCreateSampler is not what it calls");
        } else if (!limit.health.ok()) {
            check(false, "the resource-limit daemon answered /health with a parseable 200");
        } else {
            check(limit.lastError.find("StaticRenderer: vkCreateSampler") != std::string::npos,
                  "a render that fails on a resource limit tells the caller which driver call failed");
            check(healthCheckStatus(limit.health.json, "gpu") == "ok",
                  "a render that fails on a resource limit leaves the GPU's recorded health alone");
            check(healthCheckDetail(limit.health.json, "gpu") == healthyDetail,
                  "a render that fails on a resource limit leaves the recorded GPU detail alone");
            // Attribution, as on the submit arm: the shader directory resolved fine and a
            // sampler that could not be created says nothing about it.
            check(healthCheckStatus(limit.health.json, "shaders") == "ok",
                  "a render that fails on a resource limit does not implicate the shaders");
            if (healthCheckStatus(limit.health.json, "gpu") != "ok" ||
                healthCheckDetail(limit.health.json, "gpu") != healthyDetail) {
                std::fprintf(stderr, "  gpu after a resource-limit render: %s (%s) (expected ok / %s); render said: %s\n",
                             healthCheckStatus(limit.health.json, "gpu").c_str(),
                             healthCheckDetail(limit.health.json, "gpu").c_str(), healthyDetail.c_str(),
                             limit.lastError.c_str());
            }
        }
    }

    // GOAL: S7.10j -- the other direction, at a site the submit arm above does not cover: a
    //       driver call that fails with a lost device while *creating an object* is recorded
    //       against the GPU. False if a daemon whose every render dies in vkCreateSampler
    //       still reports gpu ok, which is a watchdog being shown a blank desktop and told
    //       the graphics stack is fine.
    // WHY-THIS-SHAPE: a fresh successful render runs first, so this arm starts from gpu ok
    //       whatever the arm above concluded -- without it, a bug that made the resource
    //       limit record a failure would leave gpu already reading fail and this check would
    //       pass on the previous arm's damage. VK_ERROR_DEVICE_LOST rather than an
    //       out-of-range code because a create is permitted to return it, so the daemon is
    //       being asked to classify a real failure rather than an impossible one.
    // CONTROL: two, both reverted. (1) `case VK_ERROR_DEVICE_LOST:` added to
    //       isResourceExhaustion in renderer/vulkan/gpu_fault.cpp, so a lost device is raised
    //       as an unclassified runtime_error -> "FAIL: a render whose object create reports a
    //       lost device is recorded against the GPU" and "FAIL: the gpu detail names the
    //       object create that failed", printing "gpu after a lost-device object create: ok
    //       (NVIDIA GeForce RTX 3080 Ti (Heap))". That one also reddens the submit arm above,
    //       which is expected -- it mutates the classifier both share. (2) The site alone:
    //       the sampler create in renderer/renderers/static_renderer.cpp changed from
    //       throwVkFailure to `throw std::runtime_error(...)` -> the same two reds and
    //       nothing else, the submit arm green. That is what establishes these two are not
    //       one check wearing two hats.
    // COVERAGE-ASSERT: same as the arm above -- `reached` true and the render failed, or the
    //       phase skips. Here a missed interposer would produce a successful render and gpu
    //       ok, which fails this check rather than passing it, so the guard is about naming
    //       the right component (X.3) rather than about vacuity.
    const std::string healthyDetailBeforeFault = restoreHealthyGpu();
    if (healthyDetailBeforeFault.empty()) {
        notVerified("S7.10j (a lost device inside a render's object creation is blamed on the GPU)",
                    "the daemon could not be returned to a state where gpu reads ok, so a later gpu: fail could "
                    "not be told from one left over by the arm before this");
        return;
    }

    const SamplerFaultOutcome lostDevice = renderWithSamplerFault(VK_ERROR_DEVICE_LOST);
    if (!lostDevice.reached || lostDevice.committed) {
        notVerified("S7.10j (a lost device inside a render's object creation is blamed on the GPU)",
                    "the render made " + std::to_string(lostDevice.observed) +
                        " vkCreateSampler call(s) with the interposer armed on the first, and " +
                        (lostDevice.committed ? "still committed a frame" : "no fault reached production"));
        return;
    }
    if (!lostDevice.health.ok()) {
        check(false, "the lost-device daemon answered /health with a parseable 200");
        return;
    }
    check(healthCheckStatus(lostDevice.health.json, "gpu") == "fail",
          "a render whose object create reports a lost device is recorded against the GPU");
    check(healthCheckDetail(lostDevice.health.json, "gpu").find("vkCreateSampler") != std::string::npos,
          "the gpu detail names the object create that failed");
    check(healthCheckStatus(lostDevice.health.json, "shaders") == "ok",
          "a lost device during object creation does not implicate the shaders");
    if (healthCheckStatus(lostDevice.health.json, "gpu") != "fail") {
        std::fprintf(stderr, "  gpu after a lost-device object create: %s (%s)\n",
                     healthCheckStatus(lostDevice.health.json, "gpu").c_str(),
                     healthCheckDetail(lostDevice.health.json, "gpu").c_str());
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
    // GOAL: X.4 as much as S7.5 -- a /config that never answered must be reported by these
    //       checks, not thrown out of the phase. False if a request that did not reach the
    //       server ends the run instead of failing here.
    // WHY-THIS-SHAPE: hasJsonObject(), not `!json.is_discarded()`. A reply that never
    //       reached the server carries a null json, which is *not* discarded, so the
    //       obvious guard passes and value() then throws type_error.306 -- which unwinds
    //       into main and drops every remaining phase with no NOT VERIFIED row.
    // CONTROL: HttpServer::kLoopbackHost changed to "192.0.2.1" (unbindable), so no phase
    //       can reach the server -> these two go red as
    //       "FAIL: /config with no monitor parameter answers for monitor 0" rather than
    //       aborting, and the phases after this one still run. Reverted.
    check(documented.hasJsonObject() && documented.json.value("monitor_id", -1) == 0,
          "/config with no monitor parameter answers for monitor 0");
    check(documented.hasJsonObject() && documented.json.value("path", std::string()) == recordedPath,
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
        check(res.hasJsonObject() && res.json.value("monitor_id", -1) == id,
              "/config echoes back the monitor id it was asked about");
        if (!(res.reached && res.status == 200)) {
            std::fprintf(stderr, "  monitor=%d answered %d\n", id, res.status);
        }
    }

    // A monitor nothing has been set on: empty strings and 200, not 404. The plugin polls
    // this once a second from session start, and a fresh monitor is the ordinary state.
    const HttpReply unset = get(cli, "/config?monitor=" + std::to_string(kMonitorNeverSet));
    check(unset.hasJsonObject() && unset.json.value("type", std::string("x")).empty() &&
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
// Phase 3b -- Step 10: asking /config by connector name instead of by monitor id.
// ---------------------------------------------------------------------------- //

// A query-string spelling of arbitrary bytes. Needed because the names this phase asks
// about deliberately contain a newline, a right-to-left override and DEL: written raw into
// a request line they would not survive the HTTP parser, so a failure to escape them would
// look like a daemon defect.
std::string percentEncode(const std::string& raw) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        const bool unreserved = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
                                b == '-' || b == '.' || b == '_' || b == '~';
        if (unreserved) {
            out += static_cast<char>(b);
            continue;
        }
        out += '%';
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
    }
    return out;
}

std::string configByOutput(const std::string& name) { return "/config?output=" + percentEncode(name); }

// nlohmann's value(key, default) defaults on an ABSENT key only -- on a present JSON null it
// throws. /config answers `monitor_id` as null for a name it does not know and `path_literal`
// as null for a stored path that is not UTF-8, so value() would turn the very regression these
// checks exist to catch into an exception that main's catch-all reports as one opaque line,
// abandoning every later phase with no NOT VERIFIED row (X.4).
std::optional<int> intField(const HttpReply& reply, const char* key) {
    if (!reply.hasJsonObject() || !reply.json.contains(key) || !reply.json[key].is_number_integer()) {
        return std::nullopt;
    }
    return reply.json[key].get<int>();
}

std::optional<std::string> stringField(const HttpReply& reply, const char* key) {
    if (!reply.hasJsonObject() || !reply.json.contains(key) || !reply.json[key].is_string()) {
        return std::nullopt;
    }
    return reply.json[key].get<std::string>();
}

std::optional<bool> boolField(const HttpReply& reply, const char* key) {
    if (!reply.hasJsonObject() || !reply.json.contains(key) || !reply.json[key].is_boolean()) {
        return std::nullopt;
    }
    return reply.json[key].get<bool>();
}

// GOAL: a name /config could never have been given is refused with 400 rather than
//       answered "I have never heard of it". False if an empty, oversized or
//       non-UTF-8 name is answered 200 -- the daemon would then be telling a caller its
//       screen is unknown when the real answer is that the request is malformed, and
//       "run the app" is the wrong advice for it.
// WHY-THIS-SHAPE: the cap is exercised on BOTH sides -- 256 bytes accepted, 257
//       rejected -- because a check on the oversized case alone passes for a daemon
//       whose bound is off by one in either direction, and a name one surface accepts
//       that another cannot hold is precisely what one shared outputNameError() exists
//       to prevent. This is also the only route on which the daemon's own UTF-8
//       rejection is reachable from a client: over DBus the bus library refuses to
//       build a message holding an invalid UTF-8 string, so that clause is enforced
//       before the daemon sees it (measured -- see the DBus tier's own note).
//       A free function rather than part of runOutputParamChecks: the daemon rejects all
//       four of these names inside parseIdentity before any lookup, so they need no
//       connector name recorded, and leaving them in the phase's setup-dependent region
//       meant a setup that threw dropped them unrun and unreported.
// CONTROL: two, reverted. (a) the `outputNameError` call deleted from parseIdentity() in
//       backend/daemon/http_server.cpp -> three FAILs, "an empty output= is rejected with
//       400", the oversized one and the non-UTF-8 one, each answering 200 with
//       known=false -- the last echoing `"output":"DP-\\xff-1"`, which is also the
//       measurement that the daemon's own UTF-8 rejection is genuinely reached on this
//       route rather than pre-empted by the HTTP layer. (b) `name.size() >
//       kMaxOutputNameBytes` in backend/daemon/config_store.h weakened to `>=` -> "FAIL:
//       a connector name of exactly the maximum length is accepted" ("a 256-byte name
//       answered 400"), while the 257-byte case stayed green. (c) the accept side is graded
//       on a parsed 200 carrying `known`, not on the status: asking it with "/config?t=1" --
//       a 200 that never went through the connector-name branch -- reddens that line, where
//       written as `reached && status == 200` the same request stayed GREEN.
void runOutputNameValidationChecks(httplib::Client& cli, const Check& check) {
    const struct {
        std::string value;
        const char* what;
    } kUnusable[] = {
        {"", "an empty output= is rejected with 400"},
        {std::string(wallpaperx::kMaxOutputNameBytes + 1, 'D'), "an oversized output= is rejected with 400"},
        {std::string("DP-\xFF-1"), "an output= that is not valid UTF-8 is rejected with 400"},
    };
    for (const auto& bad : kUnusable) {
        const HttpReply res = get(cli, configByOutput(bad.value));
        check(res.reached && res.status == 400, bad.what);
        if (!(res.reached && res.status == 400)) {
            std::fprintf(stderr, "  a %zu-byte name answered %d: %s\n", bad.value.size(), res.status,
                         res.body.c_str());
        }
    }
    const HttpReply atCap = get(cli, configByOutput(std::string(wallpaperx::kMaxOutputNameBytes, 'D')));
    // A parsed 200 carrying `known`, not a bare 200: httplib answers 200 by default for a
    // handler that writes nothing, so "did not refuse" and "answered" are different states,
    // and `known` is emitted only by the connector-name branch -- a name at the cap that fell
    // through to the monitor branch would otherwise grade as accepted.
    const bool atCapAccepted = atCap.ok() && atCap.json.contains("known");
    check(atCapAccepted, "a connector name of exactly the maximum length is accepted");
    if (!atCapAccepted) {
        std::fprintf(stderr, "  a %zu-byte name answered %d: %s\n", wallpaperx::kMaxOutputNameBytes, atCap.status,
                     atCap.body.c_str());
    }
}

void runOutputParamChecks(WallpaperManager& manager, httplib::Client& cli, const std::string& monitor0Path,
                          const std::string& namedPath, const std::string& otherPath, const Check& check) {
    // Bytes every display surface has to escape, and valid UTF-8 so the name is one the
    // daemon will accept at all: U+202E, a newline and DEL.
    const std::string hostileName = std::string("DP-\xE2\x80\xAE\nHDMI\x7F-1");
    const std::string unknownName = "DVI-I-99-never-told";

    // COVERAGE-ASSERT for every path comparison and body search below: the three wallpapers
    // must be three different files, must survive the daemon's display escaper unchanged, and
    // must carry no byte nlohmann escapes. Graded before the setup because it is a statement
    // about the fixtures alone. The first clause keeps the not-the-other-monitor's checks
    // falsifiable; the second is what lets `path` be compared for equality (the handler runs
    // it through sanitizeForLog); the third is what keeps the raw-body searches from going
    // quietly vacuous under a build directory holding a quote or a backslash.
    // CONTROL: `namedPath` set to bluePath at the call site in main() -> "FAIL: the three
    //       wallpapers this phase distinguishes between are three different files", plus the
    //       not-monitor-0's-wallpaper line it makes unsatisfiable. Restored.
    //       And `displaySafe(namedPath)` called as `displaySafe(namedPath + "\"")` -> "FAIL:
    //       the three wallpapers ... survive display escaping and JSON encoding unchanged",
    //       nothing else red. Restored.
    const auto displaySafe = [](const std::string& p) {
        return wallpaperx::sanitizeForLog(p, 4096, wallpaperx::LogFraming::Bare) == p &&
               p.find_first_of("\"\\") == std::string::npos;
    };
    check(namedPath != otherPath && namedPath != monitor0Path && otherPath != monitor0Path,
          "the three wallpapers this phase distinguishes between are three different files");
    check(displaySafe(namedPath) && displaySafe(otherPath) && displaySafe(monitor0Path),
          "the three wallpapers this phase distinguishes between survive display escaping and JSON encoding "
          "unchanged");

    // Ahead of the setup, and outside its try: these need no connector name recorded, so a
    // setup that throws must not take them with it.
    runOutputNameValidationChecks(cli, check);

    // Every goal the region below this setup grades, named in one place: a goal that vanishes
    // from the ledger reads afterwards as a goal that was established. S10.8 is not among them
    // -- its hostile name is bound in its own try, further down, so a name the daemon refuses
    // does not take with it the goals whose fixtures are already recorded.
    static constexpr const char* kSetupDependentGoals =
        "S10.1, S10.2, S10.3, S10.6 (asking /config by connector name), that the plugin's own polled shape "
        "(output= beside a cache-buster) resolves like the bare form, and that `known` and `output` appear on "
        "no other /config reply";
    try {
        manager.requestWallpaper("static", namedPath, kMonitorNamedSet);
        manager.requestWallpaper("static", otherPath, kMonitorNamedOther);
        nameScreen(manager, kMonitorNamedSet, "DP-1");
        nameScreen(manager, kMonitorNamedOther, "DP-2");
        nameScreen(manager, kMonitorNamedUnset, "HDMI-A-2");
    } catch (const wallpaperx::InvalidRequest& e) {
        // The code under test refusing input it is required to accept is a defect, not a host
        // condition: reported through check() so the run exits 1 rather than 77. Without this
        // arm a daemon that started rejecting "DP-1" would make its own feature untestable and
        // ctest would print Skipped.
        // CONTROL: a fixture name the daemon must accept made one it refuses ("HDMI\xff-A-2")
        //       -> "FAIL: the daemon accepts the connector names and wallpapers this phase's
        //       fixtures use", the refusal printed, the ledger row beside it, exit 1. With this
        //       arm removed the same mutation printed the ledger row alone and exited 77 --
        //       a daemon defect reported to ctest as an unexercisable host.
        check(false, "the daemon accepts the connector names and wallpapers this phase's fixtures use");
        std::fprintf(stderr, "  a fixture the daemon must accept was refused: %s\n", e.what());
        notVerified(kSetupDependentGoals,
                    std::string("the connector names this phase asks about were refused: ") + e.what());
        return;
    } catch (const std::exception& e) {
        // X.3: the phase could not build its precondition, so nothing below is graded
        // against the HTTP server. Reported as a skip rather than as a page of FAIL lines.
        notVerified(kSetupDependentGoals,
                    std::string("the connector names this phase asks about could not be recorded: ") + e.what());
        return;
    }

    // GOAL: S10.1 -- a name the daemon has been told about answers with THAT monitor's
    //       wallpaper. False if it answers with another monitor's, or refuses a name it
    //       was given.
    // WHY-THIS-SHAPE: two named monitors with two different wallpapers, and each answer is
    //       asserted to carry its own path AND not the other's. One named monitor cannot
    //       distinguish a lookup from a constant -- every wrong answer would also be the
    //       right one -- and monitor 0's path is excluded as well, since 0 is what a
    //       handler that ignored the parameter would fall back to. Both path fields are
    //       pinned, not just `path_literal`: the shipped plugin renders `path` and reads
    //       "no wallpaper chosen for this screen" off it being empty
    //       (backend/kde/kpackage/contents/ui/main.qml), so an output= branch that filled
    //       only the actionable field would blank every named screen with this phase green.
    //       The two negative clauses compare the DECODED path_literal rather than searching
    //       the response text: a substring search is satisfied both by a reply that carries
    //       no answer at all (a 400, a 500, an empty body) and by one whose path is present
    //       but spelled with JSON escapes, so it would report green for a daemon handing
    //       every named screen monitor 0's picture.
    // CONTROL: three, reverted. (a) `if (known) monitorId = bound->second;` in the /config
    //       handler in backend/daemon/http_server.cpp made `monitorId = 0` -> 7 FAILs, led by
    //       "a named monitor's own wallpaper is served for its name (DP-1)" and the same for
    //       DP-2, both printing monitor 0's blue path with monitor_id 0, and both
    //       never-served-monitor-0's lines red with them. Every unknown-name check stayed
    //       green, which is what keeps this about resolution and not about the field.
    //       (b) `if (kind == IdentityKind::Output) out["path"] = "";` inserted just before
    //       the final sendJson in backend/daemon/http_server.cpp -> exactly the two "a named
    //       monitor's own wallpaper is served for its name" lines red, with every
    //       path_literal-based and ?monitor= check green -- which is what shows the display
    //       field is pinned on the output= route specifically.
    //       (c) the output= branch made to answer 400 -> the two "never served another named
    //       monitor's wallpaper" lines and the two "never served monitor 0's wallpaper" lines
    //       red. Written as a search of res.body those four stayed GREEN under the same
    //       mutation, since an error body contains no path either: that is the measurement
    //       the decoded comparison rests on.
    // COVERAGE-ASSERT: the fixtures are asserted distinct and escape-free above the setup,
    //       which is what keeps the not-the-other-monitor's clauses falsifiable.
    const struct {
        const char* name;
        int monitorId;
        const std::string& path;
    } kNamed[] = {
        {"DP-1", kMonitorNamedSet, namedPath},
        {"DP-2", kMonitorNamedOther, otherPath},
    };
    for (const auto& bound : kNamed) {
        const HttpReply res = get(cli, configByOutput(bound.name));
        const std::optional<std::string> literal = stringField(res, "path_literal");
        const bool served = res.ok() && boolField(res, "known") == true &&
                            intField(res, "monitor_id") == bound.monitorId && literal == bound.path &&
                            stringField(res, "path") == bound.path &&
                            stringField(res, "type") == std::string("static");
        bool clean = served;
        check(served, (std::string("a named monitor's own wallpaper is served for its name (") + bound.name +
                       ")").c_str());
        for (const auto& foreign : kNamed) {
            if (foreign.monitorId == bound.monitorId) continue;
            const bool notForeign = literal.has_value() && *literal != foreign.path;
            clean = clean && notForeign;
            check(notForeign, (std::string("a named monitor is never served another named monitor's wallpaper (") +
                               bound.name + " must not carry " + foreign.name + "'s)").c_str());
        }
        const bool notMonitorZero = literal.has_value() && *literal != monitor0Path;
        clean = clean && notMonitorZero;
        check(notMonitorZero,
              (std::string("a named monitor is never served monitor 0's wallpaper (") + bound.name + ")").c_str());
        // Gated on all three results, not on `served` alone: the two negative checks fail on
        // a well-formed 200 that simply carries the wrong path, and that is the failure the
        // body is needed to explain.
        if (!clean) {
            std::fprintf(stderr, "  %s answered %d: %s\n", bound.name, res.status, res.body.c_str());
        }
    }

    // GOAL: the request shape the shipped plugin actually polls with -- output= alongside the
    //       cache-buster main.qml appends to every poll -- resolves exactly as the bare form
    //       does. False if a second query parameter changes or refuses the answer, which
    //       would leave every Plasma screen showing a refusal while every check above stayed
    //       green, since nothing else in the tree ever sends that shape.
    // WHY-THIS-SHAPE: asserted against the bare reply's own id and path rather than against
    //       the fixture constants, so it cannot pass while the bare form has silently stopped
    //       resolving; and both sides are required to have a value, because two absent fields
    //       compare equal and would make the comparison vacuous on a pair of error replies.
    //       The cache-buster is not folded into configByOutput: that would move every check
    //       in the phase onto the polled shape and lose the bare form entirely.
    // CONTROL: `if (hasMonitor && hasOutput)` in parseIdentity (backend/daemon/http_server.cpp)
    //       widened to `if (hasOutput && req.params.size() > 1)` -> exactly this check red,
    //       "the plugin's own request shape (output= with a cache-buster) resolves like the
    //       bare form", with the three monitor=-and-output=-are-400 checks still green.
    //       Reverted.
    const HttpReply bare = get(cli, configByOutput("DP-1"));
    const HttpReply asPolled = get(cli, configByOutput("DP-1") + "&t=1234567890");
    const bool pollShapeAgrees = asPolled.ok() && bare.ok() && intField(bare, "monitor_id").has_value() &&
                                 intField(asPolled, "monitor_id") == intField(bare, "monitor_id") &&
                                 stringField(bare, "path_literal").has_value() &&
                                 stringField(asPolled, "path_literal") == stringField(bare, "path_literal");
    check(pollShapeAgrees, "the plugin's own request shape (output= with a cache-buster) resolves like the bare form");
    if (!pollShapeAgrees) {
        std::fprintf(stderr, "  bare answered %d: %s\n  with &t= answered %d: %s\n", bare.status, bare.body.c_str(),
                     asPolled.status, asPolled.body.c_str());
    }

    // GOAL: S10.2 -- a name the daemon has never been told about answers 200 with
    //       known=false, an empty type and path, and no monitor id at all. False if a
    //       freshly started daemon serves monitor 0's entry to an unrecognised screen.
    // WHY-THIS-SHAPE: monitor_id is required to be JSON null rather than merely "not this
    //       monitor". 0 would be a well-formed id the plugin could hand straight back to
    //       ?monitor=, which puts monitor 0's picture on a screen that is not monitor 0 --
    //       the substitution this endpoint exists to refuse. The status is asserted 200
    //       separately from the body: this state is ordinary, and a 404 would make every
    //       plugin instance on a screen the app has not yet named look like an error.
    // CONTROL: two, reverted. (a) `out["monitor_id"] = nullptr;` and the early return deleted
    //       from backend/daemon/http_server.cpp, so an unknown name falls through to the
    //       monitor lookup with monitorId still 0 -> 3 FAILs: "an unknown connector name is
    //       answered with no monitor id rather than with 0", "... is answered with an empty
    //       type and path", and "... is not answered with monitor 0's wallpaper".
    //       (b) the output= branch made to answer 400 -> "an unknown connector name is not
    //       answered with monitor 0's wallpaper" red; written as `unknown.reached &&` it
    //       stayed GREEN under the same mutation.
    const HttpReply unknown = get(cli, configByOutput(unknownName));
    const bool unknownAnswered = unknown.reached && unknown.status == 200;
    const bool unknownNotKnown = unknown.hasJsonObject() && unknown.json.contains("known") &&
                                 unknown.json["known"].is_boolean() && !unknown.json["known"].get<bool>();
    const bool unknownHasNoId =
        unknown.hasJsonObject() && unknown.json.contains("monitor_id") && unknown.json["monitor_id"].is_null();
    const bool unknownIsEmpty = stringField(unknown, "type") == std::string() &&
                                stringField(unknown, "path") == std::string();
    // A parsed 200, not merely a reply that arrived: `reached` alone is satisfied by any error
    // body, which naturally holds no path, so a daemon refusing every output= query would
    // grade green here. Measured -- see this block's CONTROL (b).
    const bool unknownCarriesNoZero = unknown.ok() && unknown.body.find(monitor0Path) == std::string::npos;
    check(unknownAnswered, "a connector name the daemon has never been told about is answered with 200, not an error");
    check(unknownNotKnown, "a connector name the daemon has never been told about is reported as not known");
    check(unknownHasNoId, "an unknown connector name is answered with no monitor id rather than with 0");
    check(unknownIsEmpty, "an unknown connector name is answered with an empty type and path");
    check(unknownCarriesNoZero, "an unknown connector name is not answered with monitor 0's wallpaper");
    // All five, not the status alone: the other four fail on a well-formed 200, and that is
    // the reply the reader needs to see.
    if (!(unknownAnswered && unknownNotKnown && unknownHasNoId && unknownIsEmpty && unknownCarriesNoZero)) {
        std::fprintf(stderr, "  an unknown name answered %d: %s\n", unknown.status, unknown.body.c_str());
    }

    // GOAL: S10.3 -- "I have never heard of this screen" and "I know this screen and
    //       nothing is set on it" are distinguishable from the response alone. False if a
    //       plugin cannot tell them apart: one is fixed by running the app, the other by
    //       picking a wallpaper, and they need different words on the desktop.
    // WHY-THIS-SHAPE: both replies are first asserted to have an EMPTY type and path, and
    //       only then are they required to differ. Without that half the check would be
    //       satisfied by two responses that differ in some unrelated field -- and it is
    //       exactly the empty type and path that makes the two states look alike to a
    //       plugin, so a check that did not pin them establishes nothing about the
    //       confusion it exists to prevent.
    // CONTROL: `out["known"] = known;` deleted from backend/daemon/http_server.cpp -> 8 FAILs,
    //       among them "a known screen with nothing set reports known=true" and
    //       "known-with-nothing-set and never-heard-of differ in the response body". Under the
    //       same mutation, `distinguishable` rewritten as the naive
    //       `knownUnset.json.value("known", false) != unknown.json.value("known", true)`
    //       stayed GREEN -- 7 FAILs, that line not among them -- which is the measurement the
    //       both-must-carry-a-boolean shape rests on. Both reverted.
    const HttpReply knownUnset = get(cli, configByOutput("HDMI-A-2"));
    check(knownUnset.ok() && stringField(knownUnset, "type") == std::string() &&
              stringField(knownUnset, "path") == std::string(),
          "a named monitor with no wallpaper answers with an empty type and path, like an unknown name");
    check(boolField(knownUnset, "known") == true, "a known screen with nothing set reports known=true");
    // Read with an explicit type guard, not value("monitor_id", -1): the handler answers this
    // field as JSON null for a name it does not resolve, which is exactly the regression this
    // check exists to catch, and value() throws on a present null rather than defaulting.
    check(intField(knownUnset, "monitor_id") == kMonitorNamedUnset,
          "a known screen with nothing set still reports which monitor it is");
    // Both replies must CARRY a boolean `known`, not merely disagree about one. Written as
    // `value("known", false) != value("known", true)` the check passed with `out["known"] =
    // known;` deleted from backend/daemon/http_server.cpp altogether -- the two defaults
    // differ, so absence satisfied it. Measured under this block's CONTROL above.
    const std::optional<bool> knownUnsetFlag = boolField(knownUnset, "known");
    const std::optional<bool> unknownFlag = boolField(unknown, "known");
    const bool distinguishable =
        knownUnsetFlag.has_value() && unknownFlag.has_value() && *knownUnsetFlag != *unknownFlag;
    check(distinguishable, "known-with-nothing-set and never-heard-of differ in the response body");
    if (!distinguishable) {
        std::fprintf(stderr, "  known-but-unset: %s\n  never heard of:  %s\n", knownUnset.body.c_str(),
                     unknown.body.c_str());
    }

    // GOAL: S10.6 -- monitor= and output= together are rejected with 400. False if one
    //       silently wins: two identities in one request cannot be reconciled, and choosing
    //       either is a guess about which screen the caller meant.
    // WHY-THIS-SHAPE: both parameter orders, and a case whose monitor id is VALID. Testing
    //       only "monitor=99&output=DP-1" would be satisfied by a handler that rejected it
    //       for the id alone and went on silently preferring one identity whenever both are
    //       well-formed. The body is checked to carry neither wallpaper, so a 400 that also
    //       answered cannot pass.
    // CONTROL: the `hasMonitor && hasOutput` branch deleted from parseIdentity() in
    //       backend/daemon/http_server.cpp, leaving output= to win -> three "monitor= and
    //       output= in one request are rejected with 400" FAILs (200 each, the id-99 case
    //       among them) and three "a request naming a screen two ways is answered with no
    //       wallpaper at all", every body carrying the DP-1 monitor's path. Reverted.
    //       The searched paths are asserted JSON-safe above the setup, so this one substring
    //       search -- the phase's last, kept because a rejection has no parsed field to read
    //       -- cannot go vacuous under a build directory holding a quote or a backslash.
    //       The body clause is gated on the 400 rather than on the reply arriving: pointing the
    //       loop at "/config?output=HDMI-A-2" -- a 200 with no wallpaper in it -- reddens both
    //       lines, where on the transport gate the no-wallpaper line stayed GREEN.
    for (const char* query : {"/config?monitor=0&output=DP-1", "/config?output=DP-1&monitor=0",
                              "/config?monitor=99&output=DP-1"}) {
        const HttpReply both = get(cli, query);
        const bool rejected = both.reached && both.status == 400;
        // Gated on the rejection, not on the reply merely arriving: any non-400 answer holds
        // no wallpaper either when it is a 500 or an empty body, so on the transport gate this
        // line graded green for replies that were not the refusal it claims to have seen.
        const bool carriesNothing = rejected && both.body.find(namedPath) == std::string::npos &&
                                    both.body.find(monitor0Path) == std::string::npos;
        check(rejected,
              (std::string("monitor= and output= in one request are rejected with 400 (") + query + ")").c_str());
        check(carriesNothing, (std::string("a request naming a screen two ways is answered with no wallpaper at "
                                           "all (") +
                               query + ")").c_str());
        // Both, not the status alone: a 400 whose body still leaked a wallpaper would
        // otherwise print an unattributable line and no body.
        if (!(rejected && carriesNothing)) {
            std::fprintf(stderr, "  %s answered %d: %s\n", query, both.status, both.body.c_str());
        }
    }

    // GOAL: S10.6's other shape -- ONE parameter given twice with two different values is
    //       rejected too. False if the daemon answers for one of them: a query string is a
    //       multimap, so "output=DP-1&output=DP-2" names two screens just as surely as
    //       monitor= beside output= does, and picking either is the same guess the
    //       both-parameters rule exists to refuse.
    // WHY-THIS-SHAPE: the values differ AND both name a real, differently-set screen, and the
    //       body is asserted to carry neither wallpaper. Repeating one value ("output=DP-1"
    //       twice) would go green on a daemon that read only the first, since first-wins and
    //       reject-both give the same answer for it. Reading only the status would let a 400
    //       that still leaked a screen's path through.
    // CONTROL: the `req.params.count(...) > 1` branch removed from parseIdentity() in
    //       backend/daemon/http_server.cpp -> "FAIL: one identity parameter given twice is
    //       rejected with 400 (/config?output=DP-1&output=DP-2)" answering 200 with DP-1's
    //       path, the same for "/config?monitor=0&monitor=6" answering 200 with monitor 0's
    //       path, and both "answered with no wallpaper at all" lines red beside them -- four in
    //       all, with the three both-parameters checks above staying green, so this grades the
    //       repeat and not the rule it sits beside. Observed 2026-08-15. The searched paths are asserted
    //       distinct and JSON-safe above the setup, which is what keeps the two body clauses
    //       falsifiable. Second control, on the body clause's gate: the loop pointed at
    //       "/config?output=HDMI-A-2" -- a 200 with no wallpaper in it -- reddens both lines,
    //       where gated on the reply merely arriving the no-wallpaper line stayed GREEN.
    for (const char* query : {"/config?output=DP-1&output=DP-2", "/config?monitor=0&monitor=6"}) {
        const HttpReply twice = get(cli, query);
        const bool rejected = twice.reached && twice.status == 400;
        // Gated on the rejection for the reason the loop above states: a reply that is not the
        // 400 carries no wallpaper either, and would otherwise pass this line.
        const bool carriesNothing = rejected && twice.body.find(namedPath) == std::string::npos &&
                                    twice.body.find(otherPath) == std::string::npos &&
                                    twice.body.find(monitor0Path) == std::string::npos;
        check(rejected,
              (std::string("one identity parameter given twice is rejected with 400 (") + query + ")").c_str());
        check(carriesNothing, (std::string("a request naming two screens through one parameter is answered with no "
                                           "wallpaper at all (") +
                               query + ")").c_str());
        if (!(rejected && carriesNothing)) {
            std::fprintf(stderr, "  %s answered %d: %s\n", query, twice.status, twice.body.c_str());
        }
    }

    // GOAL: the three fields output= introduces -- known, output, output_literal -- exist
    //       ONLY on output= replies. False if the bare /config or ?monitor= shape grows any
    //       of them, which is a response shape the app and wpxctl already consume.
    // WHY-THIS-SHAPE: absence is asserted with contains(), not with value("known", false).
    //       The latter cannot fail -- it returns the default for an absent key and for a
    //       present false one alike -- so it would go green on exactly the change it is
    //       here to catch.
    // CONTROL: `out["known"] = known;` and the `out["output"] = ...` line moved out of the
    //       `kind == IdentityKind::Output` block in backend/daemon/http_server.cpp, so every
    //       reply carries them -> three "a /config reply that was not asked by connector name
    //       carries no connector-name field" FAILs, one per query, each printing a body with
    //       `"known":true,"output":""` in it, while every output= check above stayed green.
    //       Reverted. Separately, `out["output_literal"] = outputName;` alone moved out of the
    //       same block -> the same three FAILs, each body carrying `"output_literal":""`, and
    //       the presence line below still green. Reverted. And separately: the handler made to
    //       answer 400 for a request naming no screen -> the "/config" and "/config?t=1" lines
    //       red; written as `hasJsonObject()` rather than `ok()` both stayed GREEN, since a
    //       400 error body carries none of the three either. Reverted.
    for (const char* query : {"/config", "/config?t=1", "/config?monitor=0"}) {
        const HttpReply plain = get(cli, query);
        // status 200 as well as a parsed object: hasJsonObject() alone is satisfied by a 400,
        // whose error body naturally carries none of the three, so this loop would report green
        // for a daemon that had started refusing the plugin's bare cache-busted poll.
        const bool unchanged = plain.ok() && !plain.json.contains("known") && !plain.json.contains("output") &&
                               !plain.json.contains("output_literal");
        check(unchanged, "a /config reply that was not asked by connector name carries no connector-name field");
        if (!unchanged) std::fprintf(stderr, "  %s answered: %s\n", query, plain.body.c_str());
    }
    const HttpReply byOutput = get(cli, configByOutput("DP-1"));
    check(byOutput.hasJsonObject() && byOutput.json.contains("known") && byOutput.json.contains("output") &&
              byOutput.json.contains("output_literal"),
          "a /config reply asked by connector name carries the known, output and output_literal fields");

    // GOAL: S10.8's /config clause -- a connector name is escaped wherever it is displayed.
    //       False if a name holding a newline, a right-to-left override or DEL reaches the
    //       field the plugin renders, or any other part of the reply, raw.
    // WHY-THIS-SHAPE: the raw bytes are asserted absent from the whole body APART FROM the
    //       one field whose contract is to be exact (output_literal, matched key and value
    //       together so an `output` echoed raw cannot be mistaken for it), and the echo is
    //       compared against sanitizeForLog's output rather than merely against the raw name.
    //       "echo != raw" alone would be satisfied by an endpoint that truncated the name or
    //       dropped it, and a check that only looked at the echoed field would miss the same
    //       bytes appearing elsewhere in the reply. Searching the entire body with no
    //       exception is not available once a literal field ships: it would forbid the exact
    //       round trip checked below rather than the leak. That the name still RESOLVES is
    //       asserted too: escaping the display copy must not change the key the lookup uses.
    // CONTROL: `out["output"] = sanitizeForLog(...)` replaced with `out["output"] =
    //       outputName;` in backend/daemon/http_server.cpp -> 3 FAILs: "a hostile connector
    //       name is echoed by /config in its escaped form", "... carries no raw
    //       bidirectional override ..." and "... no raw DEL byte ...", followed by the
    //       escaped body -- the reply was a well-formed 200, so `ok()` gates none of them out,
    //       and the exactness line below stayed green, which is what keeps the two halves
    //       apart. Reverted. And the two absence lines are gated on ok(), not on `reached`:
    //       asking the block with a name the daemon refuses (an empty output=, a 400) reddens
    //       all five under ok() -- measured, body `{"error":"output name must not be empty"}` --
    //       where under `reached` the two absence lines stayed GREEN, an error body holding
    //       neither byte sequence either.
    // COVERAGE-ASSERT: the name is asserted to be one sanitizeForLog actually alters, so a
    //       future edit that tamed it fails here instead of leaving the four checks below
    //       vacuous -- for a tame name the escaped echo, the exact echo and the request are
    //       all the same bytes, and none of the four can tell a raw echo from an escaped one.
    //       CONTROL: hostileName replaced with "DP-tame-1" -> "FAIL: the hostile connector
    //       name is one sanitizeForLog alters, so the escaping checks can discriminate", and
    //       nothing else red. Restored.
    const std::string escapedName =
        wallpaperx::sanitizeForLog(hostileName, wallpaperx::kMaxOutputNameBytes, wallpaperx::LogFraming::Bare);
    check(escapedName != hostileName,
          "the hostile connector name is one sanitizeForLog alters, so the escaping checks can discriminate");

    // Bound here rather than in the phase's setup: this name is the one most likely to be
    // refused by a future tightening of the name rules, and it is used by nothing above, so a
    // refusal must cost S10.8 alone instead of every goal whose fixtures are already recorded.
    // CONTROL: this binding made a name the daemon refuses ("\xff") -> one skip row, naming
    //       S10.8 alone, with every other check in the phase still graded and green.
    bool hostileBound = true;
    try {
        nameScreen(manager, kMonitorNamedHostile, hostileName);
    } catch (const std::exception& e) {
        notVerified("S10.8's /config clause (a connector name is escaped wherever it is displayed)",
                    std::string("the hostile connector name could not be recorded: ") + e.what());
        hostileBound = false;
    }
    if (hostileBound) {
        const HttpReply hostile = get(cli, configByOutput(hostileName));
        const bool hostileResolves = hostile.ok() && boolField(hostile, "known") == true &&
                                     intField(hostile, "monitor_id") == kMonitorNamedHostile;
        const bool hostileEchoEscaped = stringField(hostile, "output") == escapedName;

        // GOAL: the name a client asked by comes back byte-for-byte in output_literal, so the
        //       caller can tell "this is the screen you asked about" from "this is some other
        //       screen". False if the only echo is the escaped one, which differs from the
        //       request for every name escaping alters -- and the daemon stores, looks up and
        //       persists the raw bytes, so the escaped form names nothing it holds.
        // WHY-THIS-SHAPE: compared against the request bytes, not against `output`. The two
        //       forms are equal for every ordinary connector name (DP-3, eDP-1), so a tame
        //       name cannot tell a raw echo from an escaped one; this uses the name asserted
        //       above to be one sanitizeForLog rewrites, which is what makes the comparison
        //       able to fail. stringField() rather than value(): a null or absent field must
        //       report as this FAIL, not as an exception that abandons the phase.
        // CONTROL: run against the shipped handler before output_literal existed -> "FAIL: the
        //       connector name a client asked by comes back unaltered in /config's
        //       output_literal", printing the escaped body. With the field added but filled
        //       from sanitizeForLog(outputName, kMaxOutputNameBytes, LogFraming::Bare) -- the
        //       defect in field form -- the same line was the only red.
        const bool hostileLiteralExact = stringField(hostile, "output_literal") == hostileName;

        // Searched for as the two byte sequences JSON serialisation passes through untouched,
        // not for the whole name. nlohmann escapes control bytes below 0x20 by itself, so the
        // newline is absent from the body of a daemon that does no escaping at all -- measured:
        // with the sanitize call removed, a whole-name search stayed GREEN while these two
        // sequences were sitting in the reply. U+202E and DEL are what actually reaches a
        // terminal or a QML label when the escaping goes.
        //
        // output_literal is removed from the copy searched, key and value together: it is the
        // one field contracted to hold the exact bytes. Erasing only that exact key/value pair
        // -- rather than the encoded name wherever it appears -- is what keeps a raw `output`
        // from being explained away as the literal field.
        // Gated on ok(), not on `reached`: an error body contains neither sequence, so on the
        // transport gate these two lines graded green for a daemon that never echoed the name.
        const std::string literalField = "\"output_literal\":" + nlohmann::json(hostileName).dump();
        std::string displayed = hostile.body;
        if (const auto at = displayed.find(literalField); at != std::string::npos) {
            displayed.erase(at, literalField.size());
        }
        const bool hostileNoOverride = hostile.ok() && displayed.find("\xE2\x80\xAE") == std::string::npos;
        const bool hostileNoDel = hostile.ok() && displayed.find('\x7F') == std::string::npos;
        check(hostileResolves, "a connector name holding a newline, U+202E and DEL still resolves to its own monitor");
        check(hostileEchoEscaped, "a hostile connector name is echoed by /config in its escaped form");
        check(hostileLiteralExact,
              "the connector name a client asked by comes back unaltered in /config's output_literal");
        check(hostileNoOverride, "/config's reply carries no raw bidirectional override outside output_literal");
        check(hostileNoDel, "/config's reply carries no raw DEL byte outside output_literal");
        // Gated on all five, not on ok(): the escaping checks fail on a perfectly well-formed
        // 200 that simply echoed the raw bytes, so gating on the transport left the one failure
        // this block exists to catch with no body printed at all. Shown escaped -- the bytes it
        // just found are exactly the ones that must not reach a terminal.
        if (!(hostileResolves && hostileEchoEscaped && hostileLiteralExact && hostileNoOverride && hostileNoDel)) {
            std::fprintf(stderr, "  a hostile name answered %d (body shown escaped): %s\n", hostile.status,
                         wallpaperx::sanitizeForLog(hostile.body, hostile.body.size(), wallpaperx::LogFraming::Bare)
                             .c_str());
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
            if (!res.ok()) continue;
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
void runAgreementChecks(httplib::Client& cli, sdbus::IProxy& proxy, WallpaperManager& manager,
                        FrameReadyLog& signals, const std::string& hostilePath, const Check& check) {
    // The fixture has to be hostile, or both checks below are trivially true of a daemon
    // that escapes nothing. Asserted rather than assumed: the filename is built from
    // constants, but a filesystem that refused one of these bytes would leave the phase
    // silently testing an ordinary path.
    check(hostilePath.find('\n') != std::string::npos, "the fixture path really contains a newline");
    check(hostilePath.find(kDel) != std::string::npos, "the fixture path really contains DEL");
    check(hostilePath.find(kBidiOverride) != std::string::npos, "the fixture path really contains U+202E");

    const HttpReply config = get(cli, "/config?monitor=" + std::to_string(kMonitorAgree));
    const StatusReply status = getStatusOrExplain(proxy);
    const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorAgree));

    if (!config.hasJsonObject() || entry == nullptr) {
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
    check(unset.hasJsonObject() && unset.json.value("type", std::string("x")).empty(),
          "/config reports no type for a monitor GetStatus has no entry for");
    check(wallpaperStatus(status, std::to_string(kMonitorNeverSet)) == nullptr,
          "GetStatus has no entry for the monitor /config reports as empty");

    // GOAL: S8.19 -- `path_literal` on both channels is the path a client may ACT on -- the bytes as
    //       stored, so handing it straight back to SetWallpaper names the same file --
    //       while `path` stays the display-escaped form and is NOT usable that way. False
    //       if the escaped form is accepted (the two forms then name the same file, so the
    //       literal buys nothing), false if the literal is rejected (a client cannot act on
    //       what it is told), and false if the two channels publish different bytes, which
    //       is the defect aded2e5 fixed for `path` reappearing on a new key.
    // WHY-THIS-SHAPE: the round trip is driven over the WIRE, not by comparing strings. The
    //       cheaper version -- assert path_literal == hostilePath -- is a check on this
    //       test's own fixture: it passes for a daemon whose literal is unusable for some
    //       reason the comparison does not model (a path the daemon normalised, a relative
    //       form), and it says nothing about the escaped form being unusable, which is the
    //       half that makes publishing two keys worth the bytes. The rejection is asserted
    //       first, so the monitor this drives ends the phase showing the frame the
    //       acceptance put there rather than holding a failed request.
    // CONTROL: two, one per channel, both reverted. (a) `path_literal` in http_server.cpp's
    //       /config handler replaced with `sanitizeForLog(it->second.path, 4096,
    //       LogFraming::Bare)` -> five FAILs: the two channels no longer publish the same
    //       bytes, /config's literal is not the stored bytes, the display form and the
    //       literal stop being different strings, and -- the one that matters -- "FAIL: the
    //       path_literal a client reads back is accepted by SetWallpaper", the escaped name
    //       not being a file. (b) the same substitution in dbus_service.cpp instead -> two
    //       FAILs, the agreement one printing "http literal: ...wpx_hostile_<U+202E>" against
    //       "dbus literal: ...wpx_hostile_\xe2\x80\xae\x7f\nname.png".
    const bool literalsPresent = config.json.contains("path_literal") && config.json["path_literal"].is_string() &&
                                 entry->contains("path_literal") && (*entry)["path_literal"].is_string();
    check(literalsPresent, "both channels publish a path_literal for a monitor with a hostile path");
    if (literalsPresent) {
        const std::string httpLiteral = config.json.value("path_literal", std::string());
        const std::string dbusLiteral = entry->value("path_literal", std::string());
        check(httpLiteral == dbusLiteral, "/config and GetStatus publish the same path_literal bytes");
        check(httpLiteral == hostilePath, "/config's path_literal is the stored bytes, not the display-escaped form");
        if (httpLiteral != dbusLiteral || httpLiteral != hostilePath) {
            std::fprintf(stderr, "  http literal: %s\n  dbus literal: %s\n", httpLiteral.c_str(),
                         dbusLiteral.c_str());
        }
        // Without this the round trip below could be two spellings of the same file, and
        // "the escaped one is rejected" would be unfalsifiable.
        check(httpPath != httpLiteral, "the display form and the literal really are different strings");

        std::string rejected;
        try {
            setWallpaper(proxy, "static", httpPath, kMonitorLiteral);
        } catch (const std::exception& e) {
            rejected = e.what();
        }
        check(!rejected.empty(), "the display-escaped path a client reads from `path` is refused by SetWallpaper");

        std::string accepted;
        try {
            setWallpaper(proxy, "static", httpLiteral, kMonitorLiteral);
        } catch (const std::exception& e) {
            accepted = e.what();
        }
        check(accepted.empty(), "the path_literal a client reads back is accepted by SetWallpaper");
        if (!accepted.empty()) std::fprintf(stderr, "  %s\n", accepted.c_str());

        // Waited for, not fire-and-forget: a monitor left mid-render turns every later
        // /health aggregate in this run into `pending`, which is a phase above grading this
        // one's leftovers.
        if (accepted.empty() && !signals.wait(kMonitorLiteral, 1)) {
            notVerified("the round trip's render",
                        "the frame for the wallpaper set through path_literal never arrived, so this run leaves a "
                        "monitor mid-render");
        }
    }

    // GOAL: S8.19's null clause -- a path that is not valid UTF-8 is published as JSON null
    //       on both channels, and neither channel breaks. False if the raw bytes go in: dump()
    //       throws on invalid UTF-8, so a single oddly-named file would take GetStatus and
    //       /config down for EVERY monitor, not just its own. Also false if the key is
    //       simply omitted -- a client cannot then tell an unusable path from an older
    //       daemon that has no such key.
    // WHY-THIS-SHAPE: recorded through the manager rather than over the bus, because DBus
    //       strings must be valid UTF-8 and the broker refuses the message -- so
    //       SetWallpaper cannot reach this state and only the in-process tier can build it.
    //       (The daemon reaches it through a config file written by another tool, or a
    //       filename created before a locale change.) The whole reply is asserted to arrive,
    //       not just the key: the failure mode being guarded is a THROW, which leaves no
    //       key to inspect.
    // CONTROL: `literal ? nlohmann::json(*literal) : nlohmann::json(nullptr)` replaced with
    //       the raw `path` in http_server.cpp and in dbus_service.cpp -> the five checks
    //       below all red, led by "/config still answers for a monitor whose path is not
    //       valid UTF-8" (500) and "GetStatus still answers ..." (sdbus error carrying
    //       json.exception.type_error.316, invalid UTF-8 byte 0x80). The blast radius is the
    //       point: four LATER phases went red too, because one monitor's filename had made
    //       GetStatus unanswerable for every monitor. Reverted.
    std::string nonUtf8Path;
    try {
        nonUtf8Path = writeSolidImage(std::string("wpx_nonutf8_\x80name.png"), 0, 0, 255);
    } catch (const std::exception& e) {
        nonUtf8Path.clear();
    }
    if (nonUtf8Path.empty() || wallpaperx::isValidUtf8(nonUtf8Path)) {
        // A filesystem that refuses the byte, or a temp path that swallowed it. Not a
        // defect in the daemon, and grading it as one would be X.3 in reverse.
        notVerified("path_literal's null clause",
                    "this host would not create a file whose name is not valid UTF-8, so no monitor can hold such "
                    "a path");
    } else {
        manager.requestWallpaper("static", nonUtf8Path, kMonitorNonUtf8);
        const HttpReply raw = get(cli, "/config?monitor=" + std::to_string(kMonitorNonUtf8));
        const StatusReply rawStatus = getStatusOrExplain(proxy);
        const nlohmann::json* rawEntry = wallpaperStatus(rawStatus, std::to_string(kMonitorNonUtf8));

        check(raw.ok(), "/config still answers for a monitor whose path is not valid UTF-8");
        check(rawStatus.ok() && rawEntry != nullptr,
              "GetStatus still answers for a monitor whose path is not valid UTF-8");
        if (!rawStatus.ok()) std::fprintf(stderr, "  %s\n", rawStatus.error().c_str());

        check(raw.ok() && raw.json.contains("path_literal") && raw.json["path_literal"].is_null(),
              "/config reports path_literal as null, not absent, for a path JSON cannot carry");
        check(rawEntry != nullptr && rawEntry->contains("path_literal") && (*rawEntry)["path_literal"].is_null(),
              "GetStatus reports path_literal as null, not absent, for a path JSON cannot carry");
        // The other key still has to work: null on both is also what a daemon that
        // published nothing at all would produce.
        check(raw.ok() && !raw.json.value("path", std::string()).empty(),
              "the display path is still published for a monitor whose path is not valid UTF-8");
    }
}

// ---------------------------------------------------------------------------- //
// Phase 6 -- /health once a device exists.
// ---------------------------------------------------------------------------- //
void runHealthAfterDeviceChecks(httplib::Client& cli, sdbus::IProxy& proxy, WallpaperManager& manager,
                                const Check& check) {
    // Bracketing the two replies, and required to agree below. The verdict this phase grades
    // is a function of how many renders are outstanding, and reading that once beside the
    // reply would be asserting on whichever moment the render worker happened to be in.
    const WallpaperManager::Snapshot before = manager.snapshot();
    const HttpReply health = get(cli, "/health");
    const StatusReply status = getStatusOrExplain(proxy);
    const WallpaperManager::Snapshot after = manager.snapshot();
    if (!health.hasJsonObject() || !status.ok()) {
        check(false, "/health and GetStatus both answered after the first render");
        return;
    }
    const nlohmann::json& statusJson = status.json();

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
    //       /health: gpu=pending". The mutation moves only the gpu line: this manager's
    //       verdict is `pending` either way -- it has renders outstanding, and a gpu check
    //       that falls back to `pending` aggregates to the same word. The shaders half is separate:
    //       `recordHealth(&DaemonHealth::shaders, HealthStatus::Ok, ...)` deleted ->
    //       "FAIL: /health reports the shader directory as ok once a render has resolved
    //       it". All reverted.
    const bool gpuUp = statusJson.contains("gpu") && statusJson["gpu"].is_object() &&
                       statusJson["gpu"].value("initialized", false);
    check(gpuUp, "GetStatus reports an initialised GPU after the first render");
    check(healthCheckStatus(health.json, "gpu") == "ok", "/health reports the GPU as ok once a device exists");
    if (gpuUp != (healthCheckStatus(health.json, "gpu") == "ok")) {
        std::fprintf(stderr, "  GetStatus: initialized=%s  /health: gpu=%s\n", gpuUp ? "true" : "false",
                     healthCheckStatus(health.json, "gpu").c_str());
    }
    if (!gpuUp) return;

    check(healthCheckStatus(health.json, "shaders") == "ok",
          "/health reports the shader directory as ok once a render has resolved it");

    // GOAL: S8.16 on the run's shared manager -- with a real device up and a monitor
    //       genuinely showing a frame, one monitor that cannot be drawn is named in the
    //       per-monitor breakdown and does not drag the daemon's verdict down with it.
    //       False if the blank monitor makes the aggregate `fail` (a watchdog restarts a
    //       daemon over a user's choice of wallpaper) and equally false if the breakdown
    //       omits it (the operator is told everything is fine with no way to find the blank
    //       screen).
    // WHY-THIS-SHAPE: the expected set of monitors is derived from the GetStatus reply taken
    //       alongside, not written as a literal, and compared as an exact set. What this
    //       manager holds by now is the sum of every phase above -- a rendered monitor, one
    //       the torn-read writer left on a `video`, one only ever recorded -- so a
    //       hand-written expectation would have to be re-derived whenever a phase above
    //       touches an id, and the version written on that day is the one that agrees with
    //       whatever /health happened to answer. Deriving it also makes the two channels
    //       answer the same question: GetStatus's `unsupported` key and this check are both
    //       isRenderableType, so a monitor one channel calls unrenderable is the monitor the
    //       other must name. Read at the same quiesced moment as /health (the last
    //       FrameReady has arrived, nothing is queued), so a disagreement cannot be two
    //       honest views of two moments.
    // CONTROL: three, because the verdict, the breakdown and the cause fail independently.
    //       (a) `firstUnsupported` dropped from the failure note carried into the pending
    //       detail (`failed > 0 ? firstFailure : std::string()`) -> only "FAIL: the wallpapers
    //       detail names the monitor GetStatus reports as unrenderable", with the detail
    //       ending "1 failed or cannot be rendered: " and nothing after it. (b) the
    //       outstanding arm's `HealthStatus::Pending` changed to `Fail` -> "FAIL: the daemon
    //       never reports a wallpaper problem or an unfinished render as a daemon failure"
    //       and "FAIL: the verdict is exactly what the manager's own outstanding count says
    //       it should be". (c) the `perMonitor[key] = ...` line deleted from the unsupported
    //       arm -> "FAIL: the breakdown names exactly the monitors GetStatus has a record
    //       of", printing the two key lists. All reverted.
    // COVERAGE-ASSERT: the mix is required before anything is graded. If this run left the
    //       manager with no unrenderable monitor, or with none showing a frame, the claim
    //       has nothing in it and the phase reports NOT VERIFIED rather than passing on a
    //       state it did not build.
    std::size_t failedCount = 0, unsupportedCount = 0, showingCount = 0, monitorCount = 0;
    int firstUnsupportedId = -1;
    std::string firstUnsupportedType;
    std::vector<std::string> statusMonitorKeys;
    std::vector<int> inFlightIds;
    if (statusJson.contains("wallpapers") && statusJson["wallpapers"].is_object()) {
        for (const auto& [key, entry] : statusJson["wallpapers"].items()) {
            if (!entry.is_object()) continue;
            ++monitorCount;
            statusMonitorKeys.push_back(key);
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
            } else {
                inFlightIds.push_back(id);
            }
        }
    }
    std::sort(statusMonitorKeys.begin(), statusMonitorKeys.end());

    // How many of the monitors with no picture are ones the daemon still owes a render for,
    // counted off the manager's own MonitorStatus. Both bracketing snapshots have to agree,
    // or the number was moving while /health was being asked and grading it would be grading
    // an interleaving (X.2).
    const auto outstandingIn = [&inFlightIds](const WallpaperManager::Snapshot& snap) {
        std::size_t n = 0;
        for (const int id : inFlightIds) {
            const auto it = snap.monitors.find(id);
            if (it != snap.monitors.end() && it->second.renderOutstanding) ++n;
        }
        return n;
    };
    const std::size_t outstanding = outstandingIn(before);
    const bool quiesced = outstanding == outstandingIn(after);

    if (failedCount != 0 || unsupportedCount == 0 || showingCount == 0) {
        notVerified("S8.16 (a blank monitor is named per monitor and does not fail the daemon)",
                    "this run left the shared manager without the mix the check needs (" +
                        std::to_string(monitorCount) + " monitors: " + std::to_string(showingCount) +
                        " showing, " + std::to_string(unsupportedCount) + " unrenderable, " +
                        std::to_string(failedCount) + " failed)");
    } else if (!quiesced) {
        // X.2 again, and reported rather than retried: the two counts disagreeing means a
        // render landed mid-phase, and the verdict below is derived from a number that was
        // true of neither reply.
        notVerified("S8.16 (a blank monitor is named per monitor and does not fail the daemon)",
                    "the number of outstanding renders moved while /health was being asked (" +
                        std::to_string(outstanding) + " before, " + std::to_string(outstandingIn(after)) +
                        " after), so no verdict can be attributed to a state");
    } else {
        // GOAL: S8.16's second clause and S7.10b-i's "never fail", on the state this run has
        //       actually built -- one monitor this build cannot draw, one showing a picture,
        //       and whatever renders the manager still owes. Neither a wallpaper the daemon
        //       cannot render nor a render it has not finished may make the verdict `fail`:
        //       `fail` is what a watchdog restarts a daemon over, and neither of those is the
        //       daemon being broken.
        // WHY-THIS-SHAPE: the expected verdict is DERIVED from the manager's own outstanding
        //       count rather than written as a literal. `== "ok"` cannot be used here: this
        //       phase runs on the manager every phase above has been writing to, and several of
        //       those monitors were recorded straight through the manager rather than set over
        //       the bus (the monitor= phase, the connector-name phase, the torn-read writer and
        //       the non-UTF-8 path in runAgreementChecks), so no render was ever queued for
        //       them and they are legitimately outstanding -- `pending` is then the contract's answer, not a defect. The
        //       opposite simplification, `!= "fail"` alone, is satisfied by an endpoint that
        //       answers `pending` to everything, which is the pre-S7.10 endpoint. Asserting
        //       both -- never `fail`, and exactly the value the state calls for -- is what
        //       distinguishes those.
        //       Note what this phase can therefore NOT establish while a render is
        //       outstanding: that the UNRENDERABLE monitor alone leaves the verdict ok. The
        //       outstanding arm is ahead of the failed-or-unsupported arm in the daemon, so
        //       that arm is not even reached here. Scenario (a2) of runHealthStateChecks
        //       grades it, on a daemon with one monitor and nothing outstanding.
        const std::string wallpapers = healthCheckStatus(health.json, "wallpapers");
        const std::string aggregate = health.json.value("status", std::string());
        const bool notADaemonFault = wallpapers != "fail" && aggregate != "fail";
        check(notADaemonFault,
              "the daemon never reports a wallpaper problem or an unfinished render as a daemon failure");
        const std::string expected = outstanding > 0 ? "pending" : "ok";
        const bool asStated = wallpapers == expected && aggregate == expected;
        check(asStated, "the verdict is exactly what the manager's own outstanding count says it should be");
        if (!notADaemonFault || !asStated) {
            std::fprintf(stderr, "  %zu of %zu monitors outstanding -> expected %s, got wallpapers %s / aggregate "
                                 "%s (body: %s)\n",
                         outstanding, monitorCount, expected.c_str(), wallpapers.c_str(), aggregate.c_str(),
                         health.body.c_str());
        }

        const nlohmann::json* monitors = healthMonitors(health.json);
        check(monitors != nullptr, "the wallpapers check carries a per-monitor breakdown after a render");
        if (monitors != nullptr) {
            const std::vector<std::string> healthKeys = sortedKeys(*monitors);
            check(healthKeys == statusMonitorKeys,
                  "the breakdown names exactly the monitors GetStatus has a record of");
            if (healthKeys != statusMonitorKeys) {
                std::fprintf(stderr, "  /health monitors: %s\n  GetStatus monitors: %s\n", monitors->dump().c_str(),
                             statusJson["wallpapers"].dump().c_str());
            }
            check(monitorState(health.json, firstUnsupportedId) == "unsupported",
                  "the monitor GetStatus calls unrenderable is `unsupported` in the breakdown");
            const std::string perMonitor = monitorDetail(health.json, firstUnsupportedId);
            check(perMonitor.find("monitor " + std::to_string(firstUnsupportedId)) != std::string::npos &&
                      perMonitor.find("'" + firstUnsupportedType + "'") != std::string::npos,
                  "the unrenderable monitor's own detail names it and its type");
            if (monitorState(health.json, firstUnsupportedId) != "unsupported") {
                std::fprintf(stderr, "  monitor %d in the breakdown: %s (%s)\n", firstUnsupportedId,
                             monitorState(health.json, firstUnsupportedId).c_str(), perMonitor.c_str());
            }
        }

        // GOAL: whichever branch the verdict came from, the detail carries BOTH numbers the
        //       state has: how many monitors are blank for the wallpaper's own reasons, and,
        //       when a render is outstanding, how many of the monitor total that is. False if
        //       an unfinished render silences the wallpaper failure the same daemon is also
        //       reporting -- the operator is then told a render is pending and never told a
        //       monitor cannot be drawn at all, which is the one of the two that will not fix
        //       itself.
        // WHY-THIS-SHAPE: both expected substrings are derived, and which pair is expected
        //       follows the branch the state puts the daemon in. Writing the "N of M" form
        //       alone -- what this check was before the daemon grew an outstanding-render
        //       branch -- asserts a sentence that is only produced when nothing is
        //       outstanding, so it goes red on a daemon that is behaving exactly to contract.
        //       Dropping the count and matching only "failed or cannot be rendered" would go
        //       the other way and pass for any count at all, including a wrong one.
        // CONTROL: `failureNote` replaced with `std::string()` at its use in the outstanding
        //       arm, i.e. the failure dropped from the pending detail -> "FAIL: the wallpapers
        //       detail counts the blank monitors against the monitor total" and "FAIL: the
        //       wallpapers detail names the monitor GetStatus reports as unrenderable",
        //       printing "detail: 2 of 5 monitors have not finished rendering". Reverted.
        const std::string detail = healthCheckDetail(health.json, "wallpapers");
        const std::string blank = std::to_string(failedCount + unsupportedCount);
        // The daemon writes the blank count as a clause of its own once a render is
        // outstanding, and as "N of M monitors ..." when it is the whole verdict.
        const std::string expectBlank = outstanding > 0
                                            ? "; " + blank + " failed or cannot be rendered"
                                            : blank + " of " + std::to_string(monitorCount) +
                                                  " monitors failed or cannot be rendered";
        const std::string expectCount = outstanding > 0
                                            ? std::to_string(outstanding) + " of " + std::to_string(monitorCount) +
                                                  " monitors have not finished rendering"
                                            : expectBlank;
        const bool counted =
            detail.find(expectCount) != std::string::npos && detail.find(expectBlank) != std::string::npos;
        check(counted, "the wallpapers detail counts the blank monitors against the monitor total");
        const bool named = detail.find("monitor " + std::to_string(firstUnsupportedId)) != std::string::npos &&
                           detail.find("'" + firstUnsupportedType + "'") != std::string::npos;
        check(named, "the wallpapers detail names the monitor GetStatus reports as unrenderable");
        if (!named || !counted) {
            std::fprintf(stderr, "  detail: %s\n  expected '%s', '%s', 'monitor %d' and '%s'\n", detail.c_str(),
                         expectCount.c_str(), expectBlank.c_str(), firstUnsupportedId, firstUnsupportedType.c_str());
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
    const std::string dbusTier = statusJson["gpu"].value("descriptor_tier", std::string());
    const std::string dbusName = statusJson["gpu"].value("name", std::string());
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
    //       The battery covers the connector-name route as well as the numeric one, and the
    //       unknown-name entry is the load-bearing one: a handler that answered `known:false`
    //       by inventing and storing a binding writes into the outputs map GetStatus dumps,
    //       which is exactly what a snapshot comparison catches and what a monitor=-only list
    //       cannot see at all.
    // CONTROL: two, both reverted. (a) a mutating route added to registerRoutes in
    //       backend/daemon/http_server.cpp --
    //           m_server->Post("/config", [this](const auto&, auto& res) {
    //               m_manager.setMonitorOutput(0, "POST-CONTROL"); res.status = 200; });
    //       -> "FAIL: a non-GET method is not a route this server serves" ("POST /config
    //       answered 200") and "FAIL: no sequence of HTTP requests changed what GetStatus
    //       reports". A mutation that throws instead of writing (requestWallpaper on an
    //       unreadable path) reddens only the first line -- the write has to succeed for the
    //       state half to be the thing under test.
    //       (b) aimed at the connector-name entries: the IdentityKind::Output branch in
    //       backend/daemon/http_server.cpp made to record the name it was asked about
    //       (`if (!known) m_manager.setMonitorOutput(0, outputName);`) -> exactly one red, "no
    //       sequence of HTTP requests changed what GetStatus reports", the after-dump carrying
    //       an eDP-9-asked-only-here entry in `outputs` the before-dump has not. Nothing in the
    //       connector-name phase saw it: that handler answers from a snapshot taken before the
    //       write, so its unknown-name checks stayed green -- this battery is the only detector.
    const StatusReply before = getStatusOrExplain(proxy);

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
    // routes that do exist as well as the methods that do not. The connector-name shapes are
    // unconditional even though the phase that records DP-1 and HDMI-A-2 can skip: a name the
    // daemon was never given is still a valid read-only probe, and it is the one that would
    // catch a handler that binds what it is asked about. That probe name is asked nowhere else
    // in the file on purpose -- a name an earlier phase already asked about would already have
    // been bound by such a handler, leaving nothing for this comparison to see.
    for (const char* path : {"/config", "/config?t=1", "/config?monitor=0", "/config?monitor=99", "/health",
                             "/", "/config/../health", "/config?monitor=0&monitor=1", "/config?output=DP-1",
                             "/config?output=HDMI-A-2", "/config?output=eDP-9-asked-only-here", "/config?output=DP-1&t=1",
                             "/config?output=", "/config?monitor=0&output=DP-1"}) {
        (void)get(cli, path);
    }

    const StatusReply after = getStatusOrExplain(proxy);

    // Both replies are asserted to have arrived, and the comparison is gated on that. An
    // unanswered GetStatus leaves an empty object behind, so an ungated "before == after"
    // is satisfied by "{}" equalling "{}" -- the state check would pass at exactly the
    // moment there was no state to read, which is the vacuous direction of this check.
    // CONTROL: DbusService::getStatus made to throw -> "FAIL: GetStatus answered on both
    //       sides of the read-only battery, so there is a state to compare" and the
    //       unchanged-state check red beside it. Reverted.
    const bool bothAnswered = before.ok() && after.ok();
    check(bothAnswered, "GetStatus answered on both sides of the read-only battery, so there is a state to compare");
    const std::string beforeDump = before.json().dump();
    const std::string afterDump = after.json().dump();
    check(bothAnswered && beforeDump == afterDump, "no sequence of HTTP requests changed what GetStatus reports");
    if (beforeDump != afterDump) {
        std::fprintf(stderr, "  before: %s\n  after:  %s\n", beforeDump.c_str(), afterDump.c_str());
    }
}

// ---------------------------------------------------------------------------- //
// Phase 8 -- a poll never waits on a render.
// ---------------------------------------------------------------------------- //
void runGpuLockChecks(sdbus::IProxy& proxy, WallpaperManager& manager, FrameReadyLog& signals, HttpServer& server,
                      const std::string& largePath, const Check& check) {
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
    //       The poller sends what the shipped plugin sends -- output=NAME beside the
    //       cache-buster main.qml appends -- not ?monitor=N: the connector-name branch does
    //       work the numeric one does not (an outputs-map lookup and a sanitize of the name),
    //       and it is that extra work a future change could move behind the render lock. The
    //       numeric shape gives up only parseMonitorParam, which takes no lock at all.
    // CONTROL: `(void)m_manager.validationCounter();` added at the top of the /config
    //       handler in backend/daemon/http_server.cpp -- one line, and it takes m_gpuMutex
    //       exactly as an accidental GPU touch would -> "FAIL: polls kept completing while a
    //       render was in flight" and "FAIL: polls kept completing in the second half of the
    //       render window", with "0 of 1 polls completed inside the 64.7 ms render window" --
    //       the poller's single connector-name request spent the whole render blocked on
    //       m_gpuMutex. Reverted.
    // COVERAGE-ASSERT: the completion counts are asserted, not just their existence, and
    //       the window length is printed, so a future edit that shortens the render or
    //       stops the poller cannot leave this quietly green.
    constexpr auto kMinConclusiveWindow = std::chrono::milliseconds(20);
    constexpr int kMinPollsInWindow = 5;
    constexpr int kMinPollsInSecondHalf = 3;

    uint64_t generation = 0;
    {
        // Two outcomes from one condition, and they are not the same statement. The
        // measurement is abandoned -- this generation is the baseline the render window is
        // measured from, so reading 0 off an empty reply would make signals.wait() wait for
        // a generation already past and grade a window nobody observed. But the unanswered
        // call is *also* graded, because the DbusService answering it lives in this process
        // (see the file header): an unanswered GetStatus here is a defect, not a host
        // condition, and notVerified alone would end the binary at exit 77 with ctest
        // reporting the whole file as Skipped.
        // CONTROL: DbusService::getStatus made to throw -> "FAIL: the in-process
        //       DbusService answers GetStatus" beside this phase's NOT VERIFIED row, and
        //       ctest Failed rather than Skipped. Reverted.
        const StatusReply status = getStatus(proxy);
        if (!status.ok()) {
            check(false, "the in-process DbusService answers GetStatus");
            notVerified("S7.12 (a poll never waits on a render)", status.error());
            return;
        }
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorGpuLock));
        generation = entry != nullptr ? entry->value("generation", uint64_t{0}) : 0;
    }

    // A name of its own, distinct from the connector-name phase's, so binding it cannot
    // disturb that phase's "a name resolves to its own monitor" checks.
    try {
        nameScreen(manager, kMonitorGpuLock, "DP-LOCK");
    } catch (const std::exception& e) {
        notVerified("S7.12 (a poll never waits on a render)",
                    std::string("the connector name the poller asks by could not be recorded: ") + e.what());
        return;
    }
    Poller poller(server.port(), std::string("/config?output=DP-LOCK&t=1"));

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
// Why this phase can destroy a server and expect the join to return: httplib's
// Server::stop() is a no-op while is_running_ is false, and listen_internal() sets that
// flag only once the serving thread reaches it, so a server destroyed before its thread
// is scheduled would leave stop() doing nothing and the join waiting forever. The
// constructor closes that window itself -- it calls wait_until_ready() before returning,
// so no HttpServer is ever handed out in that state. Nothing here relies on having
// served a request first.
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

    // GOAL: S7.8 on the reporting side -- a WallpaperManager that outlives its HttpServer
    //       stops calling the channel healthy. False if the manager goes on publishing the
    //       last thing the server said, which is `ok`, for a port the check above has just
    //       established is closed.
    // WHY-THIS-SHAPE: read off the manager rather than over GetStatus because this phase
    //       holds no proxy, and the manager's record is the value GetStatus serialises. Two
    //       checks, not one: "not ok" alone would still pass if teardown stamped a record
    //       naming some other server, and naming the port alone would pass on a record that
    //       still said ok. The port is the one this phase's server actually bound, not a
    //       constant, so the pair cannot both be satisfied by a fixed string.
    // CONTROL: two, reverted. (a) ~Registration's recordConfigServerShutdown() call deleted
    //       -> "FAIL: a manager that outlives its config server stops reporting it healthy"
    //       and "FAIL: the shutdown record names the port that stopped serving". (b) only
    //       std::to_string(m_port) dropped from that call's text -> the names-the-port check
    //       red on its own.
    const wallpaperx::HealthCheck outlived = manager.snapshot().health.configServer;
    check(outlived.status != wallpaperx::HealthStatus::Ok,
          "a manager that outlives its config server stops reporting it healthy");
    check(outlived.detail.find(std::to_string(port)) != std::string::npos,
          "the shutdown record names the port that stopped serving");
    if (outlived.status == wallpaperx::HealthStatus::Ok ||
        outlived.detail.find(std::to_string(port)) == std::string::npos) {
        std::fprintf(stderr, "  config_server after the server on 127.0.0.1:%d was destroyed: %s / %s\n", port,
                     wallpaperx::toString(outlived.status), outlived.detail.c_str());
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

// A connected socket that has already had one request served on it, so the server
// demonstrably accepted it and gave it a worker. Returns -1 with `why` set on any failure;
// the caller must close it.
int servedConnection(int port, std::string& why) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        why = "could not create a socket";
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        why = "could not connect to the server this phase started";
        ::close(fd);
        return -1;
    }
    const std::string request = "GET /health HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                "\r\nConnection: keep-alive\r\n\r\n";
    std::string response;
    if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size()) ||
        !readOneResponse(fd, 5000, response) || response.find(" 200 ") == std::string::npos) {
        why = "the connection never had a request served on it, so nothing proves the server gave it a worker";
        ::close(fd);
        return -1;
    }
    return fd;
}

// The first line of a request and nothing more: no CRLF, so the server is mid-parse and
// blocks in the timed read rather than in the keep-alive poll.
bool sendPartialRequest(int fd) {
    static constexpr std::string_view kPartial = "GET /health HTTP/1.1";
    return ::send(fd, kPartial.data(), kPartial.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(kPartial.size());
}

// ---------------------------------------------------------------------------- //
// Phase 12 -- a client that goes silent mid-request cannot hold the server open.
// ---------------------------------------------------------------------------- //
void runSilentClientShutdownChecks(WallpaperManager& manager, const Check& check) {
    constexpr const char* kGoal = "S8.21 (the read timeout bounds how long a client that goes silent mid-request "
                                  "can hold a serving thread, and so how long it can delay shutdown)";
    // The healthy cost is one read timeout: 500 ms, http_server.cpp's kIoTimeoutMicroseconds.
    // What this guards is that constant going back up -- it was 5 s until afe32a3, measured
    // at 4932 ms of held shutdown. 1500 ms sits 3x above the healthy value and 3.3x below
    // the regression, so neither a loaded host nor the scheduler can move a run across it.
    constexpr double kBudgetMs = 1500.0;

    std::string reason;
    std::unique_ptr<HttpServer> server = startServer(manager, reason);
    if (!server) {
        notVerified(kGoal, reason);
        return;
    }
    const int port = server->port();

    // GOAL: S8.21 -- a client that sends part of a request and then nothing is dropped by the server
    //       within the read timeout. False if it is held indefinitely, or for the 5 s this
    //       was: the serving thread is parked for that long, and ~HttpServer joins it, so a
    //       status client -- or anything that crashed mid-request -- decides when the daemon
    //       may finish stopping.
    // WHY-THIS-SHAPE: the timeout is measured from the CLIENT side, as the time until the
    //       server drops the connection, and a deadline that expires is a FAIL rather than a
    //       number. Timing the destructor alone is the obvious version and it is not
    //       falsifiable on its own: whether the worker has entered the timed read or is
    //       still in httplib's 10 ms keep-alive poll when stop() lands is a race, and the
    //       fast side of that race passes. This measurement has no fast side -- the server
    //       either drops the connection or the phase fails. The partial request is what
    //       makes it the case at all: an idle connection that sent nothing is bounded by
    //       keep_alive()'s stop check instead, which the phase above already covers.
    //       Distinct from S7.17's decided non-goal, which is the client that keeps
    //       DRIBBLING bytes: each byte restarts the timeout, that is unbounded by design,
    //       and no such connection is left open here.
    // CONTROL: kIoTimeoutSeconds/kIoTimeoutMicroseconds in http_server.cpp put back to
    //       `5, 0`, the value before afe32a3 -> both checks red: "a client that stops
    //       mid-request is dropped by the server rather than held indefinitely" (the 4 s
    //       deadline expired first, which is the failure branch doing its job) and "...
    //       within the read timeout", printing "the server held a silent client for 4019.00
    //       ms (budget 1500 ms) and never dropped it". Unmutated: 501.90 ms. Reverted.
    std::string why;
    int fd = servedConnection(port, why);
    if (fd < 0) {
        notVerified(kGoal, why);
        return;
    }
    struct FdGuard {
        int fd;
        ~FdGuard() {
            if (fd >= 0) ::close(fd);
        }
    } guard{fd};

    if (!sendPartialRequest(fd)) {
        notVerified(kGoal, "the partial request that puts the server into a timed read could not be sent");
        return;
    }

    const Clock::time_point sentAt = Clock::now();
    bool dropped = false;
    // 4 s, comfortably past the 5 s a regression would take? No -- deliberately below it:
    // the deadline expiring is a FAIL, so it has to be reachable by the failure this exists
    // to catch, and 4 s already distinguishes 500 ms from anything second-scale.
    const Clock::time_point deadline = sentAt + std::chrono::milliseconds(4000);
    while (!dropped) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) break;
        pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, static_cast<int>(left));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) break;
        char buf[512];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        // 0 is the server closing after the timeout; anything else it says first (a 408) is
        // read and ignored until the close arrives.
        if (n <= 0) dropped = true;
    }
    const double heldMs = millis(Clock::now() - sentAt);

    check(dropped, "a client that stops mid-request is dropped by the server rather than held indefinitely");
    check(dropped && heldMs < kBudgetMs, "a client that stops mid-request is dropped within the read timeout");
    if (!dropped || heldMs >= kBudgetMs) {
        std::fprintf(stderr, "  the server held a silent client for %.2f ms (budget %.0f ms)%s\n", heldMs, kBudgetMs,
                     dropped ? "" : " and never dropped it");
    }
    std::printf("http_server_test: a client silent mid-request was dropped after %.2f ms\n", heldMs);
    std::fflush(stdout);

    // GOAL: S8.21 on the thing that actually matters -- destroying the server
    //       while a silent client is being held. False if the join waits longer than the
    //       read timeout that bounds the worker.
    // WHY-THIS-SHAPE: the pause before the destruction is what makes this measure anything:
    //       without it the worker is still in the keep-alive poll, stop() ends its loop, and
    //       the destruction returns in a millisecond having tested nothing -- which is
    //       exactly how a first attempt at this measured 1 ms and looked like a pass. The
    //       pause is not a wait for a result (X.1): the result is the elapsed time of the
    //       destructor, measured afterwards.
    // COVERAGE-ASSERT: a destruction that returned promptly is reported as a state this run
    //       failed to build, NOT as a pass. That is the only way this check can go green
    //       without the property holding, so it is the thing asserted.
    // CONTROL: the same 5 s timeout -> "FAIL: destroying the server while a silent client is
    //       held does not take longer than the read timeout", printing "  ~HttpServer took
    //       4925.22 ms with one silent client held (budget 1500 ms)", against 401.90 ms
    //       unmutated. Reverted.
    std::string secondWhy;
    const int second = servedConnection(port, secondWhy);
    if (second < 0) {
        notVerified(kGoal, "the second connection this phase needs: " + secondWhy);
        return;
    }
    FdGuard secondGuard{second};
    if (!sendPartialRequest(second)) {
        notVerified(kGoal, "the partial request for the destruction measurement could not be sent");
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const Clock::time_point before = Clock::now();
    server.reset();  // ~HttpServer: stop() then join()
    const double destroyMs = millis(Clock::now() - before);

    if (destroyMs < 50.0) {
        notVerified(kGoal, "destroying the server took " + std::to_string(destroyMs) +
                               " ms, so its worker was not holding the silent client at that moment and the "
                               "measurement grades nothing");
    } else {
        check(destroyMs < kBudgetMs,
              "destroying the server while a silent client is held does not take longer than the read timeout");
        if (destroyMs >= kBudgetMs) {
            std::fprintf(stderr, "  ~HttpServer took %.2f ms with one silent client held (budget %.0f ms)\n",
                         destroyMs, kBudgetMs);
        }
    }
    std::printf("http_server_test: shutdown with one silent client held: %.2f ms\n", destroyMs);
    std::fflush(stdout);
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "http_server_test";

    // Points $XDG_STATE_HOME at a scratch directory for the whole process. Config::stateDir
    // defaults to empty and the one phase here that wants a state directory names its own, so
    // nothing is expected to reach this one; it is the containment for the paths that resolve
    // a state directory for themselves whatever the Config says -- preserveConfig() keeps its
    // copy of an unreadable config under defaultStateDir(). Construct it before any Config.
    const wallpaperx::testing::ScratchStateHome stateHome;
    if (!stateHome.ok()) {
        notVerified("every goal in this file",
                    "no scratch state directory could be created, and running without one would let anything "
                    "here that resolves a state directory for itself write into the developer's own");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // Wrapped in a Check rather than called directly because every phase below takes one:
    // the std::function is what lets a phase be handed the grader without this file's
    // reporting internals.
    const Check check = wallpaperx::testing::check;

    // This thread drives every request, so it allocates on the client side of each one.
    // Marking it keeps the allocation probe from mistaking the client for the handler.
    t_testOwnedThread = true;

    try {
        const std::string bluePath = writeSolidImage("http_blue.png", 0, 0, 255);
        const std::string videoPath = writeSolidImage("http_video.mp4", 255, 0, 0);
        const std::string hostilePath = writeHostileNamedImage();
        const std::string junkPath = writeUndecodableFile("http_not_an_image.png");
        // Two more distinct files, so the connector-name phase can tell "this screen's
        // wallpaper" from "some screen's wallpaper".
        const std::string namedPath = writeSolidImage("http_named.png", 0, 255, 0);
        const std::string namedOtherPath = writeSolidImage("http_named_other.png", 255, 255, 0);
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
            // listening server, so none of them runs and none of them is graded. Step 10's
            // connector-name phase is graded in this branch too, so it is named here -- a goal
            // that drops out of the ledger reads afterwards as a goal that was established.
            notVerified("every Step 7 goal, plus S10.1, S10.2, S10.3, S10.6 and S10.8's /config clause "
                        "(asking /config by connector name), including the refusal of names /config could "
                        "never have been given",
                        reason);
        } else {
            std::printf("http_server_test: serving on 127.0.0.1:%d (never the shipped 18462, so a running daemon "
                        "and a parallel ctest entry cannot collide with this run)\n",
                        server->port());
            std::fflush(stdout);

            httplib::Client cli = makeClient(server->port());

            runBindChecks(*proxy, manager, *server, check);

            // Before anything renders: the whole point of the S7.10b pre-device check is
            // that nothing has been tried yet on this manager, so `pending` is the true
            // answer for all three checks.
            runHealthBeforeDeviceChecks(cli, check);

            // Before any device exists, on a daemon of its own: the fewer threads are
            // allocating in this process, the more exactly the one-shot below lands.
            runHandlerExceptionChecks(check);

            // Runs on managers of its own, so it neither needs nor disturbs the state
            // above -- and so it can be reached on a host with no Vulkan at all, where the
            // main render below never happens.
            runHealthStateChecks(bluePath, videoPath, junkPath, check);

            // Recorded through the manager rather than through SetWallpaper, so no render
            // is queued and the "before any device" state above survives into this phase.
            manager.requestWallpaper("static", bluePath, kMonitorParam);
            runMonitorParamChecks(cli, bluePath, check);

            // Same reasoning as the line above: recorded through the manager, so the phase
            // adds monitors and connector names without queueing a render.
            runOutputParamChecks(manager, cli, bluePath, namedPath, namedOtherPath, check);

            runTornReadChecks(manager, cli, bluePath, videoPath, check);

            // First render of the run. Everything after it needs a Device to exist.
            setWallpaper(*proxy, "static", hostilePath, kMonitorAgree);
            const bool rendered = signals.wait(kMonitorAgree, 1);
            if (!rendered) {
                // X.3 again: on a host with no working Vulkan this is not a fault of the
                // HTTP server, and grading the four phases below would produce a page of
                // FAIL lines pointing at the wrong component.
                const StatusReply status = getStatus(*proxy);
                const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorAgree));
                notVerified("S7.3 (channel agreement), S7.4 (escaping), S7.2 (read-only), "
                            "S7.10b (/health reads ok once the daemon has used its dependencies), "
                            "S7.12 (poll never waits on a render)",
                            "no frame was ever rendered on this host" +
                                (entry != nullptr && entry->contains("last_error")
                                     ? " -- last_error: " + entry->value("last_error", std::string())
                                     : std::string()));
            } else {
                runAgreementChecks(cli, *proxy, manager, signals, hostilePath, check);
                runHealthAfterDeviceChecks(cli, *proxy, manager, check);
                runReadOnlyChecks(cli, *proxy, check);
                runGpuLockChecks(*proxy, manager, signals, *server, largePath, check);
            }

            // Destroyed before the manager it reads, which is the ordering main.cpp gets
            // from declaring the HttpServer last.
            server.reset();
        }

        runShutdownChecks(manager, check);
        runKeepAliveShutdownChecks(manager, check);
        runSilentClientShutdownChecks(manager, check);

        // GOAL: S7.20's release half again, from the direction the phases feel it -- the
        //       registration guard may turn away no HttpServer built anywhere in this run.
        //       False if a claim outlives the server that made it, which is what the two
        //       shutdown phases above walk into: they build a server on the manager main
        //       just released.
        // WHY-THIS-SHAPE: graded here, not inside startServer's catch. A phase whose server
        //       is null can only report NOT VERIFIED, so a leaked claim would otherwise end
        //       the run at exit 77 -- a ctest *skip* -- with the defect named in a skip
        //       reason nobody grades. One flag, one check, so the phases still report
        //       individually and the run still fails.
        //       Only std::logic_error, the guard's own contract, reaches this flag. The
        //       obvious simpler version grades EVERY constructor throw, but HttpServer also
        //       throws std::system_error out of std::thread creation under an fd or thread
        //       limit -- a host fact this file makes likely by building dozens of servers and
        //       pollers -- and that would print a named daemon defect on top of the skip the
        //       phase already reported, for a defect that does not exist (X.3).
        // CONTROL: three, reverted. (a) ~Registration's detachConfigServer() call deleted ->
        //       "FAIL: no HttpServer was refused by the registration guard during this run",
        //       with the release-half check red and both shutdown phases printing NOT
        //       VERIFIED instead of the run ending on an unexpected exception. (b) a
        //       std::system_error thrown from the constructor -> no FAIL at all, every goal
        //       NOT VERIFIED and ctest Skipped, with the host-fault line printed. (c) the
        //       same throw made a std::logic_error -> this check red and ctest Failed, which
        //       is what keeps (b) a statement about the type and not about the throw.
        check(g_serverRegistrationRefused.empty(),
              "no HttpServer was refused by the registration guard during this run");
        if (!g_serverRegistrationRefused.empty()) {
            std::fprintf(stderr, "  an HttpServer constructor threw: %s\n", g_serverRegistrationRefused.c_str());
        }

        // GOAL: an HttpServer that reports no bind failure is serving. False if the daemon
        //       can take a port and then never answer on it.
        // WHY-THIS-SHAPE: graded here for the same reason as the flag above -- startServer
        //       can only hand its callers a null server, and every caller turns that into a
        //       skip. Reached only from startServer's tail, so a run where some later attempt
        //       succeeded says nothing here; what it forbids is the run ending on a server
        //       that bound and stayed silent being read as a host fact.
        // CONTROL: startServer's /health probe forced to fail, with freePort() made to fail
        //       after its first call so the tail is reached -> "FAIL: every HttpServer that
        //       bound a port went on to answer /health" and ctest Failed rather than Skipped.
        //       Reverted.
        check(g_serverBoundButNeverServed.empty(), "every HttpServer that bound a port went on to answer /health");
        if (!g_serverBoundButNeverServed.empty()) {
            std::fprintf(stderr, "  %s\n", g_serverBoundButNeverServed.c_str());
        }

        // Printed, never graded: a constructor that fails on a thread or descriptor limit is
        // this host running out, and the phase that met it already reported NOT VERIFIED.
        if (!g_serverConstructorHostFault.empty()) {
            std::fprintf(stderr,
                         "  an HttpServer constructor threw what looks like a host limit rather than a daemon "
                         "defect, so it is reported as a skip above and not graded: %s\n",
                         g_serverConstructorHostFault.c_str());
        }
    } catch (const std::exception& e) {
        // Without this an escaping sdbus::Error -- what a host with no session bus
        // produces -- would abort the process, so ctest would report an opaque SIGABRT
        // instead of the named failures above.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++wallpaperx::testing::g_failures;
    }

    // Asked before the not-verified count, so a run that is both broken and incomplete
    // reports as broken rather than hiding behind ctest's "Skipped".
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed(kSkipReasonMixed);
}
