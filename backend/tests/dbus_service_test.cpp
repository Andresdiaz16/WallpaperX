// SPDX-License-Identifier: GPL-2.0-or-later

// The real DbusService on a private session bus, driven through sdbus-c++ proxies on separate
// connections of this process, so every message round-trips through the bus.
//
// Everything that needs no GPU runs first, so the wire-signature lock survives a host whose Vulkan
// stack cannot come up. Nothing waits on a sleep: SetWallpaper returns before the render, so a
// frame is known ready only from FrameReady, and "no signal for this one" is settled by a later
// request that does signal on the single FIFO render thread.
#include "test_image_dir.h"
#include "daemon/config_store.h"
#include "daemon/crash_record.h"
#include "daemon/dbus_service.h"
#include "daemon/render_worker.h"
#include "daemon/wallpaper_manager.h"

#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

// The renderer's test scaffolding. The backend's own is tests/client/, not tests/support/: a
// quoted include searches this file's directory first, so a backend tests/support/ would shadow it.
#include "support/exported_frame_map.h"
#include "support/validation_grade.h"
#include "tests/client/call_gate.h"
#include "tests/client/daemon_client.h"
#include "tests/client/scratch_dir.h"
#include "tests/manager_output.h"
#include "tests/served_pixels.h"
#include "tests/stream_capture.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>
#include <systemd/sd-bus.h>

#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

using wallpaperx::DbusService;

// Where phases hold or fail the render thread inside the daemon's own work: framebuffer creation
// (finishing every rebuilt image) and destruction (releasing them). Defined here, so the daemon's
// calls resolve to these; unarmed, each costs one atomic load. The gate is tests/client/call_gate.h.
using wallpaperx::testing::CallGate;

CallGate g_framebufferGate;
CallGate g_destroyFramebufferGate;  // a monitor's images being released: held there, never failed

extern "C" void vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* allocator) {
    using Fn = void (*)(VkDevice, VkFramebuffer, const VkAllocationCallbacks*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "vkDestroyFramebuffer"));
    if (real == nullptr) return;
    g_destroyFramebufferGate.enter();
    real(device, framebuffer, allocator);
}

extern "C" VkResult vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
                                       const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer) {
    using Fn = VkResult (*)(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "vkCreateFramebuffer"));
    if (real == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (g_framebufferGate.enter()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return real(device, info, allocator, framebuffer);
}

// Where the post-call phase fails the daemon's reply to one named consumer: sd-bus's send, which
// sdbus-c++ reaches as a dynamic symbol, so defining it here (exported by the CMake entry) puts a
// throw inside the worker task that answers the call -- the one path no bus client can cause.
std::atomic<bool> g_failReplyArmed{false};
std::mutex g_failReplyMutex;
std::string g_failReplyTo;   // guarded by g_failReplyMutex
int g_failReplyLetThrough = 0;  // guarded by g_failReplyMutex: matching sends passed before the one failed
std::atomic<int> g_repliesFailed{0};

// Arms the interposer below for the next send to `to` after `letThrough` others to it.
void failReplyTo(const std::string& to, int letThrough = 0) {
    std::lock_guard<std::mutex> lock(g_failReplyMutex);
    g_failReplyTo = to;
    g_failReplyLetThrough = letThrough;
    g_failReplyArmed.store(true);
}

extern "C" int sd_bus_send(sd_bus* bus, sd_bus_message* message, uint64_t* cookie) {
    using Fn = int (*)(sd_bus*, sd_bus_message*, uint64_t*);
    using DestinationFn = const char* (*)(sd_bus_message*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "sd_bus_send"));
    static const DestinationFn destination =
        reinterpret_cast<DestinationFn>(::dlsym(RTLD_DEFAULT, "sd_bus_message_get_destination"));
    if (real == nullptr) return -ENOSYS;
    if (g_failReplyArmed.load() && message != nullptr && destination != nullptr) {
        const char* to = destination(message);
        std::lock_guard<std::mutex> lock(g_failReplyMutex);
        if (to != nullptr && g_failReplyTo == to && g_failReplyLetThrough-- <= 0 && g_failReplyArmed.exchange(false)) {
            ++g_repliesFailed;
            return -EIO;
        }
    }
    return real(bus, message, cookie);
}

// Where the descriptor-cap phase fails one export: the memory-descriptor query, answered the way a
// driver out of descriptors answers it (VK_SUCCESS with fd -1), so the daemon takes its failed-export
// path on a healthy device. exportFrame looks the entry point up on every call that sends the memory
// (a reply leaving it out never asks), so the first such lookup after arming gets the failing one, once.
std::atomic<bool> g_failNextMemoryExport{false};
std::atomic<int> g_memoryExportsFailed{0};

VKAPI_ATTR VkResult VKAPI_CALL failingGetMemoryFd(VkDevice, const VkMemoryGetFdInfoKHR*, int* fd) {
    ++g_memoryExportsFailed;
    if (fd != nullptr) *fd = -1;
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    using Fn = PFN_vkVoidFunction (*)(VkDevice, const char*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "vkGetDeviceProcAddr"));
    if (real == nullptr) return nullptr;
    if (name != nullptr && std::strcmp(name, "vkGetMemoryFdKHR") == 0 && g_failNextMemoryExport.exchange(false)) {
        return reinterpret_cast<PFN_vkVoidFunction>(&failingGetMemoryFd);
    }
    return real(device, name);
}

namespace {

// This process's stdout, where the daemon under test logs: the lines the service printed, counted.
struct StdoutCapture : wallpaperx::testing::StreamCapture {
    StdoutCapture() : StreamCapture(STDOUT_FILENO) {}
};

// The client-side wire helpers, shared with daemon_system_test (tests/client/daemon_client.h).
using wallpaperx::testing::Check;
using wallpaperx::testing::errorNameFrom;
using wallpaperx::testing::exportDelivered;
using wallpaperx::testing::Frame;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::getFrame;
using wallpaperx::testing::getStatusOrExplain;
using wallpaperx::testing::openFdCount;
using wallpaperx::testing::setImportModifiers;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::StatusReply;
using wallpaperx::testing::validationWaived;
using wallpaperx::testing::wallpaperStatus;

// Pass/fail per tests/test_report.h. A host with no validation layer is a failure, not a skip.
// Two clauses cannot be asked everywhere and report notVerified (a skip): S6.12's unreadable file
// (root reads mode 000) and S6.24's content half where the frame can be neither mapped nor imported
// (a build without the GL importer, or no GL context on the exporting GPU).

// Not square, so a width/height swap between target and wire fails.
constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 48;

// Disjoint per scenario, so a "nothing rendered yet" check never meets another check's monitor.
constexpr int32_t kMonitorStatic = 0;
constexpr int32_t kMonitorUnrenderable = 1;
constexpr int32_t kMonitorCorrupt = 2;
constexpr int32_t kMonitorBarrier = 3;
constexpr int32_t kMonitorBurstFailLast = 4;
constexpr int32_t kMonitorBurstRenderLast = 5;
constexpr int32_t kMonitorBurstContent = 6;
constexpr int32_t kMonitorCoalesceBarrier = 7;
constexpr int32_t kMonitorErrorRecovery = 8;
constexpr int32_t kMonitorNeverSet = 9;
constexpr int32_t kMonitorWorkerCoalesce = 10;
// Kept off kMonitorNeverSet: a rejected path that stopped being refused would be recorded and
// fail the never-set checks too, pointing at the wrong property.
constexpr int32_t kMonitorRejectedPaths = 11;

// runUnservicedRequestChecks' own manager; numbered on so no id means two things in this file.
constexpr int32_t kMonitorUnservicedStatic = 12;
constexpr int32_t kMonitorUnservicedUnrenderable = 13;
constexpr int32_t kMonitorUnservicedNeverSet = 14;

// Connector names on the shared manager; naming records nothing in GetStatus's `wallpapers`.
constexpr int32_t kMonitorOutputA = 15;
constexpr int32_t kMonitorOutputB = 16;
// Holds "DP-KEEP", then receives every refused name: the kept name is what lets the refusal
// check grade destroys-nothing, not only adds-nothing.
constexpr int32_t kMonitorOutputRejected = 17;
constexpr int32_t kMonitorOutputAtCap = 18;
constexpr int32_t kMonitorOutputHostile = 19;

// runImportModifierChecks' own manager: one re-rendered, one whose re-render has not run.
constexpr int32_t kMonitorModifiersRerendered = 20;
constexpr int32_t kMonitorModifiersPending = 21;

// Shared manager, over the bus; its own id so the generation it waits for is its alone.
constexpr int32_t kMonitorModifiersWire = 22;

// The repeated report's barrier, apart from the monitor above so it cannot coalesce with the
// re-render under test; first set inside that check, so the repeat cannot re-post it.
constexpr int32_t kMonitorModifiersBarrier = 23;

// runImportModifierClientChecks' own manager, never rendered: a recorded request with no frame is
// re-posted on every real change of the set and on nothing else.
constexpr int32_t kMonitorModifiersIdle = 24;

// Shared manager: a monitor held at "no frame" across a departure, and a barrier of its own.
constexpr int32_t kMonitorWithdrawRender = 25;
constexpr int32_t kMonitorWithdrawBarrier = 26;

// runImportModifierChecks' manager: one made undrawable while it owns its image (S6.53), and a
// pair whose frames must survive a report undone before the render thread sees it (S6.54).
constexpr int32_t kMonitorModifiersUnrenderable = 27;
constexpr int32_t kMonitorModifiersUndoneA = 28;
constexpr int32_t kMonitorModifiersUndoneB = 29;

// runImportModifierAnnouncementChecks' manager: an undrawable type (never re-rendered) and a
// renderable monitor with no frame (re-rendered on every change).
constexpr int32_t kMonitorAnnounceUnrenderable = 30;
constexpr int32_t kMonitorAnnounceCondemned = 31;

// Low ids again, each on a manager of its own (the shared one uses all of kMaxMonitors).
// runBufferHoldChecks: the ring driven to its reserve by two consumers at their cap, and apart
// from it one consumer that never releases and must run out on the cap alone.
constexpr int32_t kMonitorHoldRing = 0;
constexpr int32_t kMonitorHoldNeverReleases = 1;
// Frames exported to a caller with no bus name.
constexpr int32_t kMonitorHoldUnnamed = 2;
// The only one driven past the holder limit, so no earlier case decides its holder count.
constexpr int32_t kMonitorHoldLimit = 3;

// On runBufferReleaseWireChecks' own manager, which is driven entirely over the bus.
constexpr int32_t kMonitorReleaseWire = 0;

// Written out rather than read from the daemon's own constants (ring_depth.h kTargetDepth,
// kMaxHeldPerClient, kMaxHoldersPerMonitor): a change there would move both sides of every
// comparison and keep the checks green for any value.
constexpr int kExpectedRingDepth = 5;
constexpr int kExpectedHeldPerClient = 2;
constexpr int kExpectedHoldersPerMonitor = 4;

// Two consumers at their cap must leave exactly the producer's reserve free.
static_assert(kExpectedRingDepth == 2 * kExpectedHeldPerClient + 1,
              "the ring phase fills the ring with exactly two consumers at their cap and expects one image left; a "
              "different depth needs a different number of consumers or the reserve is never reached");

// The holder-limit checks put every holder on one image, so a refusal there is the holder count.
static_assert(kExpectedHoldersPerMonitor >= 2 && kExpectedRingDepth >= 2,
              "the holder-limit checks need at least two holders on one image and a ring with room to spare, or the "
              "refusal they grade could equally be the producer's reserve");

// Well-formed modifiers no driver can allocate (0x0f is an unassigned vendor byte in
// drm_fourcc.h): a layout a consumer reads and this GPU cannot produce. Not kDrmModifierInvalid,
// which is the "no layout" sentinel.
constexpr uint64_t kUnavailableModifierA = 0x0f00000000000001ull;
constexpr uint64_t kUnavailableModifierB = 0x0f00000000000002ull;
constexpr uint64_t kUnavailableModifierC = 0x0f00000000000003ull;
constexpr uint64_t kUnavailableModifierD = 0x0f00000000000004ull;

// The bytes that make the hostile connector name hostile, as http_server_test.cpp spells them.
constexpr const char* kBidiOverride = "\xE2\x80\xAE";  // U+202E RIGHT-TO-LEFT OVERRIDE
constexpr char kDel = '\x7F';

// GetFrame's three frameless errors (S6.39), named once so no two are spelled alike by accident.
constexpr const char* kErrNoFrame = "io.wallpaperx.Daemon.Error.NoFrame";
constexpr const char* kErrRenderFailed = "io.wallpaperx.Daemon.Error.RenderFailed";
constexpr const char* kErrUnsupported = "io.wallpaperx.Daemon.Error.Unsupported";
static_assert(std::string_view(kErrNoFrame) != std::string_view(kErrRenderFailed) &&
                  std::string_view(kErrNoFrame) != std::string_view(kErrUnsupported) &&
                  std::string_view(kErrRenderFailed) != std::string_view(kErrUnsupported),
              "the three frameless error names must be distinct, or the S6.39 checks that assert them "
              "side by side would pass for a daemon that had collapsed two of them");

// How many requests each coalescing burst fires at one monitor. Five is enough that a
// worker draining one per render still leaves several to collapse.
constexpr int kBurstSize = 5;

std::string testAssetDir() { return wallpaperx::testing::testImageDir(); }

// The message a call failed with. Local: only this tier asserts on message text.
std::string errorMessageFrom(const std::function<void()>& call) {
    try {
        call();
    } catch (const sdbus::Error& e) {
        return e.getMessage();
    }
    return "";
}

// Overwrites `path` with a solid-colour PNG, keeping the path: one phase makes an undecodable
// file decodable without a SetWallpaper, which would itself re-post the render.
void writeSolidPng(const std::string& path, unsigned char r, unsigned char g, unsigned char b) {
    const int w = 8;
    const int h = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        pixels[i * 4 + 0] = r;
        pixels[i * 4 + 1] = g;
        pixels[i * 4 + 2] = b;
        pixels[i * 4 + 3] = 255;
    }
    if (!stbi_write_png(path.c_str(), w, h, 4, pixels.data(), w * 4)) {
        throw std::runtime_error("dbus_service_test: failed to write " + path);
    }
}

std::string writeTestImage() {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/dbus_test_blue.png";
    writeSolidPng(path, 0, 0, 255);
    return path;
}

// The coalescing burst's solid colours; the first and last differ in every channel, so a daemon
// serving the first request instead of the last is wrong in red, green and blue.
constexpr int kBurstColorCount = kBurstSize;
constexpr unsigned char kBurstColors[kBurstColorCount][3] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {32, 160, 224},
};

static_assert(kBurstColors[0][0] != kBurstColors[kBurstColorCount - 1][0] &&
                  kBurstColors[0][1] != kBurstColors[kBurstColorCount - 1][1] &&
                  kBurstColors[0][2] != kBurstColors[kBurstColorCount - 1][2],
              "the burst palette's first and last colours must differ in every channel, or the pixel comparison in "
              "runCoalescingChecks cannot tell first-wins from last-wins");

// Writes one solid-colour PNG per burst step and returns their paths, in order.
std::vector<std::string> writeBurstImages() {
    std::filesystem::create_directories(testAssetDir());
    std::vector<std::string> paths;
    for (int i = 0; i < kBurstColorCount; ++i) {
        const std::string path = testAssetDir() + "/dbus_burst_" + std::to_string(i) + ".png";

        const int w = 8;
        const int h = 8;
        std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
        for (int p = 0; p < w * h; ++p) {
            pixels[p * 4 + 0] = kBurstColors[i][0];
            pixels[p * 4 + 1] = kBurstColors[i][1];
            pixels[p * 4 + 2] = kBurstColors[i][2];
            pixels[p * 4 + 3] = 255;
        }
        if (!stbi_write_png(path.c_str(), w, h, 4, pixels.data(), w * 4)) {
            throw std::runtime_error("dbus_service_test: failed to write burst test image");
        }
        paths.push_back(path);
    }
    return paths;
}


// Every <arg> of one introspected method or signal, in order, as (direction, "type:name"); a
// signal's direction is empty.
std::vector<std::pair<std::string, std::string>> parseArgs(const std::string& xml, const std::string& element,
                                                           const std::string& name) {
    std::vector<std::pair<std::string, std::string>> args;
    // [\s\S] rather than . -- the XML is multi-line and . does not match newline.
    const std::regex elementRe("<" + element + " name=\"" + name + "\">([\\s\\S]*?)</" + element + ">");
    std::smatch elementMatch;
    if (!std::regex_search(xml, elementMatch, elementRe)) {
        return args;
    }
    const std::string body = elementMatch[1].str();

    const std::regex argRe("<arg\\b[^>]*/>");
    const std::regex attrRe("(\\w+)=\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), argRe); it != std::sregex_iterator(); ++it) {
        const std::string tag = it->str();
        std::string type, argName, direction;
        for (auto a = std::sregex_iterator(tag.begin(), tag.end(), attrRe); a != std::sregex_iterator(); ++a) {
            const std::string key = (*a)[1].str(), value = (*a)[2].str();
            if (key == "type") type = value;
            else if (key == "name") argName = value;
            else if (key == "direction") direction = value;
        }
        args.emplace_back(direction, type + ":" + argName);
    }
    return args;
}

using ArgList = std::vector<std::pair<std::string, std::string>>;

// The body of one <interface name="..."> element, or "" when the object does not export it.
std::string interfaceBody(const std::string& xml, const std::string& name) {
    const std::regex interfaceRe("<interface name=\"" + name + "\">([\\s\\S]*?)</interface>");
    std::smatch match;
    if (!std::regex_search(xml, match, interfaceRe)) return {};
    return match[1].str();
}

// Every member of one kind the interface declares, sorted: what the per-member comparison, which
// walks the expected table, cannot see.
std::vector<std::string> elementNames(const std::string& ifaceXml, const std::string& element) {
    std::vector<std::string> names;
    const std::regex re("<" + element + " name=\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(ifaceXml.begin(), ifaceXml.end(), re); it != std::sregex_iterator(); ++it) {
        names.push_back((*it)[1].str());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string joinQuoted(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) out += ", ";
        out += "'" + value + "'";
    }
    return out.empty() ? "(none)" : out;
}

// Checks that need no GPU, first, so a host without Vulkan still verifies the wire contract.
void runHostChecks(sdbus::IProxy& proxy, const std::string& imagePath, const Check& check) {
    // GOAL: S6.1 -- the bus name, object path and interface are exactly the documented strings.
    // WHY-THIS-SHAPE: literals, not the constants: every other check reaches the daemon through
    //       DbusService's constants, so a typo there moves client and service together.
    // CONTROL: all three constants in dbus_service.h changed to "...Deamon" -> "FAIL: the bus name
    //       is exactly io.wallpaperx.Daemon" plus the object-path and interface lines.
    check(std::string(DbusService::kBusName) == "io.wallpaperx.Daemon",
          "the bus name is exactly io.wallpaperx.Daemon");
    check(std::string(DbusService::kObjectPath) == "/io/wallpaperx/Daemon",
          "the object path is exactly /io/wallpaperx/Daemon");
    check(std::string(DbusService::kInterface) == "io.wallpaperx.Daemon",
          "the interface is exactly io.wallpaperx.Daemon");

    const StatusReply status = getStatusOrExplain(proxy);
    check(status.ok(), "GetStatus returns parseable JSON");
    const nlohmann::json& json = status.json();
    check(json.value("running", false), "GetStatus reports running=true");

    // No GPU needed yet, so none to report. is_object() first: value() on a non-object throws.
    check(json.contains("gpu") && json["gpu"].is_object() && json["gpu"].value("initialized", true) == false,
          "GetStatus reports the GPU as uninitialised before the first render");

    // GOAL: S6.16 -- gpu.name and gpu.descriptor_tier are absent before the first render.
    // WHY-THIS-SHAPE: contains(), since initialized==false beside a guessed tier passes the line above.
    // CONTROL: dbus_service.cpp writing gpuJson["descriptor_tier"] before `if (gpu.initialized)` ->
    //       "FAIL: no GPU name or descriptor tier is reported before the device exists".
    check(json.contains("gpu") && !json["gpu"].contains("name") && !json["gpu"].contains("descriptor_tier"),
          "no GPU name or descriptor tier is reported before the device exists");

    // CONTROL: backend/daemon/wallpaper_manager.cpp:1139 `if (!isKnownType(type))` made `if (type.empty())`,
    //       control_batch 2026-10-15, dbus_service_lavapipe '^host$' -> "FAIL: an unknown wallpaper type is
    //       rejected as InvalidArgs", alone; restored, green.
    check(errorNameFrom([&] { setWallpaper(proxy, "banana", imagePath, kMonitorUnrenderable); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "an unknown wallpaper type is rejected as InvalidArgs");

    // CONTROL: backend/daemon/wallpaper_manager.cpp:62, `monitorId < 0 ||` removed from requireValidMonitorId,
    //       control_batch 2026-10-15, dbus_service_lavapipe '^host$' -> "FAIL: a negative monitor id is rejected
    //       as InvalidArgs" and "FAIL: GetFrame rejects a negative monitor id as InvalidArgs, not NoFrame";
    //       restored, green.
    check(errorNameFrom([&] { setWallpaper(proxy, "static", imagePath, -1); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a negative monitor id is rejected as InvalidArgs");

    const int32_t tooLarge = wallpaperx::WallpaperManager::kMaxMonitors;
    check(errorNameFrom([&] { setWallpaper(proxy, "static", imagePath, tooLarge); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a monitor id at or above the cap is rejected as InvalidArgs");

    // Both entry points reject an id alike; NoFrame would tell a consumer to retry forever.
    check(errorNameFrom([&] { getFrame(proxy, -1); }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "GetFrame rejects a negative monitor id as InvalidArgs, not NoFrame");
    check(errorNameFrom([&] { getFrame(proxy, tooLarge); }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "GetFrame rejects an out-of-range monitor id as InvalidArgs, not NoFrame");

    // Rendering is asynchronous, so a path that is not a file must fail on the reply.
    check(errorNameFrom([&] { setWallpaper(proxy, "static", "/nonexistent/wallpaperx.png", kMonitorNeverSet); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a static path that is not a file is rejected synchronously as InvalidArgs");
    check(errorNameFrom([&] { getFrame(proxy, kMonitorNeverSet); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "a rejected request leaves the monitor with no frame");

    // GOAL: S6.12 -- a `static` path that is not a regular file, and one that exists but cannot be
    //       read, are both rejected on the reply as InvalidArgs; existence alone is not the check.
    // WHY-THIS-SHAPE: a directory and a mode-000 file both exist and each fails a different clause;
    //       the nonexistent path above passes for a bare exists() test.
    // CONTROL: WallpaperManager::requestWallpaper's condition made `!std::filesystem::exists(path,
    //       ec)` -> "FAIL: a directory is rejected synchronously as InvalidArgs" and "FAIL: an
    //       unreadable file is rejected synchronously as InvalidArgs".
    check(errorNameFrom([&] { setWallpaper(proxy, "static", testAssetDir(), kMonitorRejectedPaths); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a directory is rejected synchronously as InvalidArgs");

    // GOAL: S6.12's access(R_OK) clause is exercised or reported unexercised, never passed over.
    // WHY-THIS-SHAPE: notVerified() naming the clause; check(false) would blame the daemon for the
    //       caller's uid (X.3).
    // CONTROL: the `|| ::access(...) == 0` disjunct forced true -> "NOT VERIFIED -- S6.12's
    //       unreadable-file clause ...", exit 77, ctest reporting Skipped.
    // Named per process: the dbus_service entries share the asset directory and run in parallel, so a
    // shared file would be made readable by one entry's restore while another expects it refused.
    const std::string unreadablePath =
        testAssetDir() + "/dbus_test_unreadable_" + std::to_string(::getpid()) + ".png";
    {
        std::ofstream out(unreadablePath, std::ios::binary);
        out << "not readable";
    }
    std::error_code permEc;
    std::filesystem::permissions(unreadablePath, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, permEc);
    if (permEc || ::access(unreadablePath.c_str(), R_OK) == 0) {
        wallpaperx::testing::notVerified(
            "S6.12's unreadable-file clause (its is_regular_file clause ran and held above)",
            "this process can still read a mode-000 file (running as root?), so the daemon accepting or "
            "rejecting it would say nothing about the access(R_OK) check");
    } else {
        check(errorNameFrom([&] { setWallpaper(proxy, "static", unreadablePath, kMonitorRejectedPaths); }) ==
                  "org.freedesktop.DBus.Error.InvalidArgs",
              "an unreadable file is rejected synchronously as InvalidArgs");
    }
    // Restored, so cleaning the asset directory is not blocked by a file nothing can open.
    std::filesystem::permissions(unreadablePath, std::filesystem::perms::owner_read |
                                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, permEc);
    std::filesystem::remove(unreadablePath, permEc);

    // GOAL: S6.38 -- an empty, a nonexistent and a directory path are InvalidArgs for `video` and
    //       `shader` exactly as for `static`, and none is recorded.
    // WHY-THIS-SHAPE: a table over both types and all three paths, since the defect was a branch
    //       on the type; the monitor must still read as never set (a recorded `shader` request
    //       would answer Unsupported).
    // CONTROL: the path check in WallpaperManager::requestWallpaper (backend/daemon/
    //       wallpaper_manager.cpp) put back inside `if (type == "static")` -> 8 FAIL lines: one per
    //       (type, path) pair, "a monitor whose every request was rejected is still reported as
    //       never set" (answered Unsupported) and the "call SetWallpaper first" line.
    // COVERAGE-ASSERT: the pairs driven are counted against the table's size.
    {
        const std::string cases[][2] = {
            {"", "an empty path"},
            {"/nonexistent/wallpaperx-video.mp4", "a nonexistent path"},
            {testAssetDir(), "a directory"},
        };
        const char* const types[] = {"video", "shader"};
        const int expectedCases =
            static_cast<int>((sizeof(types) / sizeof(types[0])) * (sizeof(cases) / sizeof(cases[0])));

        int driven = 0;
        for (const char* type : types) {
            for (const auto& [path, description] : cases) {
                ++driven;
                const std::string message = std::string("a ") + type + " wallpaper with " + description +
                                            " is rejected as InvalidArgs";
                check(errorNameFrom([&] { setWallpaper(proxy, type, path, kMonitorRejectedPaths); }) ==
                          "org.freedesktop.DBus.Error.InvalidArgs",
                      message.c_str());
            }
        }
        check(driven == expectedCases,
              "every (type, path) rejection case in the table was actually driven");

        // A rejected request must not be recorded: a recorded shader request answers Unsupported.
        const std::string afterRejections = errorNameFrom([&] { getFrame(proxy, kMonitorRejectedPaths); });
        check(afterRejections == kErrNoFrame,
              "a monitor whose every request was rejected is still reported as never set");
        if (afterRejections != kErrNoFrame) {
            std::fprintf(stderr, "  it answered '%s', so one of the rejected requests was recorded\n",
                         afterRejections.c_str());
        }
        check(errorMessageFrom([&] { getFrame(proxy, kMonitorRejectedPaths); }).find("call SetWallpaper first") !=
                  std::string::npos,
              "a monitor whose every request was rejected is told to call SetWallpaper first");
    }

    // The whole wire contract the plugin is written against: every method and signal and every
    // argument, in order. Parsed, not substring-matched, so libsystemd's formatting is not part of it.
    std::string introspection;
    proxy.callMethod("Introspect").onInterface("org.freedesktop.DBus.Introspectable").storeResultsTo(introspection);

    // GOAL: S6.1/S6.2 -- the methods and signals belong to io.wallpaperx.Daemon itself.
    // WHY-THIS-SHAPE: read inside that interface's body; a search of the whole document passes for
    //       a vtable under any interface name.
    // CONTROL: kInterface changed to "io.wallpaperx.Deamon" -> "FAIL: the object exports the
    //       io.wallpaperx.Daemon interface" and every signature line.
    const std::string ifaceXml = interfaceBody(introspection, "io.wallpaperx.Daemon");
    check(!ifaceXml.empty(), "the object exports the io.wallpaperx.Daemon interface");

    const std::vector<std::tuple<std::string, std::string, ArgList>> expected = {
        {"method", "SetWallpaper",
         ArgList{{"in", "s:type"}, {"in", "s:path"}, {"in", "i:monitor_id"}}},
        // GOAL: S15.96 -- properties travel as their own `a{sv}` on two methods of their own, and
        //       SetWallpaper keeps its three arguments.
        // CONTROL: in DbusService::registerVTable (backend/daemon/dbus_service.cpp), the last name of
        //       SetWallpaperWithProperties made "values", and the first of SetWallpaperProperties made
        //       "monitor" -> each, "FAIL: wire signature of method <that method> is unchanged".
        {"method", "SetWallpaperWithProperties",
         ArgList{{"in", "s:type"}, {"in", "s:path"}, {"in", "i:monitor_id"}, {"in", "a{sv}:properties"}}},
        {"method", "SetWallpaperProperties", ArgList{{"in", "i:monitor_id"}, {"in", "a{sv}:properties"}}},
        // GOAL: S15.283 -- the one frame method takes the images the caller can still draw and
        //       whether it asks on its timeline, and answers the memory as 0 or 1 descriptors.
        // CONTROL: in DbusService::registerVTable (backend/daemon/dbus_service.cpp),
        //       "known_buffer_ids" renamed "known" -> "FAIL: wire signature of method GetFrame is
        //       unchanged".
        {"method", "GetFrame",
         ArgList{{"in", "i:monitor_id"},
                 {"in", "at:known_buffer_ids"},
                 {"in", "b:on_timeline"},
                 {"out", "ah:memory_fd"},
                 {"out", "u:width"},
                 {"out", "u:height"},
                 {"out", "t:memory_size"},
                 {"out", "i:handle_type"},
                 {"out", "ah:sync_fds"},
                 {"out", "t:frame_generation"},
                 {"out", "u:drm_format"},
                 {"out", "t:drm_modifier"},
                 {"out", "a(tt):planes"},
                 {"out", "ay:device_uuid"},
                 {"out", "t:buffer_id"},
                 {"out", "t:acquire_value"},
                 {"out", "t:handover"}}},
        // CONTROL: in DbusService::registerVTable (backend/daemon/dbus_service.cpp), each of these
        //       renamed alone -> "FAIL: wire signature of method <it> is unchanged": GetAcquireTimeline's
        //       "timeline" as "fd"; ReleaseBuffer's "handover" as "number" (and GetFrame's last output
        //       likewise, S15.307); SetMonitorOutput's "output_name" as "output"; SetOutputSize's "width"
        //       as "w"; SetOutputVisible's "visible" as "shown"; SetMonitorPause's "mode" as
        //       "pause_mode" and SetOutputFocus's "window_maximized" as "maximized" (S15.121). Dropping
        //       a name is no control: sd-bus then refuses the whole vtable.
        {"method", "GetAcquireTimeline", ArgList{{"out", "h:timeline"}}},
        {"method", "ReleaseBuffer", ArgList{{"in", "i:monitor_id"}, {"in", "t:buffer_id"}, {"in", "t:handover"}}},
        {"method", "SetImportModifiers", ArgList{{"in", "u:drm_format"}, {"in", "at:modifiers"}}},
        {"method", "SetMonitorOutput", ArgList{{"in", "i:monitor_id"}, {"in", "s:output_name"}}},
        {"method", "SetOutputSize", ArgList{{"in", "s:output_name"}, {"in", "u:width"}, {"in", "u:height"}}},
        {"method", "SetOutputVisible", ArgList{{"in", "s:output_name"}, {"in", "b:visible"}}},
        {"method", "SetMonitorPause", ArgList{{"in", "i:monitor_id"}, {"in", "s:mode"}}},
        {"method", "SetOutputFocus",
         ArgList{{"in", "s:output_name"}, {"in", "b:window_focused"}, {"in", "b:window_maximized"}}},
        // GOAL: S15.446 -- the two sound setters and the focus report naming the desktop are exported
        //       with these signatures, and SetOutputFocus keeps its own beside them for an older plugin.
        // CONTROL: via control_batch.sh, --phase '^host$', 2026-10-11, in DbusService::registerVTable
        //       (backend/daemon/dbus_service.cpp), each alone: SetMonitorSound's "on" renamed "enabled" ->
        //       "FAIL: wire signature of method SetMonitorSound is unchanged"; SetOutputFocusState's "focus"
        //       renamed "state" -> "FAIL: wire signature of method SetOutputFocusState is unchanged". Reverted.
        {"method", "SetSound", ArgList{{"in", "b:on"}}},
        {"method", "SetMonitorSound", ArgList{{"in", "i:monitor_id"}, {"in", "b:on"}}},
        {"method", "SetOutputFocusState",
         ArgList{{"in", "s:output_name"}, {"in", "s:focus"}, {"in", "b:window_maximized"}}},
        // GOAL: the four frame-rate setters are exported with these signatures: an int rate, a monitor's
        //       int rate, a bool keep-higher, and an output's refresh rate as a double.
        // CONTROL: via control_batch.sh, --phase '^host$', 2026-10-20, in DbusService::registerVTable
        //       (backend/daemon/dbus_service.cpp), each alone -> "FAIL: wire signature of method <it> is
        //       unchanged": SetFrameRate's "rate" renamed "fps"; SetMonitorFrameRate's rate taken as bool;
        //       SetKeepHigherFrameRate's "keep" renamed "on"; SetOutputRefreshRate's rate taken as int32_t.
        //       Reverted.
        {"method", "SetFrameRate", ArgList{{"in", "i:rate"}}},
        {"method", "SetMonitorFrameRate", ArgList{{"in", "i:monitor_id"}, {"in", "i:rate"}}},
        {"method", "SetKeepHigherFrameRate", ArgList{{"in", "b:keep"}}},
        {"method", "SetOutputRefreshRate", ArgList{{"in", "s:output_name"}, {"in", "d:refresh_rate"}}},
        {"method", "GetStatus", ArgList{{"out", "s:status"}}},
        {"signal", "FrameReady", ArgList{{"", "i:monitor_id"}, {"", "t:frame_generation"}}},
        // Argument-free: what the layouts now are is what the other consumers reported.
        {"signal", "ImportModifiersChanged", ArgList{}},
        // GOAL: S15.393 -- ConfigChanged is exported, with no arguments: a consumer reads /config again.
        // CONTROL: `sdbus::registerSignal("ConfigChanged")` deleted from DbusService::registerVTable
        //       (backend/daemon/dbus_service.cpp) -> "FAIL: io.wallpaperx.Daemon exports exactly the
        //       signals this table names".
        {"signal", "ConfigChanged", ArgList{}},
        // GOAL: S15.685 -- PersistenceChanged is exported, with no arguments: a consumer reads GetStatus again.
        // CONTROL: control_batch.sh 2026-10-17, phase host, `sdbus::registerSignal("PersistenceChanged")` in
        //       DbusService::registerVTable (backend/daemon/dbus_service.cpp) renamed "PersistenceNotice" ->
        //       "FAIL: io.wallpaperx.Daemon exports exactly the signals this table names". Reverted.
        {"signal", "PersistenceChanged", ArgList{}},
    };

    for (const auto& [element, name, expectedArgs] : expected) {
        const ArgList actualArgs = parseArgs(ifaceXml, element, name);
        const std::string label = element + " " + name;
        check(actualArgs == expectedArgs, ("wire signature of " + label + " is unchanged").c_str());
        if (actualArgs != expectedArgs) {
            std::fprintf(stderr, "  %s expected:", label.c_str());
            for (const auto& [dir, arg] : expectedArgs) std::fprintf(stderr, " %s(%s)", arg.c_str(), dir.c_str());
            std::fprintf(stderr, "\n  %s actual:  ", label.c_str());
            for (const auto& [dir, arg] : actualArgs) std::fprintf(stderr, " %s(%s)", arg.c_str(), dir.c_str());
            std::fprintf(stderr, "\n");
        }
    }

    // GOAL: io.wallpaperx.Daemon exports exactly the members named above; no extra one appears
    //       unreviewed.
    // WHY-THIS-SHAPE: sorted name lists, because the loop above walks the table and cannot see a
    //       member the table lacks.
    // CONTROL: re-run via control_batch.sh, --phase '^host$', 2026-10-20: `sdbus::registerMethod("Debug")
    //       .implementedAs([this]() { (void)this; })` added to DbusService::registerVTable
    //       (backend/daemon/dbus_service.cpp) -> "FAIL: io.wallpaperx.Daemon exports exactly the methods this
    //       table names", alone. SetOutputRefreshRate's registration deleted -> that line and "FAIL: wire
    //       signature of method SetOutputRefreshRate is unchanged". Reverted.
    {
        std::vector<std::string> expectedMethods, expectedSignals;
        for (const auto& [element, name, expectedArgs] : expected) {
            (void)expectedArgs;
            (element == "method" ? expectedMethods : expectedSignals).push_back(name);
        }
        std::sort(expectedMethods.begin(), expectedMethods.end());
        std::sort(expectedSignals.begin(), expectedSignals.end());

        const std::pair<const std::vector<std::string>*, const char*> kinds[] = {
            {&expectedMethods, "method"},
            {&expectedSignals, "signal"},
        };
        for (const auto& [expectedNames, element] : kinds) {
            const std::vector<std::string> actualNames = elementNames(ifaceXml, element);
            const std::string message =
                std::string("io.wallpaperx.Daemon exports exactly the ") + element + "s this table names";
            check(actualNames == *expectedNames, message.c_str());
            if (actualNames != *expectedNames) {
                std::fprintf(stderr, "  %ss expected: %s\n  %ss actual:   %s\n", element,
                             joinQuoted(*expectedNames).c_str(), element, joinQuoted(actualNames).c_str());
            }
        }
    }
}

// SetMonitorOutput: which screen answers to which connector name. Needs no GPU.

// Every connector name GetStatus publishes for one monitor; a list, since "at most one" is a count.
std::vector<std::string> namesFor(const StatusReply& status, int32_t monitorId) {
    std::vector<std::string> names;
    const nlohmann::json* outputs = wallpaperx::testing::outputBindings(status);
    if (outputs == nullptr) return names;
    for (const auto& [name, id] : outputs->items()) {
        if (id.is_number_integer() && id.get<int>() == monitorId) names.push_back(name);
    }
    return names;
}

// Escaped again here: a failing run is when a name holds the newline or override the checks
// forbid, and printed raw a name holding "\nFAIL: " would forge an uncounted failure.
std::string joinNames(const std::vector<std::string>& names) {
    std::vector<std::string> escaped;
    escaped.reserve(names.size());
    for (const std::string& name : names) {
        escaped.push_back(
            wallpaperx::sanitizeForLog(name, wallpaperx::kMaxOutputNameBytes, wallpaperx::LogFraming::Bare));
    }
    return joinQuoted(escaped);
}

void runOutputNameChecks(sdbus::IProxy& proxy, const Check& check) {
    using wallpaperx::testing::boundMonitor;
    using wallpaperx::testing::outputBindings;
    using wallpaperx::testing::setMonitorOutput;

    constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

    check(outputBindings(getStatusOrExplain(proxy)) != nullptr,
          "GetStatus carries an outputs object mapping connector names to monitors");

    // Every SetMonitorOutput below goes through errorNameFrom (or its own try), so an unexpected
    // error reply is a named FAIL and not an exception ending the phase.
    // CONTROL: `if (outputName == "DP-MOVE") throw std::runtime_error(...)` added after the argument
    //       checks in WallpaperManager::setMonitorOutput (backend/daemon/wallpaper_manager.cpp),
    //       --phase '^(outputName|render)$' -> five named FAIL lines from "naming monitor A 'DP-MOVE'
    //       is accepted" on, and the render phase still ran.

    // GOAL: S10.7 -- an out-of-range monitor id, an empty name and an oversized name are each
    //       refused, none becomes a binding, and none costs the target monitor its existing name.
    // WHY-THIS-SHAPE: graded on the state left, not the error name. Name refusals target monitor
    //       17, which holds "DP-KEEP" first and must still hold it (destroys-nothing, not only
    //       adds-nothing); id refusals are graded through the name they used ("DP-1" never
    //       published), and repeated carrying "DP-KEEP", since a bad id with an unheld name has
    //       nothing to destroy -- the shape a stale id takes after an unplug.
    // CONTROL: in WallpaperManager::setMonitorOutput (backend/daemon/wallpaper_manager.cpp), --phase
    //       '^outputName$': (a) `requireValidMonitorId(monitorId);` deleted -> the four InvalidArgs
    //       lines, "FAIL: a refused connector name neither binds ..." ("monitor 17 answers to
    //       (none)") and the cross-monitor check; (b) that call moved after the save's try/catch ->
    //       only the last two; (c) `requireValidOutputName(outputName);` deleted -> the empty and
    //       oversized InvalidArgs lines, the refusal-state line and the cross-monitor check; (d)
    //       bindOutputLocked's release loop copied between the two argument checks, and (e) its
    //       displacement block copied above `requireValidMonitorId` -> each, only the refusal-state
    //       line and the cross-monitor check.
    // Split rather than ANDed, so a setup fault does not print as failures of the validation.
    // CONTROL: the setup name made "" -> "FAIL: the connector name the refusals below rely on is
    //       accepted" and "FAIL: the monitor the refusals below target answers to a connector name
    //       before they are made".
    const std::string keepError =
        errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputRejected, "DP-KEEP"); });
    check(keepError.empty(), "the connector name the refusals below rely on is accepted");
    if (!keepError.empty()) {
        std::fprintf(stderr, "  SetMonitorOutput refused 'DP-KEEP' with %s\n", keepError.c_str());
    }
    const int keepBinding = boundMonitor(getStatusOrExplain(proxy), "DP-KEEP");
    check(keepBinding == kMonitorOutputRejected,
          "the monitor the refusals below target answers to a connector name before they are made");
    if (keepBinding != kMonitorOutputRejected) {
        std::fprintf(stderr, "  'DP-KEEP' resolves to monitor %d, not %d\n", keepBinding, kMonitorOutputRejected);
    }

    check(errorNameFrom([&] { setMonitorOutput(proxy, -1, "DP-1"); }) == kInvalidArgs,
          "a negative monitor id is rejected by SetMonitorOutput as InvalidArgs");
    check(errorNameFrom([&] {
              setMonitorOutput(proxy, wallpaperx::WallpaperManager::kMaxMonitors, "DP-1");
          }) == kInvalidArgs,
          "a monitor id at or above the cap is rejected by SetMonitorOutput as InvalidArgs");
    // Again, carrying a name a live monitor holds: the only form in which a bad id can destroy.
    check(errorNameFrom([&] { setMonitorOutput(proxy, -1, "DP-KEEP"); }) == kInvalidArgs,
          "a negative monitor id is rejected even when the connector name is one a monitor already holds");
    check(errorNameFrom([&] {
              setMonitorOutput(proxy, wallpaperx::WallpaperManager::kMaxMonitors, "DP-KEEP");
          }) == kInvalidArgs,
          "a monitor id at or above the cap is rejected even when the connector name is one a monitor already holds");
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputRejected, ""); }) == kInvalidArgs,
          "an empty connector name is rejected as InvalidArgs");
    check(errorNameFrom([&] {
              setMonitorOutput(proxy, kMonitorOutputRejected,
                               std::string(wallpaperx::kMaxOutputNameBytes + 1, 'D'));
          }) == kInvalidArgs,
          "an oversized connector name is rejected as InvalidArgs");

    // GOAL: S10.7's UTF-8 clause -- a name that is not valid UTF-8 never becomes a binding.
    // WHY-THIS-SHAPE: the daemon never sees it -- sd-bus refuses to serialize the argument
    //       client-side -- so the outcome (no binding) is graded, not who refused; the daemon's own
    //       UTF-8 arm is graded by config_store_test and http_server_test. The throw itself is
    //       recorded (errorMessageFrom returns "" for success too), and a transport failure
    //       (NoReply, ServiceUnknown, Disconnected) never counts as a refusal.
    // CONTROL: not controllable from the daemon while the client refuses the argument: with the
    //       name validation deleted (control (c) above) this stays green. Harness side: (i)
    //       `badUtf8Name` made "DP-3" -> "FAIL: a connector name that is not valid UTF-8 does not
    //       complete as a successful call", the refusal-safety line and the cross-monitor check;
    //       (ii) the call sent to a name nobody owns -> the same FAIL line, "failed without an answer
    //       (...ServiceUnknown...)".
    const std::string badUtf8Name = std::string("DP\xFF-3");
    bool utf8NameRefused = false;
    std::string busRefusal;
    std::string refusalName;
    try {
        setMonitorOutput(proxy, kMonitorOutputRejected, badUtf8Name);
    } catch (const sdbus::Error& e) {
        utf8NameRefused = true;
        refusalName = std::string(e.getName());
        // Name as well as message: an error carrying no text still has to say which
        // component answered, or the diagnostic reads as though nothing happened.
        busRefusal = refusalName + (e.getMessage().empty() ? "" : ": " + std::string(e.getMessage()));
    }
    const bool noAnswer = refusalName == "org.freedesktop.DBus.Error.NoReply" ||
                          refusalName == "org.freedesktop.DBus.Error.ServiceUnknown" ||
                          refusalName == "org.freedesktop.DBus.Error.Disconnected";
    if (!utf8NameRefused) {
        std::printf("dbus_service_test: a connector name that is not valid UTF-8 unexpectedly completed as a "
                    "successful call\n");
    } else if (noAnswer) {
        std::printf("dbus_service_test: the call carrying a connector name that is not valid UTF-8 failed without an "
                    "answer (%s), so this run establishes nothing about the name\n",
                    busRefusal.c_str());
    } else if (wallpaperx::testing::daemonErrorName(refusalName)) {
        std::printf("dbus_service_test: a connector name that is not valid UTF-8 reached the daemon, which refused "
                    "it: '%s'\n",
                    busRefusal.c_str());
    } else {
        // InvalidArgs is the one name both sides can produce -- sd-bus reports a client-side
        // serialization refusal under it, and the daemon's own UTF-8 arm answers with it --
        // so the message text is printed instead of an attribution the name cannot support.
        std::printf("dbus_service_test: a connector name that is not valid UTF-8 was refused with '%s' -- the message "
                    "says which component answered, the error name alone does not\n",
                    busRefusal.c_str());
    }
    std::fflush(stdout);
    check(utf8NameRefused && !noAnswer,
          "a connector name that is not valid UTF-8 does not complete as a successful call");

    // One snapshot, read two ways: the name-validation refusals graded through the monitor
    // they targeted, the id refusals through the name they used.
    const StatusReply afterRefusals = getStatusOrExplain(proxy);
    const std::vector<std::string> refusedNames = namesFor(afterRefusals, kMonitorOutputRejected);
    const std::vector<std::string> keptOnly{"DP-KEEP"};
    check(refusedNames == keptOnly,
          "a refused connector name neither binds to the monitor it targeted nor disturbs the name it already "
          "answered to");
    if (refusedNames != keptOnly) {
        std::fprintf(stderr, "  monitor %d answers to %s\n", kMonitorOutputRejected, joinNames(refusedNames).c_str());
    }

    // The reply's arrival is asserted: a GetStatus that never answered publishes no "DP-1" either.
    const nlohmann::json* afterRefusalBindings = outputBindings(afterRefusals);
    const bool badIdNamePublished = afterRefusalBindings != nullptr && afterRefusalBindings->contains("DP-1");
    check(afterRefusalBindings != nullptr && !badIdNamePublished,
          "the connector name refused for an out-of-range monitor id is not published at all");
    if (badIdNamePublished) {
        std::fprintf(stderr, "  'DP-1' resolves to monitor %s\n", (*afterRefusalBindings)["DP-1"].dump().c_str());
    }

    // Split: refused outright and accepted-but-unpublished are different faults.
    // CONTROL: `status["outputs"]` dropped from DbusService::getStatus (backend/daemon/
    //       dbus_service.cpp) -> only "FAIL: a connector name of exactly the maximum length is
    //       published for the monitor it was given to"; `name.size() > kMaxOutputNameBytes` made
    //       `>=` in config_store.h -> both lines, the first "refused a 256-byte name".
    const std::string atCap(wallpaperx::kMaxOutputNameBytes, 'D');
    const std::string atCapError = errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputAtCap, atCap); });
    check(atCapError.empty(), "a connector name of exactly the maximum length is accepted");
    if (!atCapError.empty()) {
        std::fprintf(stderr, "  SetMonitorOutput refused a %zu-byte name with %s\n", atCap.size(),
                     atCapError.c_str());
    }
    const int atCapBinding = boundMonitor(getStatusOrExplain(proxy), atCap);
    check(atCapBinding == kMonitorOutputAtCap,
          "a connector name of exactly the maximum length is published for the monitor it was given to");
    if (atCapBinding != kMonitorOutputAtCap) {
        std::fprintf(stderr, "  the %zu-byte name resolves to monitor %d, not %d\n", atCap.size(), atCapBinding,
                     kMonitorOutputAtCap);
    }

    // GOAL: S10.4 -- a connector name maps to exactly one monitor: assigning a name another monitor
    //       holds moves it, and the old monitor answers to it no longer.
    // WHY-THIS-SHAPE: read as which monitor the name resolves to (name -> id cannot show a name held
    //       twice); the first binding is asserted before the move; the release is graded on the
    //       reply having arrived, since a lost reply lists nothing either.
    // CONTROL: --phase '^outputName$': (a) the displacement in WallpaperManager::bindOutputLocked
    //       (backend/daemon/wallpaper_manager.cpp) made `return BindResult::Unchanged;` -> "FAIL:
    //       reassigning a connector name moves it to the new monitor", "FAIL: the monitor a connector
    //       name was taken from answers to it no longer" and the cross-monitor check; (b)
    //       `status["outputs"] = std::move(outputs);` dropped from DbusService::getStatus -> both,
    //       the second "GetStatus published no outputs object".
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputA, "DP-MOVE"); }).empty(),
          "naming monitor A 'DP-MOVE' is accepted");
    check(boundMonitor(getStatusOrExplain(proxy), "DP-MOVE") == kMonitorOutputA,
          "a connector name resolves to the monitor it was just given to");
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputB, "DP-MOVE"); }).empty(),
          "reassigning 'DP-MOVE' to monitor B is accepted");
    // One snapshot read both ways: arrival and release are two halves of one state.
    const StatusReply afterMoveStatus = getStatusOrExplain(proxy);
    const int afterMove = boundMonitor(afterMoveStatus, "DP-MOVE");
    check(afterMove == kMonitorOutputB, "reassigning a connector name moves it to the new monitor");
    if (afterMove != kMonitorOutputB) {
        std::fprintf(stderr, "  'DP-MOVE' still resolves to monitor %d, not %d\n", afterMove, kMonitorOutputB);
    }
    const nlohmann::json* afterMoveBindings = outputBindings(afterMoveStatus);
    const std::vector<std::string> namesOnOldMonitor = namesFor(afterMoveStatus, kMonitorOutputA);
    check(afterMoveBindings != nullptr && namesOnOldMonitor.empty(),
          "the monitor a connector name was taken from answers to it no longer");
    if (afterMoveBindings == nullptr) {
        std::fprintf(stderr, "  GetStatus published no outputs object to read the old monitor from\n");
    } else if (!namesOnOldMonitor.empty()) {
        std::fprintf(stderr, "  monitor %d still answers to %s\n", kMonitorOutputA,
                     joinNames(namesOnOldMonitor).c_str());
    }

    // GOAL: S10.4's converse -- naming a monitor releases the name it answered to, so it answers
    //       to at most one.
    // WHY-THIS-SHAPE: the monitor's name count, since "DP-SECOND resolves to 15" holds whether or
    //       not DP-FIRST was released.
    // CONTROL: the release loop deleted from WallpaperManager::bindOutputLocked (backend/daemon/
    //       wallpaper_manager.cpp) -> "FAIL: naming a monitor releases the name it answered to
    //       before" and "FAIL: naming a monitor with the name it already has leaves exactly one
    //       binding".
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputA, "DP-FIRST"); }).empty(),
          "naming monitor A 'DP-FIRST' is accepted");
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputA, "DP-SECOND"); }).empty(),
          "renaming monitor A to 'DP-SECOND' is accepted");
    const std::vector<std::string> namesAfterRename = namesFor(getStatusOrExplain(proxy), kMonitorOutputA);
    const bool exactlyOne = namesAfterRename == std::vector<std::string>{"DP-SECOND"};
    check(exactlyOne, "naming a monitor releases the name it answered to before");
    if (!exactlyOne) {
        std::fprintf(stderr, "  monitor %d answers to %s\n", kMonitorOutputA, joinNames(namesAfterRename).c_str());
    }

    // Idempotent, and complete on return (no render behind it), so GetStatus is read at once.
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputA, "DP-SECOND"); }).empty(),
          "naming a monitor with the name it already has is not an error");
    check(namesFor(getStatusOrExplain(proxy), kMonitorOutputA) == std::vector<std::string>{"DP-SECOND"},
          "naming a monitor with the name it already has leaves exactly one binding");

    // GOAL: S10.8 -- a connector name is escaped wherever it is displayed, GetStatus included: no
    //       newline, right-to-left override or DEL reaches a display field raw.
    // WHY-THIS-SHAPE: the escaped key must resolve to its monitor, and separately no key may hold a
    //       raw byte class; read on the parsed keys, since JSON's encoder escapes bytes below 0x20 in
    //       the wire text by itself.
    // CONTROL: in DbusService::getStatus (backend/daemon/dbus_service.cpp), (a) the
    //       `sanitizeForLog(name, ...)` made `name` -> "FAIL: a hostile connector name is published by
    //       GetStatus in its escaped form, bound to its own monitor" and "FAIL: no connector name
    //       GetStatus publishes holds a raw newline, U+202E or DEL"; (b) `status["outputs"]` dropped
    //       -> the scan red, "saw no key at all for monitor 19".
    // COVERAGE-ASSERT: the fixture carries all three byte classes and is one sanitizeForLog alters;
    //       kDel dropped from it -> that line alone red.
    const std::string hostileName = std::string("DP-") + kBidiOverride + "\nHDMI" + kDel + "-1";
    const std::string escapedName = wallpaperx::sanitizeForLog(hostileName, wallpaperx::kMaxOutputNameBytes,
                                                              wallpaperx::LogFraming::Bare);
    check(hostileName.find(kBidiOverride) != std::string::npos && hostileName.find(kDel) != std::string::npos &&
              hostileName.find('\n') != std::string::npos && escapedName != hostileName,
          "the hostile connector name holds every byte class the scan looks for and is one sanitizeForLog alters");
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputHostile, hostileName); }).empty(),
          "the hostile connector name is accepted by SetMonitorOutput");
    const StatusReply hostileStatus = getStatusOrExplain(proxy);
    // The raw name: boundMonitor escapes what it is given, and escaping is not idempotent.
    const int hostileBinding = boundMonitor(hostileStatus, hostileName);
    check(hostileBinding == kMonitorOutputHostile,
          "a hostile connector name is published by GetStatus in its escaped form, bound to its own monitor");
    if (hostileBinding != kMonitorOutputHostile) {
        std::fprintf(stderr, "  the escaped name resolves to %d, not %d\n", hostileBinding, kMonitorOutputHostile);
    }
    // The hostile monitor's own key must be seen: an empty outputs object scans clean.
    const nlohmann::json* hostileBindings = outputBindings(hostileStatus);
    std::string rawKey;
    bool hostileKeyScanned = false;
    if (hostileBindings != nullptr) {
        for (const auto& [name, id] : hostileBindings->items()) {
            if (id.is_number_integer() && id.get<int>() == kMonitorOutputHostile) hostileKeyScanned = true;
            if (rawKey.empty() && (name.find(kBidiOverride) != std::string::npos ||
                                   name.find(kDel) != std::string::npos || name.find('\n') != std::string::npos)) {
                rawKey = name;
            }
        }
    }
    check(hostileKeyScanned && rawKey.empty(),
          "no connector name GetStatus publishes holds a raw newline, U+202E or DEL");
    if (!hostileKeyScanned) {
        std::fprintf(stderr, "  the scan saw no key at all for monitor %d, so it graded nothing\n",
                     kMonitorOutputHostile);
    }
    // Shown escaped, so the reported key cannot split or reorder this line.
    if (!rawKey.empty()) {
        std::fprintf(stderr, "  raw key published (shown escaped): '%s'\n",
                     wallpaperx::sanitizeForLog(rawKey, wallpaperx::kMaxOutputNameBytes,
                                                wallpaperx::LogFraming::Bare)
                         .c_str());
    }

    // GOAL: a client finds its own screen in GetStatus under the bytes it sent.
    // WHY-THIS-SHAPE: looked up in `outputs_literal` directly; boundMonitor() escapes first and would
    //       pass for a daemon publishing only the escaped map.
    // CONTROL: `status["outputs_literal"] = std::move(outputsLiteral);` dropped from
    //       DbusService::getStatus (backend/daemon/dbus_service.cpp) -> "FAIL: a connector name
    //       resolves in GetStatus under the exact bytes the client sent", alone.
    const nlohmann::json& hostileJson = hostileStatus.json();
    const bool literalIsObject = hostileJson.is_object() && hostileJson.contains("outputs_literal") &&
                                 hostileJson["outputs_literal"].is_object();
    const bool literalCarries = literalIsObject && hostileJson["outputs_literal"].contains(hostileName) &&
                                hostileJson["outputs_literal"][hostileName].is_number_integer();
    check(literalCarries && hostileJson["outputs_literal"][hostileName].get<int>() == kMonitorOutputHostile,
          "a connector name resolves in GetStatus under the exact bytes the client sent");
    if (!literalCarries) {
        std::fprintf(stderr, "  %s\n",
                     literalIsObject ? "outputs_literal came back without the name this phase assigned"
                                     : "no outputs_literal object came back");
    }

    // GOAL: naming one monitor leaves every other monitor's connector name intact.
    // WHY-THIS-SHAPE: once, at the end, over every monitor this phase named: each check above reads
    //       back only the monitor it named, so an over-erasing release loop is invisible to them.
    // CONTROL: the `if (it->second != monitorId) { ++it; continue; }` guard deleted from
    //       WallpaperManager::bindOutputLocked's release loop (backend/daemon/wallpaper_manager.cpp)
    //       -> "FAIL: naming one monitor leaves every other monitor's connector name intact", the
    //       only red line in the phase.
    // COVERAGE-ASSERT: the monitors compared are counted against the table (the loop's `continue`
    //       made `break` -> that line alone red), and the reply's arrival is part of `independent`.
    {
        const StatusReply finalNames = getStatusOrExplain(proxy);
        const std::pair<int32_t, std::string> stillHeld[] = {
            {kMonitorOutputA, "DP-SECOND"},
            {kMonitorOutputB, "DP-MOVE"},
            {kMonitorOutputRejected, "DP-KEEP"},
            {kMonitorOutputAtCap, atCap},
            {kMonitorOutputHostile, escapedName},
        };
        int compared = 0;
        bool independent = outputBindings(finalNames) != nullptr;
        for (const auto& [monitorId, expectedName] : stillHeld) {
            ++compared;
            const std::vector<std::string> held = namesFor(finalNames, monitorId);
            if (held == std::vector<std::string>{expectedName}) continue;
            independent = false;
            std::fprintf(stderr, "  monitor %d answers to %s, not to the one name it was last given\n", monitorId,
                         joinNames(held).c_str());
        }
        check(independent, "naming one monitor leaves every other monitor's connector name intact");
        check(compared == static_cast<int>(sizeof(stillHeld) / sizeof(stillHeld[0])),
              "every monitor this phase named was compared by the cross-monitor check");
    }
}

// Checks that render on the GPU and export a real frame.
void runRenderChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& imagePath,
                     const std::string& corruptPath, const std::string& unrenderablePath, const Check& check) {
    // Timed against this run's own render, not a fixed bound: with the render inside the handler
    // the reply took 186 ms on NVIDIA and 29 ms on Lavapipe, so no fixed bound separates the two.
    const auto requestedAt = std::chrono::steady_clock::now();
    setWallpaper(proxy, "static", imagePath, kMonitorStatic);
    const auto replyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - requestedAt).count();

    // CONTROL: backend/daemon/dbus_service.cpp:398, FrameReady emitted with `generation + 1`, control_batch
    //       2026-10-15, dbus_service_lavapipe '^render$' -> "FAIL: FrameReady(monitor=0, generation=1) arrives
    //       after the first render", with "FAIL: the monitor used for the failure case renders a frame first"
    //       and "FAIL: FrameReady arrives for the barrier request" (each wait failing); restored, green.
    check(signals.wait(kMonitorStatic, 1), "FrameReady(monitor=0, generation=1) arrives after the first render");
    const auto frameMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - requestedAt).count();

    std::printf("dbus_service_test: SetWallpaper replied in %lld ms, FrameReady at %lld ms\n",
                static_cast<long long>(replyMs), static_cast<long long>(frameMs));
    // CONTROL: backend/daemon/dbus_service.cpp, a 400 ms sleep at the start of DbusService::setWallpaper,
    //       control_batch 2026-10-15, dbus_service_lavapipe '^render$' -> "FAIL: SetWallpaper replies without
    //       waiting for the render to finish", alone; restored, green.
    //       Re-run 2026-10-20 (T9b), the line number it named having moved: the same line, alone; restored, green.
    check(replyMs <= 2 || replyMs * 4 < frameMs, "SetWallpaper replies without waiting for the render to finish");

    {
        const Frame frame = getFrame(proxy, kMonitorStatic);

        check(frame.fd.isValid(), "GetFrame returns a valid Unix fd after a static render");
        check(frame.width == kTargetWidth && frame.height == kTargetHeight,
              "GetFrame reports the target's dimensions");
        // CONTROL: GetFrame's reply in DbusService::sendFrame (backend/daemon/dbus_service.cpp) given
        //       `handle.generation + 1`, control_batch 2026-10-15, dbus_service_lavapipe '^render$' ->
        //       "FAIL: the exported frame carries the generation FrameReady announced" and "FAIL:
        //       frame_generation increments on re-render"; restored, green.
        check(frame.generation == 1, "the exported frame carries the generation FrameReady announced");
        // CONTROL: backend/daemon/dbus_service.cpp:807, DMA_BUF published as handle type 2, control_batch
        //       2026-10-15, dbus_service_lavapipe '^render$' -> "FAIL: handle_type is OPAQUE_FD (0) or DMA_BUF (1)"
        //       and "FAIL: layout metadata is only published on the DMA_BUF path"; restored, green.
        check(frame.handleType == 0 || frame.handleType == 1, "handle_type is OPAQUE_FD (0) or DMA_BUF (1)");

        // GOAL: S6.63, S6.64 -- a frame carries an acquire descriptor exactly when this daemon says
        //       it produces them, and it arrives as an open file the client owns.
        // WHY-THIS-SHAPE: gpu.frame_sync (set when the GPU came up) against sync_fds (this export's
        //       result), each driver grading its own mode; two hand-overs of one generation, since
        //       the plugin re-fetches on every scene change.
        // CONTROL: (a) exportFrameAcquireFd (renderer/vulkan/frame_export.cpp) made to `return -1;`
        //       -> "FAIL: sync_fds carries an acquire descriptor exactly when GetStatus says this
        //       daemon produces them" twice on the NVIDIA entries, Lavapipe green; (b) in
        //       WallpaperManager::exportHandle (backend/daemon/wallpaper_manager.cpp) only the first
        //       hand-over given an acquire -> the same line once, for the second; (c) DbusService::
        //       getStatus publishing frame_sync "yes"/"no" -> "FAIL: GetStatus names the frame
        //       hand-over mode as either \"semaphore\" or \"none\"".
        // COVERAGE-ASSERT: the mode is one of the two names, and two hand-overs were compared.
        const StatusReply syncModeReply = getStatusOrExplain(proxy);
        const nlohmann::json syncGpu = syncModeReply.json().contains("gpu") && syncModeReply.json()["gpu"].is_object()
                                           ? syncModeReply.json()["gpu"]
                                           : nlohmann::json::object();
        const std::string syncMode = syncGpu.value("frame_sync", std::string());
        check(syncMode == "semaphore" || syncMode == "none",
              "GetStatus names the frame hand-over mode as either \"semaphore\" or \"none\"");
        if (syncMode != "semaphore" && syncMode != "none") {
            std::fprintf(stderr, "  gpu.frame_sync was [%s]\n", syncMode.c_str());
        } else {
            const size_t expectedSyncFds = syncMode == "semaphore" ? 1 : 0;
            // Held together, so the second cannot be the first's descriptor handed back.
            const Frame refetched = getFrame(proxy, kMonitorStatic);
            int handOversCompared = 0;
            for (const Frame* served : {&frame, &refetched}) {
                ++handOversCompared;
                check(served->syncFds.size() == expectedSyncFds,
                      "sync_fds carries an acquire descriptor exactly when GetStatus says this daemon produces them");
                if (served->syncFds.size() != expectedSyncFds) {
                    std::fprintf(stderr, "  gpu.frame_sync=[%s] but this hand-over carried %zu sync fd(s)\n",
                                 syncMode.c_str(), served->syncFds.size());
                }
                size_t open = 0;
                for (const sdbus::UnixFd& syncFd : served->syncFds) {
                    if (syncFd.get() >= 0 && fcntl(syncFd.get(), F_GETFD) != -1) ++open;
                }
                check(open == served->syncFds.size(),
                      "every descriptor in sync_fds arrives as an open file the client now holds");
            }
            check(refetched.generation == frame.generation,
                  "re-fetching hands back the same generation, so the two hand-overs are of one frame");
            check(handOversCompared == 2, "both hand-overs of that generation were compared against the mode");
            std::printf("dbus_service_test: gpu.frame_sync=%s, sync_fds per hand-over=%zu\n", syncMode.c_str(),
                        frame.syncFds.size());
        }

        // The upper bound (no more than the fd holds) is graded by readServedBgra (served_pixels.h).
        // CONTROL: memory_size sent as `uint64_t{1}` in DbusService::sendFrame (backend/daemon/
        //       dbus_service.cpp), control_batch 2026-10-15, dbus_service_lavapipe '^render$' -> "FAIL:
        //       memory_size covers at least the frame's pixels", alone; restored, green.
        check(frame.memorySize >= static_cast<uint64_t>(kTargetWidth) * kTargetHeight * 4,
              "memory_size covers at least the frame's pixels");

        // The fourcc (channel order) reaches the wire on both handle types. A wrong mapping moves
        // both sides together, so frame_import_probe's sampled check grades that instead.
        // CONTROL: `frame.drmFormat = drmFormat;` in exportFrame made 0 -> "FAIL: every exported
        //       frame reports the target format's fourcc, whichever handle type it uses".
        check(frame.drmFormat == wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat),
              "every exported frame reports the target format's fourcc, whichever handle type it uses");

        // The layout fields come together or not at all: half of them builds a wrong image.
        const bool populated = frame.drmModifier != wallpaperx::kDrmModifierInvalid;
        if (populated) {
            check(frame.handleType == 1, "layout metadata is only published on the DMA_BUF path");
            check(!frame.planes.empty(), "a populated frame carries at least one plane");
            // CONTROL: backend/daemon/dbus_service.cpp:799, `plane.rowPitch / 2` put on the wire, control_batch
            //       2026-10-15, dbus_service_lavapipe '^render$' -> "FAIL: each plane's rowPitch covers a row of
            //       pixels", alone; restored, green.
            for (const auto& plane : frame.planes) {
                check(plane.get<1>() >= static_cast<uint64_t>(kTargetWidth) * 4,
                      "each plane's rowPitch covers a row of pixels");
            }
        } else {
            check(frame.planes.empty(), "an unpopulated frame reports no planes");
        }
        std::printf("dbus_service_test: handle_type=%d drm_format=0x%08x modifier=0x%llx planes=%zu\n",
                    frame.handleType, frame.drmFormat, static_cast<unsigned long long>(frame.drmModifier),
                    frame.planes.size());
    }

    // The GPU is up, so GetStatus names it and its descriptor tier.
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json& json = status.json();
        const nlohmann::json gpu = json.contains("gpu") && json["gpu"].is_object()
                                       ? json["gpu"]
                                       : nlohmann::json::object();
        const bool reported = gpu.value("initialized", false) && !gpu.value("name", std::string()).empty() &&
                              !gpu.value("descriptor_tier", std::string()).empty();
        // CONTROL: `gpuJson["name"] = "";` in DbusService's GetStatus (backend/daemon/dbus_service.cpp),
        //       control_batch 2026-10-15, dbus_service_lavapipe '^render$' -> "FAIL: GetStatus reports the
        //       GPU name and resolved descriptor tier once initialised", alone; restored, green.
        check(reported, "GetStatus reports the GPU name and resolved descriptor tier once initialised");
        if (gpu.value("initialized", false)) {
            std::printf("dbus_service_test: gpu=%s tier=%s\n", gpu.value("name", "?").c_str(),
                        gpu.value("descriptor_tier", "?").c_str());
        }

        // GOAL: S6.34 -- the Classic descriptor tier works end to end through the daemon.
        // WHY-THIS-SHAPE: dbus_service_classic forces the tier; without this line that entry could
        //       silently be a second run of the default tier.
        // CONTROL: the entry run with WALLPAPERX_FORCE_TIER=buffer -> "FAIL: WALLPAPERX_FORCE_TIER is
        //       honoured through the daemon (tier is Classic)".
        if (const char* forced = std::getenv("WALLPAPERX_FORCE_TIER");
            forced != nullptr && std::strcmp(forced, "classic") == 0) {
            const std::string tier = gpu.value("descriptor_tier", std::string());
            check(tier == "Classic", "WALLPAPERX_FORCE_TIER is honoured through the daemon (tier is Classic)");
            if (tier != "Classic") {
                std::fprintf(stderr, "  WALLPAPERX_FORCE_TIER=classic but the daemon reports tier '%s'\n",
                             tier.c_str());
            }
        }
    }

    // GOAL: S15.274 -- under the per-caller descriptor cap, a consumer that gives each frame back
    //       before asking again is served every time, and the daemon keeps no copy of any export.
    // WHY-THIS-SHAPE: the process fd count (service and client share it); the client's own fd is
    //       always open, since sdbus-c++ dups it. Each frame is released before the next fetch, so
    //       the cap (S15.265) does not shape the 32 exports measured; one warm-up first.
    // CONTROL: `sdbus::UnixFd memory(frame.fd, sdbus::adopt_fd);` made `sdbus::UnixFd memory(frame.fd);`
    //       in DbusService::sendFrame (backend/daemon/dbus_service.cpp) -> "FAIL: repeated GetFrame
    //       calls, each frame given back, do not leak descriptors" ("grew from 58 to 90").
    // COVERAGE-ASSERT: all 32 must be served and released.
    const auto releaseOrNote = [&proxy](const Frame& frame) {
        try {
            wallpaperx::testing::releaseFrame(proxy, kMonitorStatic, frame);
            return true;
        } catch (const sdbus::Error& e) {
            std::fprintf(stderr, "  ReleaseBuffer(%d, %llu) answered %s\n", kMonitorStatic,
                         static_cast<unsigned long long>(frame.bufferId), e.getName().c_str());
            return false;
        }
    };
    {
        const Frame warmup = getFrame(proxy, kMonitorStatic);
        (void)releaseOrNote(warmup);
    }
    const size_t fdsBefore = openFdCount();
    constexpr int kPollCount = 32;
    int polledAndReleased = 0;
    for (int i = 0; i < kPollCount; ++i) {
        Frame polled;
        try {
            polled = getFrame(proxy, kMonitorStatic);
        } catch (const sdbus::Error& e) {
            std::fprintf(stderr, "  poll %d of %d was refused: %s\n", i + 1, kPollCount, e.getName().c_str());
            break;
        }
        check(polled.fd.isValid(), "each polled frame carries a valid fd");
        if (!releaseOrNote(polled)) break;
        ++polledAndReleased;
    }
    const size_t fdsAfter = openFdCount();
    check(polledAndReleased == kPollCount, ("every poll was served and its frame given back (" +
                                            std::to_string(polledAndReleased) + " of " + std::to_string(kPollCount) +
                                            ")")
                                               .c_str());
    check(fdsAfter <= fdsBefore + 4, "repeated GetFrame calls, each frame given back, do not leak descriptors");
    if (fdsAfter > fdsBefore + 4) {
        std::fprintf(stderr, "  fd count grew from %zu to %zu across %d GetFrame calls\n", fdsBefore, fdsAfter,
                     polledAndReleased);
    }

    // A re-render runs the queue-family acquire paired with the exports' release (second frame on).
    setWallpaper(proxy, "static", imagePath, kMonitorStatic);
    check(signals.wait(kMonitorStatic, 2), "FrameReady(generation=2) arrives after a re-render");
    {
        const Frame frame = getFrame(proxy, kMonitorStatic);
        // CONTROL: backend/daemon/dbus_service.cpp:816, GetFrame replies `std::min<uint64_t>(handle.generation, 1)`,
        //       control_batch 2026-10-15, dbus_service_lavapipe '^render$' -> "FAIL: frame_generation increments
        //       on re-render", alone; restored, green.
        check(frame.generation == 2, "frame_generation increments on re-render");
        check(frame.fd.isValid(), "a re-rendered frame still exports a valid fd");
    }

    // --- Requests that must produce no frame and no signal. ---

    // A working wallpaper first, so a failed render has a frame it must not keep serving.
    setWallpaper(proxy, "static", imagePath, kMonitorCorrupt);
    check(signals.wait(kMonitorCorrupt, 1), "the monitor used for the failure case renders a frame first");
    check(errorNameFrom([&] { getFrame(proxy, kMonitorCorrupt); }).empty(),
          "the monitor used for the failure case starts with a valid frame");

    // Posted before a request that signals: on the one FIFO render thread, the barrier's FrameReady
    // proves these were processed (the corrupt render attempted, the shader request through).
    setWallpaper(proxy, "shader", unrenderablePath, kMonitorUnrenderable);
    setWallpaper(proxy, "static", corruptPath, kMonitorCorrupt);
    setWallpaper(proxy, "static", imagePath, kMonitorBarrier);
    check(signals.wait(kMonitorBarrier, 1), "FrameReady arrives for the barrier request");

    // CONTROL: two parts, either alone green: drawableType() (backend/daemon/wallpaper_manager.h)
    //       mapping `shader` to a still, and the recorded path swapped for the decodable image ->
    //       "FAIL: a shader request emits no FrameReady" beside the other unrenderable-state lines.
    check(signals.countFor(kMonitorUnrenderable) == 0, "a shader request emits no FrameReady");
    // Still 1: the good render above, and nothing for the corrupt one that followed.
    check(signals.countFor(kMonitorCorrupt) == 1, "a render that fails emits no FrameReady");

    // GOAL: S6.39 -- the frameless taxonomy has three names: NoFrame (retry), RenderFailed (stop,
    //       show why), Unsupported (stop asking).
    // WHY-THIS-SHAPE: three monitors driven into the three states and asserted together, so a
    //       daemon collapsing any pair fails on the state it lost; a lone `== NoFrame` passes for a
    //       daemon answering NoFrame to everything.
    // CONTROL: (a) the `reason == Unsupported` branch disabled in DbusService::completeFrame
    //       (backend/daemon/dbus_service.cpp) -> "FAIL: a wallpaper type this build cannot render is
    //       reported as Unsupported", the reply-text line and the unserviced phase's keying line;
    //       (b) exportCurrentFrame's `!isRenderableType(...)` branch disabled -> the same, NoFrame
    //       observed; (c) isRenderableType() accepting `shader` -> this line and the reply-text line.
    {
        const std::string neverSet = errorNameFrom([&] { getFrame(proxy, kMonitorNeverSet); });
        const std::string failed = errorNameFrom([&] { getFrame(proxy, kMonitorCorrupt); });
        const std::string unsupported = errorNameFrom([&] { getFrame(proxy, kMonitorUnrenderable); });
        std::printf("dbus_service_test: frameless taxonomy -- never set: '%s', failed render: '%s', "
                    "unrenderable type: '%s'\n",
                    neverSet.c_str(), failed.c_str(), unsupported.c_str());

        check(neverSet == kErrNoFrame, "a monitor that was never set is reported as NoFrame");
        check(failed == kErrRenderFailed,
              "a failed render is reported as RenderFailed, distinctly from a monitor with nothing rendered yet");
        check(unsupported == kErrUnsupported,
              "a wallpaper type this build cannot render is reported as Unsupported, distinctly from both");

        // NoFrame's "call SetWallpaper first" text here would send the client round the same loop.
        const std::string unsupportedText = errorMessageFrom([&] { getFrame(proxy, kMonitorUnrenderable); });
        check(unsupportedText.find("call SetWallpaper first") == std::string::npos,
              "the Unsupported reply does not tell the client to call SetWallpaper first");
        check(unsupportedText.find("no frame will ever be produced") != std::string::npos,
              "the Unsupported reply says the wait is pointless rather than merely that there is no frame");
        // GOAL: S15.326 -- the Unsupported reply names the type the monitor holds, so a client can
        //       say which kind of wallpaper this build cannot draw.
        // WHY-THIS-SHAPE: matched with its quotes and the words around it: a bare "shader" would also
        //       match a future text that mentions shaders without naming what the monitor holds.
        // CONTROL: in DbusService::completeFrame (backend/daemon/dbus_service.cpp) the
        //       `sanitizeForLog(exported.type, 64) + " wallpaper` part dropped -> "FAIL: the Unsupported
        //       reply names the type it cannot render (...)".
        check(unsupportedText.find("holds a \"shader\" wallpaper") != std::string::npos,
              ("the Unsupported reply names the type it cannot render (" + unsupportedText + ")").c_str());
    }

    // GOAL: S6.14 -- the RenderFailed reply carries the render's own error text.
    // WHY-THIS-SHAPE: compared with GetStatus's last_error, not a literal, so a canned string fails
    //       and a reworded renderer message does not.
    // CONTROL: `+ exported.lastError` dropped from the RenderFailed reply in DbusService::completeFrame
    //       (backend/daemon/dbus_service.cpp) -> "FAIL: the RenderFailed reply carries the same error
    //       text GetStatus reports".
    {
        const std::string replyText = errorMessageFrom([&] { getFrame(proxy, kMonitorCorrupt); });
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* corrupt = wallpaperStatus(status, std::to_string(kMonitorCorrupt));
        const std::string statusText = corrupt == nullptr ? std::string() : corrupt->value("last_error", std::string());
        check(!statusText.empty() && replyText.find(statusText) != std::string::npos,
              "the RenderFailed reply carries the same error text GetStatus reports");
        if (statusText.empty() || replyText.find(statusText) == std::string::npos) {
            std::fprintf(stderr, "  reply said '%s' but GetStatus reports last_error '%s'\n", replyText.c_str(),
                         statusText.c_str());
        }
    }

    // SetWallpaper had replied before the render failed, so GetStatus is where it is reported.
    // CONTROL: `entry["last_error"] = monitor.lastError;` removed from GetStatus (backend/daemon/
    //       dbus_service.cpp), control_batch 2026-10-15, dbus_service_lavapipe '^render$' -> "FAIL:
    //       GetStatus reports last_error for an asynchronous render failure" and S6.14's reply-text line;
    //       restored, green.
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* corrupt = wallpaperStatus(status, std::to_string(kMonitorCorrupt));
        const bool reported = corrupt != nullptr && !corrupt->value("last_error", std::string()).empty();
        check(reported, "GetStatus reports last_error for an asynchronous render failure");
    }

    // A monitor that rendered successfully must not carry a stale error.
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* still = wallpaperStatus(status, std::to_string(kMonitorStatic));
        const nlohmann::json* unrenderable = wallpaperStatus(status, std::to_string(kMonitorUnrenderable));
        const bool reflected =
            still != nullptr && unrenderable != nullptr && still->value("type", std::string()) == "static" &&
            still->value("path", std::string()) == imagePath && still->value("has_frame", false) &&
            still->value("generation", uint64_t{0}) == 2 && still->value("width", uint32_t{0}) == kTargetWidth &&
            still->value("height", uint32_t{0}) == kTargetHeight && !still->contains("last_error") &&
            unrenderable->value("type", std::string()) == "shader" && !unrenderable->value("has_frame", true);
        // CONTROL: `{"type", monitor.type}` in DbusService::getStatus (backend/daemon/
        //       dbus_service.cpp) made "static" -> "FAIL: GetStatus reflects per-monitor type, path,
        //       size, generation and frame availability".
        check(reflected, "GetStatus reflects per-monitor type, path, size, generation and frame availability");
    }

    // GOAL: S6.39's second half -- GetStatus carries an always-present `unsupported` boolean, since
    //       has_frame=false without last_error is also a render in flight.
    // WHY-THIS-SHAPE: a pair (true on the shader monitor, false on the rendered one) and presence on
    //       both, so a flag set everywhere or a missing key read as false cannot pass; last_error is
    //       absent on the shader monitor, since an accepted wallpaper has not failed.
    // CONTROL: `entry["unsupported"] = !isRenderableType(monitor.type);` in DbusService::getStatus
    //       (backend/daemon/dbus_service.cpp) made `false` -> only "FAIL: GetStatus marks a wallpaper
    //       type this build cannot render as unsupported..."; made `true` -> only the rendered-monitor
    //       line.
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* unrenderable = wallpaperStatus(status, std::to_string(kMonitorUnrenderable));
        const nlohmann::json* rendered = wallpaperStatus(status, std::to_string(kMonitorBarrier));
        auto flag = [](const nlohmann::json* entry) {
            if (entry == nullptr) return std::string("<no such monitor>");
            if (!entry->contains("unsupported")) return std::string("<key absent>");
            return std::string(entry->value("unsupported", false) ? "true" : "false");
        };
        std::printf("dbus_service_test: GetStatus unsupported -- shader monitor: %s, rendered monitor: %s\n",
                    flag(unrenderable).c_str(), flag(rendered).c_str());

        check(unrenderable != nullptr && rendered != nullptr && unrenderable->contains("unsupported") &&
                  rendered->contains("unsupported"),
              "GetStatus emits the unsupported key for every monitor, not only for the unsupported ones");
        check(unrenderable != nullptr && unrenderable->value("unsupported", false) &&
                  !unrenderable->contains("last_error"),
              "GetStatus marks a wallpaper type this build cannot render as unsupported, without also "
              "claiming the request failed");
        check(rendered != nullptr && rendered->value("has_frame", false) && !rendered->contains("last_error") &&
                  !rendered->value("unsupported", true),
              "a monitor that rendered successfully carries no last_error and is not marked unsupported (so "
              "both fields still mean something)");
    }
}

// A request recorded and not yet rendered. Over the bus an export never overtakes the render of
// the request before it (one FIFO for both), so the request is recorded with
// WallpaperManager::requestWallpaper directly, which queues no render; everything else crosses
// the real wire. Its own manager and service on a unique name; it never builds a Device.
void runUnservicedRequestChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                                const std::string& unrenderablePath, const Check& check) {
    wallpaperx::WallpaperManager manager(config);

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();

    auto proxyOwner = sdbus::createProxy(sdbus::ServiceName{serviceConn->getUniqueName()},
                                         sdbus::ObjectPath{DbusService::kObjectPath});
    sdbus::IProxy& proxy = *proxyOwner;

    // Recorded, not queued; the premise is asserted below.
    manager.requestWallpaper("static", imagePath, kMonitorUnservicedStatic);
    manager.requestWallpaper("shader", unrenderablePath, kMonitorUnservicedUnrenderable);

    // GOAL: S6.7 -- a recorded-but-unrendered monitor is NoFrame, told apart from a never-set one by
    //       its message ("in flight" against "call SetWallpaper first").
    // WHY-THIS-SHAPE: both directions, on the message, since both are NoFrame by design; racing a
    //       real render instead would assert on the scheduler.
    // CONTROL: NoFrameReason::NotRendered folded into NeverRequested in
    //       WallpaperManager::exportCurrentFrame -> "FAIL: a recorded-but-unrendered monitor is not
    //       told to call SetWallpaper first", the never-set line green.
    const std::string pendingName = errorNameFrom([&] { getFrame(proxy, kMonitorUnservicedStatic); });
    check(pendingName == kErrNoFrame,
          "a recorded-but-unrendered static monitor is reported as NoFrame, not as a failure");
    const std::string pendingText = errorMessageFrom([&] { getFrame(proxy, kMonitorUnservicedStatic); });
    check(pendingText.find("call SetWallpaper first") == std::string::npos,
          "a recorded-but-unrendered monitor is not told to call SetWallpaper first");
    check(errorMessageFrom([&] { getFrame(proxy, kMonitorUnservicedNeverSet); }).find("call SetWallpaper first") !=
              std::string::npos,
          "a monitor that was never set is told to call SetWallpaper first");
    if (pendingName != kErrNoFrame) {
        std::fprintf(stderr, "  the unserviced monitor answered '%s': '%s'\n", pendingName.c_str(),
                     pendingText.c_str());
    }

    // GOAL: S6.39's keying -- Unsupported is decided from the recorded type, not from whether the
    //       worker has serviced the request.
    // WHY-THIS-SHAPE: the only place it shows: here nothing is serviced, beside the static monitor
    //       that must answer NoFrame at the same instant.
    // CONTROL: exportCurrentFrame's `if (!isRenderableType(it->second.type))` also requiring
    //       `servicedSeq == requestSeq` -> "FAIL: an unrenderable type is Unsupported from the moment
    //       it is recorded, before any worker has seen it", the only red line in the run;
    //       isRenderableType() accepting `shader` -> this line too.
    const std::string unservicedUnrenderable =
        errorNameFrom([&] { getFrame(proxy, kMonitorUnservicedUnrenderable); });
    check(unservicedUnrenderable == kErrUnsupported,
          "an unrenderable type is Unsupported from the moment it is recorded, before any worker has seen it");
    if (unservicedUnrenderable != kErrUnsupported) {
        std::fprintf(stderr, "  an unserviced shader monitor answered '%s'\n", unservicedUnrenderable.c_str());
    }

    // GOAL: the premise of both checks above -- both monitors are recorded and frameless.
    // WHY-THIS-SHAPE: both monitors, so routing one request through setWallpaper cannot leave the
    //       keying check a duplicate of the bus tier's. NOT COVERED: a serviced shader request leaves
    //       nothing on the wire (no frame, generation or lastError), so "the worker passed over it" is
    //       ruled out only by this phase never calling setWallpaper.
    // CONTROL: (a) the static `manager.requestWallpaper` replaced by setWallpaper plus a 3 s settle ->
    //       "a recorded-but-unrendered static monitor is reported as NoFrame" and "the unserviced static
    //       monitor really is unserviced..." (the same for the shader request: nothing red, the gap
    //       above); (b) `{"type", monitor.type}` in DbusService::getStatus (backend/daemon/
    //       dbus_service.cpp) made "static" -> "FAIL: the unserviced shader monitor is the request this
    //       phase recorded, and it produced no frame" and the per-monitor GetStatus line.
    const StatusReply status = getStatusOrExplain(proxy);
    const nlohmann::json* pending = wallpaperStatus(status, std::to_string(kMonitorUnservicedStatic));
    check(pending != nullptr && !pending->value("has_frame", true) && !pending->contains("last_error"),
          "the unserviced static monitor really is unserviced: recorded, no frame, and no error");
    const nlohmann::json* pendingUnrenderable =
        wallpaperStatus(status, std::to_string(kMonitorUnservicedUnrenderable));
    check(pendingUnrenderable != nullptr && pendingUnrenderable->value("type", std::string()) == "shader" &&
              pendingUnrenderable->value("path", std::string()) == unrenderablePath &&
              !pendingUnrenderable->value("has_frame", true) &&
              pendingUnrenderable->value("generation", uint64_t{1}) == 0,
          "the unserviced shader monitor is the request this phase recorded, and it produced no frame");
}

// --------------------------------------------------------------------------- //
// Step 15 chunk 3b -- a wallpaper's properties on the wire: what is refused, what replaces what, and
// what GetStatus shows. Shader wallpapers, which this build records and never draws. Driven on a
// manager of its own whose config file is `configFile`; runPropertiesWireChecks below builds both.
// --------------------------------------------------------------------------- //
constexpr int32_t kMonitorPropsHeld = 1;      // holds a set every refusal is compared against
constexpr int32_t kMonitorPropsFresh = 2;     // never set: a refused SetWallpaperWithProperties must not create it
constexpr int32_t kMonitorPropsPlain = 3;     // set with SetWallpaper, so it has no properties
constexpr int32_t kMonitorPropsLimits = 4;    // set at both limits at once
constexpr int32_t kMonitorPropsCleared = 5;   // set with properties, then with SetWallpaper
constexpr int32_t kMonitorPropsCap = 6;       // set at each of the three speeds

// One monitor's row in a config file's text, or null. Given the text configTextOnceIdle() returned,
// so a write that failed (empty text) reads as no row rather than as whatever an earlier write left.
nlohmann::json configRowOf(const std::string& text, int32_t monitorId) {
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("monitors") || !root["monitors"].is_array()) {
        return nullptr;
    }
    for (const nlohmann::json& row : root["monitors"]) {
        if (row.is_object() && row.value("monitor_id", -1) == monitorId) return row;
    }
    return nullptr;
}

// The config file's text once the writer holds nothing queued or in flight (persistence Pending);
// empty, with the reason, if it never went idle in 10 s or went idle on a failed write, so "nothing
// was written" is never read off a write that failed.
// CONTROL: ConfigStore::save() (backend/daemon/config_store.cpp) failing once the file exists ->
//       "went idle on a write that did not succeed" twice and "FAIL: no refused call wrote the config
//       file"; with the Ok test below also removed, that check stays green on a stale file.
std::string configTextOnceIdle(const wallpaperx::WallpaperManager& manager, const std::filesystem::path& file) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (manager.snapshot().health.persistence.status == wallpaperx::HealthStatus::Pending) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "  the config writer never went idle: %s\n",
                         manager.snapshot().health.persistence.detail.c_str());
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const wallpaperx::HealthCheck persistence = manager.snapshot().health.persistence;
    if (persistence.status != wallpaperx::HealthStatus::Ok) {
        std::fprintf(stderr, "  the config writer went idle on a write that did not succeed (%s: %s)\n",
                     wallpaperx::toString(persistence.status), persistence.detail.c_str());
        return {};
    }
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void runPropertiesWireChecksOn(wallpaperx::WallpaperManager& manager, const std::filesystem::path& configFile,
                               const std::string& imagePath, const std::string& unrenderablePath, const Check& check) {
    using wallpaperx::testing::setWallpaperProperties;
    using wallpaperx::testing::setWallpaperWithProperties;
    using wallpaperx::testing::WireProperties;
    using sdbus::Variant;
    constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    auto proxyOwner = sdbus::createProxy(sdbus::ServiceName{serviceConn->getUniqueName()},
                                         sdbus::ObjectPath{DbusService::kObjectPath});
    sdbus::IProxy& proxy = *proxyOwner;

    // A monitor's GetStatus entry, or null when it has none.
    const auto entryOf = [&proxy](int32_t monitorId) -> nlohmann::json {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
        return entry != nullptr ? *entry : nlohmann::json(nullptr);
    };
    // Absence proved only from a GetStatus that answered: entryOf()'s null also stands for a failed read,
    // which would pass an absence check.
    const auto absent = [&proxy](int32_t monitorId) {
        const StatusReply status = getStatusOrExplain(proxy);
        return status.ok() && wallpaperStatus(status, std::to_string(monitorId)) == nullptr;
    };
    const auto withProperties =[&](int32_t monitorId, const WireProperties& properties) {
        return errorNameFrom(
            [&] { setWallpaperWithProperties(proxy, "shader", unrenderablePath, monitorId, properties); });
    };
    const auto replaceProperties = [&](int32_t monitorId, const WireProperties& properties) {
        return errorNameFrom([&] { setWallpaperProperties(proxy, monitorId, properties); });
    };

    const WireProperties held{{"speed", Variant(0.5)},
                              {"mute", Variant(true)},
                              {"tint", Variant(std::string("#FF8800"))},
                              {"wave.height-2_x", Variant(0.25)}};
    const nlohmann::json heldPublished = {{"speed", 0.5}, {"mute", true}, {"tint", "#ff8800"}, {"wave.height-2_x", 0.25}};
    const std::string heldError = withProperties(kMonitorPropsHeld, held);
    const std::string plainError =
        errorNameFrom([&] { setWallpaper(proxy, "shader", unrenderablePath, kMonitorPropsPlain); });
    check(heldError.empty() && plainError.empty(), "a wallpaper can be set with properties, and without");
    if (!heldError.empty() || !plainError.empty()) {
        std::fprintf(stderr, "  SetWallpaperWithProperties answered '%s', SetWallpaper '%s'\n", heldError.c_str(),
                     plainError.c_str());
        return;
    }

    // GOAL: S15.104 -- every wallpaper GetStatus lists carries `properties`, an object: empty for one
    //       set with none, and otherwise the values AS SET -- a colour sent in capitals comes back in
    //       the lower case the daemon stores; bools and numbers keep their JSON types.
    // WHY-THIS-SHAPE: the whole object is compared, so a missing, extra or retyped entry fails it;
    //       the empty case is asserted to be an OBJECT, which a field left out would not be.
    // CONTROL: (a) `entry["properties"] = ...` in DbusService::getStatus (backend/daemon/
    //       dbus_service.cpp) written only for a non-empty set -> "FAIL: a wallpaper set without
    //       properties is listed in GetStatus with an empty properties object" and the two later
    //       emptied-set checks.
    {
        const nlohmann::json plain = entryOf(kMonitorPropsPlain);
        const bool emptyObject = plain.is_object() && plain.contains("properties") && plain["properties"].is_object() &&
                                 plain["properties"].empty();
        check(emptyObject, "a wallpaper set without properties is listed in GetStatus with an empty properties object");
        if (!emptyObject) std::fprintf(stderr, "  GetStatus entry: %s\n", plain.dump().c_str());
        const nlohmann::json heldEntry = entryOf(kMonitorPropsHeld);
        const bool asSet = heldEntry.is_object() && heldEntry.value("properties", nlohmann::json()) == heldPublished;
        check(asSet, "a wallpaper set with properties is listed with them as set");
        if (!asSet) {
            std::fprintf(stderr, "  expected %s, GetStatus entry: %s\n", heldPublished.dump().c_str(),
                         heldEntry.dump().c_str());
        }
    }

    // GOAL: S15.663 -- a dictionary naming one key twice is refused InvalidArgs by both methods, and
    //       nothing of it is applied: the monitor keeps its wallpaper and its set.
    // WHY-THIS-SHAPE: sent as a vector of entries, since the map the other calls use cannot carry a key
    //       twice; each rides beside "mute": false, which would change the held set if the rest applied.
    // CONTROL: control_batch.sh, re-run 2026-10-17, phase propertiesWire, fromWire's single repeat test made
    //       `if (false)` (backend/daemon/dbus_service.cpp) -> "FAIL: a property key given twice is refused
    //       InvalidArgs by both methods and nothing is applied", beside S15.687's line. Reverted.
    {
        using Entries = std::vector<sdbus::DictEntry<std::string, Variant>>;
        const std::vector<Entries> twice{
            {{"mute", Variant(false)}, {"speed", Variant(2.0)}, {"speed", Variant(true)}},
            {{"mute", Variant(false)}, {"speed", Variant(1.0)}, {"speed", Variant(1.0)}},
        };
        const nlohmann::json before = entryOf(kMonitorPropsHeld);
        bool refused = before.is_object();
        for (const Entries& entries : twice) {
            const std::string viaReplace = errorNameFrom([&] {
                proxy.callMethod("SetWallpaperProperties").onInterface(DbusService::kInterface).withArguments(kMonitorPropsHeld, entries);
            });
            const std::string viaSet = errorNameFrom([&] {
                proxy.callMethod("SetWallpaperWithProperties").onInterface(DbusService::kInterface)
                    .withArguments(std::string("static"), imagePath, kMonitorPropsHeld, entries);
            });
            if (viaReplace != kInvalidArgs || viaSet != kInvalidArgs) {
                refused = false;
                std::fprintf(stderr, "  answered '%s' and '%s'\n", viaReplace.c_str(), viaSet.c_str());
            }
        }
        const nlohmann::json after = entryOf(kMonitorPropsHeld);
        check(refused && after == before, "a property key given twice is refused InvalidArgs by both methods and nothing is applied");
        if (after != before) std::fprintf(stderr, "  entry %s -> %s\n", before.dump().c_str(), after.dump().c_str());

        // GOAL: S15.687 -- a key repeated with a malformed second value (a wrong D-Bus type, a string that
        //       is not a colour) is refused as a repeat, by both methods, not as a bad value.
        // WHY-THIS-SHAPE: the message is read, not only the error name: both refusals are InvalidArgs, so
        //       the name cannot tell which check fired.
        // CONTROL: control_batch.sh 2026-10-17, phase propertiesWire, in fromWire (backend/daemon/dbus_service.cpp):
        //       the repeat test narrowed to `properties.contains(key) && value.containsValueOfType<double>()` ->
        //       "FAIL: a repeated key whose second value is malformed is refused as a repeat by both methods"
        //       ("refused with 'property "speed" has D-Bus type "i"; ...'"), beside S15.663's line. Reverted.
        const std::vector<Entries> badSecond{
            {{"speed", Variant(1.0)}, {"speed", Variant(int32_t{3})}},
            {{"tint", Variant(std::string("#ff0000"))}, {"tint", Variant(std::string("#12345g"))}},
        };
        int asRepeat = 0;
        for (const Entries& entries : badSecond) {
            const std::string viaReplace = errorMessageFrom([&] {
                proxy.callMethod("SetWallpaperProperties").onInterface(DbusService::kInterface).withArguments(kMonitorPropsHeld, entries);
            });
            const std::string viaSet = errorMessageFrom([&] {
                proxy.callMethod("SetWallpaperWithProperties").onInterface(DbusService::kInterface)
                    .withArguments(std::string("static"), imagePath, kMonitorPropsHeld, entries);
            });
            for (const std::string& message : {viaReplace, viaSet}) {
                if (message.find("given more than once") != std::string::npos) {
                    ++asRepeat;
                } else {
                    std::fprintf(stderr, "  refused with '%s'\n", message.c_str());
                }
            }
        }
        check(asRepeat == 4, "a repeated key whose second value is malformed is refused as a repeat by both methods");
    }

    // GOAL: S15.97's limits -- 64 entries whose keys are 64 characters are accepted and kept whole,
    //       so every refusal below is of its bad entry.
    // CONTROL: `key.size() > kMaxPropertyKeyBytes` in propertyError (backend/daemon/
    //       wallpaper_properties.cpp) made `>=` -> "FAIL: a call with 64 entries whose keys are 64
    //       characters is accepted and kept whole".
    {
        WireProperties atLimits;
        for (int i = 0; i < 64; ++i) {
            std::string key = "k" + std::to_string(i);
            key.resize(64, 'x');
            atLimits.emplace(key, Variant(static_cast<double>(i)));
        }
        const std::string limitsError = withProperties(kMonitorPropsLimits, atLimits);
        const nlohmann::json entry = entryOf(kMonitorPropsLimits);
        const bool kept = limitsError.empty() && entry.is_object() && entry.contains("properties") &&
                          entry["properties"].size() == 64;
        check(kept, "a call with 64 entries whose keys are 64 characters is accepted and kept whole");
        if (!kept) std::fprintf(stderr, "  it answered '%s'; entry: %s\n", limitsError.c_str(), entry.dump().c_str());
    }

    // GOAL: S15.97 -- a call with any one bad entry is refused whole as InvalidArgs, by both methods,
    //       and nothing of it is applied: the monitor keeps the wallpaper and the set it had, and a
    //       refused SetWallpaperWithProperties on a monitor never set leaves it unset.
    //       S15.105: a speed other than 0.5, 1 or 2 is one of these, and the config file is not written.
    // WHY-THIS-SHAPE: every bad case rides beside a good entry that would change the held set (speed
    //       2; or mute false), so dropping the bad entry and applying the rest shows;
    //       the file is compared byte for byte across the table once the writer is idle.
    // CONTROL: each red on its own cases' lines (both methods), beside "no refused call wrote the
    //       config file" and the held-set line. In fromWire (backend/daemon/dbus_service.cpp), re-run
    //       2026-10-17 via control_batch.sh: the unexpected-type throw made `continue` (types i and as); the
    //       non-colour throw likewise. In
    //       propertyError (backend/daemon/wallpaper_properties.cpp): keyCharacter() accepting ' ', or any
    //       byte past 0x7f; the isfinite() test, the speed rule,
    //       the mute rule, `key.empty() ||`, or the 65-character test deleted. The count bound deleted
    //       from both fromWire and propertiesError -> the "65 entries" lines (fromWire's alone stays
    //       green: the wire bound has no check of its own). propertiesError() deleted from
    //       WallpaperManager::requestWallpaper -> its 16 lines, the error-text check, "creates no monitor",
    //       the file, held-set and replace checks; from WallpaperManager::setWallpaperProperties -> its 16
    //       lines, the error-text, file and held-set checks (re-run via control_batch.sh, --phase
    //       '^propertiesWire$', 2026-10-14, each alone, reverted). Since 2026-10-14:
    //       `!std::ranges::contains(kPlaybackSpeeds, *number)` in propertyError made `*number <= 0` via
    //       control_batch.sh, --phase '^propertiesWire$', 2026-10-14 -> the "speed 0.25", "0.75", "1.5", "2.0001"
    //       and "3" lines of both methods, the error-text check below, "no refused call wrote the config
    //       file", the held-set line and the replace check after it. Reverted.
    // COVERAGE-ASSERT: the number of refusals driven is asserted.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const Variant good(2.0);
    WireProperties tooMany{{"speed", good}};
    for (int i = 0; i < 64; ++i) tooMany.emplace("k" + std::to_string(i), Variant(1.0));
    const std::vector<std::pair<const char*, WireProperties>> refusals{
        {"a value of D-Bus type i", {{"level", Variant(int32_t{3})}, {"speed", good}}},
        {"a value of D-Bus type as", {{"list", Variant(std::vector<std::string>{"a"})}, {"speed", good}}},
        {"a string that is not #rrggbb", {{"tint", Variant(std::string("#12345g"))}, {"speed", good}}},
        {"65 entries", tooMany},
        {"an empty key", {{"", Variant(1.0)}, {"speed", good}}},
        {"a 65-character key", {{std::string(65, 'k'), Variant(1.0)}, {"speed", good}}},
        {"a key with a space", {{"wave height", Variant(1.0)}, {"speed", good}}},
        {"a key outside ASCII", {{"t\xC3\xA9", Variant(1.0)}, {"speed", good}}},
        {"a NaN", {{"level", Variant(nan)}, {"speed", good}}},
        {"an infinity", {{"level", Variant(inf)}, {"speed", good}}},
        {"speed 0", {{"speed", Variant(0.0)}, {"mute", Variant(false)}}},
        {"a negative speed", {{"speed", Variant(-1.0)}, {"mute", Variant(false)}}},
        {"speed as a bool", {{"speed", Variant(true)}, {"mute", Variant(false)}}},
        {"speed as a colour", {{"speed", Variant(std::string("#000000"))}, {"mute", Variant(false)}}},
        {"speed 2.0001", {{"speed", Variant(2.0001)}, {"mute", Variant(false)}}},
        {"speed 3", {{"speed", Variant(3.0)}, {"mute", Variant(false)}}},
        {"speed 0.25", {{"speed", Variant(0.25)}, {"mute", Variant(false)}}},
        {"speed 0.75", {{"speed", Variant(0.75)}, {"mute", Variant(false)}}},
        {"speed 1.5", {{"speed", Variant(1.5)}, {"mute", Variant(false)}}},
        {"mute as a number", {{"mute", Variant(0.0)}, {"speed", good}}},
    };
    const std::string fileBefore = configTextOnceIdle(manager, configFile);
    int driven = 0;
    for (const auto& [what, properties] : refusals) {
        const std::string viaSet = errorNameFrom(
            [&] { setWallpaperWithProperties(proxy, "static", imagePath, kMonitorPropsHeld, properties); });
        const std::string viaReplace = replaceProperties(kMonitorPropsHeld, properties);
        check(viaSet == kInvalidArgs,
              (std::string("SetWallpaperWithProperties with ") + what + " is refused as InvalidArgs").c_str());
        check(viaReplace == kInvalidArgs,
              (std::string("SetWallpaperProperties with ") + what + " is refused as InvalidArgs").c_str());
        if (viaSet != kInvalidArgs || viaReplace != kInvalidArgs) {
            std::fprintf(stderr, "  %s: SetWallpaperWithProperties answered '%s', SetWallpaperProperties '%s'\n", what,
                         viaSet.c_str(), viaReplace.c_str());
        }
        driven += 2;
    }
    check(driven == 2 * 20, "every refusal case in the table was driven through both methods");

    // GOAL: S15.105 -- a speed refusal tells the caller the rule: the D-Bus error text of both methods names
    //       "property speed must be 0.5, 1 or 2" for 0.25, 0.75, 1.5, 3, 0 and a bool.
    // WHY-THIS-SHAPE: the error NAME above is InvalidArgs for every rule, so it cannot tell which one fired;
    //       only the text says the refusal was the speed rule and that it lists the three choices.
    // CONTROL: the sentence in propertyError (backend/daemon/wallpaper_properties.cpp) made "property speed
    //       is out of range" via control_batch.sh, --phase '^propertiesWire$', 2026-10-14 -> "FAIL: a refused
    //       speed's error text names the three speeds, by both methods", alone. Reverted.
    // COVERAGE-ASSERT: the number of texts read is asserted.
    {
        const std::string sentence = "property speed must be 0.5, 1 or 2";
        int read = 0;
        bool named = true;
        for (const auto& [what, speed] : std::vector<std::pair<const char*, Variant>>{
                 {"0.25", Variant(0.25)}, {"0.75", Variant(0.75)}, {"1.5", Variant(1.5)},
                 {"3", Variant(3.0)},     {"0", Variant(0.0)},     {"a bool", Variant(true)}}) {
            const std::string viaSet = errorMessageFrom([&] {
                setWallpaperWithProperties(proxy, "static", imagePath, kMonitorPropsHeld, {{"speed", speed}});
            });
            const std::string viaReplace =
                errorMessageFrom([&] { setWallpaperProperties(proxy, kMonitorPropsHeld, {{"speed", speed}}); });
            read += 2;
            if (viaSet.find(sentence) == std::string::npos || viaReplace.find(sentence) == std::string::npos) {
                named = false;
                std::fprintf(stderr, "  speed %s: '%s' / '%s'\n", what, viaSet.c_str(), viaReplace.c_str());
            }
        }
        check(named && read == 12, "a refused speed's error text names the three speeds, by both methods");
    }
    // A case the manager decides, not the wire: a wire refusal never reaches the code that would create it.
    const std::string freshError = withProperties(kMonitorPropsFresh, {{"speed", Variant(0.0)}});
    check(freshError == kInvalidArgs && absent(kMonitorPropsFresh), "a refused SetWallpaperWithProperties creates no monitor");
    {
        const std::string fileAfter = configTextOnceIdle(manager, configFile);
        const bool unwritten = !fileBefore.empty() && fileAfter == fileBefore;
        check(unwritten, "no refused call wrote the config file");
        if (!unwritten) std::fprintf(stderr, "  before: %s\n  after:  %s\n", fileBefore.c_str(), fileAfter.c_str());
    }
    {
        const nlohmann::json after = entryOf(kMonitorPropsHeld);
        const bool untouched = after.is_object() && after.value("type", std::string()) == "shader" &&
                               after.value("path", std::string()) == unrenderablePath &&
                               after.value("properties", nlohmann::json()) == heldPublished;
        check(untouched, "after every refusal the held monitor keeps its wallpaper and its set, and nothing of it was applied");
        if (!untouched) std::fprintf(stderr, "  GetStatus entry: %s\n", after.dump().c_str());
    }

    // GOAL: S15.97's last clause -- SetWallpaperProperties on a monitor that holds no wallpaper, or on
    //       an id outside [0, 32), is refused as InvalidArgs and sets nothing up.
    // WHY-THIS-SHAPE: absence through `absent` (a failed GetStatus cannot satisfy it); the range
    //       refusal told apart by "monitor id must be", since ids 32 and -1 also hold no wallpaper.
    // CONTROL: in WallpaperManager::setWallpaperProperties (backend/daemon/wallpaper_manager.cpp), (a)
    //       the `holds no wallpaper` throw made `return;` -> "FAIL: SetWallpaperProperties on a monitor
    //       holding no wallpaper is refused as InvalidArgs, and creates nothing", alone; (b)
    //       `requireValidMonitorId(monitorId);` deleted -> "FAIL: ... outside [0, 32) is refused as
    //       InvalidArgs, by the range check", alone.
    {
        const std::string unset = replaceProperties(kMonitorPropsFresh, {{"speed", Variant(2.0)}});
        check(unset == kInvalidArgs && absent(kMonitorPropsFresh),
              "SetWallpaperProperties on a monitor holding no wallpaper is refused as InvalidArgs, and creates nothing");
        if (unset != kInvalidArgs) std::fprintf(stderr, "  it answered '%s'\n", unset.c_str());
        bool byRange = true;
        for (const int32_t id : {32, -1}) {
            const std::string name = replaceProperties(id, {{"speed", Variant(2.0)}});
            const std::string message =
                errorMessageFrom([&] { setWallpaperProperties(proxy, id, {{"speed", Variant(2.0)}}); });
            if (name != kInvalidArgs || message.find("monitor id must be") == std::string::npos) {
                byRange = false;
                std::fprintf(stderr, "  id %d: answered '%s': %s\n", id, name.c_str(), message.c_str());
            }
        }
        check(byRange, "SetWallpaperProperties on a monitor id outside [0, 32) is refused as InvalidArgs, by the range check");
    }

    // GOAL: S15.105's other side -- each of 0.5, 1 and 2 is accepted by both methods and shown as set.
    // CONTROL: via control_batch.sh, --phase '^propertiesWire$', 2026-10-14, reverted: in propertyError
    //       (backend/daemon/wallpaper_properties.cpp) `!std::ranges::contains(kPlaybackSpeeds, *number)` made
    //       `*number >= 2 || !...` -> "FAIL: speed 2 is accepted by both methods" and the replace-whole check
    //       after it, which sets speed 2; made `*number == 1 || !...` -> "FAIL: speed 1 is accepted by both
    //       methods", alone.
    for (const double speed : {0.5, 1.0, 2.0}) {
        const std::string viaSet = withProperties(kMonitorPropsCap, {{"speed", Variant(speed)}});
        const nlohmann::json afterSet = entryOf(kMonitorPropsCap);
        const std::string viaReplace = replaceProperties(kMonitorPropsCap, {{"speed", Variant(speed)}, {"mute", Variant(true)}});
        const nlohmann::json afterReplace = entryOf(kMonitorPropsCap);
        const bool accepted = viaSet.empty() && viaReplace.empty() && afterSet.is_object() &&
                              afterSet.value("properties", nlohmann::json()) == nlohmann::json({{"speed", speed}}) &&
                              afterReplace.is_object() &&
                              afterReplace.value("properties", nlohmann::json()) ==
                                  nlohmann::json({{"speed", speed}, {"mute", true}});
        char line[64];
        std::snprintf(line, sizeof line, "speed %g is accepted by both methods", speed);
        check(accepted, line);
        if (!accepted) {
            std::fprintf(stderr, "  SetWallpaperWithProperties answered '%s', SetWallpaperProperties '%s'; entry: %s\n",
                         viaSet.c_str(), viaReplace.c_str(), afterReplace.dump().c_str());
        }
    }

    // GOAL: S15.740 -- `position` is a string naming cover, fit, center, tile, stretch or free;
    //       position.zoom a number in 0.1..10; position.x/.y numbers in -10..10. Anything else is refused
    //       InvalidArgs by both methods, all-or-nothing: the held set and the config file are unchanged.
    // WHY-THIS-SHAPE: each bad entry rides beside "mute": false, which would change the held set if the rest
    //       applied; the bounds are probed just past each end, so a rule at the wrong bound or missing goes red.
    // CONTROL: control_batch.sh 2026-10-18, phase propertiesWire, each alone, reverted: in propertyError
    //       (backend/daemon/wallpaper_properties.cpp) the position.zoom rule made `if (false)` -> the zoom 0.0999,
    //       10.001 and bool lines (the `i` stays refused by the wire); `positionOffsetAllowed` made `isfinite`
    //       (shared/wallpaper_position.h) -> the x -10.001, y 10.001 and y -10.001 lines; the position rule made
    //       to accept any value -> the number and colour lines. Each also turned red "a refused position call
    //       applies nothing" and the replace-whole check below, whose starting set the leak changed.
    // COVERAGE-ASSERT: the number of refusals driven is asserted.
    {
        const Variant keep(false);
        const auto text = [](const char* s) { return Variant(std::string(s)); };
        const std::vector<std::pair<const char*, WireProperties>> positionRefusals{
            {"position named centre", {{"position", text("centre")}, {"mute", keep}}},
            {"position named COVER", {{"position", text("COVER")}, {"mute", keep}}},
            {"position as a number", {{"position", Variant(1.0)}, {"mute", keep}}},
            {"position as a colour", {{"position", text("#000000")}, {"mute", keep}}},
            {"a mode under another key", {{"fill", text("fit")}, {"mute", keep}}},
            {"position.zoom 0.0999", {{"position.zoom", Variant(0.0999)}, {"mute", keep}}},
            {"position.zoom 10.001", {{"position.zoom", Variant(10.001)}, {"mute", keep}}},
            {"position.zoom as a bool", {{"position.zoom", Variant(true)}, {"mute", keep}}},
            {"position.zoom of D-Bus type i", {{"position.zoom", Variant(int32_t{1})}, {"mute", keep}}},
            {"position.x -10.001", {{"position.x", Variant(-10.001)}, {"mute", keep}}},
            {"position.x as a mode", {{"position.x", text("fit")}, {"mute", keep}}},
            {"position.y 10.001", {{"position.y", Variant(10.001)}, {"mute", keep}}},
            {"position.y -10.001", {{"position.y", Variant(-10.001)}, {"mute", keep}}},
        };
        const std::string fileBeforePosition = configTextOnceIdle(manager, configFile);
        const nlohmann::json before = entryOf(kMonitorPropsHeld);
        int positionDriven = 0;
        for (const auto& [what, properties] : positionRefusals) {
            const std::string viaSet = errorNameFrom(
                [&] { setWallpaperWithProperties(proxy, "static", imagePath, kMonitorPropsHeld, properties); });
            const std::string viaReplace = replaceProperties(kMonitorPropsHeld, properties);
            check(viaSet == kInvalidArgs && viaReplace == kInvalidArgs,
                  (std::string("a call with ") + what + " is refused as InvalidArgs by both methods").c_str());
            if (viaSet != kInvalidArgs || viaReplace != kInvalidArgs) {
                std::fprintf(stderr, "  %s: answered '%s' and '%s'\n", what, viaSet.c_str(), viaReplace.c_str());
            }
            ++positionDriven;
        }
        check(positionDriven == 13, "every position refusal was driven through both methods");
        const nlohmann::json after = entryOf(kMonitorPropsHeld);
        const std::string fileAfterPosition = configTextOnceIdle(manager, configFile);
        check(before.is_object() && after == before && !fileBeforePosition.empty() && fileAfterPosition == fileBeforePosition,
              "a refused position call applies nothing and writes nothing");
        if (after != before) std::fprintf(stderr, "  entry %s -> %s\n", before.dump().c_str(), after.dump().c_str());

        // GOAL: S15.740's other side -- every mode but tile, and free at both ends of each bound, is
        //       accepted by both methods on a wallpaper that is not a still, and listed as set.
        // CONTROL: control_batch.sh 2026-10-18, phase propertiesWire, reverted: positionZoomAllowed's `<=` made
        //       `<` (shared/wallpaper_position.h) -> "FAIL: free at zoom 10, x 10, y -10 is accepted" alone;
        //       `positionModeShowsVideo` made `mode == PositionMode::Cover` -> the fit, center, stretch and free
        //       lines, both free-bound lines and "all five modes ... were graded".
        int acceptedModes = 0;
        for (const char* mode : {"cover", "fit", "center", "stretch", "free"}) {
            const std::string viaSet = withProperties(kMonitorPropsCap, {{"position", text(mode)}});
            const std::string viaReplace = replaceProperties(kMonitorPropsCap, {{"position", text(mode)}, {"mute", Variant(true)}});
            const nlohmann::json entry = entryOf(kMonitorPropsCap);
            const bool ok = viaSet.empty() && viaReplace.empty() && entry.is_object() &&
                            entry.value("properties", nlohmann::json()) == nlohmann::json({{"position", mode}, {"mute", true}});
            check(ok, (std::string("position ") + mode + " is accepted by both methods and listed as set").c_str());
            if (!ok) std::fprintf(stderr, "  answered '%s' and '%s'; entry %s\n", viaSet.c_str(), viaReplace.c_str(), entry.dump().c_str());
            acceptedModes += ok ? 1 : 0;
        }
        for (const double end : {1.0, -1.0}) {
            const WireProperties free{{"position", text("free")},
                                      {"position.zoom", Variant(end > 0 ? 10.0 : 0.1)},
                                      {"position.x", Variant(10.0 * end)},
                                      {"position.y", Variant(-10.0 * end)}};
            const std::string viaSet = withProperties(kMonitorPropsCap, free);
            const std::string viaReplace = replaceProperties(kMonitorPropsCap, free);
            const nlohmann::json entry = entryOf(kMonitorPropsCap);
            const nlohmann::json want = {{"position", "free"}, {"position.zoom", end > 0 ? 10.0 : 0.1},
                                         {"position.x", 10.0 * end}, {"position.y", -10.0 * end}};
            const bool ok = viaSet.empty() && viaReplace.empty() && entry.is_object() &&
                            entry.value("properties", nlohmann::json()) == want;
            check(ok, end > 0 ? "free at zoom 10, x 10, y -10 is accepted by both methods"
                              : "free at zoom 0.1, x -10, y 10 is accepted by both methods");
            if (!ok) std::fprintf(stderr, "  answered '%s' and '%s'; entry %s\n", viaSet.c_str(), viaReplace.c_str(), entry.dump().c_str());
        }

        // GOAL: S15.741 -- tile is refused for anything but a still image: by SetWallpaperWithProperties
        //       for a video, and by SetWallpaperProperties against the wallpaper the screen holds (a shader here).
        // WHY-THIS-SHAPE: the message is read, since every refusal is InvalidArgs; the screen keeps its set.
        // CONTROL: control_batch.sh 2026-10-18, phase propertiesWire, each alone, reverted: positionError()
        //       deleted from WallpaperManager::requestWallpaper (backend/daemon/wallpaper_manager.cpp) -> "FAIL:
        //       SetWallpaperWithProperties refuses tile for a video" and the next line (the accepted call replaced
        //       the screen's set); deleted from setWallpaperProperties -> "FAIL: SetWallpaperProperties refuses tile
        //       for a screen holding no still" alone.
        const nlohmann::json capBefore = entryOf(kMonitorPropsCap);
        const std::string forVideo = errorMessageFrom([&] {
            setWallpaperWithProperties(proxy, "video", imagePath, kMonitorPropsCap, {{"position", text("tile")}});
        });
        check(forVideo.find("is for still images only") != std::string::npos,
              "SetWallpaperWithProperties refuses tile for a video");
        if (forVideo.find("is for still images only") == std::string::npos) std::fprintf(stderr, "  answered '%s'\n", forVideo.c_str());
        const std::string forHeld =
            errorMessageFrom([&] { setWallpaperProperties(proxy, kMonitorPropsCap, {{"position", text("tile")}}); });
        check(forHeld.find("is for still images only") != std::string::npos && entryOf(kMonitorPropsCap) == capBefore,
              "SetWallpaperProperties refuses tile for a screen holding no still, and keeps its set");
        if (forHeld.find("is for still images only") == std::string::npos) std::fprintf(stderr, "  answered '%s'\n", forHeld.c_str());
        check(acceptedModes == 5, "all five modes a non-still may take were graded");
    }

    // GOAL: S15.99's wire half -- SetWallpaperProperties replaces the WHOLE set and keeps the
    //       wallpaper: a key it does not name is gone afterwards, and an empty set empties it.
    // WHY-THIS-SHAPE: the new set shares one key with the old and drops three, so a merge keeps
    //       three it should not and fails the comparison, while a replace-and-forget-the-wallpaper
    //       fails the type and path.
    // CONTROL: WallpaperManager::setWallpaperProperties (backend/daemon/wallpaper_manager.cpp) merging
    //       with insert_or_assign in place of the swap -> "FAIL: SetWallpaperProperties replaces the
    //       whole set and keeps the wallpaper" and "...and an empty set empties it", alone.
    {
        const std::string replaced = replaceProperties(kMonitorPropsHeld, {{"mute", Variant(false)}});
        const nlohmann::json after = entryOf(kMonitorPropsHeld);
        check(replaced.empty() && after.is_object() && after.value("type", std::string()) == "shader" &&
                  after.value("path", std::string()) == unrenderablePath &&
                  after.value("properties", nlohmann::json()) == nlohmann::json({{"mute", false}}),
              "SetWallpaperProperties replaces the whole set and keeps the wallpaper");
        if (!replaced.empty()) std::fprintf(stderr, "  it answered '%s'\n", replaced.c_str());
        const std::string emptied = replaceProperties(kMonitorPropsHeld, {});
        const nlohmann::json emptiedEntry = entryOf(kMonitorPropsHeld);
        check(emptied.empty() && emptiedEntry.is_object() &&
                  emptiedEntry.value("properties", nlohmann::json()) == nlohmann::json::object(),
              "...and an empty set empties it");
    }

    // GOAL: S15.98 -- a wallpaper's properties belong to it: SetWallpaperWithProperties replaces the
    //       previous wallpaper's set whole, and SetWallpaper -- a wallpaper with none -- clears them,
    //       in GetStatus and in the config file, whose row then carries no `properties` key at all.
    // WHY-THIS-SHAPE: the file is read after SetWallpaper returns, which saves synchronously; the key's
    //       ABSENCE is asserted, since an empty object and no key read alike as "no properties".
    // CONTROL: in WallpaperManager::requestWallpaper (backend/daemon/wallpaper_manager.cpp), (a)
    //       `state.properties.swap(nextProperties);` only for a non-empty set -> "FAIL: SetWallpaper after
    //       SetWallpaperWithProperties clears the properties, in GetStatus and in the file", alone; (b)
    //       merging instead -> that line and "...replaces the previous wallpaper's set whole".
    {
        const std::string first =
            withProperties(kMonitorPropsCleared, {{"speed", Variant(2.0)}, {"tint", Variant(std::string("#010203"))}});
        const std::string second = withProperties(kMonitorPropsCleared, {{"mute", Variant(true)}});
        const nlohmann::json afterSecond = entryOf(kMonitorPropsCleared);
        check(first.empty() && second.empty() && afterSecond.is_object() &&
                  afterSecond.value("properties", nlohmann::json()) == nlohmann::json({{"mute", true}}),
              "SetWallpaperWithProperties replaces the previous wallpaper's set whole");
        const std::string plain =
            errorNameFrom([&] { setWallpaper(proxy, "shader", unrenderablePath, kMonitorPropsCleared); });
        const nlohmann::json afterPlain = entryOf(kMonitorPropsCleared);
        const std::string text = configTextOnceIdle(manager, configFile);
        const nlohmann::json row = configRowOf(text, kMonitorPropsCleared);
        const bool cleared = plain.empty() && afterPlain.is_object() &&
                             afterPlain.value("properties", nlohmann::json()) == nlohmann::json::object() &&
                             !text.empty() && row.is_object() && !row.contains("properties");
        check(cleared, "SetWallpaper after SetWallpaperWithProperties clears the properties, in GetStatus and in the file");
        if (!cleared) {
            std::fprintf(stderr, "  SetWallpaper answered '%s'; GetStatus entry: %s; file row: %s; file: %s\n", plain.c_str(),
                         afterPlain.dump().c_str(), row.dump().c_str(), text.empty() ? "(write failed)" : text.c_str());
        }
    }
}

// SetImportModifiers on the wire: what it refuses, and which monitors an accepted report reaches.
// Last on the shared manager: an excluding report re-renders monitors. An opaque fd has no layout,
// so a host exporting one can show only that nothing happens (S6.47); a dma_buf host shows both
// directions (S6.45, S6.46). The committed frame's modifier, read off the wire, decides which.
void runImportModifierWireChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& imagePath,
                                 const Check& check) {
    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
    const std::vector<uint64_t> reported{wallpaperx::testing::kDrmModifierLinear};

    // GOAL: a report the daemon cannot act on -- another format, an empty set, an over-long list -- is
    //       refused on the reply as InvalidArgs (an empty stored set would read as "nobody reported").
    // CONTROL: each clause of WallpaperManager::setImportModifiers (backend/daemon/wallpaper_manager.cpp)
    //       disabled in turn: the format test -> the format line alone; the length cap -> the length
    //       line and nine later checks; the empty-set test alone stays green (the empty-intersection
    //       refusal answers it), and with that refusal disabled too -> "FAIL: an empty modifier report
    //       is refused as InvalidArgs rather than stored" and seven more.
    check(errorNameFrom([&] { setImportModifiers(proxy, exportedFormat ^ 0xFFu, reported); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a modifier report for a format this daemon does not export is refused as InvalidArgs");
    check(errorNameFrom([&] { setImportModifiers(proxy, exportedFormat, {}); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "an empty modifier report is refused as InvalidArgs rather than stored");
    check(errorNameFrom([&] {
              setImportModifiers(proxy, exportedFormat,
                                 std::vector<uint64_t>(wallpaperx::WallpaperManager::kMaxImportModifiers + 1, 0));
          }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "a modifier report longer than the daemon's limit is refused as InvalidArgs");

    // This phase's own monitor, at a known generation before anything is reported.
    setWallpaper(proxy, "static", imagePath, kMonitorModifiersWire);
    if (!signals.wait(kMonitorModifiersWire, 1)) {
        // X.1: abandoned with a reason, or the exact counts below would pass on nothing rendered.
        check(false, "the import-modifier wire phase's own monitor produced its first frame");
        return;
    }

    // An opaque fd publishes kDrmModifierInvalid, which no report can exclude.
    uint64_t committed = wallpaperx::kDrmModifierInvalid;
    try {
        committed = getFrame(proxy, kMonitorModifiersWire).drmModifier;
    } catch (const sdbus::Error& e) {
        check(false, "the import-modifier wire phase's own monitor serves the frame it just announced");
        std::fprintf(stderr, "  GetFrame(monitor=%d) answered '%s'\n", kMonitorModifiersWire, e.getName().c_str());
        return;
    }
    const bool opaqueExport = committed == wallpaperx::kDrmModifierInvalid;

    // GOAL: S6.45 / S6.47 over the wire -- an accepted report leaves alone a monitor whose committed
    //       frame it does not exclude (a layout it names, or any opaque fd).
    // WHY-THIS-SHAPE: "no render" settled by a barrier on a separate monitor: barriering on this one
    //       lets RenderWorker::post() collapse the re-render into the barrier's queued entry.
    // COVERAGE-ASSERT: the exact signal count, so a monitor that stopped rendering fails.
    // CONTROL: `if (stillReadable) continue;` disabled in WallpaperManager::adoptImportModifiersLocked
    //       (backend/daemon/wallpaper_manager.cpp) -> this check ("monitor 22 announced 2 frames"), the
    //       serve check below and the manager-tier checks, on both ICDs.
    std::vector<uint64_t> keep{kUnavailableModifierA};
    if (!opaqueExport) keep.push_back(committed);
    setImportModifiers(proxy, exportedFormat, keep);
    setWallpaper(proxy, "static", imagePath, kMonitorModifiersBarrier);
    if (!signals.wait(kMonitorModifiersBarrier, 1)) {
        // X.1: otherwise "no extra render" could not be told from "not yet".
        check(false, "the barrier frame arrives after an import-modifier report");
        return;
    }
    const size_t afterKeep = signals.countFor(kMonitorModifiersWire);
    check(afterKeep == 1, opaqueExport
                              ? "a modifier report does not re-render a monitor whose frame is an opaque fd"
                              : "a modifier report does not re-render a monitor whose frame's layout it still names");
    if (afterKeep != 1) {
        std::fprintf(stderr, "  monitor %d announced %zu frames; expected exactly 1 -- its first render, and no "
                             "rebuild for a report that excludes nothing it holds\n",
                     kMonitorModifiersWire, afterKeep);
    }
    uint64_t servedAfterKeep = 0;
    try {
        servedAfterKeep = getFrame(proxy, kMonitorModifiersWire).generation;
    } catch (const sdbus::Error& e) {
        std::fprintf(stderr, "  GetFrame after the report answered '%s'\n", e.getName().c_str());
    }
    check(servedAfterKeep == 1, "the monitor a report did not exclude still serves the frame it already had");

    if (opaqueExport) {
        // Stated, not skipped silently; the manager-tier phase reports it notVerified() once.
        std::printf("dbus_service_test: this host exports opaque fds, so the wire tier grades S6.47 -- a report "
                    "invalidates nothing -- and cannot reach S6.46, which needs a report that excludes the "
                    "committed layout\n");
        std::fflush(stdout);
        return;
    }

    // GOAL: S6.46 over the wire -- a report that excludes the committed frame's layout does
    //       reach that monitor, and the frame no reporting consumer could read is withdrawn.
    // WHY-THIS-SHAPE: polls GetStatus for one of the two legitimate endings -- rebuilt at a newer
    //       generation, or failed with a recorded cause -- and fails on neither; a FrameReady wait
    //       would treat a driver that can never rebuild as a stall.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, on Lavapipe: (a)
    //       `it->second.frameModifier = target->drmModifier;` deleted from renderPassLocked -> the
    //       settled-state check; (b) all three clears of hasFrame (applyImportModifiersLocked,
    //       forgetImages, the commit's error arm) deleted -> "FAIL: a report that excludes the
    //       committed frame's layout withdraws that frame" (one or two of them alone stays green).
    // LINEAR is offered unless already in use, so a second usable layout leaves a rebuild to grade.
    std::vector<uint64_t> exclude{kUnavailableModifierA};
    if (committed != wallpaperx::testing::kDrmModifierLinear) {
        exclude.push_back(wallpaperx::testing::kDrmModifierLinear);
    }
    setImportModifiers(proxy, exportedFormat, exclude);

    uint64_t excludedGeneration = 0;
    bool excludedHasFrame = true;
    std::string excludedError;
    bool settled = false;
    for (int attempt = 0; attempt < 100 && !settled; ++attempt) {
        // A named local: wallpaperStatus() points into it.
        const StatusReply reply = getStatusOrExplain(proxy);
        const nlohmann::json* state = wallpaperStatus(reply, std::to_string(kMonitorModifiersWire));
        if (state != nullptr) {
            excludedHasFrame = state->value("has_frame", false);
            excludedGeneration = state->value("generation", uint64_t{0});
            excludedError = state->value("last_error", std::string());
            settled = (excludedHasFrame && excludedGeneration > 1) || !excludedError.empty();
        }
        if (!settled) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!settled) {
        // X.1: the stall is a failure, never data.
        check(false, "the monitor whose layout a report excluded reaches a settled state");
        std::fprintf(stderr, "  monitor %d still reports has_frame=%d generation=%llu with no error after 5 s\n",
                     kMonitorModifiersWire, static_cast<int>(excludedHasFrame),
                     static_cast<unsigned long long>(excludedGeneration));
        return;
    }
    if (excludedError.empty()) {
        check(excludedGeneration > 1,
              "a report that excludes the committed frame's layout withdraws that frame and rebuilds it");
    } else {
        // The withdrawal is asserted; a rebuild in a reported layout cannot be on a single-layout
        // driver, and is graded by the renderer's consumer_modifier_probe.
        check(!excludedHasFrame, "a report that excludes the committed frame's layout withdraws that frame");
        std::printf("dbus_service_test: the withdrawn frame was not rebuilt -- this driver offers no layout in "
                    "the reported set: %s\n",
                    excludedError.c_str());
        std::fflush(stdout);
    }

    // Recovery separates "withdrawn" from "lost for good".
    setImportModifiers(proxy, exportedFormat, keep);
    bool recovered = false;
    for (int attempt = 0; attempt < 100 && !recovered; ++attempt) {
        const StatusReply reply = getStatusOrExplain(proxy);
        const nlohmann::json* state = wallpaperStatus(reply, std::to_string(kMonitorModifiersWire));
        recovered = state != nullptr && state->value("has_frame", false);
        if (!recovered) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(recovered, "a monitor a report withdrew renders again once a layout it can use is reported");
}

// S6.51 over the real bus: a departing reporter's monitors are handed back
// (WallpaperManager::forgetImportModifiers) AND posted (DbusService::forgetClient). The monitor is
// held frameless by an undecodable file, not by the set, so every host -- opaque fds included --
// can answer. Last on the shared manager: it re-reports a wider set from the shared proxy.
void runModifierWithdrawalRenderChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& imagePath,
                                       const Check& check) {
    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);

    // A reference frame whose layout every set below keeps, so only the blocked monitor is re-posted.
    setWallpaper(proxy, "static", imagePath, kMonitorWithdrawBarrier);
    if (!signals.wait(kMonitorWithdrawBarrier, 1)) {
        check(false, "the withdrawal phase's barrier monitor produced its first frame");
        return;
    }
    uint64_t committed = wallpaperx::kDrmModifierInvalid;
    try {
        committed = getFrame(proxy, kMonitorWithdrawBarrier).drmModifier;
    } catch (const sdbus::Error& e) {
        check(false, "the withdrawal phase's barrier monitor serves the frame it just announced");
        std::fprintf(stderr, "  GetFrame(monitor=%d) answered '%s'\n", kMonitorWithdrawBarrier, e.getName().c_str());
        return;
    }

    // Nested, so the departure widens the set; both keep the committed layout (no withdrawal).
    std::vector<uint64_t> wide{kUnavailableModifierA, kUnavailableModifierB};
    std::vector<uint64_t> narrow{kUnavailableModifierA};
    if (committed != wallpaperx::kDrmModifierInvalid) {
        wide.push_back(committed);
        narrow.push_back(committed);
    }
    setImportModifiers(proxy, exportedFormat, wide);

    // Held frameless by an undecodable file, made decodable later without a SetWallpaper (a post).
    const std::string blockedPath =
        wallpaperx::testing::writeUndecodableFile(testAssetDir(), "dbus_test_withdraw.png");
    setWallpaper(proxy, "static", blockedPath, kMonitorWithdrawRender);
    bool blocked = false;
    for (int attempt = 0; attempt < 100 && !blocked; ++attempt) {
        const StatusReply reply = getStatusOrExplain(proxy);
        const nlohmann::json* state = wallpaperStatus(reply, std::to_string(kMonitorWithdrawRender));
        blocked = state != nullptr && !state->value("last_error", std::string()).empty();
        if (!blocked) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!blocked) {
        // X.1: abandoned with a reason; "not rendered yet" would satisfy every check below.
        check(false, "the withdrawal phase's blocked monitor records why it could not render");
        return;
    }

    // On a connection of its own, so destroying it is a real departure.
    auto leaver = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    setImportModifiers(*leaver, exportedFormat, narrow);

    // Drains the worker past the narrowing's re-render; on another monitor, so it cannot coalesce.
    setWallpaper(proxy, "static", imagePath, kMonitorWithdrawBarrier);
    if (!signals.wait(kMonitorWithdrawBarrier, 2)) {
        check(false, "the withdrawal phase's barrier frame arrives after the narrowing report");
        return;
    }

    // Decodable only now, so only a render posted after this line can draw the monitor.
    writeSolidPng(blockedPath, 0, 255, 0);
    if (signals.countFor(kMonitorWithdrawRender) != 0) {
        check(false, "the withdrawal phase's blocked monitor announced no frame before the departure");
        std::fprintf(stderr, "  monitor %d announced %zu frame(s) while its file could not be decoded\n",
                     kMonitorWithdrawRender, signals.countFor(kMonitorWithdrawRender));
        return;
    }

    // GOAL: S6.51 end to end -- a consumer leaving the bus does not merely widen the effective
    //       set, it gets the monitors that set was blocking re-rendered.
    // WHY-THIS-SHAPE: a frame, not the set (S6.50 grades the widening); nothing else can post this
    //       monitor, and the bounded wait fails with the monitor's state printed.
    // CONTROL: (a) WallpaperManager::forgetImportModifiers (backend/daemon/wallpaper_manager.cpp)
    //       returning `{}` after adopting -> this and the departure announcement
    //       line; (b) the `m_worker.post(monitorId)` loop deleted from
    //       DbusService::applyImportModifierUpdate (backend/daemon/dbus_service.cpp) -> this alone.
    leaver.reset();
    const bool rendered = signals.wait(kMonitorWithdrawRender, 1);
    check(rendered, "a consumer leaving the bus gets the monitor its set was blocking re-rendered");
    if (!rendered) {
        const StatusReply reply = getStatusOrExplain(proxy);
        const nlohmann::json* state = wallpaperStatus(reply, std::to_string(kMonitorWithdrawRender));
        std::fprintf(stderr, "  monitor %d announced no frame after the reporting consumer left; has_frame=%d "
                             "last_error='%s'\n",
                     kMonitorWithdrawRender, state != nullptr && state->value("has_frame", false),
                     state != nullptr ? state->value("last_error", std::string()).c_str() : "(no status)");
    }
}

// What a modifier report does to monitors that have rendered. A report excluding a committed
// layout drops those targets and re-posts the monitors, rebuilt one at a time, so between the
// first and the last a condemned monitor has no target and must not claim a frame (S6.44). An
// opaque fd has no layout: only S6.47 is answerable there. Driven at the manager: over the bus
// the export always queues behind both re-renders, so the window is unreachable without racing.
void runImportModifierChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                             const std::string& unrenderablePath, const Check& check) {
    // Its own Device, so its own validation grade, run after the manager is destroyed.
    std::shared_ptr<const std::atomic<int>> counter;
    bool layerActive = false;
    struct GradeOnExit {
        const std::shared_ptr<const std::atomic<int>>& counter;
        const bool& layerActive;
        const Check& check;
        ~GradeOnExit() {
            int failures = 0;
            int ungraded = 0;
            {
                wallpaperx::testing::ValidationGrade grade("dbus_service_test (import-modifier phase)", failures);
                if (wallpaperx::testing::validationWaived()) grade.expectNoDevice();
                grade.reportUngradedTo(ungraded);
                grade.adopt(counter, layerActive);
            }
            if (ungraded > 0) {
                std::fprintf(stderr,
                             "dbus_service_test: the import-modifier phase's validation grade could not be made "
                             "-- VK_LAYER_KHRONOS_validation is not loaded. The phase itself is still judged.\n");
            }
            check(failures == 0, "the import-modifier phase's own device passed its validation grade");
        }
    } gradeOnExit{counter, layerActive, check};

    const std::string kConsumerName = ":test.consumer";

    wallpaperx::WallpaperManager manager(config);
    struct CaptureOnExit {
        const wallpaperx::WallpaperManager& manager;
        std::shared_ptr<const std::atomic<int>>& counter;
        bool& layerActive;
        ~CaptureOnExit() {
            counter = manager.validationCounter();
            layerActive = manager.validationLayerActive();
        }
    } captureOnExit{manager, counter, layerActive};

    // One export's result and layout (kDrmModifierInvalid: an opaque fd).
    struct Exported {
        bool hasFrame = false;
        wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
        uint64_t modifier = wallpaperx::kDrmModifierInvalid;
    };

    // Closes the fd an export hands back, so no leak reaches the fd-count checks elsewhere.
    const auto exportOnce = [&manager](int32_t monitorId) {
        Exported out;
        wallpaperx::FrameExport got = exportDelivered(manager, monitorId);
        out.hasFrame = got.handle.has_value();
        out.reason = got.reason;
        if (out.hasFrame) {
            out.modifier = got.handle->frame.modifier;
            if (got.handle->frame.fd >= 0) close(got.handle->frame.fd);
        }
        return out;
    };

    manager.requestWallpaper("static", imagePath, kMonitorModifiersRerendered);
    manager.requestWallpaper("static", imagePath, kMonitorModifiersPending);
    const wallpaperx::RenderOutcome firstRerendered = manager.renderPending(kMonitorModifiersRerendered);
    const wallpaperx::RenderOutcome firstPending = manager.renderPending(kMonitorModifiersPending);
    if (!firstRerendered.rendered || !firstPending.rendered) {
        wallpaperx::testing::notVerified(
            "S6.44 through S6.47",
            "neither monitor could be given a frame to begin with, so the states these goals are about -- a "
            "monitor that HAS a frame and a report that does or does not exclude its layout -- were never reached");
        return;
    }

    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);

    // Read off a real export: WALLPAPERX_FORCE_HANDLE_TYPE flips this GPU between the two.
    const Exported firstExport = exportOnce(kMonitorModifiersRerendered);
    if (!firstExport.hasFrame) {
        check(false, "the monitor this phase just rendered exports the frame it committed");
        return;
    }
    const uint64_t committed = firstExport.modifier;
    const bool opaqueExport = committed == wallpaperx::kDrmModifierInvalid;

    // GOAL: S6.45 (dma_buf) / S6.47 (opaque fd) -- an accepted report neither re-renders nor
    //       withdraws the frame of a monitor it does not exclude.
    // WHY-THIS-SHAPE: two checks, since the daemon decides twice: adoptImportModifiersLocked() picks
    //       what to re-post, applyImportModifiersLocked() drops targets on the render thread. A render
    //       on the other monitor carries the set to the GPU side, or the survivor check would grade a
    //       set the render thread never saw.
    // COVERAGE-ASSERT: that carrying render is asserted to have run (no control of its own).
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, (a) `if (stillReadable) continue;` disabled in
    //       adoptImportModifiersLocked -> the re-post check; (b) applyImportModifiersLocked's
    //       `monitorIds` made every monitor -> the survivor check and the wire tier's serve check.
    std::vector<uint64_t> keep{kUnavailableModifierA};
    if (!opaqueExport) keep.push_back(committed);
    const std::vector<int> keepRender = manager.setImportModifiers(kConsumerName, exportedFormat, keep).toRender;
    check(keepRender.empty(),
          opaqueExport ? "a modifier report re-posts no monitor whose frame is an opaque fd"
                       : "a modifier report re-posts no monitor whose frame's layout it still names");

    manager.requestWallpaper("static", imagePath, kMonitorModifiersRerendered);
    const wallpaperx::RenderOutcome carriedSet = manager.renderPending(kMonitorModifiersRerendered);
    check(carriedSet.rendered, "the render that carries the accepted set to the GPU side ran");
    const Exported survivor = exportOnce(kMonitorModifiersPending);
    check(survivor.hasFrame,
          opaqueExport ? "an opaque-fd frame survives a report being applied on the render thread"
                       : "a frame whose layout the report still names survives the report being applied");

    if (opaqueExport) {
        wallpaperx::testing::notVerified(
            "S6.44, S6.46, S6.53 and S6.54",
            "this host exports opaque file descriptors, which carry no DRM format modifier, so no report can "
            "exclude the committed frame and the state all four goals are about -- a monitor that HAD a frame and "
            "whose target a report then dropped -- cannot be reached here at all");
        return;
    }

    // GOAL: S6.46 -- a report excluding the committed layout returns both monitors for re-render,
    //       and the frame no reporting consumer could read is withdrawn.
    // WHY-THIS-SHAPE: both monitors: the S6.44 window exists only because several are condemned.
    // CONTROL: `it->second.frameModifier = target->drmModifier;` deleted from
    //       WallpaperManager::renderPassLocked (backend/daemon/wallpaper_manager.cpp) -> this and
    //       nine more on Lavapipe, the NotRendered window and both recoveries among them.
    // LINEAR unless already in use, so a second usable layout leaves a rebuild to grade.
    std::vector<uint64_t> exclude{kUnavailableModifierA};
    if (committed != wallpaperx::testing::kDrmModifierLinear) {
        exclude.push_back(wallpaperx::testing::kDrmModifierLinear);
    }
    const std::vector<int> toRender = manager.setImportModifiers(kConsumerName, exportedFormat, exclude).toRender;
    check(std::find(toRender.begin(), toRender.end(), kMonitorModifiersRerendered) != toRender.end() &&
              std::find(toRender.begin(), toRender.end(), kMonitorModifiersPending) != toRender.end(),
          "a report excluding the committed frame's layout returns both rendered monitors for re-render");

    // Only one of the two: the window, both targets dropped and the second not yet re-rendered.
    const wallpaperx::RenderOutcome reRendered = manager.renderPending(kMonitorModifiersRerendered);

    // GOAL: S6.44 -- a monitor whose target the report dropped, its re-render not yet run,
    //       answers GetFrame NotRendered rather than claiming a frame it cannot produce.
    // WHY-THIS-SHAPE: the reply, not the flag: a stale hasFrame throws on the missing target and
    //       reaches the client as a generic error, not "ask again".
    // CONTROL: `it->second.hasFrame = false;` deleted from both
    //       WallpaperManager::applyImportModifiersLocked and forgetImages (backend/daemon/
    //       wallpaper_manager.cpp; either alone stays green) -> this check red on Lavapipe ("monitor
    //       21 is marked as rendered but has no offscreen target") and the re-post line beside it.
    bool threw = false;
    std::string threwWhat;
    Exported pending;
    try {
        pending = exportOnce(kMonitorModifiersPending);
    } catch (const std::exception& e) {
        threw = true;
        threwWhat = e.what();
    }
    check(!threw, "a monitor awaiting its re-render answers GetFrame rather than throwing");
    if (threw) {
        std::fprintf(stderr, "  the export threw: %s\n", threwWhat.c_str());
    } else {
        check(!pending.hasFrame && pending.reason == wallpaperx::NoFrameReason::NotRendered,
              "a monitor awaiting its re-render is NotRendered, not still holding its old frame");
    }

    // GOAL: the rebuild half of S6.46 -- the re-render the report scheduled produces an image
    //       in a layout the consumer reported, not merely any image.
    // WHY-THIS-SHAPE: branched on the rebuild: a single-layout driver has nothing left to allocate
    //       (the driver's limit, X.3); that branch still requires a recorded cause.
    // CONTROL: `modifiers = std::move(agreed);` deleted from Memory::createExportableImage
    //       (renderer/vulkan/memory.cpp) plus the chosen-modifier guard in populateDmaBufLayout
    //       (renderer/vulkan/frame_export.cpp) disabled, under WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf on
    //       NVIDIA -> red, rebuilt as 0x300000000606013 where only LINEAR was reported.
    if (reRendered.rendered) {
        check(reRendered.generation > carriedSet.generation,
              "the monitor that was re-posted first rebuilds its image and commits a newer generation");
        const Exported rebuilt = exportOnce(kMonitorModifiersRerendered);
        check(rebuilt.hasFrame && std::find(exclude.begin(), exclude.end(), rebuilt.modifier) != exclude.end(),
              "the rebuilt frame carries a layout from the set the consumer reported");
        if (rebuilt.hasFrame) {
            std::printf("dbus_service_test: the excluded frame's layout 0x%llx was rebuilt as 0x%llx\n",
                        static_cast<unsigned long long>(committed),
                        static_cast<unsigned long long>(rebuilt.modifier));
            std::fflush(stdout);
        }
    } else {
        const wallpaperx::WallpaperManager::Snapshot snapshot = manager.snapshot();
        const auto entry = snapshot.monitors.find(kMonitorModifiersRerendered);
        const std::string why = entry != snapshot.monitors.end() ? entry->second.lastError : std::string();
        // A re-render never serviced records no cause, so dropped work fails here.
        check(!why.empty(), "a re-render that could not rebuild records why");
        std::printf("dbus_service_test: the re-render this report scheduled had nothing left to allocate, so the "
                    "rebuilt frame's layout was not graded: %s\n",
                    why.empty() ? "(no cause recorded)" : why.c_str());
        std::fflush(stdout);
    }

    // Recovery separates "withdrawn" from "lost for good".
    const std::vector<int> restored = manager.setImportModifiers(kConsumerName, exportedFormat, keep).toRender;
    check(std::find(restored.begin(), restored.end(), kMonitorModifiersPending) != restored.end(),
          "a later report re-posts a monitor the previous one left without a frame");
    const wallpaperx::RenderOutcome pendingReRendered = manager.renderPending(kMonitorModifiersPending);
    const wallpaperx::RenderOutcome rerenderedAgain = manager.renderPending(kMonitorModifiersRerendered);
    check(pendingReRendered.rendered && pendingReRendered.generation > firstPending.generation,
          "the monitor that waited rebuilds its image once a layout this GPU can use is reported");
    bool bothExportAgain = false;
    try {
        bothExportAgain = exportOnce(kMonitorModifiersPending).hasFrame &&
                          exportOnce(kMonitorModifiersRerendered).hasFrame && rerenderedAgain.rendered;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  the recovery export threw: %s\n", e.what());
    }
    check(bothExportAgain, "both monitors export a frame again once the withdrawn layout is replaced");

    // A fresh pair holding frames allocated under `keep`.
    manager.requestWallpaper("static", imagePath, kMonitorModifiersUndoneA);
    manager.requestWallpaper("static", imagePath, kMonitorModifiersUndoneB);
    const wallpaperx::RenderOutcome undoneA = manager.renderPending(kMonitorModifiersUndoneA);
    const wallpaperx::RenderOutcome undoneB = manager.renderPending(kMonitorModifiersUndoneB);
    if (!undoneA.rendered || !undoneB.rendered) {
        check(false, "the pair S6.53 and S6.54 are graded on both hold a frame to begin with");
        return;
    }

    // No driver can allocate this, so a re-render under it can only fail: the release shows.
    const std::vector<uint64_t> unusable{kUnavailableModifierA};

    // GOAL: S6.54 -- the condemned set is re-derived from the set in force, not accumulated. A
    //       monitor condemned by a narrow report whose set widens again before the render thread
    //       has applied anything keeps the frame it already had.
    // WHY-THIS-SHAPE: rendering A carries the widened set across; B is read never serviced, so a
    //       frame B has was never withdrawn (a rebuild would hide the teardown).
    // CONTROL: the std::erase_if block deleted from WallpaperManager::adoptImportModifiersLocked
    //       (backend/daemon/wallpaper_manager.cpp) -> "a monitor whose condemnation was undone before
    //       the render thread saw it keeps its frame", alone, on Lavapipe.
    const std::vector<int> condemned = manager.setImportModifiers(kConsumerName, exportedFormat, unusable).toRender;
    check(std::find(condemned.begin(), condemned.end(), kMonitorModifiersUndoneA) != condemned.end() &&
              std::find(condemned.begin(), condemned.end(), kMonitorModifiersUndoneB) != condemned.end(),
          "a report excluding both monitors' layout condemns both");
    manager.setImportModifiers(kConsumerName, exportedFormat, keep);
    const wallpaperx::RenderOutcome carriedUndo = manager.renderPending(kMonitorModifiersUndoneA);
    check(carriedUndo.rendered, "the render that carries the undone report to the GPU side ran");
    bool keptItsFrame = false;
    try {
        keptItsFrame = exportOnce(kMonitorModifiersUndoneB).hasFrame;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  the export of the monitor whose condemnation was undone threw: %s\n", e.what());
    }
    check(keptItsFrame, "a monitor whose condemnation was undone before the render thread saw it keeps its frame");

    // GOAL: S6.53 -- a monitor whose wallpaper type this build cannot draw still loses the image
    //       it holds when a report excludes that image's layout. The image is released, so it is
    //       not there to be reused when the monitor becomes renderable again.
    // WHY-THIS-SHAPE: the type changes while the static image is still allocated (the only window
    //       for it); graded on the re-render once renderable again, since acquireTarget() would
    //       silently reuse a surviving image of matching size.
    // CONTROL: in WallpaperManager::adoptImportModifiersLocked (backend/daemon/wallpaper_manager.cpp),
    //       (a) `if (!isRenderableType(state.type)) continue;` restored first in the loop -> this, the
    //       export check and the two below ("the driver chose DRM format modifier 0..."); (b) the
    //       second isRenderableType test, guarding the requestSeq bump, deleted (or drawableType()
    //       mapping `shader` to a still) -> "not asked to re-render" and the condemns-no-monitor change line.
    manager.requestWallpaper("static", imagePath, kMonitorModifiersUnrenderable);
    const wallpaperx::RenderOutcome unrenderableFirst = manager.renderPending(kMonitorModifiersUnrenderable);
    if (!unrenderableFirst.rendered) {
        check(false, "the monitor S6.53 is graded on holds an image before its type changes");
        return;
    }
    manager.requestWallpaper("shader", unrenderablePath, kMonitorModifiersUnrenderable);

    const std::vector<int> condemnedAgain =
        manager.setImportModifiers(kConsumerName, exportedFormat, unusable).toRender;
    check(std::find(condemnedAgain.begin(), condemnedAgain.end(), kMonitorModifiersUnrenderable) ==
              condemnedAgain.end(),
          "a monitor whose type this build cannot draw is not asked to re-render");
    // Carries the report to the render thread, which releases before it (fails to) allocate.
    manager.renderPending(kMonitorModifiersUndoneA);

    manager.requestWallpaper("static", imagePath, kMonitorModifiersUnrenderable);
    const wallpaperx::RenderOutcome reused = manager.renderPending(kMonitorModifiersUnrenderable);
    check(!reused.rendered,
          "a monitor that was unrenderable when the report arrived has no old image left to draw into");
    bool staleThrew = false;
    Exported stale;
    try {
        stale = exportOnce(kMonitorModifiersUnrenderable);
    } catch (const std::exception& e) {
        staleThrew = true;
        std::fprintf(stderr, "  exporting it threw, which is what a reused image does: %s\n", e.what());
    }
    check(!staleThrew && !stale.hasFrame,
          "it reports no frame rather than serving an image in a layout the consumer excluded");
    {
        const wallpaperx::WallpaperManager::Snapshot snapshot = manager.snapshot();
        const auto entry = snapshot.monitors.find(kMonitorModifiersUnrenderable);
        const std::string why = entry != snapshot.monitors.end() ? entry->second.lastError : std::string();
        // So the negative above cannot pass on dropped work.
        check(!why.empty(), "the render that found no image to reuse records why it could not make one");
        std::printf("dbus_service_test: the unrenderable monitor's image was released, so its re-render had to "
                    "allocate: %s\n",
                    why.empty() ? "(no cause recorded)" : why.c_str());
        std::fflush(stdout);
    }

    // Recovery separates "released" from "lost".
    manager.setImportModifiers(kConsumerName, exportedFormat, keep);
    const wallpaperx::RenderOutcome unrenderableBack = manager.renderPending(kMonitorModifiersUnrenderable);
    bool exportsAgain = false;
    try {
        exportsAgain = unrenderableBack.rendered && exportOnce(kMonitorModifiersUnrenderable).hasFrame;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  the recovery export threw: %s\n", e.what());
    }
    check(exportsAgain, "that monitor renders and exports again once a usable layout is reported");
}

// The per-consumer bookkeeping behind SetImportModifiers: what takes effect is the intersection
// of every consumer's report, a report emptying it is refused, and a list dies with its reporter.
// At the manager, with no Device: "did the set change" is the monitor list setImportModifiers
// returns, since a recorded, frameless monitor is re-posted on every real change and on nothing
// else. The modifiers are the unallocatable ones, so the host's driver does not matter.
void runImportModifierClientChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                                   const Check& check) {
    wallpaperx::WallpaperManager manager(config);
    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);

    // Recorded, never rendered -- so it is re-posted exactly when the effective set changes.
    manager.requestWallpaper("static", imagePath, kMonitorModifiersIdle);

    struct Report {
        bool refused = false;
        bool changed = false;
        std::string why;
    };
    int probes = 0;
    const auto report = [&](const std::string& client, const std::vector<uint64_t>& set) {
        ++probes;
        Report out;
        try {
            out.changed = !manager.setImportModifiers(client, exportedFormat, set).toRender.empty();
        } catch (const wallpaperx::InvalidRequest& e) {
            out.refused = true;
            out.why = e.what();
        }
        return out;
    };

    // A name per probe: a consumer's own next report would overwrite a refused one's leftover.
    const std::string consumerA = ":test.consumer.a";
    const std::string consumerB = ":test.consumer.b";
    const std::string consumerC = ":test.consumer.c";
    const std::string consumerD = ":test.consumer.d";
    const std::string consumerE = ":test.consumer.e";
    const std::string consumerF = ":test.consumer.f";
    const std::string consumerG = ":test.consumer.g";
    const std::string consumerH = ":test.consumer.h";
    constexpr uint64_t m1 = kUnavailableModifierA;
    constexpr uint64_t m2 = kUnavailableModifierB;
    constexpr uint64_t m3 = kUnavailableModifierC;

    const Report firstA = report(consumerA, {m1, m2});
    check(!firstA.refused && firstA.changed, "the first consumer's report takes effect");

    // S6.43's idempotency, graded where a frameless monitor makes a repeat distinguishable.
    // CONTROL: the `if (m_importModifiers == modifiers) return {};` early-out disabled in
    //       WallpaperManager::adoptImportModifiersLocked (backend/daemon/wallpaper_manager.cpp) ->
    //       this, the two other unchanged-set checks here and the wire tier's S6.59 silence check.
    check(!report(consumerA, {m1, m2}).changed, "an identical report from the same consumer changes nothing");

    // GOAL: S6.52 -- what a consumer reports is a SET. The same layouts sent in a different
    //       order, or with a duplicate added, are the same report: nothing is re-posted.
    // WHY-THIS-SHAPE: permutation and duplicate apart: sorting and deduplicating are separate
    //       operations. A spurious change bumps every frameless monitor's request, superseding the
    //       render in flight, so a per-frame permutation would keep monitors blank.
    // CONTROL: std::sort and std::unique deleted from WallpaperManager::setImportModifiers
    //       (backend/daemon/wallpaper_manager.cpp) -> exactly these two checks.
    check(!report(consumerA, {m2, m1}).changed, "the same layouts reported in a different order change nothing");
    check(!report(consumerA, {m1, m2, m2}).changed, "the same layouts reported with a duplicate change nothing");

    // GOAL: S6.52's second clause -- the length limit is applied to the list as sent, so an
    //       over-long report cannot be admitted by collapsing its duplicates away first.
    // WHY-THIS-SHAPE: one repeated layout: over the limit as sent, one long once deduplicated.
    // CONTROL: the canonicalisation moved above the `modifiers.size() > kMaxImportModifiers` test in
    //       WallpaperManager::setImportModifiers (backend/daemon/wallpaper_manager.cpp) -> this, the
    //       wire tier's length check and eight cascading lines.
    check(report(consumerA, std::vector<uint64_t>(wallpaperx::WallpaperManager::kMaxImportModifiers + 1, m1)).refused,
          "an over-long report of one repeated layout is refused rather than deduplicated into a short one");

    // GOAL: S6.48 -- what is in force with two consumers reporting is the intersection, not
    //       the most recent report and not the union.
    // WHY-THIS-SHAPE: A {m1,m2} and B {m2,m3} intersect to {m2}: {m3} would pass under last-wins
    //       and union, {m1} under first-wins and union; both refused, and exactly {m2} accepted
    //       unchanged (the set is private state, X.6).
    // COVERAGE-ASSERT: the probe count is asserted at the end of the phase.
    // CONTROL: the intersection in WallpaperManager::setImportModifiers (backend/daemon/
    //       wallpaper_manager.cpp) made the caller's own list -> both refusal probes, the exact-set
    //       probe and six more.
    check(report(consumerB, {m2, m3}).changed, "a second consumer's report narrows what is in force");
    check(report(consumerC, {m3}).refused,
          "a layout only one consumer reported is refused as another consumer's whole set");
    check(report(consumerD, {m1}).refused,
          "a layout only the first consumer reported is refused as another consumer's whole set");
    const Report exact = report(consumerE, {m2});
    check(!exact.refused && !exact.changed,
          "a report of exactly the layouts in force is accepted and changes nothing");

    // GOAL: S6.49 -- a refused report leaves the daemon's effective set exactly as it was,
    //       for a caller with no earlier entry and for one with an earlier entry alike.
    // WHY-THIS-SHAPE: a leftover shows only in the stored map the next report intersects, so each
    //       rollback is graded by a later report from a different consumer.
    // CONTROL: the two `m_importModifiersByClient` restore lines disabled in
    //       WallpaperManager::setImportModifiers's empty-intersection branch (backend/daemon/
    //       wallpaper_manager.cpp) -> both rollback checks, the exact-set probe, both S6.50 probes
    //       and three more.
    check(!exact.refused, "a refused report leaves a consumer that had no earlier entry unregistered");
    check(report(consumerA, {m1}).refused, "a report that would leave no shared layout is refused");
    const Report afterRollback = report(consumerF, {m1, m2});
    check(!afterRollback.refused && !afterRollback.changed,
          "a refused report restores the caller's earlier entry, so the set in force is untouched");

    // GOAL: S6.50 -- a consumer that goes away stops constraining the exporter, and the set
    //       widens back to the intersection of those that remain.
    // WHY-THIS-SHAPE: {m3}, refused above, offered again after the departure; then everyone gone
    //       and {m1}, disjoint from the last reporter's, must pass (unconstrained, not "the last").
    // CONTROL: `m_importModifiersByClient.erase(client)` made a `count()` test in
    //       WallpaperManager::forgetImportModifiers (backend/daemon/wallpaper_manager.cpp) -> both
    //       checks, the wire tier's departure check and four more.
    manager.forgetImportModifiers(consumerA);
    manager.forgetImportModifiers(consumerE);
    manager.forgetImportModifiers(consumerF);
    const Report afterExit = report(consumerG, {m3});
    check(!afterExit.refused,
          "a layout a departed consumer excluded becomes reportable again once it is forgotten");
    manager.forgetImportModifiers(consumerB);
    manager.forgetImportModifiers(consumerG);
    check(!report(consumerH, {m1}).refused,
          "with no consumer left reporting, the exporter is unconstrained again");

    // GOAL: S6.56 -- a consumer that has reported before and is refused for an empty
    //       intersection is told how many OTHER consumers it shares no layout with.
    // WHY-THIS-SHAPE: exactly one other reporter, so counting the caller reads 2; the refusal is
    //       asserted too, or a daemon that stopped refusing would pass on an empty message.
    // CONTROL: `others` made `m_importModifiersByClient.size()` in WallpaperManager::setImportModifiers
    //       (backend/daemon/wallpaper_manager.cpp) -> this alone ("...with the 2 already reported...").
    const std::string consumerI = ":test.consumer.i";
    check(!report(consumerI, {m1, m2}).refused, "a second consumer sharing a layout with the first is accepted");
    const Report miscounted = report(consumerI, {m2});
    check(miscounted.refused && miscounted.why.find("with the 1 already reported") != std::string::npos,
          "the refusal counts the other consumers, not the caller");
    if (!miscounted.why.empty() && miscounted.why.find("with the 1 already reported") == std::string::npos) {
        std::fprintf(stderr, "  the refusal said: %s\n  (one other consumer was reporting)\n",
                     miscounted.why.c_str());
    }

    check(probes == 15, "the per-consumer phase ran every probe it grades on");
    if (probes != 15) {
        std::fprintf(stderr, "  %d report probes ran; expected 15\n", probes);
    }
}

// The per-consumer contract over the wire: a report is filed under the sender's unique name, and
// DbusService::watchClientExits() forgets a departed reporter. Its own manager and service, no
// Device; each consumer a connection of its own, so destroying it is a real departure.
void runImportModifierClientWireChecks(const wallpaperx::WallpaperManager::Config& config, const Check& check) {
    wallpaperx::WallpaperManager manager(config);

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};

    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
    // Leaver{A,B} and Stayer{B,C} give {B}, excluding Latecomer{C,D}; without the leaver {C}
    // admits it; {D} passes only once the stayer is gone too -- so forgetting the wrong client,
    // or everybody, differs from forgetting the right one.
    const std::vector<uint64_t> setLeaver{kUnavailableModifierA, kUnavailableModifierB};
    const std::vector<uint64_t> setStayer{kUnavailableModifierB, kUnavailableModifierC};
    const std::vector<uint64_t> setLatecomer{kUnavailableModifierC, kUnavailableModifierD};
    const std::vector<uint64_t> setLast{kUnavailableModifierD};

    auto consumerA = sdbus::createProxy(daemonName, objectPath);
    auto consumerStayer = sdbus::createProxy(daemonName, objectPath);
    auto consumerB = sdbus::createProxy(daemonName, objectPath);

    // GOAL: a report is filed under the caller's own bus name, so consumers on separate
    //       connections are separate reporters and not one.
    // WHY-THIS-SHAPE: one constant key would make each report replace the last and accept the third.
    // CONTROL: the `sender` in DbusService::registerVTable's SetImportModifiers handler
    //       (backend/daemon/dbus_service.cpp) made "" -> this and the stayer check below.
    setImportModifiers(*consumerA, exportedFormat, setLeaver);
    setImportModifiers(*consumerStayer, exportedFormat, setStayer);
    check(errorNameFrom([&] { setImportModifiers(*consumerB, exportedFormat, setLatecomer); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a report sharing no layout with what the other consumers reported is refused as InvalidArgs");

    // GOAL: S6.50 end to end -- a consumer that leaves the bus stops constraining the
    //       exporter, through the daemon's own NameOwnerChanged subscription.
    // WHY-THIS-SHAPE: the refused report retried, bounded, failing at the bound (X.1): the departure
    //       is handled asynchronously on the daemon's event loop.
    // CONTROL: `applyImportModifierUpdate(m_manager.forgetImportModifiers(client));` deleted from
    //       DbusService::forgetClient (backend/daemon/dbus_service.cpp) -> this ("still in force after
    //       100 attempts"), the re-render and the announcement departure checks.
    consumerA.reset();
    bool acceptedAfterExit = false;
    int attempts = 0;
    for (; attempts < 100 && !acceptedAfterExit; ++attempts) {
        try {
            setImportModifiers(*consumerB, exportedFormat, setLatecomer);
            acceptedAfterExit = true;
        } catch (const sdbus::Error&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    check(acceptedAfterExit, "a consumer that leaves the bus stops constraining the exporter");
    if (!acceptedAfterExit) {
        std::fprintf(stderr, "  the departed consumer's set was still in force after %d attempts over 5 s\n",
                     attempts);
        return;
    }

    // GOAL: only the departing consumer is forgotten, not every reporter.
    // WHY-THIS-SHAPE: {D} is disjoint from the stayer's set only, so it is refused only while the
    //       stayer is on the books.
    // CONTROL: `m_importModifiersByClient.clear();` added after the erase in
    //       WallpaperManager::forgetImportModifiers (backend/daemon/wallpaper_manager.cpp) -> this.
    auto consumerLast = sdbus::createProxy(daemonName, objectPath);
    check(errorNameFrom([&] { setImportModifiers(*consumerLast, exportedFormat, setLast); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "the consumer that stayed still constrains the exporter after another one left");

    // No control of its own (anything breaking it breaks the departure check first); the stayer
    // constrained one call ago, so this is the transition to nobody reporting.
    consumerStayer.reset();
    consumerB.reset();
    bool unconstrained = false;
    for (int attempt = 0; attempt < 100 && !unconstrained; ++attempt) {
        try {
            setImportModifiers(*consumerLast, exportedFormat, setLast);
            unconstrained = true;
        } catch (const sdbus::Error&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    check(unconstrained, "with every reporting consumer gone, the exporter is unconstrained again");
}

// ImportModifiersChanged (S6.58-S6.60): a consumer refused for another's set hears when that set
// leaves, so the signal must fire on every change and on nothing else. The manager decides
// whether the set changed (a flag beside the monitors to redraw, which the wire cannot tell
// apart); the wire shows what the flag does. Its own managers, no Device; the wire half has no
// monitors, so every change condemns nothing -- S6.60's case, and a fresh daemon's state.

// True once every signal the daemon sent before this call has been counted by a handler running on
// `listener`'s connection. That connection asks GetStatus without waiting: the daemon answers it after
// every signal it sent earlier, and the connection's one event-loop thread runs callbacks in arrival
// order, so the reply's runs after theirs. False, printed, when no reply came.
bool announcementsSettled(sdbus::IProxy& listener) {
    auto answered = std::make_shared<std::promise<void>>();
    std::future<void> done = answered->get_future();
    listener.callMethodAsync("GetStatus")
        .onInterface(DbusService::kInterface)
        .uponReplyInvoke([answered](std::optional<sdbus::Error>, std::string) { answered->set_value(); });
    if (done.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        std::fprintf(stderr, "  the listener's GetStatus had no answer in 10 s, so its count is not settled\n");
        return false;
    }
    return true;
}

// Counts a signal (ImportModifiersChanged, ConfigChanged); every wait has a failure branch (X.1).
class AnnouncementLog {
public:
    void record() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_count;
        }
        m_arrived.notify_all();
    }

    bool waitFor(int atLeast) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_arrived.wait_for(lock, std::chrono::seconds(5), [&] { return m_count >= atLeast; });
    }

    int count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_count;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_arrived;
    int m_count = 0;
};

void runImportModifierAnnouncementChecks(const wallpaperx::WallpaperManager::Config& config,
                                         const std::string& imagePath, const std::string& unrenderablePath,
                                         const Check& check) {
    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
    const std::string consumer = ":test.announce.consumer";

    {
        wallpaperx::WallpaperManager bookkeeping(config);

        // Undrawable, so never handed back: every change below condemns nothing.
        bookkeeping.requestWallpaper("shader", unrenderablePath, kMonitorAnnounceUnrenderable);

        // GOAL: S6.60 -- a change that condemns no monitor is still reported as a change.
        // WHY-THIS-SHAPE: the flag and the empty list together; over the bus "unchanged" and "nothing
        //       to redraw" are the same silence.
        // CONTROL: WallpaperManager::adoptImportModifiersLocked (backend/daemon/wallpaper_manager.cpp)
        //       returning `ImportModifierUpdate{!toRender.empty(), ...}` -> this and the wire tier's
        //       first announcement, the condemning check green; drawableType() mapping `shader` to a
        //       still -> this too.
        const wallpaperx::ImportModifierUpdate condemnsNothing =
            bookkeeping.setImportModifiers(consumer, exportedFormat, {kUnavailableModifierA, kUnavailableModifierB});
        check(condemnsNothing.changed && condemnsNothing.toRender.empty(),
              "a report that changes the layouts in force but condemns no monitor is still reported as a change");
        if (!condemnsNothing.toRender.empty()) {
            std::fprintf(stderr, "  %zu monitor(s) were handed back for re-render; only a type this build cannot "
                                 "draw was registered, so this phase's premise is gone\n",
                         condemnsNothing.toRender.size());
        }

        // GOAL: S6.60's other side -- a change that condemns a monitor is a change too.
        // WHY-THIS-SHAPE: a renderable monitor with no frame is condemned on every host; excluding a
        //       committed layout would need a real render, unreachable on an opaque-fd GPU.
        // CONTROL: `toRender.push_back(id);` deleted from WallpaperManager::adoptImportModifiersLocked
        //       (backend/daemon/wallpaper_manager.cpp) -> this and four checks reading the same list.
        bookkeeping.requestWallpaper("static", imagePath, kMonitorAnnounceCondemned);
        const wallpaperx::ImportModifierUpdate condemns =
            bookkeeping.setImportModifiers(consumer, exportedFormat, {kUnavailableModifierA});
        check(condemns.changed && !condemns.toRender.empty(),
              "a report that condemns a monitor is reported as a change too");
    }

    wallpaperx::WallpaperManager announcer(config);
    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, announcer);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};

    // Declared before the proxy whose dispatch thread writes to it, so it outlives that thread.
    AnnouncementLog announced;
    // A subscriber that reports nothing, as a consumer latched on a refusal is.
    auto listener = sdbus::createProxy(daemonName, objectPath);
    listener->uponSignal("ImportModifiersChanged")
        .onInterface(DbusService::kInterface)
        .call([&announced] { announced.record(); });

    // Each on a connection of its own, so destroying one is a real departure.
    auto reporter = sdbus::createProxy(daemonName, objectPath);
    auto departing = sdbus::createProxy(daemonName, objectPath);

    // GOAL: S6.58's first half -- a report moving the intersection reaches subscribers.
    // WHY-THIS-SHAPE: heard by a third connection that reports nothing (the audience); bounded,
    //       abandoning the phase at the bound (X.1).
    // CONTROL: the `m_object->emitSignal("ImportModifiersChanged")` call deleted from
    //       DbusService::applyImportModifierUpdate (backend/daemon/dbus_service.cpp) -> this alone.
    setImportModifiers(*reporter, exportedFormat, {kUnavailableModifierA, kUnavailableModifierB});
    const bool announcedReport = announced.waitFor(1);
    check(announcedReport, "a report that changes the layouts in force is announced to a subscriber");
    if (!announcedReport) {
        std::fprintf(stderr, "  no ImportModifiersChanged arrived within 5 s of an accepted report\n");
        return;
    }

    // The identical re-report, then a narrowing report as a barrier: signals arrive in send order,
    // so once the barrier's has, any the re-report emitted is counted. The barrier is asserted
    // (no control of its own), so a stall is not read as silence.
    setImportModifiers(*reporter, exportedFormat, {kUnavailableModifierA, kUnavailableModifierB});
    setImportModifiers(*departing, exportedFormat, {kUnavailableModifierA});
    const bool announcedBarrier = announced.waitFor(2);
    check(announcedBarrier, "a second consumer's report narrowing the layouts in force is announced");
    if (!announcedBarrier) {
        std::fprintf(stderr, "  only %d announcement(s) arrived; the silence check below has no barrier and was "
                             "abandoned\n",
                     announced.count());
        return;
    }

    // GOAL: S6.59 on the wire -- re-reporting the set in force announces nothing.
    // WHY-THIS-SHAPE: an exact count after the barrier, not right after the re-report's reply, and read
    //       once announcementsSettled() has ordered a reply behind every signal already sent: the at-least
    //       wait above can be met by an extra signal before the barrier's own has arrived.
    // CONTROL: `if (!update.changed) return;` deleted from DbusService::applyImportModifierUpdate
    //       (backend/daemon/dbus_service.cpp) -> this alone ("3 announcement(s) arrived").
    const bool importSettled = announcementsSettled(*listener);
    check(importSettled && announced.count() == 2, "re-reporting the set already in force announces nothing");
    if (announced.count() != 2) {
        std::fprintf(stderr, "  %d announcement(s) arrived across two changes and one identical re-report\n",
                     announced.count());
    }

    // GOAL: S6.58's second half -- a reporting consumer leaving the bus is announced too.
    // WHY-THIS-SHAPE: on the subscriber; a retried report shows the set widened, not that anyone
    //       was told.
    // CONTROL: DbusService::forgetClient (backend/daemon/dbus_service.cpp) calling a bare
    //       `m_manager.forgetImportModifiers(client);` -> this and S6.51's re-render check.
    departing.reset();
    const bool announcedDeparture = announced.waitFor(3);
    check(announcedDeparture, "a reporting consumer leaving the bus is announced to the others");
    if (!announcedDeparture) {
        std::fprintf(stderr, "  %d announcement(s) arrived within 5 s of the reporting consumer's departure; 3 were "
                             "expected\n",
                     announced.count());
    }
}

// ConfigChanged: what /config answers may have changed, so the plugin reads it again. Sent for an
// accepted SetWallpaper/SetWallpaperWithProperties and a SetMonitorOutput that moved a name; nothing
// else. Shader wallpapers, which this build records and never draws, so there is no Device.
constexpr int32_t kMonitorNoticeFirst = 0;
constexpr int32_t kMonitorNoticeSecond = 1;

// A manager for one no-Device phase, graded as one (PhaseDeviceGrade, below), handed to `body`.
void withNoDeviceManager(const wallpaperx::WallpaperManager::Config& config, const char* phase, const Check& check,
                         const std::function<void(wallpaperx::WallpaperManager&)>& body);

void runConfigChangedWireChecks(const wallpaperx::WallpaperManager::Config& base, const std::string& unrenderablePath,
                                const Check& check) {
    using sdbus::Variant;
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setMonitorPause;
    using wallpaperx::testing::setOutputFocus;
    using wallpaperx::testing::setOutputSize;
    using wallpaperx::testing::setOutputVisible;
    using wallpaperx::testing::setWallpaperProperties;
    using wallpaperx::testing::setWallpaperWithProperties;
    const char* kGoals = "S15.394-S15.397 (when the daemon announces ConfigChanged)";
    const std::string kName = "DP-NOTICE";

    {
        const wallpaperx::testing::ScratchDir scratch("dbusnotice");
        if (!scratch.ok()) {
            wallpaperx::testing::notVerified(kGoals, "a scratch directory for this phase's config file could not be made");
            return;
        }
        wallpaperx::WallpaperManager::Config config = base;
        config.configFile = scratch.path() / "wallpaperx" / "config.json";
        withNoDeviceManager(config, "the config-notice wire phase", check, [&](wallpaperx::WallpaperManager& manager) {
        auto serviceConn = sdbus::createSessionBusConnection();
        DbusService service(*serviceConn, manager);
        serviceConn->enterEventLoopAsync();
        const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
        const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
        AnnouncementLog announced;
        auto listener = sdbus::createProxy(daemonName, objectPath);
        listener->uponSignal("ConfigChanged").onInterface(DbusService::kInterface).call([&announced] { announced.record(); });
        auto proxyOwner = sdbus::createProxy(daemonName, objectPath);
        sdbus::IProxy& proxy = *proxyOwner;

        // Signals from the daemon's one connection arrive in the order it sent them. An exact count is read
        // after announcementsSettled(), never on the at-least wait alone, which an extra signal could meet
        // before the barrier call's own has arrived.
        // GOAL: S15.394 -- a SetMonitorOutput that names a screen announces ConfigChanged.
        // CONTROL: `if (rebound) announceConfigChanged();` deleted from DbusService::setMonitorOutput
        //       (backend/daemon/dbus_service.cpp) -> "naming a screen announces ConfigChanged" red, and the
        //       phase abandoned there.
        const std::string named = errorNameFrom([&] { setMonitorOutput(proxy, kMonitorNoticeFirst, kName); });
        check(named.empty() && announced.waitFor(1), "naming a screen announces ConfigChanged");
        if (!named.empty() || announced.count() < 1) {
            std::fprintf(stderr, "  SetMonitorOutput answered '%s'; %d announcement(s) within 5 s\n", named.c_str(),
                         announced.count());
            return;
        }

        // GOAL: S15.395 and S15.396 -- an accepted SetWallpaper announces once, and repeating a binding
        //       already on disk announces nothing.
        // WHY-THIS-SHAPE: the SetWallpaper is the repeat's barrier; an exact count after it, never a
        //       count taken when the repeat's reply came back.
        // CONTROL: `announceConfigChanged();` deleted from DbusService::setWallpaper
        //       (backend/daemon/dbus_service.cpp) -> "an accepted SetWallpaper announces ConfigChanged" red, and
        //       the repeat's line beside it (its barrier never came).
        // CONTROL: `if (rebound)` dropped from DbusService::setMonitorOutput's announcement -> "repeating
        //       a binding already on disk announces nothing" red (3 announcements).
        const std::string repeated = errorNameFrom([&] { setMonitorOutput(proxy, kMonitorNoticeFirst, kName); });
        const std::string set = errorNameFrom([&] { setWallpaper(proxy, "shader", unrenderablePath, kMonitorNoticeFirst); });
        check(repeated.empty() && set.empty() && announced.waitFor(2), "an accepted SetWallpaper announces ConfigChanged");
        const bool repeatSettled = announcementsSettled(*listener);
        check(repeatSettled && announced.count() == 2, "repeating a binding already on disk announces nothing");
        if (announced.count() != 2) {
            std::fprintf(stderr, "  repeat answered '%s', SetWallpaper '%s'; %d announcement(s)\n", repeated.c_str(),
                         set.c_str(), announced.count());
            return;
        }

        // GOAL: S15.396 -- a refused SetMonitorOutput or SetWallpaper announces nothing, and an accepted
        //       SetWallpaperWithProperties announces once.
        // CONTROL: in DbusService::setWallpaper (backend/daemon/dbus_service.cpp), `announceConfigChanged();`
        //       moved above the `try` -> "refused calls announce nothing" red.
        // COVERAGE-ASSERT: both calls must have been refused, or their silence is about something else.
        const std::string emptyName = errorNameFrom([&] { setMonitorOutput(proxy, kMonitorNoticeSecond, ""); });
        const std::string badType = errorNameFrom([&] { setWallpaper(proxy, "nonsense", unrenderablePath, kMonitorNoticeSecond); });
        check(!emptyName.empty() && !badType.empty(), "the two refused calls were refused");
        const std::string withProperties = errorNameFrom([&] {
            setWallpaperWithProperties(proxy, "shader", unrenderablePath, kMonitorNoticeSecond, {{"speed", Variant(0.5)}});
        });
        check(withProperties.empty() && announced.waitFor(3), "an accepted SetWallpaperWithProperties announces ConfigChanged");
        const bool refusedSettled = announcementsSettled(*listener);
        check(refusedSettled && announced.count() == 3, "refused calls announce nothing");
        if (announced.count() != 3) {
            std::fprintf(stderr, "  refused with '%s' and '%s'; %d announcement(s)\n", emptyName.c_str(), badType.c_str(),
                         announced.count());
            return;
        }

        // GOAL: S15.396 -- a screen's reports and a monitor's settings announce nothing: SetOutputSize,
        //       SetOutputVisible, SetOutputFocus, SetMonitorPause, SetWallpaperProperties. And S15.394's
        //       other half: moving a name to another monitor announces once.
        // CONTROL: `announceConfigChanged();` added at the end of DbusService::setOutputSize
        //       (backend/daemon/dbus_service.cpp) -> "a screen's reports and a monitor's settings announce
        //       nothing" red.
        // COVERAGE-ASSERT: all five must have been accepted, so their silence is not a refusal's.
        const std::string reports = errorNameFrom([&] {
            setOutputSize(proxy, kName, 800, 600);
            setOutputVisible(proxy, kName, false);
            setOutputFocus(proxy, kName, true, false);
            setMonitorPause(proxy, kMonitorNoticeFirst, "paused_while_focused");
            setWallpaperProperties(proxy, kMonitorNoticeFirst, {{"speed", Variant(0.5)}});
        });
        check(reports.empty(), "the five report and setting calls were accepted");
        if (!reports.empty()) std::fprintf(stderr, "  one answered '%s'\n", reports.c_str());
        const std::string moved = errorNameFrom([&] { setMonitorOutput(proxy, kMonitorNoticeSecond, kName); });
        check(moved.empty() && announced.waitFor(4), "moving a name to another monitor announces ConfigChanged");
        const bool reportsSettled = announcementsSettled(*listener);
        check(reportsSettled && announced.count() == 4, "a screen's reports and a monitor's settings announce nothing");
        if (announced.count() != 4) std::fprintf(stderr, "  %d announcement(s)\n", announced.count());
        });
    }

    // GOAL: S15.397 -- a SetMonitorOutput whose config write fails still announces ConfigChanged:
    //       the binding may stand for this run, and the daemon cannot tell that from one undone.
    // WHY-THIS-SHAPE: the config file's directory is under a regular file, so the write fails on any
    //       host; the refusal is required to be RenderFailed, the write-failure answer, or the phase
    //       is not verified -- an InvalidArgs would mean the call never reached the write.
    // CONTROL: `announceConfigChanged();` deleted from DbusService::setMonitorOutput's failure branch
    //       (backend/daemon/dbus_service.cpp) -> "a SetMonitorOutput whose write failed still announces
    //       ConfigChanged" red.
    {
        const wallpaperx::testing::ScratchDir scratch("dbusnoticefail");
        if (!scratch.ok() || !std::ofstream(scratch.path() / "blocker").put('x')) {
            wallpaperx::testing::notVerified("S15.397", "a scratch file to block the config directory could not be made");
            return;
        }
        wallpaperx::WallpaperManager::Config config = base;
        config.configFile = scratch.path() / "blocker" / "wallpaperx" / "config.json";
        withNoDeviceManager(config, "the config-notice failed-write phase", check, [&](wallpaperx::WallpaperManager& manager) {
        auto serviceConn = sdbus::createSessionBusConnection();
        DbusService service(*serviceConn, manager);
        serviceConn->enterEventLoopAsync();
        const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
        const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
        AnnouncementLog announced;
        auto listener = sdbus::createProxy(daemonName, objectPath);
        listener->uponSignal("ConfigChanged").onInterface(DbusService::kInterface).call([&announced] { announced.record(); });
        auto proxy = sdbus::createProxy(daemonName, objectPath);

        const std::string failed = errorNameFrom([&] { setMonitorOutput(*proxy, kMonitorNoticeFirst, kName); });
        if (failed != "io.wallpaperx.Daemon.Error.RenderFailed") {
            wallpaperx::testing::notVerified("S15.397", "the staged write failure answered '" + failed +
                                                            "', not RenderFailed, so the failure branch was not reached");
            return;
        }
        check(announced.waitFor(1), "a SetMonitorOutput whose write failed still announces ConfigChanged");
        });
    }
}

// PersistenceChanged: a config write returned, so GetStatus's persistence may have changed. One per
// finished write, failed or not; none for a save that was never queued.
void runPersistenceChangedWireChecks(const wallpaperx::WallpaperManager::Config& base, const std::string& unrenderablePath,
                                     const Check& check) {
    using sdbus::Variant;
    using wallpaperx::HealthStatus;
    using wallpaperx::testing::setWallpaperProperties;

    // `layout` names where the config file goes under a fresh scratch directory; `stage` prepares it.
    // `writes` is how many writes the three calls below finish; `status` what persistence must end at.
    struct Case {
        const char* label;
        const char* goal;
        std::filesystem::path layout;
        std::function<bool(const std::filesystem::path&)> stage;
        int writes;
        HealthStatus status;
    };
    // Read-only, so the file is absent at load (no refusal) and every write fails. False under root or
    // a filesystem ignoring mode bits, where no write can be made to fail (X.3).
    const auto readOnlyDirectory = [](const std::filesystem::path& root) {
        std::error_code ec;
        std::filesystem::create_directories(root / "ro", ec);
        std::filesystem::permissions(root / "ro", std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace, ec);
        { std::ofstream probe(root / "ro" / "probe"); }
        return !ec && !std::filesystem::exists(root / "ro" / "probe");
    };
    const auto unreadable = [](const std::filesystem::path& root) {
        std::filesystem::create_directories(root / "wallpaperx");
        return static_cast<bool>(std::ofstream(root / "wallpaperx" / "config.json") << "{ not json");
    };
    const Case cases[] = {
        {"a successful write", "S15.685 (success)", "wallpaperx/config.json", nullptr, 3, HealthStatus::Ok},
        {"a failed write", "S15.685 (failure)", "ro/config.json", readOnlyDirectory, 3, HealthStatus::Fail},
        {"a refused save", "S15.685 (refused)", "wallpaperx/config.json", unreadable, 0, HealthStatus::Fail},
    };

    // GOAL: S15.685 -- each finished config write sends exactly one PersistenceChanged, whether it
    //       succeeded or failed, and a save that is refused (the file could not be read) sends none.
    // WHY-THIS-SHAPE: each call is awaited until its own signal has arrived before the next, so the
    //       250 ms coalescing cannot merge two writes and the expected count is the number of calls; the
    //       exact count is read once persistence is no longer pending and announcementsSettled() has
    //       ordered a reply behind every signal sent, never on the at-least wait alone.
    // CONTROL: control_batch.sh 2026-10-17, phase persistenceChangedWire, each alone and reverted. In the
    //       writer loop (backend/daemon/config_writer.cpp): `notice->written()` not called -> "FAIL: a
    //       successful write sends PersistenceChanged within 5 s" and the failed write's; called only when
    //       `saved.error.empty()` -> the failed write's alone; called twice -> "FAIL: a successful write sends
    //       exactly one PersistenceChanged per finished write" ("4 signal(s), 3 expected") and the failed
    //       write's. ConfigWriter::queue()'s refusal branch calling it -> "FAIL: a refused save sends exactly
    //       one ..." ("3 signal(s), 0 expected"). DbusService's constructor not installing the hook
    //       (backend/daemon/dbus_service.cpp) -> the two "within 5 s" lines.
    // COVERAGE-ASSERT: the three calls must be accepted and persistence must end Ok or Fail as staged,
    //       or the case is not verified rather than graded.
    for (const Case& c : cases) {
        const wallpaperx::testing::ScratchDir scratch("dbuspersist");
        if (!scratch.ok() || (c.stage && !c.stage(scratch.path()))) {
            wallpaperx::testing::notVerified(c.goal, "the scratch config for this case could not be staged");
            continue;
        }
        wallpaperx::WallpaperManager::Config config = base;
        config.configFile = scratch.path() / c.layout;
        withNoDeviceManager(config, "the PersistenceChanged wire phase", check, [&](wallpaperx::WallpaperManager& manager) {
            auto serviceConn = sdbus::createSessionBusConnection();
            DbusService service(*serviceConn, manager);
            serviceConn->enterEventLoopAsync();
            const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
            const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
            AnnouncementLog announced;
            auto listener = sdbus::createProxy(daemonName, objectPath);
            listener->uponSignal("PersistenceChanged").onInterface(DbusService::kInterface).call([&announced] { announced.record(); });
            auto proxyOwner = sdbus::createProxy(daemonName, objectPath);
            sdbus::IProxy& proxy = *proxyOwner;

            const std::function<void()> calls[] = {
                [&] { setWallpaper(proxy, "shader", unrenderablePath, kMonitorNoticeFirst); },
                [&] { setWallpaperProperties(proxy, kMonitorNoticeFirst, {{"speed", Variant(0.5)}}); },
                [&] { setWallpaperProperties(proxy, kMonitorNoticeFirst, {{"speed", Variant(2.0)}}); },
            };
            int expected = 0;
            for (const auto& call : calls) {
                const std::string refused = errorNameFrom(call);
                if (!refused.empty()) {
                    wallpaperx::testing::notVerified(c.goal, std::string("a call was refused with ") + refused);
                    return;
                }
                if (c.writes > 0) {
                    ++expected;
                    if (!announced.waitFor(expected)) {
                        check(false, (std::string(c.label) + " sends PersistenceChanged within 5 s").c_str());
                        std::fprintf(stderr, "  %d signal(s) after call %d\n", announced.count(), expected);
                        return;
                    }
                }
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (manager.snapshot().health.persistence.status == HealthStatus::Pending &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            const wallpaperx::HealthCheck persistence = manager.snapshot().health.persistence;
            if (persistence.status != c.status) {
                wallpaperx::testing::notVerified(c.goal, std::string("persistence ended ") + wallpaperx::toString(persistence.status) +
                                                             " (" + persistence.detail + "), not as staged");
                return;
            }
            const bool settled = announcementsSettled(*listener);
            check(settled && announced.count() == c.writes,
                  (std::string(c.label) + " sends exactly one PersistenceChanged per finished write").c_str());
            if (announced.count() != c.writes) {
                std::fprintf(stderr, "  %d signal(s), %d expected\n", announced.count(), c.writes);
            }
        });
    }
}

// Every task the worker is handed is answered exactly once: a dropped export task (queue full,
// worker stopping) would leave its caller waiting out sd-bus's ~25 s timeout. Driven at the worker,
// since no bus sequence stages either state; on the coalescing phase's manager (no renders here, so
// no second Device user), last in that phase, since it stops a worker.
void runWorkerTaskAnswerChecks(wallpaperx::WallpaperManager& manager, const Check& check) {
    using TaskState = wallpaperx::RenderWorker::TaskState;
    constexpr int kMax = static_cast<int>(wallpaperx::RenderWorker::kMaxQueuedTasks);

    // Every answer, counted; read only once the worker is joined.
    struct Answers {
        std::atomic<int> invocations{0};
        std::atomic<int> ran{0};
        std::atomic<int> shuttingDown{0};
        std::atomic<int> busy{0};
        void record(TaskState state) {
            ++invocations;
            if (state == TaskState::Run) ++ran;
            else if (state == TaskState::ShuttingDown) ++shuttingDown;
            else ++busy;
        }
    } answers;
    int posted = 0;

    // Released on every path out, a throw included, or the join would hang.
    struct Gate {
        std::promise<void> promise;
        std::atomic<bool> open{false};
        void release() {
            if (!open.exchange(true)) promise.set_value();
        }
        ~Gate() { release(); }
    };
    Gate holdOne;
    Gate holdTwo;
    const std::shared_future<void> gateOne = holdOne.promise.get_future().share();
    const std::shared_future<void> gateTwo = holdTwo.promise.get_future().share();

    std::promise<void> enteredOne;
    std::promise<void> enteredTwo;
    std::future<void> insideOne = enteredOne.get_future();
    std::future<void> insideTwo = enteredTwo.get_future();
    std::atomic<bool> secondTaskFinished{false};

    wallpaperx::RenderWorker worker(manager, [](int, uint64_t) {});

    // Held, so everything below is posted while the worker dequeues nothing.
    worker.postTask([&answers, &enteredOne, gateOne](TaskState state) mutable {
        answers.record(state);
        enteredOne.set_value();
        gateOne.wait();
    });
    ++posted;
    if (insideOne.wait_for(wallpaperx::testing::kSignalTimeout) != std::future_status::ready) {
        check(false, "the render worker picked up the task that holds it");
        std::fprintf(stderr, "dbus_service_test: the worker never entered the holding task, so the queue "
                             "states below cannot be staged; skipping them rather than measuring a worker "
                             "that is not running\n");
        return;
    }

    // GOAL: RenderWorker::post() ignores a negative monitor id (a hand-edited config's), so no
    //       phantom job spends one of the task queue's slots.
    // WHY-THIS-SHAPE: the queue's remaining capacity after the phantom jobs are dequeued, the only
    //       point the miscount exists ("nothing rendered" holds either way).
    // CONTROL: `|| monitorId < 0` dropped from RenderWorker::post() (backend/daemon/render_worker.cpp)
    //       -> "FAIL: a negative monitor id costs the task queue none of its capacity" ("4 task(s) fit
    //       where 1 should"), and the shutdown count as a knock-on.
    constexpr int kNegativePosts = 3;
    for (int i = 0; i < kNegativePosts; ++i) worker.post(-1 - i);

    // A second hold, after the phantom jobs, with tasks queued behind it.
    worker.postTask([&answers, &enteredTwo, gateTwo, &secondTaskFinished](TaskState state) mutable {
        answers.record(state);
        enteredTwo.set_value();
        gateTwo.wait();
        // Widens the window in which a stop() that skipped the join returns early (200 ms, once).
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        secondTaskFinished.store(true);
    });
    ++posted;

    const auto fill = [&worker, &answers, &posted] {
        worker.postTask([&answers](TaskState state) { answers.record(state); });
        ++posted;
    };
    for (int i = 1; i < kMax; ++i) fill();  // the holding task above is the first of kMax

    // GOAL: a task posted at the queue's limit is answered Busy on the posting thread, not dropped.
    // WHY-THIS-SHAPE: read right after postTask returns, since the claim is "inline".
    // CONTROL: the `else if (m_queuedTasks >= kMaxQueuedTasks)` arm deleted from RenderWorker::postTask()
    //       (backend/daemon/render_worker.cpp) -> "FAIL: a task posted past the queue limit is
    //       answered Busy before postTask returns", the capacity check and the shutdown count.
    std::atomic<TaskState> overflowAnswer{TaskState::Run};
    worker.postTask([&answers, &overflowAnswer](TaskState state) {
        answers.record(state);
        overflowAnswer.store(state);
    });
    ++posted;
    const bool refusedInline = overflowAnswer.load() == TaskState::Busy;
    check(refusedInline, "a task posted past the queue limit is answered Busy before postTask returns");
    if (!refusedInline) {
        std::fprintf(stderr, "  the task past the limit was answered %d rather than Busy (Run=0, ShuttingDown=1, "
                             "Busy=2), after %d posts against a limit of %d\n",
                     static_cast<int>(overflowAnswer.load()), posted, kMax);
    }

    // Lets the worker drain the phantom jobs and reach the second holding task.
    holdOne.release();
    if (insideTwo.wait_for(wallpaperx::testing::kSignalTimeout) != std::future_status::ready) {
        check(false, "the render worker reached the second holding task");
        std::fprintf(stderr, "dbus_service_test: the worker did not reach the second holding task; the "
                             "remaining checks are skipped rather than measured against a stalled queue\n");
        holdTwo.release();
        return;
    }

    // One slot came free with the second hold's dequeue, so exactly one more task fits.
    int accepted = 0;
    for (int i = 0; i < kNegativePosts + 2; ++i) {
        // shared_ptr: an accepted task is answered from stop()'s drain, after this frame is gone.
        const auto answer = std::make_shared<std::atomic<TaskState>>(TaskState::Run);
        worker.postTask([&answers, answer](TaskState state) {
            answers.record(state);
            answer->store(state);
        });
        ++posted;
        if (answer->load() == TaskState::Busy) break;
        ++accepted;
    }
    check(accepted == 1, "a negative monitor id costs the task queue none of its capacity");
    if (accepted != 1) {
        std::fprintf(stderr, "  %d task(s) fit where 1 should, after %d post(s) with a negative id\n", accepted,
                     kNegativePosts);
    }

    // GOAL: stop() answers every queued task ShuttingDown, and every caller of stop() waits for the
    //       join (one returning on another thread's flag would tear down state still in use).
    // WHY-THIS-SHAPE: the second stop() is released once an inline ShuttingDown proves the flag is
    //       set (a timer would race and fail a correct build); asserted only when the probe saw it.
    // CONTROL: in RenderWorker (backend/daemon/render_worker.cpp), (a) stop()'s drain loop made
    //       `dropped.clear();` -> "FAIL: every task still queued when the worker stops is answered,
    //       not dropped" and "FAIL: every task this phase posted was invoked exactly once"; (b) `if
    //       (m_stop) return;` restored at the top of stop() -> "FAIL: a second caller of stop() waits
    //       for the worker rather than returning on another thread's flag", alone, three runs of three.
    // COVERAGE-ASSERT: invocations are compared with the posts made, so fewer or doubled answers fail.
    std::atomic<bool> sawStopping{false};
    std::atomic<bool> secondStopSawItFinished{false};
    {
        std::jthread releaser([&] {
            for (int i = 0; i < 2000 && !sawStopping.load(); ++i) {
                // shared_ptr, in case the queue were not full and this were queued.
                const auto seen = std::make_shared<std::atomic<TaskState>>(TaskState::Run);
                worker.postTask([seen](TaskState state) { seen->store(state); });
                if (seen->load() == TaskState::ShuttingDown) {
                    sawStopping.store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // Released either way, so the gate never wedges the join.
            holdTwo.release();
            worker.stop();
            secondStopSawItFinished.store(secondTaskFinished.load());
        });
        worker.stop();
    }

    const int expectedShuttingDown = kMax;
    const bool answeredOnShutdown = answers.shuttingDown.load() == expectedShuttingDown;
    check(answeredOnShutdown, "every task still queued when the worker stops is answered, not dropped");
    if (!answeredOnShutdown) {
        std::fprintf(stderr, "  %d task(s) answered ShuttingDown, expected %d\n", answers.shuttingDown.load(),
                     expectedShuttingDown);
    }

    if (sawStopping.load()) {
        check(secondStopSawItFinished.load(),
              "a second caller of stop() waits for the worker rather than returning on another thread's flag");
        if (!secondStopSawItFinished.load()) {
            std::fprintf(stderr, "  stop() returned on the second thread while the task the worker was running "
                                 "had not finished\n");
        }
    } else {
        std::fprintf(stderr, "dbus_service_test: the stop flag was never observed from the second thread, so "
                             "whether that thread waits for the join was not established on this run\n");
    }

    // The third way to drop a task: handing it to a stopped worker.
    std::atomic<TaskState> afterStop{TaskState::Run};
    worker.postTask([&answers, &afterStop](TaskState state) {
        answers.record(state);
        afterStop.store(state);
    });
    ++posted;
    const bool answeredAfterStop = afterStop.load() == TaskState::ShuttingDown;
    check(answeredAfterStop, "a task posted to a worker that has stopped is answered ShuttingDown, not dropped");
    if (!answeredAfterStop) {
        std::fprintf(stderr, "  it was answered %d (Run=0, ShuttingDown=1, Busy=2)\n",
                     static_cast<int>(afterStop.load()));
    }

    const bool eachAnsweredOnce = answers.invocations.load() == posted;
    check(eachAnsweredOnce, "every task this phase posted was invoked exactly once");
    if (!eachAnsweredOnce) {
        std::fprintf(stderr, "  %d invocation(s) for %d posted task(s): %d ran, %d shutting down, %d busy\n",
                     answers.invocations.load(), posted, answers.ran.load(), answers.shuttingDown.load(),
                     answers.busy.load());
    }
}

// Five images per monitor, and who may be drawn over. Three consumers: two at their cap of two
// bring the ring to the producer's reserve, and a third, holding nothing, asks for it -- either
// of the first two would be refused by its cap anyway. Manager and wire tiers are separate phases:
// a manager with a DbusService has its Device on the render worker, so this thread may not call
// renderPending() on it.

// A phase's own manager has its own Device and validation counter. Graded in a destructor, so a
// throw cannot skip it, from a counter snapshotted after ~WallpaperManager (CaptureGradeOnExit),
// whose teardown is where the layer complains.
class PhaseDeviceGrade {
public:
    PhaseDeviceGrade(std::string label, const Check& check) : m_label(std::move(label)), m_check(check) {}

    PhaseDeviceGrade(const PhaseDeviceGrade&) = delete;
    PhaseDeviceGrade& operator=(const PhaseDeviceGrade&) = delete;

    // Must be called while `manager` is still alive.
    void capture(const wallpaperx::WallpaperManager& manager) {
        m_counter = manager.validationCounter();
        m_layerActive = manager.validationLayerActive();
    }

    // For a phase that could not render and said so through notVerified(): no Device is no FAIL.
    void expectNoDevice() { m_deviceOptional = true; }

    ~PhaseDeviceGrade() {
        int failures = 0;
        int ungraded = 0;
        {
            wallpaperx::testing::ValidationGrade grade("dbus_service_test (" + m_label + ")", failures);
            // The waiver covers a missing device here as well as a missing validator:
            // ValidationGrade waives only the latter, correctly for a probe, but a phase whose
            // manager was built with validation off legitimately has no counter at all.
            if (wallpaperx::testing::validationWaived() || m_deviceOptional) grade.expectNoDevice();
            // Its own bucket: an absent layer package says nothing about this daemon and must
            // not be charged to the check below. A package that IS present while the device
            // failed to activate it still counts as a failure, which is right.
            grade.reportUngradedTo(ungraded);
            grade.adopt(m_counter, m_layerActive);
        }
        if (ungraded > 0) {
            std::fprintf(stderr,
                         "dbus_service_test: %s's validation grade could not be made -- VK_LAYER_KHRONOS_validation "
                         "is not loaded. The phase itself is still judged.\n",
                         m_label.c_str());
        }
        // Named for what was actually judged: ValidationGrade has already printed the specific
        // verdict, so a label asserting "zero validation warnings" would contradict two of its
        // three outcomes.
        m_check(failures == 0, ("dbus_service_test: " + m_label + "'s own device passed its validation grade").c_str());
    }

private:
    std::string m_label;
    const Check& m_check;
    std::shared_ptr<const std::atomic<int>> m_counter;
    bool m_layerActive = false;
    bool m_deviceOptional = false;
};

// Snapshots the grade off `manager` as the manager's scope unwinds, on the exception path too.
// Declare it immediately after the manager it names, and the PhaseDeviceGrade before it.
struct CaptureGradeOnExit {
    PhaseDeviceGrade& grade;
    const wallpaperx::WallpaperManager& manager;
    ~CaptureGradeOnExit() { grade.capture(manager); }
};

void withNoDeviceManager(const wallpaperx::WallpaperManager::Config& config, const char* phase, const Check& check,
                         const std::function<void(wallpaperx::WallpaperManager&)>& body) {
    PhaseDeviceGrade grade(phase, check);
    grade.expectNoDevice();
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    body(manager);
}

void runPropertiesWireChecks(const wallpaperx::WallpaperManager::Config& base, const std::string& imagePath,
                             const std::string& unrenderablePath, const Check& check) {
    const wallpaperx::testing::ScratchDir scratch("dbusproperties");
    if (!scratch.ok()) {
        wallpaperx::testing::notVerified("S15.97, S15.98, S15.99 and S15.104 over the wire",
                                         "a scratch directory for this phase's config file could not be made");
        return;
    }
    wallpaperx::WallpaperManager::Config config = base;
    config.configFile = scratch.path() / "wallpaperx" / "config.json";
    PhaseDeviceGrade grade("the properties wire phase", check);
    // Nothing here needs drawing: a shader is recorded and never rendered.
    grade.expectNoDevice();
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    runPropertiesWireChecksOn(manager, config.configFile, imagePath, unrenderablePath, check);
}

// --------------------------------------------------------------------------- //
// Step 15 chunk 3c -- a monitor's pause setting and its screen's focus report on the wire: what is
// refused, which report each mode pauses on, what ends a report, and what GetStatus and the file
// show. GetStatus's `paused` is the daemon's own decision (the one the video clock asks); whether a
// paused video really stops drawing is video_wallpaper_test's pause phase. Shader wallpapers, which
// this build records and never draws.
// --------------------------------------------------------------------------- //
constexpr int32_t kMonitorPauseFocused = 0;    // paused_while_focused, answers to DP-PF
constexpr int32_t kMonitorPauseMaximized = 1;  // paused_while_maximized, answers to DP-PM
constexpr int32_t kMonitorPauseBare = 2;       // a setting and no wallpaper
constexpr int32_t kMonitorPauseKept = 3;       // a setting that must outlive two SetWallpaper calls
constexpr int32_t kMonitorPauseTaker = 4;      // paused_while_focused, takes DP-PF from monitor 0

void runPauseWireChecks(const wallpaperx::WallpaperManager::Config& base, const std::string& unrenderablePath,
                        const Check& check) {
    using wallpaperx::testing::pauseStatus;
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setMonitorPause;
    using wallpaperx::testing::setOutputFocus;
    using wallpaperx::testing::setWallpaperWithProperties;
    constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
    constexpr const char* kUnknownOutput = "io.wallpaperx.Daemon.Error.UnknownOutput";
    const char* kGoals = "S15.121-S15.131 (the pause setting and the focus report on the wire)";
    const wallpaperx::testing::ScratchDir scratch("dbuspause");
    if (!scratch.ok()) {
        wallpaperx::testing::notVerified(kGoals, "a scratch directory for this phase's config file could not be made");
        return;
    }
    wallpaperx::WallpaperManager::Config config = base;
    config.configFile = scratch.path() / "wallpaperx" / "config.json";
    PhaseDeviceGrade grade("the pause wire phase", check);
    grade.expectNoDevice();
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    auto proxyOwner = sdbus::createProxy(daemonName, sdbus::ObjectPath{DbusService::kObjectPath});
    sdbus::IProxy& proxy = *proxyOwner;

    // The monitor's `pause` entry, or null -- which also stands for a GetStatus that failed, so no
    // check below reads null as an answer.
    const auto pauseOf = [&proxy](int32_t monitorId) -> nlohmann::json {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = pauseStatus(status, std::to_string(monitorId));
        return entry != nullptr ? *entry : nlohmann::json(nullptr);
    };
    const auto paused = [&pauseOf](int32_t monitorId) -> std::optional<bool> {
        const nlohmann::json entry = pauseOf(monitorId);
        if (!entry.is_object() || !entry.contains("paused") || !entry["paused"].is_boolean()) return std::nullopt;
        return entry["paused"].get<bool>();
    };
    const auto pausedIs = [&paused](int32_t monitorId, bool expected) {
        const std::optional<bool> now = paused(monitorId);
        return now && *now == expected;
    };

    const std::string setupError = errorNameFrom([&] {
        setMonitorOutput(proxy, kMonitorPauseFocused, "DP-PF");
        setMonitorOutput(proxy, kMonitorPauseMaximized, "DP-PM");
        setWallpaper(proxy, "shader", unrenderablePath, kMonitorPauseFocused);
        setWallpaper(proxy, "shader", unrenderablePath, kMonitorPauseMaximized);
        setWallpaper(proxy, "shader", unrenderablePath, kMonitorPauseKept);
        setMonitorPause(proxy, kMonitorPauseFocused, "paused_while_focused");
        setMonitorPause(proxy, kMonitorPauseMaximized, "paused_while_maximized");
    });
    // CONTROL: via control_batch.sh, --phase '^pauseWire$', 2026-10-01, PauseMode::WhileWindowMaximized dropped
    //       from kPauseModes (backend/daemon/pause_mode.h) -> this line red: SetMonitorPause refuses
    //       paused_while_maximized. Reverted.
    check(setupError.empty(), "the two window modes can be set on two named monitors");
    if (!setupError.empty()) {
        std::fprintf(stderr, "  the setup answered '%s'\n", setupError.c_str());
        return;
    }

    // GOAL: S15.125 -- before any SetOutputFocus, a screen is taken to have no focused and no
    //       maximized window, so both window modes play, and GetStatus shows both window fields false.
    // CONTROL: via control_batch.sh, --phase '^pauseWire$', re-run 2026-10-11, in pausedLocked()
    //       (backend/daemon/wallpaper_manager.cpp), the focused case made `return report == nullptr ||
    //       report->focus == ScreenFocus::Window;` -> "FAIL: before any focus report, both window modes play",
    //       beside the S15.126 and S15.127 lines that need an unreported screen to play. Reverted.
    {
        const nlohmann::json focused = pauseOf(kMonitorPauseFocused);
        const nlohmann::json maximized = pauseOf(kMonitorPauseMaximized);
        const auto unreported = [](const nlohmann::json& entry, const char* mode) {
            return entry.is_object() && entry == nlohmann::json({{"mode", mode},
                                                                 {"paused", false},
                                                                 {"focus", "none"},
                                                                 {"window_focused", false},
                                                                 {"window_maximized", false}});
        };
        const bool plays = unreported(focused, "paused_while_focused") && unreported(maximized, "paused_while_maximized");
        check(plays, "before any focus report, both window modes play");
        if (!plays) std::fprintf(stderr, "  monitor 0: %s; monitor 1: %s\n", focused.dump().c_str(), maximized.dump().c_str());
    }

    // GOAL: S15.124 -- each window mode pauses on its own field of the report and ignores the other:
    //       paused_while_focused on window_focused, paused_while_maximized on window_maximized; and
    //       S15.131's window half -- GetStatus's two window fields are the screen's last report.
    //       S15.662 -- a screen whose focus another screen's report took resumes paused_while_focused.
    // WHY-THIS-SHAPE: focus is reported on one screen at a time, since a focus report now takes the
    //       focus from every other screen; then both are sent only the maximized bit, which takes nothing.
    //       So each mode is seen paused by its own field and playing under the other's. One report of
    //       both bits would pass a mode that read either field.
    // CONTROL: control_batch.sh 2026-10-16, setOutputFocus's take-the-focus loop deleted
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a screen whose focus another screen's report took
    //       resumes paused_while_focused", alone. Reverted.
    //       Via control_batch.sh, --phase '^pauseWire$', re-run 2026-10-16, the two report terms
    //       (`report->focus == ScreenFocus::Window`, `report->windowMaximized`) swapped in pausedLocked()
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a focused window pauses paused_while_focused and
    //       not paused_while_maximized" and "FAIL: a maximized window pauses paused_while_maximized and not
    //       paused_while_focused", with the resume line and the later checks that rely on a focused report
    //       pausing. Reverted. For
    //       the window fields: snapshot()'s `status.focus = report->focus;` made `= ScreenFocus::None;` ->
    //       "FAIL: GetStatus shows what each screen last reported about its windows", alone. Reverted.
    {
        const std::string first = errorNameFrom([&] { setOutputFocus(proxy, "DP-PF", true, false); });
        const bool byFocus = first.empty() && pausedIs(kMonitorPauseFocused, true) && pausedIs(kMonitorPauseMaximized, false);
        const nlohmann::json focusedReport = pauseOf(kMonitorPauseFocused);
        const std::string taker = errorNameFrom([&] { setOutputFocus(proxy, "DP-PM", true, false); });
        const nlohmann::json takenReport = pauseOf(kMonitorPauseFocused);
        const bool resumed = byFocus && taker.empty() && takenReport.is_object() && takenReport.value("paused", true) == false &&
                             takenReport.value("focus", std::string()) == "none" && pausedIs(kMonitorPauseMaximized, false);
        check(resumed, "a screen whose focus another screen's report took resumes paused_while_focused");
        if (!resumed) std::fprintf(stderr, "  answered '%s'; after DP-PM's report: %s\n", taker.c_str(), takenReport.dump().c_str());
        const std::string second = errorNameFrom([&] {
            setOutputFocus(proxy, "DP-PF", false, true);
            setOutputFocus(proxy, "DP-PM", false, true);
        });
        const bool byMaximized =
            second.empty() && pausedIs(kMonitorPauseFocused, false) && pausedIs(kMonitorPauseMaximized, true);
        const nlohmann::json maximizedReport = pauseOf(kMonitorPauseMaximized);
        check(byFocus, "a focused window pauses paused_while_focused and not paused_while_maximized");
        check(byMaximized, "a maximized window pauses paused_while_maximized and not paused_while_focused");
        const bool fields = focusedReport.is_object() && focusedReport.value("window_focused", false) &&
                            !focusedReport.value("window_maximized", true) && maximizedReport.is_object() &&
                            !maximizedReport.value("window_focused", true) &&
                            maximizedReport.value("window_maximized", false);
        check(fields, "GetStatus shows what each screen last reported about its windows");
        if (!byFocus || !byMaximized || !fields) {
            std::fprintf(stderr, "  answered '%s' then '%s'; after the focused report: %s; after the maximized: %s\n",
                         first.c_str(), second.c_str(), focusedReport.dump().c_str(), maximizedReport.dump().c_str());
        }
    }

    // GOAL: S15.126 -- a report ends when its sender leaves the bus, with no other call: a screen whose
    //       report came from a connection that closed is taken to have no focused window again. Only
    //       that sender's reports go; another client's report on another screen stands.
    // WHY-THIS-SHAPE: the report comes from a second connection that is then closed, and nothing else
    //       is sent. The wait has a deadline and a failure branch. DP-PM's report, sent on this phase's
    //       own connection, is what makes "only that sender's" checkable: a forget that cleared every
    //       report would pass the first half.
    // CONTROL: via control_batch.sh, --phase '^pauseWire$', 2026-09-30, the `if
    //       (m_manager.forgetOutputFocus(client)) m_worker.wake();` line deleted from
    //       DbusService::forgetClient (backend/daemon/dbus_service.cpp) -> "FAIL: a focus report ends when
    //       the connection that sent it leaves the bus", alone, "10 s after it left" still paused.
    //       forgetOutputFocus() (backend/daemon/wallpaper_manager.cpp) made to erase every report,
    //       `std::erase_if(m_outputFocus, [](const auto&) { return true; })` -> "FAIL: and only that sender's
    //       reports end", and S15.127's second half, which had no report left to lose. Reverted.
    {
        const std::string cleared = errorNameFrom([&] { setOutputFocus(proxy, "DP-PF", false, false); });
        bool reported = false;
        std::string sent;
        {
            auto reporter = sdbus::createProxy(daemonName, sdbus::ObjectPath{DbusService::kObjectPath});
            sent = errorNameFrom([&] { setOutputFocus(*reporter, "DP-PF", true, false); });
            reported = sent.empty() && pausedIs(kMonitorPauseFocused, true);
        }
        check(cleared.empty() && reported, "a second connection's focus report pauses its screen (the premise)");
        if (!cleared.empty() || !reported) {
            std::fprintf(stderr, "  the clear answered '%s', the report '%s'; entry: %s\n", cleared.c_str(), sent.c_str(),
                         pauseOf(kMonitorPauseFocused).dump().c_str());
        } else {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool ended = pausedIs(kMonitorPauseFocused, false);
            while (!ended && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                ended = pausedIs(kMonitorPauseFocused, false);
            }
            check(ended, "a focus report ends when the connection that sent it leaves the bus");
            if (!ended) std::fprintf(stderr, "  10 s after it left: %s\n", pauseOf(kMonitorPauseFocused).dump().c_str());
            check(pausedIs(kMonitorPauseMaximized, true), "and only that sender's reports end");
        }
    }

    // GOAL: S15.127 -- a report goes with its screen name's binding: moving a name to another monitor
    //       leaves neither monitor paused by it (the one losing the name has no screen, the one taking
    //       it has had no report), and a name a monitor gives up does not bring its old report back
    //       when it is bound again.
    // WHY-THIS-SHAPE: the monitor taking the name is in the same focused mode, so a report that
    //       travelled with the name pauses it. The second half renames monitor 1 away from DP-PM and
    //       back: a report left on the unbound name is invisible until the name is bound again.
    // CONTROL: via control_batch.sh, --phase '^pauseWire$', 2026-09-30, in bindOutputLocked()
    //       (backend/daemon/wallpaper_manager.cpp), each alone: `m_outputFocus.erase(existing->first);`
    //       deleted -> "FAIL: a screen name moved to another monitor takes no focus report with it",
    //       monitor 4 reading "paused":true; the loop's `m_outputFocus.erase(it->first);` deleted -> "FAIL: a
    //       name a monitor gave up brings no old report back when bound again", monitor 1 paused. Each alone.
    //       Reverted.
    {
        const std::string moved = errorNameFrom([&] {
            setOutputFocus(proxy, "DP-PF", true, false);
            setMonitorPause(proxy, kMonitorPauseTaker, "paused_while_focused");
        });
        const bool premise = moved.empty() && pausedIs(kMonitorPauseFocused, true);
        const std::string taken = errorNameFrom([&] { setMonitorOutput(proxy, kMonitorPauseTaker, "DP-PF"); });
        const bool noTravel = premise && taken.empty() && pausedIs(kMonitorPauseTaker, false) &&
                              pausedIs(kMonitorPauseFocused, false);
        check(noTravel, "a screen name moved to another monitor takes no focus report with it");
        if (!noTravel) {
            std::fprintf(stderr, "  answered '%s', '%s'; monitor 0: %s; monitor 4: %s\n", moved.c_str(), taken.c_str(),
                         pauseOf(kMonitorPauseFocused).dump().c_str(), pauseOf(kMonitorPauseTaker).dump().c_str());
        }
        const bool maximizedBefore = pausedIs(kMonitorPauseMaximized, true);
        const std::string away = errorNameFrom([&] { setMonitorOutput(proxy, kMonitorPauseMaximized, "DP-PM2"); });
        const std::string back = errorNameFrom([&] { setMonitorOutput(proxy, kMonitorPauseMaximized, "DP-PM"); });
        const bool noReturn = maximizedBefore && away.empty() && back.empty() && pausedIs(kMonitorPauseMaximized, false);
        check(noReturn, "a name a monitor gave up brings no old report back when bound again");
        if (!noReturn) {
            std::fprintf(stderr, "  paused before %s; answered '%s', '%s'; monitor 1: %s\n",
                         maximizedBefore ? "yes" : "no", away.c_str(), back.c_str(),
                         pauseOf(kMonitorPauseMaximized).dump().c_str());
        }
    }

    // GOAL: S15.130 -- play is written as no field: pausing a monitor and setting it back to play
    //       leaves the config file byte for byte as it was before the pause, with no `pause` key.
    // CONTROL: via control_batch.sh, --phase '^pauseWire$', 2026-09-30, in
    //       WallpaperManager::setMonitorPause (backend/daemon/wallpaper_manager.cpp), `if (*parsed !=
    //       PauseMode::Play) next = toJsonText(*parsed);` made unconditional -> "FAIL: play writes no
    //       pause field, leaving the file as it was before the pause", alone. Reverted.
    {
        const std::string before = configTextOnceIdle(manager, config.configFile);
        const std::string pauseError = errorNameFrom([&] { setMonitorPause(proxy, kMonitorPauseKept, "always_paused"); });
        const std::string pausedText = configTextOnceIdle(manager, config.configFile);
        const std::string playError = errorNameFrom([&] { setMonitorPause(proxy, kMonitorPauseKept, "play"); });
        const std::string after = configTextOnceIdle(manager, config.configFile);
        const nlohmann::json row = configRowOf(after, kMonitorPauseKept);
        const bool asBefore = pauseError.empty() && playError.empty() && !before.empty() && pausedText != before &&
                              after == before && row.is_object() && !row.contains("pause");
        check(asBefore, "play writes no pause field, leaving the file as it was before the pause");
        if (!asBefore) {
            std::fprintf(stderr, "  answered '%s', '%s'\n  before: %s\n  paused: %s\n  after:  %s\n", pauseError.c_str(),
                         playError.c_str(), before.c_str(), pausedText.c_str(), after.c_str());
        }
    }

    // GOAL: S15.128 -- the setting belongs to the monitor, not the wallpaper: SetWallpaper and
    //       SetWallpaperWithProperties leave it in force, in GetStatus and in the config file.
    // CONTROL: via control_batch.sh, --phase '^pauseWire$', 2026-09-30, `m_pauseSettings.erase(monitorId);`
    //       added to WallpaperManager::requestWallpaper's commit (backend/daemon/wallpaper_manager.cpp) ->
    //       "FAIL: a pause setting survives SetWallpaper and SetWallpaperWithProperties, in GetStatus and
    //       in the file", the entry reading "mode":"play", alone. Reverted.
    {
        const std::string answered = errorNameFrom([&] {
            setMonitorPause(proxy, kMonitorPauseKept, "always_paused");
            setWallpaper(proxy, "shader", unrenderablePath, kMonitorPauseKept);
            setWallpaperWithProperties(proxy, "shader", unrenderablePath, kMonitorPauseKept,
                                       {{"speed", sdbus::Variant(2.0)}});
        });
        const nlohmann::json entry = pauseOf(kMonitorPauseKept);
        const std::string text = configTextOnceIdle(manager, config.configFile);
        const nlohmann::json row = configRowOf(text, kMonitorPauseKept);
        const bool kept = answered.empty() && entry.is_object() && entry.value("mode", std::string()) == "always_paused" &&
                          entry.value("paused", false) && !text.empty() && row.is_object() &&
                          row.value("pause", nlohmann::json()) == "always_paused";
        check(kept, "a pause setting survives SetWallpaper and SetWallpaperWithProperties, in GetStatus and in the file");
        if (!kept) {
            std::fprintf(stderr, "  answered '%s'; entry: %s; file row: %s; file: %s\n", answered.c_str(), entry.dump().c_str(),
                         row.dump().c_str(), text.empty() ? "(write failed)" : text.c_str());
        }
    }

    // GOAL: S15.129 -- GetStatus's `pause` lists a monitor that has a setting and no wallpaper, with
    //       its mode.
    // CONTROL: via control_batch.sh, --phase '^pauseWire$', 2026-09-30, the second loop in snapshot()
    //       (backend/daemon/wallpaper_manager.cpp), over m_pauseSettings, deleted -> "FAIL: a monitor with
    //       a pause setting and no wallpaper is listed in GetStatus's pause object", and S15.127's first
    //       line, whose monitor 4 has no wallpaper either. Reverted. The file half is S15.133's, graded
    //       in config_store_test.
    {
        const std::string answered = errorNameFrom([&] { setMonitorPause(proxy, kMonitorPauseBare, "always_paused"); });
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = pauseStatus(status, std::to_string(kMonitorPauseBare));
        const bool listed = answered.empty() && status.ok() &&
                            wallpaperStatus(status, std::to_string(kMonitorPauseBare)) == nullptr && entry != nullptr &&
                            *entry == nlohmann::json({{"mode", "always_paused"},
                                                      {"paused", true},
                                                      {"focus", "none"},
                                                      {"window_focused", false},
                                                      {"window_maximized", false}});
        check(listed, "a monitor with a pause setting and no wallpaper is listed in GetStatus's pause object");
        if (!listed) std::fprintf(stderr, "  answered '%s'; status: %s\n", answered.c_str(), status.json().dump().c_str());
    }

    // GOAL: S15.122 -- SetMonitorPause refuses an unknown mode and a monitor id outside [0, 32) as
    //       InvalidArgs, and changes nothing: no monitor's pause entry moves and the file is not written.
    //       S15.123 -- SetOutputFocus refuses a name no monitor answers to as UnknownOutput, and an empty
    //       or over-long name as InvalidArgs (a name that is not UTF-8 at the manager, since sd-bus will
    //       not send one), recording nothing.
    // WHY-THIS-SHAPE: every refusal asks for a mode that would change something (always_paused on ids
    //       that have none, a focused window on a focused-mode screen), so an accepted refusal shows in
    //       the pause object or the file; the file is compared once the writer is idle.
    // CONTROL: via control_batch.sh, --phase '^pauseWire$', 2026-09-30, each alone, reverted. In
    //       WallpaperManager::setMonitorPause (backend/daemon/wallpaper_manager.cpp): the unknown-mode
    //       throw replaced by falling back to play -> "FAIL: SetMonitorPause refuses an unknown mode as
    //       InvalidArgs", all three names answered '', and "FAIL: no refused call changed a pause entry or
    //       wrote the file"; `requireValidMonitorId(monitorId);` deleted -> "FAIL: SetMonitorPause refuses a
    //       monitor id outside [0, 32) as InvalidArgs" and the same unchanged line, a "-1" entry appearing.
    //       In WallpaperManager::setOutputFocus, re-run 2026-10-11 after its ScreenFocus signature: the
    //       unbound-name throw made `return;` -> "FAIL: SetOutputFocus refuses a name no monitor answers to as
    //       UnknownOutput", alone; and its `requireValidOutputName(outputName);` deleted -> "FAIL:
    //       SetOutputFocus refuses an empty or over-long name as InvalidArgs" and the non-UTF-8 line below.
    {
        const auto snapshotAll = [&proxy] {
            const StatusReply status = getStatusOrExplain(proxy);
            return status.ok() && status.json().contains("pause") ? status.json()["pause"].dump() : std::string();
        };
        const std::string fileBefore = configTextOnceIdle(manager, config.configFile);
        const std::string pauseBefore = snapshotAll();
        bool modes = true;
        for (const char* mode : {"sometimes", "", "Always_Paused"}) {
            const std::string answer = errorNameFrom([&] { setMonitorPause(proxy, kMonitorPauseKept, mode); });
            if (answer != kInvalidArgs) {
                modes = false;
                std::fprintf(stderr, "  SetMonitorPause(3, \"%s\") answered '%s'\n", mode, answer.c_str());
            }
        }
        check(modes, "SetMonitorPause refuses an unknown mode as InvalidArgs");
        bool ids = true;
        for (const int32_t id : {-1, 32}) {
            const std::string answer = errorNameFrom([&] { setMonitorPause(proxy, id, "always_paused"); });
            if (answer != kInvalidArgs) {
                ids = false;
                std::fprintf(stderr, "  SetMonitorPause(%d, always_paused) answered '%s'\n", id, answer.c_str());
            }
        }
        check(ids, "SetMonitorPause refuses a monitor id outside [0, 32) as InvalidArgs");
        const std::string unbound = errorNameFrom([&] { setOutputFocus(proxy, "HDMI-UNBOUND", true, true); });
        check(unbound == kUnknownOutput, "SetOutputFocus refuses a name no monitor answers to as UnknownOutput");
        if (unbound != kUnknownOutput) std::fprintf(stderr, "  it answered '%s'\n", unbound.c_str());
        bool names = true;
        for (const std::string& name : {std::string(), std::string(wallpaperx::kMaxOutputNameBytes + 1, 'D')}) {
            const std::string answer = errorNameFrom([&] { setOutputFocus(proxy, name, true, true); });
            if (answer != kInvalidArgs) {
                names = false;
                std::fprintf(stderr, "  SetOutputFocus of a %zu-byte name answered '%s'\n", name.size(), answer.c_str());
            }
        }
        check(names, "SetOutputFocus refuses an empty or over-long name as InvalidArgs");
        // GOAL: S15.123's non-UTF-8 half -- the manager refuses such a name as invalid before any lookup.
        // WHY-THIS-SHAPE: UnknownOutput derives from InvalidRequest, and an unvalidated non-UTF-8 name is
        //       simply unbound, so catching InvalidRequest alone would pass with validation gone; the
        //       subclass is caught first and counts as a failure.
        // CONTROL: via control_batch.sh, --phase '^pauseWire$', re-run 2026-10-11, `requireValidOutputName(outputName);`
        //       deleted from WallpaperManager::setOutputFocus (backend/daemon/wallpaper_manager.cpp) -> this
        //       line red. Reverted.
        bool notUtf8 = false;
        try {
            manager.setOutputFocus(":1.pause", "DP-\x80", wallpaperx::ScreenFocus::Window, true);
            std::fprintf(stderr, "  a non-UTF-8 name was accepted\n");
        } catch (const wallpaperx::UnknownOutput&) {
            std::fprintf(stderr, "  a non-UTF-8 name reached the bound-name lookup\n");
        } catch (const wallpaperx::InvalidRequest&) {
            notUtf8 = true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  a non-UTF-8 name threw '%s'\n", e.what());
        }
        check(notUtf8, "the manager refuses a non-UTF-8 screen name as invalid, before looking it up");
        const std::string fileAfter = configTextOnceIdle(manager, config.configFile);
        const std::string pauseAfter = snapshotAll();
        const bool unchanged = !fileBefore.empty() && fileAfter == fileBefore && !pauseBefore.empty() && pauseAfter == pauseBefore;
        check(unchanged, "no refused call changed a pause entry or wrote the file");
        if (!unchanged) {
            std::fprintf(stderr, "  pause before: %s\n  pause after:  %s\n  file before: %s\n  file after:  %s\n",
                         pauseBefore.c_str(), pauseAfter.c_str(), fileBefore.c_str(), fileAfter.c_str());
        }
    }
}

// --------------------------------------------------------------------------- //
// Step 15 chunk 5b -- sound on every screen and per monitor, the "would be heard" answer, and the
// focus report that can name the desktop. Shader wallpapers, recorded and never drawn.
// --------------------------------------------------------------------------- //
constexpr int32_t kMonitorSoundNamed = 0;   // a wallpaper, answers to DP-SND
constexpr int32_t kMonitorSoundBare = 3;    // a sound setting first, a wallpaper later
constexpr int32_t kMonitorHeard = 0;        // a wallpaper, answers to DP-HEARD
constexpr int32_t kMonitorHeardPaused = 1;  // a wallpaper and paused_while_focused, answers to DP-HEARDP

// The parsed root of a config file, or null.
nlohmann::json configRootOf(const std::string& text) {
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    return root.is_discarded() || !root.is_object() ? nlohmann::json(nullptr) : root;
}

void runSoundWireChecks(const wallpaperx::WallpaperManager::Config& base, const std::string& unrenderablePath,
                        const Check& check) {
    using sdbus::Variant;
    using wallpaperx::testing::monitorSoundStatus;
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setMonitorSound;
    using wallpaperx::testing::setSound;
    using wallpaperx::testing::setWallpaperProperties;
    using wallpaperx::testing::setWallpaperWithProperties;
    using wallpaperx::testing::soundStatus;
    constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
    const char* kGoals = "S15.447-S15.452 (the sound settings on the wire)";
    const wallpaperx::testing::ScratchDir scratch("dbussound");
    if (!scratch.ok()) {
        wallpaperx::testing::notVerified(kGoals, "a scratch directory for this phase's config file could not be made");
        return;
    }
    wallpaperx::WallpaperManager::Config config = base;
    config.configFile = scratch.path() / "wallpaperx" / "config.json";
    withNoDeviceManager(config, "the sound wire phase", check, [&](wallpaperx::WallpaperManager& manager) {
    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
    AnnouncementLog announced;
    auto listener = sdbus::createProxy(daemonName, objectPath);
    listener->uponSignal("ConfigChanged").onInterface(DbusService::kInterface).call([&announced] { announced.record(); });
    auto proxyOwner = sdbus::createProxy(daemonName, objectPath);
    sdbus::IProxy& proxy = *proxyOwner;

    // Null also for a GetStatus that failed, so no check reads null as an answer.
    const auto soundOf = [&proxy]() -> nlohmann::json {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* sound = soundStatus(status);
        return sound != nullptr ? *sound : nlohmann::json(nullptr);
    };
    const auto monitorOf = [&proxy](int32_t monitorId) -> nlohmann::json {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = monitorSoundStatus(status, std::to_string(monitorId));
        return entry != nullptr ? *entry : nlohmann::json(nullptr);
    };
    const auto settledCount = [&]() -> std::optional<int> {
        if (!announcementsSettled(*listener)) return std::nullopt;
        return announced.count();
    };

    const std::string setupError = errorNameFrom([&] {
        setWallpaper(proxy, "shader", unrenderablePath, kMonitorSoundNamed);
        setMonitorOutput(proxy, kMonitorSoundNamed, "DP-SND");
    });
    if (!setupError.empty()) {
        wallpaperx::testing::notVerified(kGoals, "the setup SetWallpaper/SetMonitorOutput answered '" + setupError + "'");
        return;
    }

    // GOAL: S15.447 -- with no sound setting anywhere, GetStatus's `sound` reports sound on and lists the
    //       monitor holding a wallpaper as on and not heard (no screen has reported a focus).
    // CONTROL: via control_batch.sh, --phase '^soundWire$', 2026-10-11, snapshot()'s loop adding every
    //       monitor in m_monitors to out.monitorSound (backend/daemon/wallpaper_manager.cpp) deleted -> this
    //       line red, and S15.449's "SetMonitorSound(true) is reported on" line. Reverted.
    {
        const nlohmann::json sound = soundOf();
        const nlohmann::json expected = {
            {"enabled", true}, {"monitors", {{"0", {{"enabled", true}, {"heard", false}}}}}};
        check(sound == expected, "with no sound setting, GetStatus reports sound on and lists the monitor with a wallpaper");
        if (sound != expected) std::fprintf(stderr, "  sound: %s\n", sound.dump().c_str());
    }

    // GOAL: S15.448 -- SetSound(false) is reported in GetStatus and written as `"sound": false` at the
    //       config root; SetSound(true) removes it again, leaving the file byte for byte as it was.
    // WHY-THIS-SHAPE: the file is compared once the writer is idle, before and after the pair; reading
    //       only GetStatus would pass a setting that never reaches the file.
    // CONTROL: via control_batch.sh, --phase '^soundWire$', 2026-10-11, each alone, reverted: ConfigStore::save
    //       (backend/daemon/config_store.cpp) not writing the root `sound` -> "FAIL: SetSound(false) is
    //       reported off and written as "sound": false at the config root"; WallpaperManager::setSound
    //       (backend/daemon/wallpaper_manager.cpp) committing the text "true" for on in place of
    //       `commitSettingLocked(m_sound, soundJsonText(on))` -> "FAIL: SetSound(true) is reported on and
    //       leaves the file as it was, with no sound field".
    //       Re-run 2026-10-20 (T9b), control_batch.sh, after serialise came to write the root settings from one table
    //       (kRootSettings): its loop skipping `&PersistedConfig::soundJson` (backend/daemon/config_store.cpp) -> the
    //       first line, the file unchanged after SetSound(false). The setSound break not re-run (untouched).
    {
        const std::string before = configTextOnceIdle(manager, config.configFile);
        const std::string offError = errorNameFrom([&] { setSound(proxy, false); });
        const nlohmann::json offStatus = soundOf();
        const std::string offText = configTextOnceIdle(manager, config.configFile);
        const std::string onError = errorNameFrom([&] { setSound(proxy, true); });
        const nlohmann::json onStatus = soundOf();
        const std::string after = configTextOnceIdle(manager, config.configFile);
        const nlohmann::json offRoot = configRootOf(offText);
        const bool off = offError.empty() && offStatus.is_object() && offStatus.value("enabled", nlohmann::json()) == false &&
                         offRoot.is_object() && offRoot.value("sound", nlohmann::json()) == false;
        check(off, "SetSound(false) is reported off and written as \"sound\": false at the config root");
        const bool on = onError.empty() && onStatus.is_object() && onStatus.value("enabled", nlohmann::json()) == true &&
                        !before.empty() && after == before && !configRootOf(after).contains("sound");
        check(on, "SetSound(true) is reported on and leaves the file as it was, with no sound field");
        if (!off || !on) {
            std::fprintf(stderr, "  answered '%s', '%s'; status off %s, on %s\n  before: %s\n  off:    %s\n  after:  %s\n",
                         offError.c_str(), onError.c_str(), offStatus.dump().c_str(), onStatus.dump().c_str(),
                         before.c_str(), offText.c_str(), after.c_str());
        }
    }

    // GOAL: S15.449 -- SetMonitorSound(id, false) on a monitor with no wallpaper lists it in GetStatus
    //       (enabled false, not heard) and writes a row of its own carrying `"sound": false` with an empty
    //       type and path; the setting survives SetWallpaper and SetWallpaperWithProperties, in GetStatus
    //       and in the file; SetMonitorSound(id, true) removes the field from the row.
    // CONTROL: via control_batch.sh, --phase '^soundWire$', 2026-10-11, each alone, reverted. The `live.sound`
    //       loop in KeptRows::rowsToWrite (backend/daemon/kept_rows.cpp) deleted -> "FAIL: SetMonitorSound(false)
    //       on a monitor with no wallpaper is listed off and written on a row of its own" and the survival
    //       line. `m_soundSettings.erase(monitorId);` added before requestWallpaper's save
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a monitor's sound setting survives SetWallpaper
    //       and SetWallpaperWithProperties, in GetStatus and the file", alone. setMonitorSound's `if (!on)`
    //       before the lock dropped, storing off whatever is asked -> "FAIL: SetMonitorSound(true) is
    //       reported on and removes the field from the monitor's row" and S15.451's line.
    //       Re-run 2026-10-20 (T9b), control_batch.sh, after rowsToWrite gained a frame-rate loop beside it: the
    //       `live.sound` loop deleted -> the same two lines. The other two not re-run (untouched).
    {
        const std::string bareError = errorNameFrom([&] { setMonitorSound(proxy, kMonitorSoundBare, false); });
        const nlohmann::json bareEntry = monitorOf(kMonitorSoundBare);
        const std::string bareText = configTextOnceIdle(manager, config.configFile);
        const nlohmann::json bareRow = configRowOf(bareText, kMonitorSoundBare);
        const bool bare = bareError.empty() && bareEntry == nlohmann::json({{"enabled", false}, {"heard", false}}) &&
                          !bareText.empty() && bareRow.is_object() && bareRow.value("type", std::string("x")).empty() &&
                          bareRow.value("path", std::string("x")).empty() &&
                          bareRow.value("sound", nlohmann::json()) == false;
        check(bare, "SetMonitorSound(false) on a monitor with no wallpaper is listed off and written on a row of its own");
        if (!bare) {
            std::fprintf(stderr, "  answered '%s'; entry %s; row %s; file: %s\n", bareError.c_str(), bareEntry.dump().c_str(),
                         bareRow.dump().c_str(), bareText.empty() ? "(write failed)" : bareText.c_str());
        }

        const std::string setError = errorNameFrom([&] {
            setWallpaper(proxy, "shader", unrenderablePath, kMonitorSoundBare);
            setWallpaperWithProperties(proxy, "shader", unrenderablePath, kMonitorSoundBare, {{"speed", Variant(2.0)}});
        });
        const nlohmann::json keptEntry = monitorOf(kMonitorSoundBare);
        const std::string keptText = configTextOnceIdle(manager, config.configFile);
        const nlohmann::json keptRow = configRowOf(keptText, kMonitorSoundBare);
        const bool kept = setError.empty() && keptEntry.value("enabled", nlohmann::json()) == false && !keptText.empty() &&
                          keptRow.is_object() && keptRow.value("type", std::string()) == "shader" &&
                          keptRow.value("sound", nlohmann::json()) == false;
        check(kept, "a monitor's sound setting survives SetWallpaper and SetWallpaperWithProperties, in GetStatus and the file");
        if (!kept) {
            std::fprintf(stderr, "  answered '%s'; entry %s; row %s; file: %s\n", setError.c_str(), keptEntry.dump().c_str(),
                         keptRow.dump().c_str(), keptText.empty() ? "(write failed)" : keptText.c_str());
        }

        const std::string onError = errorNameFrom([&] { setMonitorSound(proxy, kMonitorSoundBare, true); });
        const nlohmann::json onEntry = monitorOf(kMonitorSoundBare);
        const std::string onText = configTextOnceIdle(manager, config.configFile);
        const nlohmann::json onRow = configRowOf(onText, kMonitorSoundBare);
        const bool cleared = onError.empty() && onEntry == nlohmann::json({{"enabled", true}, {"heard", false}}) &&
                             !onText.empty() && onRow.is_object() && !onRow.contains("sound");
        check(cleared, "SetMonitorSound(true) is reported on and removes the field from the monitor's row");
        if (!cleared) {
            std::fprintf(stderr, "  answered '%s'; entry %s; row %s; file: %s\n", onError.c_str(), onEntry.dump().c_str(),
                         onRow.dump().c_str(), onText.empty() ? "(write failed)" : onText.c_str());
        }
    }

    // GOAL: S15.450 -- SetMonitorSound refuses a monitor id outside [0, 32) as InvalidArgs and changes
    //       nothing: GetStatus's `sound` and the file are as before, and nothing is announced.
    // WHY-THIS-SHAPE: each refused call asks for sound off, which an accepted one would show in the
    //       sound object, the file and the announcement count.
    // CONTROL: via control_batch.sh, --phase '^soundWire$', 2026-10-11, `requireValidMonitorId(monitorId);`
    //       deleted from WallpaperManager::setMonitorSound (backend/daemon/wallpaper_manager.cpp) -> both lines
    //       red. Reverted.
    {
        const std::string fileBefore = configTextOnceIdle(manager, config.configFile);
        const nlohmann::json soundBefore = soundOf();
        const std::optional<int> countBefore = settledCount();
        bool refused = true;
        for (const int32_t id : {-1, 32}) {
            const std::string answer = errorNameFrom([&] { setMonitorSound(proxy, id, false); });
            if (answer != kInvalidArgs) {
                refused = false;
                std::fprintf(stderr, "  SetMonitorSound(%d, false) answered '%s'\n", id, answer.c_str());
            }
        }
        check(refused, "SetMonitorSound refuses a monitor id outside [0, 32) as InvalidArgs");
        const std::optional<int> countAfter = settledCount();
        const std::string fileAfter = configTextOnceIdle(manager, config.configFile);
        const nlohmann::json soundAfter = soundOf();
        const bool unchanged = !fileBefore.empty() && fileAfter == fileBefore && soundBefore.is_object() &&
                               soundAfter == soundBefore && countBefore && countAfter && *countAfter == *countBefore;
        check(unchanged, "a refused SetMonitorSound changes no sound entry, writes nothing and announces nothing");
        if (!unchanged) {
            std::fprintf(stderr, "  sound before %s, after %s; announcements %d -> %d\n", soundBefore.dump().c_str(),
                         soundAfter.dump().c_str(), countBefore.value_or(-1), countAfter.value_or(-1));
        }
    }

    // GOAL: S15.451 -- SetSound and SetMonitorSound announce ConfigChanged exactly once for a change of the
    //       setting and not at all for a call that repeats the setting in force.
    // WHY-THIS-SHAPE: the count is read after announcementsSettled(), which orders a reply behind every
    //       earlier signal on the listener's own connection. Waiting for "at least N" and reading the count
    //       then would pass a repeat's extra signal that had not arrived yet.
    // CONTROL: via control_batch.sh, --phase '^soundWire$', 2026-10-11, each alone, reverted: in
    //       DbusService::setSound and then DbusService::setMonitorSound (backend/daemon/dbus_service.cpp),
    //       `changed = ... || true;` -> "FAIL: a sound setting announces ConfigChanged once per change and
    //       never for a repeat", the repeat announcing 1. Re-run 2026-10-11 where the answer now comes from,
    //       WallpaperManager's two commitSettingLocked overloads (backend/daemon/wallpaper_manager.cpp):
    //       the string one's `changed` made `true`, then the map one's -> the same line, each alone.
    // COVERAGE-ASSERT: all six steps must have been compared. Controlled with DbusService::setMonitorSound
    //       made to throw InvalidRequest (backend/daemon/dbus_service.cpp) -> "FAIL: every announcement step
    //       was compared", beside the S15.449 lines. Reverted.
    {
        struct Step {
            const char* what;
            std::function<void()> call;
            int expected;
        };
        const std::vector<Step> steps = {
            {"SetSound(false), a change", [&] { setSound(proxy, false); }, 1},
            {"SetSound(false) again", [&] { setSound(proxy, false); }, 0},
            {"SetMonitorSound(0, false), a change", [&] { setMonitorSound(proxy, kMonitorSoundNamed, false); }, 1},
            {"SetMonitorSound(0, false) again", [&] { setMonitorSound(proxy, kMonitorSoundNamed, false); }, 0},
            {"SetMonitorSound(0, true), a change", [&] { setMonitorSound(proxy, kMonitorSoundNamed, true); }, 1},
            {"SetSound(true), a change", [&] { setSound(proxy, true); }, 1},
        };
        int compared = 0;
        bool exact = true;
        std::optional<int> before = settledCount();
        for (const Step& step : steps) {
            if (!before) break;
            const std::string answer = errorNameFrom(step.call);
            const std::optional<int> after = settledCount();
            if (!answer.empty() || !after) {
                std::fprintf(stderr, "  %s answered '%s'\n", step.what, answer.c_str());
                exact = false;
                break;
            }
            ++compared;
            if (*after - *before != step.expected) {
                exact = false;
                std::fprintf(stderr, "  %s announced %d time(s), expected %d\n", step.what, *after - *before, step.expected);
            }
            before = after;
        }
        check(exact, "a sound setting announces ConfigChanged once per change and never for a repeat");
        check(compared == static_cast<int>(steps.size()), "every announcement step was compared");
    }

    // GOAL: S15.452 -- `volume` is refused as InvalidArgs unless it is a number from 0 to 1, by
    //       SetWallpaperProperties and SetWallpaperWithProperties alike, and a refusal leaves the stored
    //       properties as they were; 0, 0.5 and 1 are stored.
    // CONTROL: via control_batch.sh, --phase '^soundWire$', 2026-10-11, propertyError's volume bound `*number > 1`
    //       made `> 2` (backend/daemon/wallpaper_properties.cpp) -> "FAIL: a volume outside 0 to 1, or not a
    //       number, is refused and changes nothing", alone. Reverted.
    {
        const auto propertiesOf = [&proxy]() -> nlohmann::json {
            const StatusReply status = getStatusOrExplain(proxy);
            const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorSoundNamed));
            return entry != nullptr && entry->contains("properties") ? (*entry)["properties"] : nlohmann::json(nullptr);
        };
        const std::string base = errorNameFrom([&] { setWallpaperProperties(proxy, kMonitorSoundNamed, {{"volume", Variant(0.25)}}); });
        const nlohmann::json held = propertiesOf();
        bool refused = base.empty() && held.is_object();
        const std::vector<std::pair<const char*, Variant>> bad = {
            {"1.5", Variant(1.5)}, {"-0.1", Variant(-0.1)}, {"true", Variant(true)}, {"\"#ffffff\"", Variant(std::string("#ffffff"))}};
        for (const auto& [label, value] : bad) {
            const std::string viaProperties = errorNameFrom([&] { setWallpaperProperties(proxy, kMonitorSoundNamed, {{"volume", value}}); });
            const std::string viaSet = errorNameFrom([&] {
                setWallpaperWithProperties(proxy, "shader", unrenderablePath, kMonitorSoundNamed, {{"volume", value}});
            });
            if (viaProperties != kInvalidArgs || viaSet != kInvalidArgs) {
                refused = false;
                std::fprintf(stderr, "  volume %s answered '%s' and '%s'\n", label, viaProperties.c_str(), viaSet.c_str());
            }
        }
        const nlohmann::json afterRefusals = propertiesOf();
        check(refused && afterRefusals == held, "a volume outside 0 to 1, or not a number, is refused and changes nothing");
        if (afterRefusals != held) std::fprintf(stderr, "  properties %s -> %s\n", held.dump().c_str(), afterRefusals.dump().c_str());
        bool accepted = true;
        for (const double value : {0.0, 0.5, 1.0}) {
            const std::string answer = errorNameFrom([&] { setWallpaperProperties(proxy, kMonitorSoundNamed, {{"volume", Variant(value)}}); });
            const nlohmann::json now = propertiesOf();
            if (!answer.empty() || !now.is_object() || now.value("volume", nlohmann::json()) != value) {
                accepted = false;
                std::fprintf(stderr, "  volume %g answered '%s'; properties %s\n", value, answer.c_str(), now.dump().c_str());
            }
        }
        check(accepted, "a volume of 0, 0.5 and 1 is stored");
    }
    });
}

// --------------------------------------------------------------------------- //
// The frame-rate settings and the screens' refresh rates. Shader wallpapers, recorded and never drawn.
// --------------------------------------------------------------------------- //
constexpr int32_t kMonitorFrameRateNamed = 0;  // a wallpaper, answers to DP-FR
constexpr int32_t kMonitorFrameRateOwn = 2;    // a frame-rate setting of its own and no wallpaper

void runFrameRateWireChecks(const wallpaperx::WallpaperManager::Config& base, const std::string& unrenderablePath,
                            const Check& check) {
    using wallpaperx::testing::frameRateStatus;
    using wallpaperx::testing::setFrameRate;
    using wallpaperx::testing::setKeepHigherFrameRate;
    using wallpaperx::testing::setMonitorFrameRate;
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputRefreshRate;
    using nlohmann::json;
    constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
    constexpr const char* kUnknownOutput = "io.wallpaperx.Daemon.Error.UnknownOutput";
    const char* kGoals = "S15.909-S15.910 (the frame-rate settings and screens' refresh rates on the wire)";
    const wallpaperx::testing::ScratchDir scratch("dbusframerate");
    if (!scratch.ok()) {
        wallpaperx::testing::notVerified(kGoals, "a scratch directory for this phase's config file could not be made");
        return;
    }
    wallpaperx::WallpaperManager::Config config = base;
    config.configFile = scratch.path() / "wallpaperx" / "config.json";
    bool firstRunComplete = false;
    withNoDeviceManager(config, "the frame-rate wire phase", check, [&](wallpaperx::WallpaperManager& manager) {
    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
    AnnouncementLog announced;
    auto listener = sdbus::createProxy(daemonName, objectPath);
    listener->uponSignal("ConfigChanged").onInterface(DbusService::kInterface).call([&announced] { announced.record(); });
    auto proxyOwner = sdbus::createProxy(daemonName, objectPath);
    sdbus::IProxy& proxy = *proxyOwner;

    // Null also for a GetStatus that failed, so no check reads null as an answer.
    const auto sectionOf = [&proxy]() -> json {
        const StatusReply status = getStatusOrExplain(proxy);
        const json* section = frameRateStatus(status);
        return section != nullptr ? *section : json(nullptr);
    };
    const auto monitorOf = [&sectionOf](int32_t monitorId) -> json {
        const json section = sectionOf();
        if (!section.is_object() || !section.contains("monitors") || !section["monitors"].is_object()) return nullptr;
        const auto it = section["monitors"].find(std::to_string(monitorId));
        return it != section["monitors"].end() ? *it : json(nullptr);
    };
    const auto settledCount = [&]() -> std::optional<int> {
        if (!announcementsSettled(*listener)) return std::nullopt;
        return announced.count();
    };
    const auto fileNow = [&]() { return configTextOnceIdle(manager, config.configFile); };

    const std::string setupError = errorNameFrom([&] {
        setWallpaper(proxy, "shader", unrenderablePath, kMonitorFrameRateNamed);
        setMonitorOutput(proxy, kMonitorFrameRateNamed, "DP-FR");
    });
    if (!setupError.empty()) {
        wallpaperx::testing::notVerified(kGoals, "the setup SetWallpaper/SetMonitorOutput answered '" + setupError + "'");
        return;
    }

    // GOAL: S15.909 -- with nothing set, GetStatus's `frame_rate` reports rate 60, keep_higher false, and the
    //       monitor holding a wallpaper as following the global rate, with no refresh rate and target 60.
    {
        const json section = sectionOf();
        const json expected = {{"rate", 60},
                               {"keep_higher", false},
                               {"monitors",
                                {{"0", {{"rate", nullptr}, {"follows_global", true}, {"refresh_rate", nullptr}, {"target", 60}}}}}};
        check(section == expected, "with no frame-rate setting, GetStatus reports 60, keep_higher off, and the monitor following it");
        if (section != expected) std::fprintf(stderr, "  frame_rate: %s\n", section.dump().c_str());
    }

    // GOAL: S15.909 -- SetFrameRate refuses every rate but 30, 60 and 120, and SetMonitorFrameRate every rate but
    //       0, 30, 60 and 120 or a monitor id out of range, as InvalidArgs; a refusal changes no status, writes
    //       nothing and announces nothing.
    // CONTROL: control_batch.sh 2026-10-20, --phase '^frameRateWire$', reverted: `requireFrameRate(rate);` deleted from WallpaperManager::setFrameRate,
    //       then from setMonitorFrameRate (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a frame rate other than
    //       30, 60 or 120 ... is refused as InvalidArgs" and "... changes no status, writes nothing", each.
    {
        const json sectionBefore = sectionOf();
        const std::string textBefore = fileNow();
        const std::optional<int> countBefore = settledCount();
        bool refused = true;
        for (const int32_t rate : {0, 29, 45, 59, 61, 90, 121, 240, -60}) {
            const std::string answer = errorNameFrom([&] { setFrameRate(proxy, rate); });
            if (answer != kInvalidArgs) {
                refused = false;
                std::fprintf(stderr, "  SetFrameRate(%d) answered '%s'\n", rate, answer.c_str());
            }
        }
        for (const auto& [monitorId, rate] : std::vector<std::pair<int32_t, int32_t>>{
                 {kMonitorFrameRateNamed, 45}, {kMonitorFrameRateNamed, -30}, {kMonitorFrameRateNamed, 240}, {-1, 60}}) {
            const std::string answer = errorNameFrom([&] { setMonitorFrameRate(proxy, monitorId, rate); });
            if (answer != kInvalidArgs) {
                refused = false;
                std::fprintf(stderr, "  SetMonitorFrameRate(%d, %d) answered '%s'\n", monitorId, rate, answer.c_str());
            }
        }
        check(refused, "a frame rate other than 30, 60 or 120 (or 0 for a monitor), or a bad monitor id, is refused as InvalidArgs");
        const json sectionAfter = sectionOf();
        const std::string textAfter = fileNow();
        const std::optional<int> countAfter = settledCount();
        check(sectionAfter == sectionBefore && !textBefore.empty() && textAfter == textBefore && countBefore && countAfter &&
                  *countAfter == *countBefore,
              "a refused frame rate changes no status, writes nothing and announces nothing");
        if (sectionAfter != sectionBefore || textAfter != textBefore) {
            std::fprintf(stderr, "  frame_rate %s -> %s\n  file %s\n    -> %s\n", sectionBefore.dump().c_str(),
                         sectionAfter.dump().c_str(), textBefore.c_str(), textAfter.c_str());
        }
    }

    // GOAL: S15.909 -- each accepted setting is reported and written: the global rate at the config root as
    //       `frame_rate` except 60, which is the default and leaves the file byte for byte as before; a monitor's
    //       rate on its row (a row of its own for a monitor with no wallpaper), 0 removing it; keep-higher at the
    //       root as `keep_higher_frame_rate`, false removing it. GetStatus's target follows the setting.
    // WHY-THIS-SHAPE: the file is read once the writer is idle, after every step; reading GetStatus alone would
    //       pass a setting that never reaches the file.
    // CONTROL: control_batch.sh 2026-10-20, --phase '^frameRateWire$', reverted: setFrameRate writing 60 too (wallpaper_manager.cpp) ->
    //       "FAIL: ... the config file is byte for byte as before (60 is not written)"; the `live.frameRate` loop's
    //       row write deleted (backend/daemon/kept_rows.cpp) -> "FAIL: each accepted frame-rate setting is reported
    //       ... and written" and the restart line; GetStatus's follows_global always true
    //       (backend/daemon/dbus_service.cpp) -> the same accepted-setting line.
    {
        const std::string pristine = fileNow();
        struct Step {
            const char* what;
            std::function<void()> call;
            std::function<bool(const json& root, const json& section)> holds;
        };
        const auto rowOf = [](const json& root, int32_t monitorId) -> json {
            if (!root.is_object() || !root.contains("monitors") || !root["monitors"].is_array()) return nullptr;
            for (const json& row : root["monitors"]) {
                if (row.is_object() && row.value("monitor_id", -1) == monitorId) return row;
            }
            return nullptr;
        };
        const auto monitorIn = [](const json& section, int32_t monitorId) -> json {
            if (!section.is_object() || !section.contains("monitors") || !section["monitors"].is_object()) return nullptr;
            const auto it = section["monitors"].find(std::to_string(monitorId));
            return it != section["monitors"].end() ? *it : json(nullptr);
        };
        const std::vector<Step> steps = {
            {"SetFrameRate(30)", [&] { setFrameRate(proxy, 30); },
             [&](const json& root, const json& section) {
                 return root.value("frame_rate", json()) == 30 && section.value("rate", json()) == 30 &&
                        monitorIn(section, kMonitorFrameRateNamed).value("target", json()) == 30;
             }},
            {"SetFrameRate(120)", [&] { setFrameRate(proxy, 120); },
             [&](const json& root, const json& section) {
                 return root.value("frame_rate", json()) == 120 && section.value("rate", json()) == 120 &&
                        monitorIn(section, kMonitorFrameRateNamed).value("target", json()) == 120;
             }},
            {"SetMonitorFrameRate(2, 120) on a monitor with no wallpaper", [&] { setMonitorFrameRate(proxy, kMonitorFrameRateOwn, 120); },
             [&](const json& root, const json& section) {
                 const json row = rowOf(root, kMonitorFrameRateOwn);
                 const json entry = monitorIn(section, kMonitorFrameRateOwn);
                 return row.is_object() && row.value("frame_rate", json()) == 120 && row.value("path", std::string("x")).empty() &&
                        entry.is_object() && entry.value("rate", json()) == 120 && entry.value("follows_global", true) == false &&
                        entry.value("target", json()) == 120;
             }},
            {"SetMonitorFrameRate(0, 30)", [&] { setMonitorFrameRate(proxy, kMonitorFrameRateNamed, 30); },
             [&](const json& root, const json& section) {
                 const json row = rowOf(root, kMonitorFrameRateNamed);
                 const json entry = monitorIn(section, kMonitorFrameRateNamed);
                 return row.is_object() && row.value("frame_rate", json()) == 30 && entry.is_object() &&
                        entry.value("rate", json()) == 30 && entry.value("follows_global", true) == false &&
                        entry.value("target", json()) == 30;
             }},
            {"SetMonitorFrameRate(0, 0)", [&] { setMonitorFrameRate(proxy, kMonitorFrameRateNamed, 0); },
             [&](const json& root, const json& section) {
                 const json row = rowOf(root, kMonitorFrameRateNamed);
                 const json entry = monitorIn(section, kMonitorFrameRateNamed);
                 return row.is_object() && !row.contains("frame_rate") && entry.is_object() && entry.value("rate", json(0)).is_null() &&
                        entry.value("follows_global", false) == true && entry.value("target", json()) == 120;
             }},
            {"SetMonitorFrameRate(2, 0)", [&] { setMonitorFrameRate(proxy, kMonitorFrameRateOwn, 0); },
             [&](const json& root, const json&) { return rowOf(root, kMonitorFrameRateOwn).is_null() || !rowOf(root, kMonitorFrameRateOwn).contains("frame_rate"); }},
            {"SetKeepHigherFrameRate(true)", [&] { setKeepHigherFrameRate(proxy, true); },
             [&](const json& root, const json& section) {
                 return root.value("keep_higher_frame_rate", json()) == true && section.value("keep_higher", json()) == true;
             }},
            {"SetKeepHigherFrameRate(false)", [&] { setKeepHigherFrameRate(proxy, false); },
             [&](const json& root, const json& section) {
                 return !root.contains("keep_higher_frame_rate") && section.value("keep_higher", json()) == false;
             }},
            {"SetFrameRate(60)", [&] { setFrameRate(proxy, 60); },
             [&](const json& root, const json& section) {
                 return root.is_object() && !root.contains("frame_rate") && section.value("rate", json()) == 60 &&
                        monitorIn(section, kMonitorFrameRateNamed).value("target", json()) == 60;
             }},
        };
        bool all = !pristine.empty();
        size_t compared = 0;
        for (const Step& step : steps) {
            const std::string answer = errorNameFrom(step.call);
            const std::string text = fileNow();
            const json root = configRootOf(text);
            const json section = sectionOf();
            ++compared;
            if (!answer.empty() || text.empty() || !step.holds(root, section)) {
                all = false;
                std::fprintf(stderr, "  %s answered '%s'\n    file: %s\n    frame_rate: %s\n", step.what, answer.c_str(),
                             text.c_str(), section.dump().c_str());
            }
        }
        check(all, "each accepted frame-rate setting is reported in GetStatus and written to the config file");
        check(compared == steps.size(), "every frame-rate setting step was compared");
        const std::string back = fileNow();
        check(!pristine.empty() && back == pristine,
              "with every frame-rate setting back at its default, the config file is byte for byte as before (60 is not written)");
        if (back != pristine) std::fprintf(stderr, "  before: %s\n  after:  %s\n", pristine.c_str(), back.c_str());
    }

    // GOAL: S15.909 -- each frame-rate setting announces ConfigChanged exactly once for a change and never for a
    //       repeat of the setting in force.
    // WHY-THIS-SHAPE: read after announcementsSettled(), as S15.451's; "at least N" would pass a repeat's late signal.
    // COVERAGE-ASSERT: every step compared.
    // CONTROL: control_batch.sh 2026-10-20, --phase '^frameRateWire$', reverted: DbusService::setFrameRate's `changed` forced true -> "FAIL: a frame-rate
    //       setting announces ConfigChanged once per change ..."; DbusService::setOutputRefreshRate announcing on a
    //       change (backend/daemon/dbus_service.cpp) -> the same line.
    {
        struct Step {
            const char* what;
            std::function<void()> call;
            int expected;
        };
        const std::vector<Step> steps = {
            {"SetFrameRate(30), a change", [&] { setFrameRate(proxy, 30); }, 1},
            {"SetFrameRate(30) again", [&] { setFrameRate(proxy, 30); }, 0},
            {"SetMonitorFrameRate(0, 120), a change", [&] { setMonitorFrameRate(proxy, kMonitorFrameRateNamed, 120); }, 1},
            {"SetMonitorFrameRate(0, 120) again", [&] { setMonitorFrameRate(proxy, kMonitorFrameRateNamed, 120); }, 0},
            {"SetMonitorFrameRate(0, 0), a change", [&] { setMonitorFrameRate(proxy, kMonitorFrameRateNamed, 0); }, 1},
            {"SetKeepHigherFrameRate(true), a change", [&] { setKeepHigherFrameRate(proxy, true); }, 1},
            {"SetKeepHigherFrameRate(true) again", [&] { setKeepHigherFrameRate(proxy, true); }, 0},
            {"SetKeepHigherFrameRate(false), a change", [&] { setKeepHigherFrameRate(proxy, false); }, 1},
            {"SetFrameRate(60), a change", [&] { setFrameRate(proxy, 60); }, 1},
            {"SetOutputRefreshRate(DP-FR, 50)", [&] { setOutputRefreshRate(proxy, "DP-FR", 50.0); }, 0},
            {"SetOutputRefreshRate(DP-FR, 0)", [&] { setOutputRefreshRate(proxy, "DP-FR", 0.0); }, 0},
        };
        int compared = 0;
        bool exact = true;
        std::optional<int> before = settledCount();
        for (const Step& step : steps) {
            if (!before) break;
            const std::string answer = errorNameFrom(step.call);
            const std::optional<int> after = settledCount();
            if (!answer.empty() || !after) {
                std::fprintf(stderr, "  %s answered '%s'\n", step.what, answer.c_str());
                exact = false;
                break;
            }
            ++compared;
            if (*after - *before != step.expected) {
                exact = false;
                std::fprintf(stderr, "  %s announced %d time(s), expected %d\n", step.what, *after - *before, step.expected);
            }
            before = after;
        }
        check(exact, "a frame-rate setting announces ConfigChanged once per change and never for a repeat; a refresh rate never");
        check(compared == static_cast<int>(steps.size()), "every frame-rate announcement step was compared");
    }

    // GOAL: S15.910 -- SetOutputRefreshRate refuses a rate that is not 0 or 1 to 1000 (fractions inside are fine)
    //       as InvalidArgs, and a name no monitor answers to as UnknownOutput, changing nothing.
    // CONTROL: control_batch.sh 2026-10-20, --phase '^frameRateWire$', reverted: refreshRateError's bound 10 x 1000 (backend/daemon/frame_rate_setting.h)
    //       -> "FAIL: a refresh rate other than 0 or 1 to 1000 is refused" and "... records nothing"; the
    //       UnknownOutput arm deleted from DbusService::setOutputRefreshRate -> "FAIL: ... refused as UnknownOutput".
    {
        bool refused = true;
        for (const double rate : {0.5, -1.0, 1000.5, 5000.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
            const std::string answer = errorNameFrom([&] { setOutputRefreshRate(proxy, "DP-FR", rate); });
            if (answer != kInvalidArgs) {
                refused = false;
                std::fprintf(stderr, "  SetOutputRefreshRate(DP-FR, %g) answered '%s'\n", rate, answer.c_str());
            }
        }
        check(refused, "a refresh rate other than 0 or 1 to 1000 is refused as InvalidArgs");
        const std::string unbound = errorNameFrom([&] { setOutputRefreshRate(proxy, "DP-NOBODY", 60.0); });
        check(unbound == kUnknownOutput, "a refresh rate for a name no monitor answers to is refused as UnknownOutput");
        if (unbound != kUnknownOutput) std::fprintf(stderr, "  it answered '%s'\n", unbound.c_str());
        const json entry = monitorOf(kMonitorFrameRateNamed);
        check(entry.is_object() && entry.value("refresh_rate", json(0)).is_null(), "a refused refresh rate records nothing");
        if (!entry.is_object() || !entry.value("refresh_rate", json(0)).is_null()) std::fprintf(stderr, "  monitor: %s\n", entry.dump().c_str());
    }

    // GOAL: S15.909/S15.910 -- GetStatus's target is min(setting, refresh rate), or the setting with keep-higher on;
    //       1 and 1000 are accepted, 0 clears the refresh rate, and no refresh rate is written to the file.
    // WHY-THIS-SHAPE: a refresh of 50 sits between the rates 30 and 60, so a target of max() or of the setting
    //       alone is told apart from min() at each step.
    // CONTROL: control_batch.sh 2026-10-20, --phase '^frameRateWire$', reverted: targetRateLocked returning max(), then ignoring keep-higher
    //       (wallpaper_manager.cpp), then GetStatus's refresh_rate always null (dbus_service.cpp) -> "FAIL: GetStatus's
    //       target is the lower of the setting and the refresh rate ...", each. The "never written" line has no
    //       control: no one-line break makes production write a refresh rate.
    {
        const std::string textBefore = fileNow();
        struct Step {
            const char* what;
            std::function<void()> call;
            json refresh;
            double target;
        };
        const std::vector<Step> steps = {
            {"refresh 50 at rate 60", [&] { setOutputRefreshRate(proxy, "DP-FR", 50.0); }, 50.0, 50.0},
            {"rate 30 at refresh 50", [&] { setFrameRate(proxy, 30); }, 50.0, 30.0},
            {"rate 60, keep-higher on, at refresh 50", [&] { setFrameRate(proxy, 60); setKeepHigherFrameRate(proxy, true); }, 50.0, 60.0},
            {"keep-higher off again", [&] { setKeepHigherFrameRate(proxy, false); }, 50.0, 50.0},
            {"refresh 1", [&] { setOutputRefreshRate(proxy, "DP-FR", 1.0); }, 1.0, 1.0},
            {"refresh 1000", [&] { setOutputRefreshRate(proxy, "DP-FR", 1000.0); }, 1000.0, 60.0},
            {"refresh 0 clears it", [&] { setOutputRefreshRate(proxy, "DP-FR", 0.0); }, nullptr, 60.0},
        };
        bool all = true;
        size_t compared = 0;
        for (const Step& step : steps) {
            const std::string answer = errorNameFrom(step.call);
            const json entry = monitorOf(kMonitorFrameRateNamed);
            ++compared;
            if (!answer.empty() || !entry.is_object() || entry.value("refresh_rate", json("absent")) != step.refresh ||
                entry.value("target", json()) != step.target) {
                all = false;
                std::fprintf(stderr, "  %s answered '%s'; monitor %s\n", step.what, answer.c_str(), entry.dump().c_str());
            }
        }
        check(all, "GetStatus's target is the lower of the setting and the refresh rate unless keep-higher is on; 1 and 1000 "
                   "are accepted and 0 clears it");
        check(compared == steps.size(), "every refresh-rate step was compared");
        // Leaves a refresh rate in force for the restart below to show it is not kept.
        const std::string answer = errorNameFrom([&] { setOutputRefreshRate(proxy, "DP-FR", 50.0); });
        const std::string textAfter = fileNow();
        check(answer.empty() && !textBefore.empty() && textAfter == textBefore,
              "a refresh rate is never written to the config file");
        if (textAfter != textBefore) std::fprintf(stderr, "  before: %s\n  after:  %s\n", textBefore.c_str(), textAfter.c_str());
        // The state the restart must bring back.
        const std::string set = errorNameFrom([&] {
            setFrameRate(proxy, 30);
            setKeepHigherFrameRate(proxy, true);
            setMonitorFrameRate(proxy, kMonitorFrameRateOwn, 120);
        });
        firstRunComplete = set.empty() && !fileNow().empty();
        if (!firstRunComplete) std::fprintf(stderr, "  setting the state for the restart answered '%s'\n", set.c_str());
    }
    });
    if (!firstRunComplete) {
        wallpaperx::testing::notVerified(kGoals, "the first daemon did not leave the settings the restart reads");
        return;
    }

    // GOAL: S15.909/S15.910 -- the frame-rate settings survive a restart; a screen's refresh rate does not.
    // CONTROL: control_batch.sh 2026-10-20, --phase '^frameRateWire$', reverted: ConfigStore's load dropping keep_higher_frame_rate
    //       (backend/daemon/config_store.cpp) -> "FAIL: the global rate, keep-higher and a monitor's own rate survive
    //       a restart". The refresh-rate line has no control, for the reason above.
    withNoDeviceManager(config, "the frame-rate wire phase after a restart", check, [&](wallpaperx::WallpaperManager& manager) {
        // As the daemon starts: the saved rows are read back before the bus is served. Nothing is posted,
        // so nothing is drawn.
        static_cast<void>(manager.restoreSaved());
        auto serviceConn = sdbus::createSessionBusConnection();
        DbusService service(*serviceConn, manager);
        serviceConn->enterEventLoopAsync();
        auto proxy = sdbus::createProxy(sdbus::ServiceName{serviceConn->getUniqueName()}, sdbus::ObjectPath{DbusService::kObjectPath});
        const StatusReply status = getStatusOrExplain(*proxy);
        const json* section = frameRateStatus(status);
        const json got = section != nullptr ? *section : json(nullptr);
        const auto monitorIn = [&got](int32_t monitorId) -> json {
            if (!got.is_object() || !got.contains("monitors") || !got["monitors"].is_object()) return nullptr;
            const auto it = got["monitors"].find(std::to_string(monitorId));
            return it != got["monitors"].end() ? *it : json(nullptr);
        };
        const json own = monitorIn(kMonitorFrameRateOwn);
        const json named = monitorIn(kMonitorFrameRateNamed);
        check(got.is_object() && got.value("rate", json()) == 30 && got.value("keep_higher", json()) == true && own.is_object() &&
                  own.value("rate", json()) == 120 && own.value("follows_global", true) == false,
              "the global rate, keep-higher and a monitor's own rate survive a restart");
        check(named.is_object() && named.value("refresh_rate", json(0)).is_null() && named.value("target", json()) == 30,
              "a screen's refresh rate does not survive a restart");
        std::printf("dbus_service_test: frameRateWire: after the restart frame_rate is %s\n", got.dump().c_str());
    });
}

void runFocusStateWireChecks(const wallpaperx::WallpaperManager::Config& base, const std::string& unrenderablePath,
                             const Check& check) {
    using sdbus::Variant;
    using wallpaperx::testing::monitorSoundStatus;
    using wallpaperx::testing::pauseStatus;
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setMonitorPause;
    using wallpaperx::testing::setMonitorSound;
    using wallpaperx::testing::setOutputFocus;
    using wallpaperx::testing::setOutputFocusState;
    using wallpaperx::testing::setSound;
    using wallpaperx::testing::setWallpaperProperties;
    constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
    constexpr const char* kUnknownOutput = "io.wallpaperx.Daemon.Error.UnknownOutput";
    const char* kGoals = "S15.453-S15.458 (the focus report naming the desktop, and when a screen is heard)";
    const wallpaperx::testing::ScratchDir scratch("dbusfocusstate");
    if (!scratch.ok()) {
        wallpaperx::testing::notVerified(kGoals, "a scratch directory for this phase's config file could not be made");
        return;
    }
    wallpaperx::WallpaperManager::Config config = base;
    config.configFile = scratch.path() / "wallpaperx" / "config.json";
    withNoDeviceManager(config, "the focus state wire phase", check, [&](wallpaperx::WallpaperManager& manager) {
    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
    AnnouncementLog announced;
    auto listener = sdbus::createProxy(daemonName, objectPath);
    listener->uponSignal("ConfigChanged").onInterface(DbusService::kInterface).call([&announced] { announced.record(); });
    // The reports are recorded against this connection, which stays on the bus for the whole phase.
    auto proxyOwner = sdbus::createProxy(daemonName, objectPath);
    sdbus::IProxy& proxy = *proxyOwner;

    // Null also for a GetStatus that failed.
    const auto pauseOf = [&proxy](int32_t monitorId) -> nlohmann::json {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = pauseStatus(status, std::to_string(monitorId));
        return entry != nullptr ? *entry : nlohmann::json(nullptr);
    };
    const auto heardOf = [&proxy](int32_t monitorId) -> std::optional<bool> {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = monitorSoundStatus(status, std::to_string(monitorId));
        if (entry == nullptr || !entry->contains("heard") || !(*entry)["heard"].is_boolean()) return std::nullopt;
        return (*entry)["heard"].get<bool>();
    };
    const auto heardIs = [&heardOf](int32_t monitorId, bool expected) {
        const std::optional<bool> now = heardOf(monitorId);
        return now && *now == expected;
    };
    const auto pausedIs = [&pauseOf](int32_t monitorId, bool expected) {
        const nlohmann::json entry = pauseOf(monitorId);
        return entry.is_object() && entry.contains("paused") && entry["paused"] == expected;
    };

    const std::string setupError = errorNameFrom([&] {
        setWallpaper(proxy, "shader", unrenderablePath, kMonitorHeard);
        setMonitorOutput(proxy, kMonitorHeard, "DP-HEARD");
        setWallpaper(proxy, "shader", unrenderablePath, kMonitorHeardPaused);
        setMonitorOutput(proxy, kMonitorHeardPaused, "DP-HEARDP");
        setMonitorPause(proxy, kMonitorHeardPaused, "paused_while_focused");
    });
    if (!setupError.empty()) {
        wallpaperx::testing::notVerified(kGoals, "the setup calls answered '" + setupError + "'");
        return;
    }
    if (!announcementsSettled(*listener)) {
        check(false, "the announcement count settled after the setup");
        return;
    }
    const int announcedAfterSetup = announced.count();

    // GOAL: S15.453 -- with sound on at every level, a screen is heard exactly when its last report names
    //       a window or the desktop as focused and no maximized window: never before any report, never
    //       for "none", never with a maximized or fullscreen window whatever has the focus. And S15.454 --
    //       GetStatus's pause entry echoes the reported focus by name, with window_focused true only for
    //       "window".
    // WHY-THIS-SHAPE: every combination of the three focus names and the maximized flag, one report each
    //       on one screen, so each wrong rule (desktop not counted, maximized ignored, a desktop report
    //       dropped as if empty, window_focused read off any focus) flips a different row.
    // CONTROL: via control_batch.sh, --phase '^focusStateWire$', each alone, reverted, in
    //       backend/daemon/wallpaper_manager.cpp unless named, re-run 2026-10-16: soundHeardLocked's `focus !=
    //       ScreenFocus::None` made `== ScreenFocus::Window` -> the heard line, S15.455's premise and the
    //       one-heard line; its `&& !report->windowMaximized` dropped -> the heard line alone; setOutputFocus's
    //       erase condition made `focus != ScreenFocus::Window` (a desktop report dropped) -> both lines,
    //       S15.455's premise, S15.456's desktop line and both S15.660/S15.667 lines. 2026-10-11: in getStatus (backend/daemon/dbus_service.cpp): `"focus"` hard-wired
    //       "none" -> the echo line (and S15.456, S15.458, which read it); window_focused made `focus !=
    //       ScreenFocus::None` -> the echo line alone.
    // COVERAGE-ASSERT: all seven rows must have been compared. Controlled with ScreenFocus::Desktop dropped
    //       from kScreenFocuses (backend/daemon/screen_focus.h), so "desktop" is refused -> "FAIL: every focus
    //       row was compared", beside the heard and echo lines. Reverted.
    {
        struct Row {
            const char* focus;
            bool maximized;
            bool heard;
        };
        const Row rows[] = {{"window", false, true}, {"none", false, false}, {"desktop", false, true},
                            {"window", true, false}, {"desktop", true, false}, {"none", true, false}};
        int compared = 0;
        bool heardRule = heardIs(kMonitorHeard, false);
        if (!heardRule) std::fprintf(stderr, "  before any report: heard %s\n", heardOf(kMonitorHeard) ? "true" : "(none)");
        if (heardOf(kMonitorHeard)) ++compared;
        bool echoed = true;
        for (const Row& row : rows) {
            const std::string answer = errorNameFrom([&] { setOutputFocusState(proxy, "DP-HEARD", row.focus, row.maximized); });
            const std::optional<bool> heard = heardOf(kMonitorHeard);
            const nlohmann::json entry = pauseOf(kMonitorHeard);
            if (!answer.empty() || !heard) {
                std::fprintf(stderr, "  (%s, %s) answered '%s'\n", row.focus, row.maximized ? "maximized" : "-", answer.c_str());
                heardRule = false;
                echoed = false;
                continue;
            }
            ++compared;
            if (*heard != row.heard) {
                heardRule = false;
                std::fprintf(stderr, "  (%s, %s): heard %s, expected %s\n", row.focus, row.maximized ? "maximized" : "-",
                             *heard ? "true" : "false", row.heard ? "true" : "false");
            }
            const bool matches = entry.is_object() && entry.value("focus", std::string()) == row.focus &&
                                 entry.value("window_focused", nlohmann::json()) == (std::string(row.focus) == "window") &&
                                 entry.value("window_maximized", nlohmann::json()) == row.maximized;
            if (!matches) {
                echoed = false;
                std::fprintf(stderr, "  (%s, %s): pause entry %s\n", row.focus, row.maximized ? "maximized" : "-",
                             entry.dump().c_str());
            }
        }
        check(heardRule, "a screen is heard exactly when a window or the desktop has the focus and nothing is maximized");
        check(echoed, "GetStatus echoes the reported focus by name, window_focused true only for a window");
        check(compared == 7, "every focus row was compared");
    }

    // GOAL: S15.459 -- a focus report announces no ConfigChanged: the six reports above sent none.
    // CONTROL: via control_batch.sh, --phase '^focusStateWire$', 2026-10-11, `announceConfigChanged();` added
    //       after SetOutputFocusState's setOutputFocus call (backend/daemon/dbus_service.cpp) -> this line red,
    //       alone. Reverted.
    {
        const bool settled = announcementsSettled(*listener);
        check(settled && announced.count() == announcedAfterSetup, "focus reports announce no ConfigChanged");
        if (settled && announced.count() != announcedAfterSetup) {
            std::fprintf(stderr, "  %d announcement(s) for six reports\n", announced.count() - announcedAfterSetup);
        }
    }

    // GOAL: S15.455 -- each sound level alone silences a focused screen: the wallpaper's `mute`, the
    //       screen's SetMonitorSound(false) and SetSound(false); with all three back on it is heard again.
    // WHY-THIS-SHAPE: the screen reports the desktop itself as focused (SetOutputFocusState "desktop")
    //       throughout, the state in which sound is meant to play; one level is switched off at a time and
    //       back, so a rule ignoring any one of them shows on its own line.
    // CONTROL: via control_batch.sh, --phase '^focusStateWire$', re-run 2026-10-16, each alone, reverted, one term
    //       of soundHeardLocked (backend/daemon/wallpaper_manager.cpp) dropped: the `soundOn(m_sound)` test ->
    //       "FAIL: switching off sound on every screen alone silences the screen"; the screen setting's ->
    //       "... the screen's sound ..."; `wallpaperMuted(...)` -> "... its wallpaper's mute ...". Each alone.
    {
        const std::string premiseError = errorNameFrom([&] { setOutputFocusState(proxy, "DP-HEARD", "desktop", false); });
        const bool premise = premiseError.empty() && heardIs(kMonitorHeard, true);
        check(premise, "the screen whose desktop has the focus is heard (the premise)");
        if (premise) {
            struct Level {
                const char* what;
                std::function<void()> off;
                std::function<void()> on;
            };
            const Level levels[] = {
                {"its wallpaper's mute", [&] { setWallpaperProperties(proxy, kMonitorHeard, {{"mute", Variant(true)}}); },
                 [&] { setWallpaperProperties(proxy, kMonitorHeard, {{"mute", Variant(false)}}); }},
                {"the screen's sound", [&] { setMonitorSound(proxy, kMonitorHeard, false); },
                 [&] { setMonitorSound(proxy, kMonitorHeard, true); }},
                {"sound on every screen", [&] { setSound(proxy, false); }, [&] { setSound(proxy, true); }},
            };
            for (const Level& level : levels) {
                const std::string offError = errorNameFrom(level.off);
                const bool silenced = offError.empty() && heardIs(kMonitorHeard, false);
                const std::string onError = errorNameFrom(level.on);
                const bool back = onError.empty() && heardIs(kMonitorHeard, true);
                check(silenced && back, (std::string("switching off ") + level.what + " alone silences the screen").c_str());
                if (!silenced || !back) {
                    std::fprintf(stderr, "  answered '%s', '%s'; silenced %s, heard again %s\n", offError.c_str(),
                                 onError.c_str(), silenced ? "yes" : "no", back ? "yes" : "no");
                }
            }
        }
    }

    // GOAL: S15.456 -- paused_while_focused pauses for a focused window and not for the focused desktop.
    // CONTROL: via control_batch.sh, --phase '^focusStateWire$', 2026-10-11, pausedLocked's WhileWindowFocused
    //       case made `report->focus != ScreenFocus::None` (backend/daemon/wallpaper_manager.cpp) -> "FAIL:
    //       paused_while_focused does not pause while the desktop has the focus", alone. Reverted.
    {
        const std::string desktopError = errorNameFrom([&] { setOutputFocusState(proxy, "DP-HEARDP", "desktop", false); });
        const bool desktopPlays = desktopError.empty() && pausedIs(kMonitorHeardPaused, false) &&
                                  pauseOf(kMonitorHeardPaused).value("focus", std::string()) == "desktop";
        const std::string windowError = errorNameFrom([&] { setOutputFocusState(proxy, "DP-HEARDP", "window", false); });
        const bool windowPauses = windowError.empty() && pausedIs(kMonitorHeardPaused, true);
        check(desktopPlays, "paused_while_focused does not pause while the desktop has the focus");
        check(windowPauses, "paused_while_focused pauses while a window has the focus");
        if (!desktopPlays || !windowPauses) {
            std::fprintf(stderr, "  answered '%s', '%s'; entry now %s\n", desktopError.c_str(), windowError.c_str(),
                         pauseOf(kMonitorHeardPaused).dump().c_str());
        }
    }

    // GOAL: S15.457 -- SetOutputFocusState refuses a focus name other than none, window and desktop as
    //       InvalidArgs and records nothing; a screen name no monitor answers to is UnknownOutput.
    // WHY-THIS-SHAPE: the refused calls carry maximized=true over a recorded ("window", false), so a
    //       refusal that recorded anything (as none, say) shows in the pause entry and in `heard`.
    // CONTROL: via control_batch.sh, --phase '^focusStateWire$', 2026-10-11, SetOutputFocusState's parse made
    //       `screenFocusFromName(focus).value_or(ScreenFocus::None)` (backend/daemon/dbus_service.cpp) -> "FAIL:
    //       SetOutputFocusState refuses an unknown focus name as InvalidArgs and records nothing", alone.
    //       Reverted. WallpaperManager::setOutputFocus's unbound-name throw made `return;`
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: SetOutputFocusState refuses a name no monitor
    //       answers to as UnknownOutput", alone. Reverted.
    {
        const std::string setError = errorNameFrom([&] { setOutputFocusState(proxy, "DP-HEARD", "window", false); });
        const nlohmann::json before = pauseOf(kMonitorHeard);
        bool refused = setError.empty() && before.is_object();
        for (const char* name : {"sideways", "", "Desktop", "window "}) {
            const std::string answer = errorNameFrom([&] { setOutputFocusState(proxy, "DP-HEARD", name, true); });
            if (answer != kInvalidArgs) {
                refused = false;
                std::fprintf(stderr, "  focus \"%s\" answered '%s'\n", name, answer.c_str());
            }
        }
        const nlohmann::json after = pauseOf(kMonitorHeard);
        check(refused && after == before && heardIs(kMonitorHeard, true),
              "SetOutputFocusState refuses an unknown focus name as InvalidArgs and records nothing");
        if (after != before) std::fprintf(stderr, "  entry %s -> %s\n", before.dump().c_str(), after.dump().c_str());
        const std::string unbound = errorNameFrom([&] { setOutputFocusState(proxy, "DP-NOWHERE", "desktop", false); });
        check(unbound == kUnknownOutput, "SetOutputFocusState refuses a name no monitor answers to as UnknownOutput");
        if (unbound != kUnknownOutput) std::fprintf(stderr, "  it answered '%s'\n", unbound.c_str());
    }

    // GOAL: S15.458 -- an older plugin's SetOutputFocus still works: (true, false) records a focused window
    //       (focus "window", heard), (false, false) none (not heard), (false, true) a maximized window and no
    //       focus.
    // CONTROL: via control_batch.sh, --phase '^focusStateWire$', 2026-10-11, SetOutputFocus's mapping made
    //       `ScreenFocus::None` whatever window_focused says (backend/daemon/dbus_service.cpp) -> this line red,
    //       alone. Reverted.
    {
        struct Old {
            bool focused;
            bool maximized;
            const char* focus;
            bool heard;
        };
        const Old reports[] = {{true, false, "window", true}, {false, false, "none", false}, {false, true, "none", false}};
        bool mapped = true;
        for (const Old& report : reports) {
            const std::string answer =
                errorNameFrom([&] { setOutputFocus(proxy, "DP-HEARD", report.focused, report.maximized); });
            const nlohmann::json entry = pauseOf(kMonitorHeard);
            const bool ok = answer.empty() && entry.is_object() && entry.value("focus", std::string()) == report.focus &&
                            entry.value("window_maximized", nlohmann::json()) == report.maximized &&
                            heardIs(kMonitorHeard, report.heard);
            if (!ok) {
                mapped = false;
                std::fprintf(stderr, "  SetOutputFocus(%d, %d) answered '%s'; entry %s; heard %s\n", report.focused,
                             report.maximized, answer.c_str(), entry.dump().c_str(),
                             heardOf(kMonitorHeard).value_or(false) ? "true" : "false");
            }
        }
        check(mapped, "SetOutputFocus maps a focused window to \"window\" and no focus to \"none\"");
    }

    // GOAL: S15.664-S15.666 -- SetMonitorPause, SetSound and SetMonitorSound log a line only for a call that
    //       changes the setting: three identical calls and two real changes give one, two and two lines.
    // WHY-THIS-SHAPE: stdout, where those lines go, is captured over the calls alone; the first pause call is
    //       itself a change (from paused_while_focused), the sound ones start where the setting already is.
    // CONTROL: control_batch.sh 2026-10-16, phase focusStateWire, in backend/daemon/dbus_service.cpp, each
    //       alone: setMonitorPause's `if (m_manager.setMonitorPause(...))` made unconditional -> "FAIL:
    //       SetMonitorPause logs only a change" (3 lines); setSound's `if (changed)` made `if (true)` -> "FAIL:
    //       SetSound logs only a change" (5); setMonitorSound's likewise -> "FAIL: SetMonitorSound ...". Reverted.
    {
        int pauseLines = -1, soundLines = -1, monitorLines = -1;
        std::string answer;
        {
            wallpaperx::testing::StreamCapture out(STDOUT_FILENO);
            if (out.ok()) {
                answer = errorNameFrom([&] {
                    for (int i = 0; i < 3; ++i) setMonitorPause(proxy, kMonitorHeardPaused, "play");
                    for (int i = 0; i < 3; ++i) setSound(proxy, true);
                    setSound(proxy, false);
                    setSound(proxy, true);
                    for (int i = 0; i < 3; ++i) setMonitorSound(proxy, kMonitorHeard, true);
                    setMonitorSound(proxy, kMonitorHeard, false);
                    setMonitorSound(proxy, kMonitorHeard, true);
                });
                pauseLines = out.count("SetMonitorPause(monitor=" + std::to_string(kMonitorHeardPaused) + ",");
                soundLines = out.count("SetSound(on=");
                monitorLines = out.count("SetMonitorSound(monitor=" + std::to_string(kMonitorHeard) + ",");
            }
        }
        if (pauseLines < 0) {
            wallpaperx::testing::notVerified("S15.664-S15.666", "stdout could not be captured");
        } else {
            check(answer.empty() && pauseLines == 1, "SetMonitorPause logs only a change");
            check(answer.empty() && soundLines == 2, "SetSound logs only a change");
            check(answer.empty() && monitorLines == 2, "SetMonitorSound logs only a change");
            if (!answer.empty() || pauseLines != 1 || soundLines != 2 || monitorLines != 2) {
                std::fprintf(stderr, "  answered '%s'; %d, %d, %d lines\n", answer.c_str(), pauseLines, soundLines, monitorLines);
            }
        }
    }

    // GOAL: S15.660/S15.661 -- one screen is heard at a time, the one whose focus report came last, and
    //       S15.667 -- a screen whose focus is taken keeps a maximized window it reported.
    // WHY-THIS-SHAPE: two playing screens report focus in turn and GetStatus is read for both after each, so
    //       a rule letting both be heard, or the first keep it, shows; DP-HEARDP's first report carries a
    //       maximized window, which must survive DP-HEARD's report taking its focus.
    // CONTROL: control_batch.sh 2026-10-16, phase focusStateWire, in setOutputFocus
    //       (backend/daemon/wallpaper_manager.cpp), each alone: the take-the-focus loop deleted -> "FAIL: only
    //       the screen whose focus was reported last is heard" and the maximized line (DP-HEARDP kept "window");
    //       its keep-the-maximized branch removed, every other focused report erased -> "FAIL: a screen whose
    //       focus is taken keeps its maximized window", alone. Reverted.
    {
        const std::string answer = errorNameFrom([&] {
            setMonitorPause(proxy, kMonitorHeardPaused, "play");
            setOutputFocusState(proxy, "DP-HEARDP", "window", true);
            setOutputFocusState(proxy, "DP-HEARD", "desktop", false);
        });
        const nlohmann::json kept = pauseOf(kMonitorHeardPaused);
        const bool keepsMaximized = answer.empty() && kept.is_object() && kept.value("focus", std::string()) == "none" &&
                                    kept.value("window_maximized", false);
        check(keepsMaximized, "a screen whose focus is taken keeps its maximized window");
        if (!keepsMaximized) std::fprintf(stderr, "  answered '%s'; DP-HEARDP: %s\n", answer.c_str(), kept.dump().c_str());
        struct Turn {
            const char* output;
            const char* focus;
            bool heard;
            bool heardP;
        };
        const Turn turns[] = {{"DP-HEARDP", "desktop", false, true}, {"DP-HEARD", "window", true, false},
                              {"DP-HEARDP", "window", false, true}};
        bool one = true;
        int compared = 0;
        for (const Turn& t : turns) {
            const std::string reply = errorNameFrom([&] { setOutputFocusState(proxy, t.output, t.focus, false); });
            const std::optional<bool> a = heardOf(kMonitorHeard), b = heardOf(kMonitorHeardPaused);
            if (reply.empty() && a && b) ++compared;
            if (!reply.empty() || !a || !b || *a != t.heard || *b != t.heardP) {
                one = false;
                std::fprintf(stderr, "  after %s %s: answered '%s', heard %d and %d\n", t.output, t.focus, reply.c_str(),
                             a ? int(*a) : -1, b ? int(*b) : -1);
            }
        }
        check(one && compared == 3, "only the screen whose focus was reported last is heard");
    }
    });
}

// One accepted request plus one render pass, on this thread. Returns whether a frame
// committed. Both halves together, because "the daemon was asked and then given a chance to
// draw" is the unit every check below is stated in.
bool renderOnce(wallpaperx::WallpaperManager& manager, int32_t monitorId, const std::string& imagePath) {
    manager.requestWallpaper("static", imagePath, monitorId);
    return manager.renderPending(monitorId).rendered;
}

// What a hand-over produced: the image id, or 0 with the daemon's own reason for not handing
// one over. Both, because "no image" and "refused so the producer keeps one" are now different
// answers and a check that saw only the 0 could not tell them apart.
struct Handover {
    uint64_t bufferId = 0;
    // Meaningful only when bufferId is 0; the export writes it on every empty-handle path.
    wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
    // The hand-over's number, which a release of the image names; 0 when none was recorded.
    uint64_t number = 0;

    bool served() const { return bufferId != 0; }
    // The daemon publishes one reason for every refusal -- the producer's reserve, the
    // per-consumer cap and the holder limit are all NoFreeImage -- so this cannot say WHICH
    // bound refused. Each check below distinguishes them by the state it drove the monitor into
    // instead, and says so where it matters.
    bool refused() const { return bufferId == 0 && reason == wallpaperx::NoFrameReason::NoFreeImage; }
};

// Every descriptor the export produced is closed here. This phase fetches a dozen frames, and
// a leak would surface as an EMFILE in some unrelated later phase rather than as anything
// readable here.
Handover fetchHandover(wallpaperx::WallpaperManager& manager, int32_t monitorId, const std::string& client) {
    wallpaperx::FrameExport exported = exportDelivered(manager, monitorId, client);
    if (!exported.handle) return Handover{0, exported.reason};
    if (exported.handle->frame.fd >= 0) close(exported.handle->frame.fd);
    if (exported.handle->acquireFd >= 0) close(exported.handle->acquireFd);
    return Handover{exported.handle->frame.bufferId, wallpaperx::NoFrameReason::NeverRequested,
                    wallpaperx::testing::handoverOf(*exported.handle)};
}

// Why a monitor that has a frame handed none over: a refusal for want of a free image (a ring too
// shallow to spare one beside the producer's reserve) is not an export that failed.
std::string whyNoHandover(const Handover& h) {
    if (h.refused()) {
        return "the monitor committed a frame but refused to hand it over for want of a free image, so its ring "
               "has no image to spare beside the producer's reserve";
    }
    return "the monitor committed a frame but its export produced none (reason " +
           std::to_string(static_cast<int>(h.reason)) + ")";
}

// The id of the image `client` is handed, or 0 when there is none.
uint64_t fetchFrame(wallpaperx::WallpaperManager& manager, int32_t monitorId, const std::string& client) {
    return fetchHandover(manager, monitorId, client).bufferId;
}

// True when every id is a different image and none is 0. Used as the premise of the ring checks:
// hand-overs that repeat an id mean the ring never rotated, and the state the check is about was
// never built.
bool allDistinct(const std::vector<uint64_t>& ids) {
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == 0) return false;
        for (size_t j = i + 1; j < ids.size(); ++j) {
            if (ids[i] == ids[j]) return false;
        }
    }
    return true;
}

std::string joinIds(const std::vector<uint64_t>& ids) {
    std::string out;
    for (uint64_t id : ids) out += (out.empty() ? "" : ", ") + std::to_string(id);
    return out;
}

wallpaperx::MonitorStatus monitorStatus(const wallpaperx::WallpaperManager& manager, int32_t monitorId) {
    const wallpaperx::WallpaperManager::Snapshot snapshot = manager.snapshot();
    const auto it = snapshot.monitors.find(monitorId);
    return it == snapshot.monitors.end() ? wallpaperx::MonitorStatus{} : it->second;
}

void runBufferHoldChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                         const Check& check) {
    PhaseDeviceGrade grade("the buffer-hold phase", check);

    // Stand-ins for the unique bus names the daemon would record. Any two distinct strings do:
    // the manager only ever compares them.
    const std::string alice = ":test.hold.alice";
    const std::string bob = ":test.hold.bob";
    // The third is what makes the reserve reachable: with nothing ever taken back, alice and bob
    // at their cap account for every image but one, and only a consumer holding none of them can
    // ask for that last one without the per-consumer cap answering first.
    const std::string carol = ":test.hold.carol";

    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    const char* kHoldGoals = "the buffer-hold goals (a held image is not drawn over, every bound on the holds is a "
                             "refusal rather than a claim taken back, and the two ways a claim ends)";

    if (!renderOnce(manager, kMonitorHoldRing, imagePath)) {
        // A host with no working Vulkan never gets a Device built here, and the reason has
        // just been printed. Charging that to this phase's device grade as well would name the
        // daemon for a fault of the machine.
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(
            kHoldGoals, "the first render on this host produced no frame at all, so no image was ever handed to a "
                        "consumer and none of the states these goals are about could be built");
        return;
    }
    const Handover first = fetchHandover(manager, kMonitorHoldRing, alice);
    const uint64_t firstId = first.bufferId;
    // CONTROL: wallpaperx::kTargetDepth (backend/daemon/ring_depth.h) set to 1 -> ctest Skipped, "NOT
    //       VERIFIED -- ... refused to hand it over for want of a free image" (a skip, not a red check).
    if (firstId == 0) {
        wallpaperx::testing::notVerified(kHoldGoals, whyNoHandover(first) + ", so no consumer ever came to hold an image");
        return;
    }

    // GOAL: a render never draws into an image a consumer is holding. With alice holding the
    //       monitor's first image, the next render still commits, and it commits into a
    //       different image.
    // WHY-THIS-SHAPE: both halves are asserted, because they fail in opposite directions and
    //       each alone is satisfied by a broken daemon. "It commits" alone is satisfied by the
    //       old single-image behaviour that repainted the frame in use; "a different id" alone
    //       is satisfied by a daemon that has stopped rendering, since the second fetch would
    //       then return nothing and 0 differs from firstId. Which image the pixels actually
    //       landed in is established at the renderer tier by target_ring_probe, whose readback
    //       works on the opaque-fd GPU where a CPU mapping of the exported frame cannot.
    // CONTROL: wallpaperx::kTargetDepth (backend/daemon/ring_depth.h) set to 2, with this phase
    //       temporarily moved to the front of main() (in place, an earlier shared-manager phase
    //       throws NoFreeImage first) -> "FAIL: that render drew into a different image from the
    //       one the consumer holds", ids printed as 1, 0, 0: the second hand-over is refused as
    //       the reserve. The "it commits" half stayed green, and depth 1 no longer reaches either
    //       line -- the first hand-over is refused and the phase reports NOT VERIFIED. So no ring
    //       depth reddens "it commits"; nothing else was tried. Observed 2026-09-22. Reverted.
    const bool secondCommitted = renderOnce(manager, kMonitorHoldRing, imagePath);
    check(secondCommitted, "a render commits while a consumer holds the monitor's first image");
    const Handover second = secondCommitted ? fetchHandover(manager, kMonitorHoldRing, alice) : Handover{};
    const uint64_t secondId = second.bufferId;
    check(secondId != 0 && secondId != firstId,
          "that render drew into a different image from the one the consumer holds");

    // A second consumer takes its own fill, and this is what makes the rest of the phase
    // reachable: nothing is ever taken back, so two consumers at the per-consumer cap are the
    // only way to account for every image but the one the producer must keep.
    std::vector<uint64_t> ringIds{firstId, secondId};
    bool everyFillCommitted = secondCommitted;
    for (int i = 0; i < kExpectedHeldPerClient; ++i) {
        if (!everyFillCommitted) break;
        everyFillCommitted = renderOnce(manager, kMonitorHoldRing, imagePath);
        const uint64_t id = everyFillCommitted ? fetchFrame(manager, kMonitorHoldRing, bob) : 0;
        if (id == 0) everyFillCommitted = false;
        ringIds.push_back(id);
    }
    // The render that fills the last image. Nobody fetches it: it is the producer's reserve, and
    // the refusal below is the daemon declining to hand it over.
    const bool reserveRender = everyFillCommitted && renderOnce(manager, kMonitorHoldRing, imagePath);
    check(reserveRender, "a render commits with two consumers holding two images each");

    // COVERAGE-ASSERT: every check below describes a monitor whose last free image is the
    // producer's reserve. If the four hand-overs did not name four different images the
    // consumers are not holding four, the reserve is under no pressure, and the refusal below
    // would be grading a monitor with slack in it. Asserted before the refusal is read, so a run
    // that never built the state cannot report the refusal as established.
    // CONTROL: wallpaperx::kFullConsumers (backend/daemon/ring_depth.h) lowered to 1, which
    //       shrinks the derived depth to three -> "FAIL: two consumers hold four of the
    //       monitor's five images, so exactly one is free", printing "the ids handed out were 1,
    //       2, 0", and the phase stops there so no reserve check below is reported as
    //       established. Observed 2026-09-22. Reverted.
    // CONTROL, the other direction: that same constant raised to 3, which deepens the ring to
    //       seven -> this line stays GREEN, correctly, and the refusal check below goes red,
    //       printing "the third consumer was handed image 5", with the four later checks that
    //       depend on that refusal (observed 2026-09-22). That is the right attribution: the
    //       premise held and the daemon's behaviour is what changed. Reverted.
    const bool ringFilled = reserveRender && ringIds.size() == static_cast<size_t>(2 * kExpectedHeldPerClient) &&
                            allDistinct(ringIds);
    check(ringFilled, "two consumers hold four of the monitor's five images, so exactly one is free");
    if (!ringFilled) {
        std::fprintf(stderr, "  the ids handed out were %s, reserve render committed: %s; the reserve checks below "
                             "have no pressure under them\n",
                     joinIds(ringIds).c_str(), reserveRender ? "yes" : "no");
        return;
    }

    // GOAL: the daemon keeps one image per monitor out of consumers' hands. A hand-over that
    //       would take the last free one is REFUSED, and refused with the reason that says so.
    // WHY-THIS-SHAPE: asked by a THIRD consumer holding nothing, so the per-consumer cap cannot
    //       be what answers -- alice and bob are both at their cap here, and asking either of
    //       them would be refused whether or not the reserve existed at all. The reason is
    //       asserted, not merely the absence of a handle: an empty handle alone is equally
    //       produced by a monitor that never rendered and by an export that failed.
    // CONTROL: the `m_targets->nextUnheld(monitorId, flattenHolds(next.holds)) == nullptr` half
    //       of the refusal in WallpaperManager::exportCurrentFrame (backend/daemon/
    //       wallpaper_manager.cpp) deleted, leaving only the cap and holder-limit half -> "FAIL:
    //       a hand-over that would take the producer's last free image is refused", printing the
    //       id the third consumer was handed instead, among eight lines.
    const Handover refusedForCarol = fetchHandover(manager, kMonitorHoldRing, carol);
    check(refusedForCarol.refused(), "a hand-over that would take the producer's last free image is refused");
    if (!refusedForCarol.refused()) {
        std::fprintf(stderr, "  the third consumer was handed image %llu instead of being refused\n",
                     static_cast<unsigned long long>(refusedForCarol.bufferId));
    }

    // GOAL: a refusal is counted and published per monitor. Without it a consumer that has
    //       stopped giving images back is invisible: the daemon goes on rendering and committing,
    //       and every other field reads like a monitor that is keeping up.
    // WHY-THIS-SHAPE: an exact count of one, which is safe because this manager is driven from
    //       this thread alone and exactly one hand-over on this monitor has been refused so far.
    //       "Greater than zero" would also pass for a counter that never resets, and the reset is
    //       half of what the field means.
    // CONTROL: `++it->second.refusedHandovers;` deleted from WallpaperManager::exportCurrentFrame
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: the refused hand-over was counted
    //       against the monitor", printing 0.
    {
        const wallpaperx::MonitorStatus afterRefusal = monitorStatus(manager, kMonitorHoldRing);
        check(afterRefusal.refusedHandovers == 1, "the refused hand-over was counted against the monitor");
        if (afterRefusal.refusedHandovers != 1) {
            std::fprintf(stderr, "  the monitor reports %u refused hand-over(s) after exactly one refusal\n",
                         afterRefusal.refusedHandovers);
        }
    }

    // GOAL: refusing a consumer costs that consumer a frame, not the monitor its renders. The
    //       next request commits, it is discharged rather than left owed, and no render is
    //       deferred for want of an image.
    // WHY-THIS-SHAPE: three facts, because each alone is satisfied by a broken daemon. "It
    //       committed" alone is also true of one that drew over a held image; the generation
    //       moving says nothing about whether the request was discharged; and deferredRenders
    //       is the one that says the reserve, rather than luck, is why the producer had
    //       somewhere to draw. renderPending()'s deferral path still exists as a backstop and
    //       is meant to be unreachable now, so a non-zero count here is a report that a
    //       hand-over got past the reserve -- which is why it is asserted at zero rather than
    //       deleted along with the states it used to grade.
    // CONTROL: the `if (m_targets->nextUnheld(...) == nullptr)` refusal inside the admission
    //       block of WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp)
    //       deleted, so claims are still recorded but no hand-over is ever refused -> "FAIL: a
    //       request made after a refusal commits rather than being deferred", "FAIL: the frame
    //       counter moved by exactly that one render", "FAIL: the committed frame discharged the
    //       request" and "FAIL: no render was deferred -- the reserve kept an image free, so the
    //       backstop never fired", printing 1 deferral (those four among eight red). Deleting
    //       the whole admission block instead does NOT redden this check: with nothing recorded
    //       no image is ever held, so no render can defer and only the refusal checks go red.
    const uint64_t generationBefore = monitorStatus(manager, kMonitorHoldRing).generation;
    const bool fourthCommitted = renderOnce(manager, kMonitorHoldRing, imagePath);
    check(fourthCommitted, "a request made after a refusal commits rather than being deferred");
    {
        const wallpaperx::MonitorStatus after = monitorStatus(manager, kMonitorHoldRing);
        check(after.generation == generationBefore + 1, "the frame counter moved by exactly that one render");
        check(!after.renderOutstanding, "the committed frame discharged the request");
        check(after.deferredRenders == 0,
              "no render was deferred -- the reserve kept an image free, so the backstop never fired");
        if (after.deferredRenders != 0) {
            std::fprintf(stderr,
                         "  %u render(s) were refused for want of a free image, which the reserve exists to make "
                         "impossible\n",
                         after.deferredRenders);
        }
        // GOAL: the refusal count is reset by a served hand-over and by nothing else. A count a
        //       commit cleared would read zero for exactly the monitor it exists to report on --
        //       one that keeps rendering while no consumer can fetch.
        // WHY-THIS-SHAPE: read after a commit that produced a new generation, which is the event
        //       that resets the neighbouring deferredRenders. Asserting only that it is non-zero
        //       somewhere would not separate the two reset rules, and those rules are the whole
        //       difference between the two counters.
        // CONTROL: `it->second.refusedHandovers = 0;` moved out of
        //       WallpaperManager::exportCurrentFrame and into commitFrameLocked(), beside
        //       `state.deferredRenders = 0;` (backend/daemon/wallpaper_manager.cpp) -> "FAIL: and a
        //       commit does not clear it -- only a served hand-over does", printing "the monitor
        //       reports 0 refused hand-over(s) after a commit that reset nothing", alone.
        check(after.refusedHandovers == 1, "and a commit does not clear it -- only a served hand-over does");
        if (after.refusedHandovers != 1) {
            std::fprintf(stderr, "  the monitor reports %u refused hand-over(s) after a commit that reset nothing\n",
                         after.refusedHandovers);
        }
    }

    // GOAL: a consumer cannot release an image another consumer holds. The claim is recorded
    //       against the caller's own name, so naming somebody else's buffer frees nothing.
    // WHY-THIS-SHAPE: graded on what the release REPORTS, not on a later render. With the
    //       reserve in force no render is ever blocked, so the old shape -- "the monitor is
    //       still refused afterwards" -- is now true whatever the release did and would grade
    //       nothing. What a release that ended a claim produces instead is an announcement of
    //       the monitor's committed frame; one that ended nothing produces none.
    // CONTROL: the `hold.client == client` comparison in findHeld (backend/daemon/wallpaper_manager.cpp),
    //       which releaseHandoversLocked calls, replaced by `true`, so any caller matches the first
    //       holder -> "FAIL: one consumer cannot release an image another consumer holds".
    {
        std::vector<wallpaperx::FrameAvailable> announced;
        const std::vector<int> byTheWrongClient =
            manager.releaseBuffer(bob, kMonitorHoldRing, secondId, second.number, &announced);
        check(byTheWrongClient.empty() && announced.empty(),
              "one consumer cannot release an image another consumer holds");
        if (!announced.empty()) {
            std::fprintf(stderr, "  it freed something and announced monitor %d generation %llu\n",
                         announced.front().monitorId,
                         static_cast<unsigned long long>(announced.front().generation));
        }
    }

    // ---------------------------------------------------------------- //
    // A caller with no bus name, on a monitor of its own.
    // ---------------------------------------------------------------- //

    // GOAL: a frame exported to a caller the daemon cannot name takes no image out of the
    //       ring. A claim recorded under an empty name could never be withdrawn -- no
    //       ReleaseBuffer names it, no departure announces it -- so it would cost the monitor
    //       an image for the life of the daemon.
    // WHY-THIS-SHAPE: the unnamed caller is asked for a WHOLE RING'S WORTH of hand-overs, which
    //       is more than twice its per-consumer cap, and every one is required to name a
    //       different image. The obvious shape -- two exports and then "a render still commits"
    //       -- stops one hand-over short of the cap at this depth and would commit either way.
    //       A single loop is enough because both bounds a recorded claim could hit are inside
    //       it: charged with claims, the caller is refused at the third hand-over by the cap,
    //       and if the cap were gone as well the reserve refuses the fifth. Measured: with the
    //       guard removed it stopped after two, and with the cap disabled as well after four.
    // CONTROL: the `if (!client.empty())` guard around the hand-over record in
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp)
    //       removed, so an empty name is recorded like any other -> "FAIL: a caller with no bus
    //       name is served for a whole ring's worth of renders", printing "it was handed [6, 7]
    //       before stopping", the only red line in the run.
    // COVERAGE-ASSERT: the hand-overs are required to name kExpectedRingDepth DIFFERENT images,
    //       not merely to succeed, so a daemon that answered every one from a ring that had
    //       stopped rotating could not pass this.
    {
        std::vector<uint64_t> unnamedIds;
        bool servedThroughout = true;
        for (int i = 0; i < kExpectedRingDepth; ++i) {
            if (!renderOnce(manager, kMonitorHoldUnnamed, imagePath)) {
                servedThroughout = false;
                break;
            }
            const uint64_t id = fetchFrame(manager, kMonitorHoldUnnamed, "");
            if (id == 0) {
                servedThroughout = false;
                break;
            }
            unnamedIds.push_back(id);
        }
        servedThroughout = servedThroughout && unnamedIds.size() == static_cast<size_t>(kExpectedRingDepth) &&
                           allDistinct(unnamedIds);
        static_assert(kExpectedRingDepth > kExpectedHeldPerClient,
                      "the loop must exceed the per-consumer cap, or a daemon charging the unnamed caller with "
                      "claims would never be refused and the check would grade nothing");
        check(servedThroughout, "a caller with no bus name is served for a whole ring's worth of renders");
        if (!servedThroughout) {
            std::fprintf(stderr, "  it was handed [%s] before stopping, so a claim was charged to a name nothing "
                                 "could withdraw\n",
                         joinIds(unnamedIds).c_str());
        }
    }

    // ---------------------------------------------------------------- //
    // The holder limit, on a monitor of its own: five consumers.
    // ---------------------------------------------------------------- //

    // GOAL: with the holder limit reached, a consumer the daemon has never served is REFUSED,
    //       and every holder keeps everything it had. False if the daemon makes room by
    //       forgetting somebody's claim: that consumer is still sampling the image, and the very
    //       next render draws into it.
    // WHY-THIS-SHAPE: survivors read through releaseBuffer, the daemon's own report of a claim there
    //       to end (the hold list is private); a holder is re-served first, so the limit must not
    //       refuse those inside it. IMPRECISE: that report is announcement machinery too -- with the
    //       announce push deleted from WallpaperManager::reportFreedImageLocked the survivors leg goes
    //       red, beside three announcement lines that name the announcer.
    // CONTROL: in WallpaperManager::withHandover (backend/daemon/wallpaper_manager.cpp), (a) `if
    //       (holds.size() >= kMaxHoldersPerMonitor) return {};` deleted -> "FAIL: a fifth consumer past
    //       the holder limit is refused"; (b) it made the eviction `holds.erase(holds.begin())` -> that
    //       line and "FAIL: and every holder still has the claim it had".
    // COVERAGE-ASSERT: four holders on one image and the re-service on it are asserted first.
    {
        const std::string reServed = ":test.limit.re-served";
        const std::string idle = ":test.limit.idle";
        const std::string bystanderA = ":test.limit.bystander-a";
        const std::string bystanderB = ":test.limit.bystander-b";
        const std::string newcomer = ":test.limit.newcomer";
        static_assert(kExpectedHoldersPerMonitor == 4,
                      "the four names below fill the holder limit exactly; a different limit needs a different "
                      "number of them or the fifth hand-over would not be refused");

        bool builtTheState = renderOnce(manager, kMonitorHoldLimit, imagePath);
        // Every holder claims the SAME image: nothing renders in between, so each hand-over
        // names the monitor's one committed frame. That keeps the producer's reserve out of
        // this -- four claims on one image still leave four of the five free -- so a refusal
        // here is the holder bookkeeping and not the ring running out.
        uint64_t shared = 0;
        int filled = 0;
        for (const std::string& holder : {reServed, idle, bystanderA, bystanderB}) {
            if (!builtTheState) break;
            const Handover handed = fetchHandover(manager, kMonitorHoldLimit, holder);
            if (!handed.served() || (shared != 0 && handed.bufferId != shared)) {
                builtTheState = false;
                break;
            }
            shared = handed.bufferId;
            ++filled;
        }
        builtTheState = builtTheState && filled == kExpectedHoldersPerMonitor;
        check(builtTheState, "four consumers hold the same image, which fills the monitor's holder limit");

        // Each step is graded only once the one before it held, so one unbuilt premise produces
        // one red line naming the thing that actually did not happen rather than three.
        bool reServedAgain = false;
        // The newest hand-over made here, so a release naming it covers every holder's claim.
        uint64_t newest = 0;
        if (builtTheState) {
            const Handover again = fetchHandover(manager, kMonitorHoldLimit, reServed);
            reServedAgain = again.served() && again.bufferId == shared;
            newest = again.number;
            check(reServedAgain, "a consumer already inside the limit is served again, so the limit refuses arrivals "
                                 "and not the holders it already has");
        }

        if (!reServedAgain) {
            std::fprintf(stderr, "  the holder limit was never reached with one holder re-served, so the refusal "
                                 "checks below were not graded\n");
        } else {
            const Handover fifth = fetchHandover(manager, kMonitorHoldLimit, newcomer);
            check(fifth.refused(), "a fifth consumer past the holder limit is refused");
            if (!fifth.refused()) {
                std::fprintf(stderr, "  it was handed image %llu, on a monitor with four images still free -- so "
                                     "the limit gave way rather than refusing\n",
                             static_cast<unsigned long long>(fifth.bufferId));
            }

            std::vector<std::string> survivors;
            const std::vector<std::string> probed = {reServed, idle, bystanderA, bystanderB, newcomer};
            for (const std::string& who : probed) {
                std::vector<wallpaperx::FrameAvailable> announced;
                const std::vector<int> toRender =
                    manager.releaseBuffer(who, kMonitorHoldLimit, shared, newest, &announced);
                if (!toRender.empty() || !announced.empty()) survivors.push_back(who);
            }
            // Guards this file, not the daemon: no production change can redden it. Dropping
            // `idle` from the probe list would leave `survivors` matching `expected` exactly and
            // the check below green while nothing asked about the holder an eviction would take.
            check(static_cast<int>(probed.size()) == kExpectedHoldersPerMonitor + 1,
                  "every consumer that could be holding was asked whether the daemon still has a claim for it");

            const std::vector<std::string> expected = {reServed, idle, bystanderA, bystanderB};
            check(survivors == expected, "and every holder still has the claim it had, because nothing was taken "
                                         "back to make room");
            if (survivors != expected) {
                std::string names;
                for (const std::string& who : survivors) names += (names.empty() ? "" : ", ") + who;
                std::fprintf(stderr, "  the survivors were [%s]; all four holders were expected, and the refused "
                                     "newcomer %s was expected to hold nothing\n",
                             names.c_str(), newcomer.c_str());
            }
        }
    }

    // ---------------------------------------------------------------- //
    // The other case, on a monitor of its own: one consumer, never releasing.
    // ---------------------------------------------------------------- //

    // GOAL: a consumer that never releases is REFUSED past the per-consumer cap rather than
    //       served by taking one of its own images back, and the monitor goes on rendering
    //       regardless. False if a third hand-over drops that consumer's oldest claim: it has
    //       given no sign of having stopped reading that image, and the next render draws into
    //       it.
    // WHY-THIS-SHAPE: on a monitor of its own with a SINGLE consumer, so the refusal can only be
    //       the cap. The producer's reserve cannot be what answers here -- one consumer at a cap
    //       of two leaves three of the five images free -- which is exactly what the free-image
    //       check below asserts, and without it this check would pass equally for a daemon whose
    //       ring had simply run out. The refusal is also required to leave the consumer's
    //       existing claims alone, read back through the release probe.
    // CONTROL: `if (holder->buffers.size() >= kMaxHeldPerClient) return {};` deleted from
    //       WallpaperManager::withHandover (backend/daemon/wallpaper_manager.cpp), restoring the
    //       trim it replaced -> "FAIL: a consumer at its cap is refused rather than having its
    //       oldest claim taken back", printing the image it was handed.
    // COVERAGE-ASSERT: the ids handed out before the cap can bite are required to name different
    //       images, so a run where the ring never rotated cannot reach the refusal and report it
    //       as established.
    std::vector<uint64_t> handedOut;
    // The newest hand-over made to alice here, which a release covering every one of hers names.
    uint64_t alicesNewest = 0;
    bool everyRenderCommitted = true;
    for (int i = 0; i < kExpectedHeldPerClient; ++i) {
        if (!renderOnce(manager, kMonitorHoldNeverReleases, imagePath)) {
            everyRenderCommitted = false;
            break;
        }
        const Handover handed = fetchHandover(manager, kMonitorHoldNeverReleases, alice);
        if (!handed.served()) {
            everyRenderCommitted = false;
            break;
        }
        handedOut.push_back(handed.bufferId);
        alicesNewest = handed.number;
    }
    const bool capFilled = everyRenderCommitted && handedOut.size() == static_cast<size_t>(kExpectedHeldPerClient) &&
                           allDistinct(handedOut);
    check(capFilled, "a consumer that never releases is handed one image per render up to its cap");
    if (!capFilled) {
        std::fprintf(stderr, "  it was handed [%s] of an expected %d different images; the cap checks below were "
                             "abandoned\n",
                     joinIds(handedOut).c_str(), kExpectedHeldPerClient);
        return;
    }

    check(renderOnce(manager, kMonitorHoldNeverReleases, imagePath),
          "the monitor still commits a render while that consumer sits at its cap");
    const Handover pastTheCap = fetchHandover(manager, kMonitorHoldNeverReleases, alice);
    check(pastTheCap.refused(), "a consumer at its cap is refused rather than having its oldest claim taken back");
    if (!pastTheCap.refused()) {
        std::fprintf(stderr, "  it was handed image %llu\n", static_cast<unsigned long long>(pastTheCap.bufferId));
    }

    // GOAL: that refusal is the CAP and not the ring running out. Without this the check above
    //       would read the same on a daemon with no cap at all whose images had merely been
    //       exhausted.
    // WHY-THIS-SHAPE: a second consumer, holding nothing, asks the same monitor and must be
    //       served. It shares the reserve with the first, so if the ring were the thing refusing
    //       it would be refused too.
    // NO CONTROL, and this is the reason rather than an omission: it is a gate on the check
    //       above, not a claim of its own. The only production change that makes it false is a
    //       ring too shallow for one consumer's cap plus the reserve, and that reddens the ring
    //       premise earlier in this phase, which abandons before this line. Measured twice:
    //       wallpaperx::kFullConsumers lowered to 1 and, separately, wallpaperx::kTargetDepth
    //       set to `1 + kMaxHeldPerClient` (both backend/daemon/ring_depth.h) each produced
    //       "FAIL: two consumers hold four of the monitor's five images, so exactly one is
    //       free" and nothing below it. Observed 2026-09-22. Both reverted.
    const Handover byAnother = fetchHandover(manager, kMonitorHoldNeverReleases, bob);
    check(byAnother.served(), "and the monitor still had images free, so it was the cap that refused and not the "
                              "ring");
    if (!byAnother.served()) {
        std::fprintf(stderr, "  a consumer holding nothing was refused too, so the ring had run out and the cap "
                             "was not what answered\n");
    }

    // The refusal took nothing back: both of the consumer's claims are still there to end.
    // Read through releaseBuffer, which is the daemon's own report of whether a claim existed --
    // and the only one it makes, so this cannot separate a lost claim from a dead announcer. See
    // the IMPRECISE note on the holder-limit survivors check above: the same mutation reddens
    // both, together with the announcement checks that make it diagnosable.
    int stillHeld = 0;
    for (uint64_t id : handedOut) {
        std::vector<wallpaperx::FrameAvailable> announced;
        const std::vector<int> toRender =
            manager.releaseBuffer(alice, kMonitorHoldNeverReleases, id, alicesNewest, &announced);
        if (!toRender.empty() || !announced.empty()) ++stillHeld;
    }
    check(stillHeld == kExpectedHeldPerClient, "and both of its images were still claimed after the refusal");
    if (stillHeld != kExpectedHeldPerClient) {
        std::fprintf(stderr, "  %d of %d claims were still there to release; the rest had been taken back\n",
                     stillHeld, kExpectedHeldPerClient);
    }
}

// One unsigned counter GetStatus reports for one monitor, read once.
//
// Read rather than waited for. Both counters this is used for are expected at an exact value the
// moment the read is made -- the monitor is quiesced by then -- and a wait for a value would have
// a success path reachable by a timeout on the way to a number that never changed.
//
// Every failure to read it is a distinct, named outcome -- a reply that never arrived, an entry
// the reply does not carry, a key the entry does not carry. A caller that only looked at `read`
// is still told which, because `error` is filled for every one of them, and `seen` is
// meaningless unless `read` is true.
struct CounterReading {
    bool read = false;
    uint32_t seen = 0;
    std::string error;
};

CounterReading readCounter(sdbus::IProxy& proxy, int32_t monitorId, const std::string& key) {
    const std::string monitor = std::to_string(monitorId);
    CounterReading out;
    const wallpaperx::testing::StatusReply status = wallpaperx::testing::getStatus(proxy);
    if (!status.ok()) {
        out.error = status.error();
        return out;
    }
    const nlohmann::json* entry = wallpaperx::testing::wallpaperStatus(status, monitor);
    if (entry == nullptr) {
        out.error = "GetStatus carries no entry for monitor " + monitor;
        return out;
    }
    if (!entry->contains(key) || !(*entry)[key].is_number_unsigned()) {
        out.error = "GetStatus's entry for monitor " + monitor + " carries no unsigned " + key +
                    " key, so that count cannot be read from the wire at all";
        return out;
    }
    out.seen = entry->value(key, uint32_t{0});
    out.read = true;
    return out;
}

void runBufferReleaseWireChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                                const Check& check) {
    PhaseDeviceGrade grade("the buffer-release wire phase", check);

    // Declared before the proxy whose dispatch thread writes to it, so it outlives that thread.
    wallpaperx::testing::FrameReadyLog signals;

    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};

    auto listener = wallpaperx::testing::subscribeFrameReady(signals, daemonName);

    // Each on a connection of its own: that is what gives it a distinct unique bus name, and
    // a unique name is what the daemon records a hand-over against and what a departure
    // withdraws. Two proxies on one connection would be one consumer as far as the daemon can
    // tell, and this whole phase needs two.
    auto alice = sdbus::createProxy(daemonName, objectPath);
    auto bob = sdbus::createProxy(daemonName, objectPath);
    // The one that is refused, and it has to be a third connection: alice and bob are both at
    // their per-consumer cap by the time the reserve is reached, so asking either of them would
    // be refused by the cap whether or not the reserve existed.
    auto carol = sdbus::createProxy(daemonName, objectPath);

    const char* kWireGoals = "the buffer-release wire goals (the reserve's refusal on the wire, ReleaseBuffer over "
                             "the bus, the re-announcement a freed image produces, and a departing consumer's "
                             "claims)";

    // Brings the ring down to its last free image: one frame per image, each fetched by alice
    // and then bob until both are at their cap, and the last left for the producer. Each
    // SetWallpaper is followed by a wait for its exact generation, which is safe here because
    // nothing else drives this manager -- one monitor, one FIFO render thread, so generations
    // run 1..kExpectedRingDepth.
    wallpaperx::testing::Frame heldByAlice;
    uint64_t generation = 0;
    for (int i = 0; i < kExpectedRingDepth; ++i) {
        wallpaperx::testing::setWallpaper(*alice, "static", imagePath, kMonitorReleaseWire);
        ++generation;
        if (!signals.wait(kMonitorReleaseWire, generation)) {
            // The first frame never arriving is what a host with no working Vulkan produces,
            // and the reason has just been printed; charging its missing Device to this phase
            // too would name the daemon for a fault of the machine.
            if (generation == 1) grade.expectNoDevice();
            wallpaperx::testing::notVerified(
                kWireGoals, "the daemon did not report frame " + std::to_string(generation) +
                                " on this host, so the ring was never brought down to its last free image and none "
                                "of the states these goals are about could be built");
            return;
        }
        // alice takes her cap, then bob takes his; the last frame is fetched by nobody, because
        // it is the image the producer must keep and the refusal below is what grades that.
        if (i >= 2 * kExpectedHeldPerClient) break;
        sdbus::IProxy& taker = i < kExpectedHeldPerClient ? *alice : *bob;
        // GOAL: the fill itself -- the ring hands out 2 * kExpectedHeldPerClient images before
        //       the reserve refuses anyone. False for a daemon holding back more than the one
        //       image the reserve is, which is the state every goal below is measured against.
        // WHY-THIS-SHAPE: caught and failed here rather than left to escape. A refusal mid-fill
        //       is a daemon answer, not a host limitation, so it must go red -- but uncaught it
        //       reaches main's catch-all, which prints "unexpected exception: [...NoFreeImage]"
        //       naming neither the phase nor which hand-over, and abandons every phase after
        //       this one (the worker-coalescing checks are graded nowhere else).
        // CONTROL: wallpaperx::kTargetDepth in backend/daemon/ring_depth.h reduced by one,
        //       dropping the reserve -> "FAIL: the ring hands out every image but the producer's
        //       reserve", printing "hand-over 4 of 4 was refused with 'io.wallpaperx.Daemon.Error.
        //       NoFreeImage'", and the phases after this one still ran. Observed 2026-09-22.
        //       Reverted.
        wallpaperx::testing::Frame frame;
        try {
            frame = wallpaperx::testing::getFrame(taker, kMonitorReleaseWire);
        } catch (const sdbus::Error& e) {
            check(false, "the ring hands out every image but the producer's reserve");
            std::fprintf(stderr,
                         "  hand-over %d of %d was refused with '%s', so the monitor holds back more than the one "
                         "image the reserve is, or its ring is shallower than %d -- none of the states the goals "
                         "below are about could be built\n",
                         i + 1, 2 * kExpectedHeldPerClient, e.getName().c_str(), kExpectedRingDepth);
            return;
        }
        if (frame.bufferId == 0) {
            wallpaperx::testing::notVerified(kWireGoals,
                                             "a frame came back over the bus with no buffer id, so no hand-over "
                                             "could be recorded against a consumer");
            return;
        }
        if (i == 0) heldByAlice = std::move(frame);
    }

    // GOAL: the reserve's refusal reaches the client as its own named error, not as "there is
    //       no frame". A consumer that cannot tell the two apart either stops asking or polls.
    // WHY-THIS-SHAPE: the error NAME is compared, because that is all a client can branch on --
    //       the message is prose and the plugin matches on the name alone. Asserting merely
    //       that the call failed would be satisfied by NoFrame, which is exactly the answer
    //       this error exists to be distinguishable from. Asked by a third connection holding
    //       nothing, so the per-consumer cap cannot be what answers.
    // CONTROL: the `if (exported.reason == NoFrameReason::NoFreeImage)` branch in
    //       DbusService::completeFrame (backend/daemon/dbus_service.cpp) disabled, so the
    //       refusal falls through to the generic no-frame reply -> "FAIL: a hand-over that
    //       would take the producer's last image is refused as NoFreeImage on the wire",
    //       printing "it answered 'io.wallpaperx.Daemon.Error.NoFrame'". Observed 2026-09-23.
    //       Reverted.
    const std::string refusedName = wallpaperx::testing::errorNameFrom(
        [&carol] { wallpaperx::testing::getFrame(*carol, kMonitorReleaseWire); });
    check(refusedName == "io.wallpaperx.Daemon.Error.NoFreeImage",
          "a hand-over that would take the producer's last image is refused as NoFreeImage on the wire");
    if (refusedName != "io.wallpaperx.Daemon.Error.NoFreeImage") {
        std::fprintf(stderr, "  it answered '%s'\n", refusedName.c_str());
        wallpaperx::testing::notVerified(kWireGoals, "the third consumer was not refused, so the states the goals "
                                                     "below are about were never built");
        return;
    }

    // GOAL: a refusal announces nothing. False for a daemon that emitted FrameReady on the way
    //       out of a refused hand-over, which would make every refused consumer re-ask forever.
    // WHY-THIS-SHAPE: an exact count, which is safe only because the monitor is quiesced -- every
    //       render has been waited for by its exact generation and the refusal above has already
    //       returned, so no further signal can be in flight. Also the anti-vacuity barrier for
    //       the re-announcement check further down: with this count pinned at one per render, the
    //       repeats there cannot be leftovers from the fill.
    // CONTROL: `emitFrameReady(monitorId, m_manager.snapshot().monitors.at(monitorId)
    //       .generation);` added at the top of the NoFreeImage branch in
    //       DbusService::completeFrame (backend/daemon/dbus_service.cpp) -> "FAIL: the
    //       refused hand-over announced nothing -- one FrameReady per render, and no more", and
    //       the barrier before the departure below red for the same reason. Observed
    //       2026-09-23. Reverted.
    check(signals.countFor(kMonitorReleaseWire) == static_cast<size_t>(kExpectedRingDepth),
          "the refused hand-over announced nothing -- one FrameReady per render, and no more");

    // GOAL: deferred_renders is on the wire, and reads zero even in the state that used to
    //       produce a deferral. The key still has to be readable -- a client cannot otherwise
    //       tell a producer that is quietly drawing less often than asked from one keeping up --
    //       and it reads zero because the reserve is what now keeps an image free.
    // WHY-THIS-SHAPE: the key's PRESENCE and its value are asserted separately from each other,
    //       through a reader that names each way the read can fail. A bare
    //       `value("deferred_renders", 0) == 0` would pass just as happily for a daemon that
    //       stopped publishing the key at all, which is the whole failure the presence half is
    //       about. renderPending()'s deferral path is kept as a backstop, so a non-zero count
    //       here is a report that a hand-over got past the reserve.
    // CONTROL: two, both reverted. `entry["deferred_renders"] = monitor.deferredRenders;` in
    //       DbusService::getStatus (backend/daemon/dbus_service.cpp) guarded by
    //       `if (monitor.deferredRenders > 0)` -> "FAIL: GetStatus carries a deferred-render
    //       count for this monitor", printing the reader's own reason. And the `if
    //       (!client.empty())` admission block deleted from
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp) -> the
    //       refusal check above goes red first and abandons the phase, so this one is graded by
    //       the manager-tier check of the same name instead.
    {
        const CounterReading deferrals = readCounter(*alice, kMonitorReleaseWire, "deferred_renders");
        check(deferrals.read, "GetStatus carries a deferred-render count for this monitor");
        if (!deferrals.read) {
            std::fprintf(stderr, "  %s\n", deferrals.error.c_str());
        } else {
            check(deferrals.seen == 0, "and it is zero -- the reserve kept an image free rather than deferring");
            if (deferrals.seen != 0) {
                std::fprintf(stderr, "  it reads %u, so a hand-over got past the reserve\n", deferrals.seen);
            }
        }
    }

    // GOAL: refused_handovers is on the wire, and it is what tells an operator that a consumer
    //       has stopped giving images back. The count beside it cannot: deferred_renders reads
    //       zero for exactly this monitor, because the reserve refuses the hand-over instead of
    //       letting a render defer.
    // WHY-THIS-SHAPE: the key's PRESENCE and its value are asserted separately, through a reader
    //       that names each way the read can fail -- a bare `value("refused_handovers", 0) == 1`
    //       would pass for a daemon that had stopped publishing the key and been read as its
    //       default. The exact value is safe because the monitor is quiesced and exactly one
    //       hand-over on it has been refused. Deliberately read AFTER the deferred_renders check
    //       above, so the pair is graded in the same state: the two reading differently is the
    //       whole reason the second field exists.
    // CONTROL: `entry["refused_handovers"] = monitor.refusedHandovers;` deleted from
    //       DbusService::getStatus (backend/daemon/dbus_service.cpp) -> "FAIL: GetStatus carries
    //       a refused-hand-over count for this monitor", printing the reader's own reason.
    //       Reverted.
    // CONTROL, the value leg: `entry.refusedHandovers = state.refusedHandovers;` in
    //       WallpaperManager::snapshot (backend/daemon/wallpaper_manager.cpp) deleted, so the
    //       field is published but always zero -> "FAIL: and it counts the hand-over the reserve
    //       refused, which the deferral count cannot", printing 0. Reverted.
    {
        const CounterReading refusals = readCounter(*alice, kMonitorReleaseWire, "refused_handovers");
        check(refusals.read, "GetStatus carries a refused-hand-over count for this monitor");
        if (!refusals.read) {
            std::fprintf(stderr, "  %s\n", refusals.error.c_str());
        } else {
            check(refusals.seen == 1,
                  "and it counts the hand-over the reserve refused, which the deferral count cannot");
            if (refusals.seen != 1) {
                std::fprintf(stderr, "  it reads %u after exactly one refused hand-over on this monitor\n",
                             refusals.seen);
            }
        }
    }

    // GOAL: refusing a consumer does not stop the monitor rendering. The next request commits
    //       and is announced, so the producer keeps working while one consumer is going without.
    // WHY-THIS-SHAPE: waits for the exact next generation rather than "any newer frame". The
    //       monitor is quiesced -- one client driving it, one FIFO render thread -- so the
    //       generation this request lands on is determined, and an at-least wait would also be
    //       satisfied by a frame that had already arrived before the request was made.
    // CONTROL: the loop bound in OffscreenTargetManager::nextUnheld
    //       (renderer/vulkan/offscreen.cpp) narrowed from `first + count` to
    //       `first + count - 1`, so the producer will not reuse the slot it just published and
    //       has nowhere to draw once two images are held -> "FAIL: the monitor keeps rendering
    //       while a consumer is being refused", printing that no FrameReady for generation 6
    //       arrived. Chosen over forcing renderPending's deferral branch unconditionally: that
    //       stops the very first render, so this phase is never reached and the check would be
    //       unattested. The mutation is to a ring shared by every phase, so the manager-tier
    //       release checks earlier in this file go red beside it; that is the blast radius of the
    //       production function, not a second thing this check grades. Reverted.
    wallpaperx::testing::setWallpaper(*alice, "static", imagePath, kMonitorReleaseWire);
    const bool keptRendering = signals.wait(kMonitorReleaseWire, generation + 1);
    check(keptRendering, "the monitor keeps rendering while a consumer is being refused");
    if (!keptRendering) {
        std::fprintf(stderr, "  no FrameReady for generation %llu arrived within the timeout\n",
                     static_cast<unsigned long long>(generation + 1));
        return;
    }
    ++generation;

    // GOAL: ReleaseBuffer over the bus ends the caller's claim, and the daemon re-announces the
    //       monitor's committed frame so the refused consumer learns it may ask again. Which
    //       also establishes that the release is attributed to the CALLER: released under any
    //       name but the sender's, alice's call would match no claim and nothing would be
    //       announced.
    // WHY-THIS-SHAPE: a REPEAT of a generation already announced is what is waited for, so the
    //       count of that exact (monitor, generation) is asserted to be 1 first and then waited
    //       up to 2. The obvious form -- wait for the next generation -- describes the old
    //       contract, where a release discharged a deferred render; nothing is going to render
    //       here, and a plain wait() would be satisfied by the announcement the commit above
    //       already made.
    // CONTROL: `releaseBuffer(sender ? sender : "", monitorId, bufferId, handover)` in the
    //       ReleaseBuffer handler in DbusService::registerVTable (backend/daemon/dbus_service.cpp)
    //       changed to `releaseBuffer("", monitorId, bufferId, handover)` -> "FAIL: releasing an image
    //       re-announces the monitor's committed frame, so a refused consumer learns it may ask again".
    // CONTROL: `announceAvailableFrames(toAnnounce);` deleted from DbusService::releaseBuffer
    //       (backend/daemon/dbus_service.cpp) -> the same line red.
    check(signals.countOf(kMonitorReleaseWire, generation) == 1,
          "the committed frame has been announced exactly once so far");
    wallpaperx::testing::releaseFrame(*alice, kMonitorReleaseWire, heldByAlice);
    const bool reannounced = signals.waitForRepeat(kMonitorReleaseWire, generation, 2);
    check(reannounced,
          "releasing an image re-announces the monitor's committed frame, so a refused consumer learns it may ask "
          "again");
    if (!reannounced) {
        std::fprintf(stderr, "  generation %llu was never announced a second time within the timeout\n",
                     static_cast<unsigned long long>(generation));
        return;
    }

    // GOAL: the refused consumer is actually served once an image has come free -- the refusal
    //       was temporary, not a consumer permanently locked out of a monitor.
    // WHY-THIS-SHAPE: asked of the SAME consumer that was refused above, on the same monitor,
    //       with nothing in between but the release. A fresh consumer would be served whatever
    //       the release did, since a client with no claims is never the one holding the ring.
    // CONTROL: the `endClaim(...)` in WallpaperManager::releaseHandoversLocked
    //       (backend/daemon/wallpaper_manager.cpp), which releaseBuffer calls, deleted, so the release
    //       reports the claim ended without removing it -> "FAIL: the consumer that was refused is served once an
    //       image has come free", alone in the phase -- the re-announcement check above stayed green,
    //       which is the measurement showing announcing and freeing are two facts and not one. Reverted.
    const std::string servedName =
        wallpaperx::testing::errorNameFrom([&carol] { wallpaperx::testing::getFrame(*carol, kMonitorReleaseWire); });
    check(servedName.empty(), "the consumer that was refused is served once an image has come free");
    if (!servedName.empty()) {
        std::fprintf(stderr, "  it was answered '%s' instead\n", servedName.c_str());
    }

    // GOAL: the refusal count on the wire goes back to zero once this monitor serves a
    //       hand-over. A count that only ever rose would name every monitor that had ever
    //       refused anything, which is no diagnosis at all.
    // WHY-THIS-SHAPE: read after the served hand-over above and asserted at exactly zero. The
    //       same reading was 1 earlier in this phase, so the pair together says the field moves
    //       in both directions rather than merely being present.
    // CONTROL: `it->second.refusedHandovers = 0;` deleted from
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp) ->
    //       "FAIL: and a served hand-over puts the refusal count back to zero", printing 1.
    if (servedName.empty()) {
        const CounterReading afterService = readCounter(*alice, kMonitorReleaseWire, "refused_handovers");
        check(afterService.read && afterService.seen == 0,
              "and a served hand-over puts the refusal count back to zero");
        if (!afterService.read) {
            std::fprintf(stderr, "  %s\n", afterService.error.c_str());
        } else if (afterService.seen != 0) {
            std::fprintf(stderr, "  it still reads %u after a hand-over was served\n", afterService.seen);
        }
    }

    // GOAL: a consumer leaving the bus gives its images back, and that is announced too --
    //       graded through DbusService's own NameOwnerChanged handler, which the manager tier
    //       cannot reach.
    // WHY-THIS-SHAPE: three steps, and the first two are what stop the last from being vacuous.
    //       A render is driven first so the monitor has a fresh committed frame with no claim on
    //       it; carol is then REFUSED, which is what establishes that alice's remaining claim is
    //       the thing holding the ring; only then does alice leave. Without the refusal step,
    //       carol's later success would be equally true of a daemon whose departure handler drops
    //       nothing at all. carol and not bob, because bob is at his per-consumer cap here and
    //       would be refused whatever the ring had free.
    // CONTROL: the `announce` argument of the reportFreedImageLocked call in
    //       WallpaperManager::forgetHeldBuffers (backend/daemon/wallpaper_manager.cpp) passed
    //       as `nullptr`, so a departure drops claims but publishes nothing -> "FAIL: a
    //       consumer leaving the bus re-announces the monitor's committed frame", printing
    //       "generation 7 was never announced a second time after the consumer left", while the
    //       ring check below stayed green. Observed 2026-09-14, re-run 2026-10-24 via control_batch.sh. Reverted.
    // CONTROL, the other leg: `std::erase_if(state.holds, ...)` in WallpaperManager::forgetHeldBuffers
    //       replaced by a `std::count_if` that
    //       reports the monitor without dropping the claim -> "FAIL: the departed consumer's
    //       images are really back in the ring", printing the NoFreeImage the surviving
    //       consumer was answered, while the announcement check stayed green. Re-run 2026-10-24 via
    //       control_batch.sh (the count kept only to skip monitors it held nothing on). Reverted.
    wallpaperx::testing::setWallpaper(*bob, "static", imagePath, kMonitorReleaseWire);
    if (!signals.wait(kMonitorReleaseWire, generation + 1)) {
        std::fprintf(stderr, "  no FrameReady for generation %llu arrived, so the departure checks had no "
                             "committed frame under them\n",
                     static_cast<unsigned long long>(generation + 1));
        wallpaperx::testing::notVerified(kWireGoals, "the monitor stopped rendering before the departure checks");
        return;
    }
    ++generation;
    const std::string beforeDeparture = wallpaperx::testing::errorNameFrom(
        [&carol] { wallpaperx::testing::getFrame(*carol, kMonitorReleaseWire); });
    // NO CONTROL, and this is the reason rather than an omission: it is a gate, not a claim.
    // Every mutation that would make it false -- the reserve removed, the hand-over cap widened,
    // the ring deepened -- also makes the FIRST refusal check above false, and that one abandons
    // the phase before this line is reached. What it protects against is a future edit to THIS
    // file that reorders the fill so the ring still has slack here; that lands in the else
    // branch, which abandons rather than grading the departure against a free image.
    check(beforeDeparture == "io.wallpaperx.Daemon.Error.NoFreeImage",
          "with the consumers holding images the monitor is down to its reserve again");
    if (beforeDeparture != "io.wallpaperx.Daemon.Error.NoFreeImage") {
        std::fprintf(stderr, "  it answered '%s', so the departure below had a free image under it and would "
                             "grade nothing\n",
                     beforeDeparture.c_str());
        wallpaperx::testing::notVerified(kWireGoals, "the ring was not down to its reserve before the departure");
        return;
    }
    check(signals.countOf(kMonitorReleaseWire, generation) == 1,
          "the newest frame has been announced exactly once before the departure");
    alice.reset();
    const bool departureAnnounced = signals.waitForRepeat(kMonitorReleaseWire, generation, 2);
    check(departureAnnounced, "a consumer leaving the bus re-announces the monitor's committed frame");
    if (!departureAnnounced) {
        std::fprintf(stderr, "  generation %llu was never announced a second time after the consumer left\n",
                     static_cast<unsigned long long>(generation));
    }
    const std::string afterDeparture = wallpaperx::testing::errorNameFrom(
        [&carol] { wallpaperx::testing::getFrame(*carol, kMonitorReleaseWire); });
    check(afterDeparture.empty(), "the departed consumer's images are really back in the ring");
    if (!afterDeparture.empty()) {
        std::fprintf(stderr, "  the surviving consumer was answered '%s' after the other one left\n",
                     afterDeparture.c_str());
    }

    // GOAL: ReleaseBuffer rejects a monitor id the daemon does not accept, on the reply, under
    //       the same error name every other malformed request gets.
    // CONTROL: `requireValidMonitorId(monitorId);` deleted from
    //       WallpaperManager::releaseBuffer (backend/daemon/wallpaper_manager.cpp) -> "FAIL:
    //       ReleaseBuffer refuses a negative monitor id as InvalidArgs", printing the empty
    //       name the call returned instead (the release names hand-over 1, so the number check passes).
    const std::string badId = wallpaperx::testing::errorNameFrom(
        [&bob] { wallpaperx::testing::releaseBuffer(*bob, -1, 1, 1); });
    check(badId == "org.freedesktop.DBus.Error.InvalidArgs",
          "ReleaseBuffer refuses a negative monitor id as InvalidArgs");
    if (badId != "org.freedesktop.DBus.Error.InvalidArgs") {
        std::fprintf(stderr, "  it answered '%s'\n", badId.c_str());
    }
}

// Coalescing where it lives: RenderWorker::m_queuedRenders drops a monitor already queued, and
// WallpaperManager::servicedSeq makes a serviced request's render a no-op. Over the bus a burst's
// render count is set by the scheduler (`<= 2` was reachable at 3), so the worker is held on a
// task while the burst is recorded, and the count is exact: either mechanism alone yields 1, both
// disabled 5. Its own manager, since a Device aborts on use from a second worker thread.
void runWorkerCoalescingChecks(const wallpaperx::WallpaperManager::Config& config,
                               const std::vector<std::string>& burstPaths, const Check& check) {
    // This phase builds a manager, and therefore a Device, of its own. The counter is
    // per-Device now, so the handle captured around the outer manager says nothing about
    // this one -- when the oracle was process-wide it covered both, and the migration
    // silently left this phase with no validation oracle at all. Declared before the
    // manager so it is destroyed after it, which is what makes teardown complaints
    // countable; graded in a destructor so an exception out of the phases below cannot
    // skip it.
    std::shared_ptr<const std::atomic<int>> counter;
    bool layerActive = false;
    struct GradeOnExit {
        const std::shared_ptr<const std::atomic<int>>& counter;
        const bool& layerActive;
        const Check& check;
        ~GradeOnExit() {
            // The same ladder ValidationGrade encodes, expressed once there rather than
            // open-coded here: waiver, then no-device, then no-validator, then the count.
            // Two copies of that policy in one suite is how the two tiers come to
            // disagree about what a clean run means -- and the waiver semantics changed
            // under both of them twice in as many rounds.
            // The waiver covers a missing device here as well as a missing validator.
            // ValidationGrade only waives the latter -- correctly, since a null counter
            // in a probe means something went wrong -- but this phase legitimately has no
            // device when the waived TSan tier builds its manager with validation off.
            // Saying so explicitly is what keeps the scoped waiver from narrowing a case
            // this call site had been relying on.
            int failures = 0;
            int ungraded = 0;
            {
                wallpaperx::testing::ValidationGrade grade(
                    "dbus_service_test (worker-coalescing phase)", failures);
                if (wallpaperx::testing::validationWaived()) grade.expectNoDevice();
                // Its own bucket: an absent layer PACKAGE says nothing about this daemon, so it
                // must not be charged to the check below. ValidationGrade still charges failures
                // when the package is present and the device simply did not activate it, which
                // is a defect here.
                grade.reportUngradedTo(ungraded);
                grade.adopt(counter, layerActive);
            }
            if (ungraded > 0) {
                std::fprintf(stderr,
                             "dbus_service_test: the worker-coalescing phase's validation grade could not be made "
                             "-- VK_LAYER_KHRONOS_validation is not loaded. The phase itself is still judged.\n");
            }
            // Named for what was actually judged. ValidationGrade has already printed the
            // specific verdict -- no device, no validator, or a nonzero count -- so a
            // single label asserting "zero validation warnings" would contradict two of
            // the three and point the operator at the GPU path for a missing package.
            check(failures == 0, "the worker-coalescing phase's own device passed its validation grade");
        }
    } gradeOnExit{counter, layerActive, check};

    wallpaperx::WallpaperManager manager(config);
    struct CaptureOnExit {
        const wallpaperx::WallpaperManager& manager;
        std::shared_ptr<const std::atomic<int>>& counter;
        bool& layerActive;
        ~CaptureOnExit() {
            counter = manager.validationCounter();
            layerActive = manager.validationLayerActive();
        }
    } captureOnExit{manager, counter, layerActive};
    std::atomic<int> frameReadySignals{0};

    // Released on every path, including an exception between here and the release below.
    // A worker left blocked on this gate would never observe stop(), so ~RenderWorker
    // would join a thread that never returns and the test would hang instead of failing.
    struct Gate {
        std::promise<void> promise;
        bool open = false;
        void release() {
            if (!open) {
                open = true;
                promise.set_value();
            }
        }
        ~Gate() { release(); }
    } gate;
    const std::shared_future<void> gateOpen = gate.promise.get_future().share();

    wallpaperx::RenderWorker worker(manager, [&](int, uint64_t) { ++frameReadySignals; });
    worker.postTask([gateOpen](wallpaperx::RenderWorker::TaskState) { gateOpen.wait(); });

    for (const std::string& path : burstPaths) {
        manager.requestWallpaper("static", path, kMonitorWorkerCoalesce);
        worker.post(kMonitorWorkerCoalesce);
    }

    // A barrier queued behind the burst (stop() does not drain, so it cannot be one).
    // GOAL: X.1 -- a wedged worker fails this phase, not stalls it.
    // WHY-THIS-SHAPE: a bounded wait, then _Exit: returning would unwind into ~RenderWorker, whose
    //       join on the wedged thread is unbounded again, and ctest would report a timeout with none
    //       of the FAIL lines printed.
    // CONTROL: a task that never returns posted ahead of the barrier -> "FAIL: the render worker
    //       drains the coalescing burst" and "ending the run here", gone at 30.5 s; with `return`
    //       for the _Exit, still running at a 75 s hard kill.
    const auto drain = [&worker](const char* what, const Check& c) {
        std::promise<void> done;
        std::future<void> future = done.get_future();
        // Moved into the task: a promise left on this stack would be destroyed while
        // the worker still held a reference to it if the wait below timed out.
        worker.postTask([done = std::move(done)](wallpaperx::RenderWorker::TaskState) mutable { done.set_value(); });
        if (future.wait_for(wallpaperx::testing::kSignalTimeout) == std::future_status::ready) return;
        c(false, what);
        std::fprintf(stderr,
                     "dbus_service_test: the render worker did not progress within the signal "
                     "timeout; ending the run here rather than joining a wedged thread\n");
        // Mandatory before _Exit: stdout is fully buffered under ctest, so every
        // progress and WARNING line written so far is still in the buffer _Exit
        // discards. stderr needs no such care -- it is unbuffered and goes straight to
        // the terminal, which is what makes the FAIL line above survive.
        std::fflush(nullptr);
        std::_Exit(1);
    };

    gate.release();
    drain("the render worker drains the coalescing burst", check);

    const wallpaperx::WallpaperManager::Snapshot snapshot = manager.snapshot();
    const auto entry = snapshot.monitors.find(kMonitorWorkerCoalesce);
    const bool present = entry != snapshot.monitors.end();
    check(present, "the monitor the coalescing burst targeted appears in the manager's snapshot");
    if (!present) return;

    // CONTROL: backend/daemon/render_worker.cpp:58 `insert(monitorId).second` made `... && false`, and
    //       backend/daemon/wallpaper_manager.cpp:3179's `servicedSeq == requestSeq` early return made `if (false)`,
    //       control_batch 2026-10-15, dbus_service_lavapipe '^workerCoalescing$' -> "FAIL: five requests posted
    //       while the render thread is held collapse into exactly one render", with "FAIL: exactly one FrameReady
    //       callback fires for the collapsed burst" and the two re-post lines; restored, green.
    check(entry->second.generation == 1,
          "five requests posted while the render thread is held collapse into exactly one render");
    check(frameReadySignals.load() == 1, "exactly one FrameReady callback fires for the collapsed burst");
    check(entry->second.path == burstPaths.back(), "the one render that happens serves the LAST request of the burst");

    if (entry->second.generation != 1) {
        std::fprintf(stderr, "  generation is %llu after a burst of %d, expected 1\n",
                     static_cast<unsigned long long>(entry->second.generation), kBurstSize);
    }

    // GOAL: S6.25 -- a render pass is idempotent per *request*: one SetWallpaper never
    //       produces two renders, two generations or two FrameReady signals, however
    //       many times the monitor is queued.
    // WHY-THIS-SHAPE: the burst above does not test this and cannot. Two mechanisms
    //       collapse a burst, and with the gate held only one of them is in play:
    //       RenderWorker::post() refuses to queue a monitor already in m_queuedRenders,
    //       so five posts become one queue entry and WallpaperManager::servicedSeq is
    //       never consulted. What servicedSeq guards is the *other* shape -- a monitor
    //       posted again after it was already dequeued and serviced -- which only
    //       happens with no new request behind it. Posting without a preceding
    //       requestWallpaper is the only way to produce that state deliberately, and it
    //       is reachable from here because this phase drives the worker directly; over
    //       the bus every post has a request in front of it.
    // CONTROL: the `servicedSeq == requestSeq` early return in
    //       WallpaperManager::renderPending() disabled. Before this block the entire
    //       suite stayed green with it disabled -- both tiers, both ICDs -- which is
    //       what made the mechanism untested. With it: "FAIL: re-posting a monitor
    //       whose request was already serviced renders nothing again" and "FAIL:
    //       a re-post emits no second FrameReady", generation 2 against 1. Reverted.
    worker.post(kMonitorWorkerCoalesce);
    drain("the render worker drains the idempotence re-post", check);

    const wallpaperx::WallpaperManager::Snapshot after = manager.snapshot();
    const auto repeated = after.monitors.find(kMonitorWorkerCoalesce);
    const bool stillPresent = repeated != after.monitors.end();
    check(stillPresent, "the re-posted monitor is still in the manager's snapshot");
    if (!stillPresent) return;

    check(repeated->second.generation == 1,
          "re-posting a monitor whose request was already serviced renders nothing again");
    check(frameReadySignals.load() == 1, "a re-post emits no second FrameReady");
    if (repeated->second.generation != 1) {
        std::fprintf(stderr, "  generation went from 1 to %llu on a re-post with no new request\n",
                     static_cast<unsigned long long>(repeated->second.generation));
    }

    runWorkerTaskAnswerChecks(manager, check);
}

// --------------------------------------------------------------------------- //
// Coalescing over the bus: when requests for one monitor arrive faster than the GPU can
// serve them, the daemon must render the LAST one.
//
// A generation counter reading 1 after five requests to one monitor cannot carry this claim
// when all five name the *same image*: a daemon that served the first request and discarded
// the rest produces an identical observation, as does one that served the last. The counter distinguishes "how many renders committed",
// which was never the question; the question is "which request's pixels won".
//
// Two independent answers below. The outcome checks decide it from whether a frame
// exists at all, which needs no access to the frame's contents and therefore runs on
// every ICD. The content check decides it from the pixels: mapped from a LINEAR dma_buf,
// imported through the plugin's GL importer otherwise (tests/served_pixels.h).
// --------------------------------------------------------------------------- //
void runCoalescingChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& imagePath,
                         const std::string& corruptPath, const std::vector<std::string>& burstPaths,
                         const Check& check) {
    // The burst monitors are drained by the time the barrier monitor's frame lands: the
    // daemon does all GPU work on one FIFO thread, and every burst request has already
    // been recorded and posted by the time SetWallpaper returns to this thread, so the
    // barrier request is unconditionally behind them in the queue. No sleep, no flake.
    int barrierGeneration = 0;
    const auto barrier = [&](const char* what) {
        ++barrierGeneration;
        setWallpaper(proxy, "static", imagePath, kMonitorCoalesceBarrier);
        check(signals.wait(kMonitorCoalesceBarrier, static_cast<uint64_t>(barrierGeneration)), what);
    };

    // --- Outcome, on every ICD: a burst ending in a request that cannot render. ---
    //
    // If the last request wins, the monitor ends with no frame and a recorded error. If
    // the first request won instead, it ends holding a perfectly good frame. Nothing
    // about the *number* of renders is asserted -- an intermediate valid request may or
    // may not commit and emit its own FrameReady depending on how fast the worker
    // drains, and asserting on that would be asserting on the scheduler.
    for (int i = 0; i < kBurstSize - 1; ++i) {
        setWallpaper(proxy, "static", imagePath, kMonitorBurstFailLast);
    }
    setWallpaper(proxy, "static", corruptPath, kMonitorBurstFailLast);
    barrier("the barrier frame arrives after a burst ending in an unrenderable request");

    // RenderFailed rather than NoFrame is itself part of the claim: it is only reachable
    // when a render actually ran and failed, so it rules out "the burst was dropped
    // entirely" in a way a bare NoFrame cannot.
    // CONTROL: backend/daemon/wallpaper_manager.cpp:1189 requestWallpaper's `state.path.swap(nextPath);` made
    //       `if (!existed) ...` (the first request wins), control_batch 2026-10-15, dbus_service_lavapipe
    //       '^coalescing$' -> "FAIL: a burst ending in an unrenderable request leaves no frame -- the last request
    //       is served, not the first", with seven other burst-outcome and recovery lines; restored, green.
    check(errorNameFrom([&] { getFrame(proxy, kMonitorBurstFailLast); }) ==
              "io.wallpaperx.Daemon.Error.RenderFailed",
          "a burst ending in an unrenderable request leaves no frame -- the last request is served, not the first");
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorBurstFailLast));
        const bool reported = monitor != nullptr && monitor->value("path", std::string()) == corruptPath &&
                              !monitor->value("has_frame", true) && monitor->contains("last_error");
        check(reported, "GetStatus attributes the burst's outcome to the last request in it");
    }

    // --- Outcome, the mirror image: a burst ending in a request that *can* render. ---
    //
    // Runs the same shape in reverse so neither direction can pass for the wrong reason.
    // Without it, a daemon that simply dropped every request after the first would fail
    // the check above and pass nothing else -- but so would one that dropped every
    // request, full stop, which is a different bug with a different fix.
    for (int i = 0; i < kBurstSize - 1; ++i) {
        setWallpaper(proxy, "static", corruptPath, kMonitorBurstRenderLast);
    }
    setWallpaper(proxy, "static", imagePath, kMonitorBurstRenderLast);
    barrier("the barrier frame arrives after a burst ending in a renderable request");

    check(errorNameFrom([&] { getFrame(proxy, kMonitorBurstRenderLast); }).empty(),
          "a burst ending in a renderable request leaves a frame, even after four failures");
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorBurstRenderLast));
        const bool served = monitor != nullptr && monitor->value("path", std::string()) == imagePath &&
                            monitor->value("has_frame", false);
        check(served, "the burst's last request is the one reflected in GetStatus");
    }

    // --- Error recovery, deliberately NOT a burst. ---
    //
    // Whether a successful render clears a previous failure's last_error cannot be asked
    // of a burst at all. Coalescing means the four corrupt requests above are never
    // rendered -- the worker reads the latest request when the monitor is dequeued, so
    // exactly one render runs and it is the valid one. last_error is therefore never set
    // for that monitor, and asserting its absence afterwards holds no matter what the
    // clearing code does: it would pass for a monitor that had never failed.
    //
    // Sequencing the two requests, each behind its own barrier, is what makes the
    // corrupt render actually execute and record an error for the next one to clear.
    setWallpaper(proxy, "static", corruptPath, kMonitorErrorRecovery);
    barrier("the barrier frame arrives after a standalone failing request");
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorErrorRecovery));
        const bool failed = monitor != nullptr && !monitor->value("has_frame", true) &&
                            !monitor->value("last_error", std::string()).empty();
        // Without this the clearing check below is unfalsifiable again, one step further
        // out: nothing else here proves the error was ever recorded in the first place.
        check(failed, "a standalone failing render records last_error, so there is an error to clear");
    }

    setWallpaper(proxy, "static", imagePath, kMonitorErrorRecovery);
    check(signals.wait(kMonitorErrorRecovery, 1), "the recovering render emits FrameReady");
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorErrorRecovery));
        const bool cleared = monitor != nullptr && monitor->value("path", std::string()) == imagePath &&
                             monitor->value("has_frame", false) && !monitor->contains("last_error");
        // Pins the outcome, not one particular line. WallpaperManager clears lastError
        // twice -- once in requestWallpaper() when the request is recorded, once in
        // renderPending() when a render commits -- and either alone is sufficient here,
        // so removing just one does not make this fail. Removing both does (verified).
        // That redundancy is worth knowing about: a future edit that deletes one of them
        // will not be caught by this check, and should not be assumed safe because it is.
        // CONTROL: both `state.lastError.clear();` removed from backend/daemon/wallpaper_manager.cpp (:1207 in
        //       requestWallpaper, :2662 in the commit), control_batch 2026-10-15, dbus_service_lavapipe
        //       '^coalescing$' -> "FAIL: a successful render leaves no last_error behind from an earlier failed
        //       one", alone; restored, green.
        check(cleared, "a successful render leaves no last_error behind from an earlier failed one");
    }

    // --- Content: which request's pixels actually reached the exported frame. ---
    for (const std::string& path : burstPaths) {
        setWallpaper(proxy, "static", path, kMonitorBurstContent);
    }
    barrier("the barrier frame arrives after a burst of five different images");

    const Frame frame = getFrame(proxy, kMonitorBurstContent);
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorBurstContent));
        check(monitor != nullptr && monitor->value("path", std::string()) == burstPaths.back(),
              "GetStatus reports the last image of the burst as the monitor's wallpaper");
    }
    // Reported, not asserted. How many renders a burst produces over the bus depends on
    // how many complete before the next request lands, which is the scheduler's decision
    // and not the daemon's -- runWorkerCoalescingChecks asserts the collapse exactly, by
    // holding the render thread so there is no window to race with. Printed as both
    // numbers rather than their difference: generation is uint64_t and kBurstSize is int,
    // so subtracting produced 18446744073709551615 in precisely the broken case the line
    // was there to explain.
    std::printf("dbus_service_test: %d rapid requests over the bus produced generation %llu\n", kBurstSize,
                static_cast<unsigned long long>(frame.generation));

    // GOAL: S6.24's content half -- that the *pixels* of the exported frame are the last
    //       burst request's, not the first's -- is decided on every export this host can read
    //       (an opaque fd included), and reported NOT VERIFIED only where it truly cannot.
    // WHY-THIS-SHAPE: read through readServedBgra() (tests/served_pixels.h): a LINEAR dma_buf is
    //       mapped, anything else is imported by the plugin's GL importer on the exporting GPU.
    //       Skipping every non-mappable export, the old shape, left NVIDIA -- the target GPU --
    //       ungraded. notVerified() for an unreadable frame, not check(false): that is this
    //       host's limit, not the daemon's.
    // CONTROL: backend/daemon/wallpaper_manager.cpp requestWallpaper() keeping a monitor's first
    //       path (first-wins), control_run.sh --phase '^coalescing$', 2026-10-08 -> "FAIL: the
    //       exported frame's pixels are the LAST image of the burst", "3072 of 3072 pixels (imported
    //       through the plugin's GL importer) differ" on dbus_service (NVIDIA), "(mapped)" on
    //       dbus_service_lavapipe. Held, restored. Test-side: the GL context's UUID zeroed in
    //       served_pixels.h -> NOT VERIFIED naming this clause, ctest Skipped. Reverted. Once the comparison
    //       moved to countOffColour() (served_pixels.h), 2026-10-09, dbus_service_lavapipe: StaticRenderer
    //       (renderer/renderers/static_renderer.cpp) uploading zeros instead of the decoded image -> "FAIL:
    //       the exported frame's pixels are the LAST image of the burst", "3072 of 3072 pixels (mapped)
    //       differ". Held, restored.
    const char* kContentGoal = "S6.24's content half -- which request's pixels reached the exported frame "
                               "(its outcome half ran and held above)";
    const wallpaperx::testing::FrameLayout layout = wallpaperx::testing::layoutOf(frame);
    const bool mapped = wallpaperx::testing::servedFrameIsMapped(layout);
    if (frame.handleType == 1 && frame.drmModifier == wallpaperx::testing::kDrmModifierLinear) {
        if (frame.planes.empty()) {
            check(false, "a LINEAR DMA_BUF frame carries a plane layout");
            return;
        }
        // S15.431, as in daemon_system_test's checkFirstFrame, whose annotation carries the control.
        check(mapped, "a LINEAR dma_buf with a plane is read by mapping it");
    }

    // The fd's size against memory_size is checked inside readServedBgra(), with a FAIL of its own.
    std::string unreadable;
    const std::vector<unsigned char> image = wallpaperx::testing::readServedBgra(layout, unreadable);
    if (image.empty()) {
        wallpaperx::testing::notVerified(kContentGoal, unreadable);
        return;
    }
    // The target is a solid fill of the last burst colour, in B8G8R8A8 order.
    const unsigned char* rgb = kBurstColors[kBurstColorCount - 1];
    const unsigned char expectedB = rgb[2];
    const unsigned char expectedG = rgb[1];
    const unsigned char expectedR = rgb[0];
    constexpr int kTolerance = 6;

    // readServedBgra() returns frame.width x frame.height tightly packed; the size itself is S6.22's.
    const size_t compared = image.size() / 4;
    unsigned char firstBad[4] = {0, 0, 0, 0};
    const size_t badPixels =
        wallpaperx::testing::countOffColour(image, expectedB, expectedG, expectedR, kTolerance, firstBad);

    check(badPixels == 0,
          "the exported frame's pixels are the LAST image of the burst, read back through its own fd");
    const char* path = mapped ? "mapped" : "imported through the plugin's GL importer";
    if (badPixels != 0) {
        std::fprintf(stderr,
                     "  %zu of %zu pixels (%s) differ; expected B,G,R = %d,%d,%d, first mismatch was %d,%d,%d "
                     "(the burst's first colour is B,G,R = %d,%d,%d)\n",
                     badPixels, compared, path, expectedB, expectedG, expectedR, firstBad[0], firstBad[1],
                     firstBad[2], kBurstColors[0][2], kBurstColors[0][1], kBurstColors[0][0]);
    } else {
        std::printf("dbus_service_test: exported frame (%s, %zu pixels) is solid B,G,R = %d,%d,%d -- the burst's "
                    "last image\n",
                    path, compared, expectedB, expectedG, expectedR);
    }
}

// --------------------------------------------------------------------------- //
// A screen's reported size (S13.94-S13.97, S13.99-S13.101, S13.103, S13.104): what a monitor is
// drawn at, what a change of it redraws, and what a redraw at a new size may and may not leave
// observable. Each phase brings its own manager, so the ids below repeat numbers used above.
// --------------------------------------------------------------------------- //

// Not the Config size (64x48), not square, and different from each other, so a swapped axis, a
// default taken for a report, or the old size taken for the new one all read differently.
constexpr uint32_t kReportedWidth = 40;
constexpr uint32_t kReportedHeight = 30;
constexpr uint32_t kResizedWidth = 50;
constexpr uint32_t kResizedHeight = 20;

constexpr int32_t kMonitorSized = 0;
constexpr int32_t kMonitorUnsized = 1;
constexpr int32_t kMonitorSizeBarrier = 2;
constexpr int32_t kMonitorSizeLostFile = 3;
constexpr int32_t kMonitorSizeIdle = 4;

void runOutputSizeWireChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                             const Check& check) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputSize;
    using wallpaperx::testing::setOutputVisible;

    PhaseDeviceGrade grade("the output-size wire phase", check);
    wallpaperx::testing::FrameReadyLog signals;
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    auto proxy =
        wallpaperx::testing::subscribeFrameReady(signals, sdbus::ServiceName{serviceConn->getUniqueName()});

    const char* kGoals = "S13.94, S13.95, S13.96, S13.99 and S13.100's lost-file clause over the wire";
    constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
    constexpr const char* kUnknownOutput = "io.wallpaperx.Daemon.Error.UnknownOutput";

    // The size of the frame GetFrame hands over, released again at once so this consumer never
    // reaches its cap. `error` is the refusal's name when there is no frame.
    struct Served {
        bool ok = false;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string error;
    };
    const auto served = [&proxy](int32_t monitorId) {
        Served out;
        try {
            const wallpaperx::testing::Frame frame = wallpaperx::testing::getFrame(*proxy, monitorId);
            wallpaperx::testing::releaseFrame(*proxy, monitorId, frame);
            out = Served{true, frame.width, frame.height, {}};
        } catch (const sdbus::Error& e) {
            out.error = e.getName();
        }
        return out;
    };
    const auto describeServed = [](const Served& s) {
        return s.ok ? std::to_string(s.width) + "x" + std::to_string(s.height) : "no frame (" + s.error + ")";
    };

    // Every render posted before this one has run by the time its FrameReady arrives: one FIFO worker.
    uint64_t barrierGeneration = 0;
    const auto settle = [&]() {
        if (!errorNameFrom([&] { setWallpaper(*proxy, "static", imagePath, kMonitorSizeBarrier); }).empty()) return false;
        return signals.wait(kMonitorSizeBarrier, ++barrierGeneration);
    };

    const std::string bindError = errorNameFrom([&] { setMonitorOutput(*proxy, kMonitorSized, "DP-SIZED"); });
    const std::string sizeError =
        errorNameFrom([&] { setOutputSize(*proxy, "DP-SIZED", kReportedWidth, kReportedHeight); });
    check(bindError.empty() && sizeError.empty(), "a named screen's size is accepted before its monitor has a wallpaper");
    if (!bindError.empty() || !sizeError.empty()) {
        std::fprintf(stderr, "  SetMonitorOutput answered '%s', SetOutputSize '%s'\n", bindError.c_str(), sizeError.c_str());
        return;
    }

    setWallpaper(*proxy, "static", imagePath, kMonitorUnsized);
    if (!signals.wait(kMonitorUnsized, 1)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the daemon drew no frame at the default size on this host, so no size "
                                                 "could be compared with it");
        return;
    }
    setWallpaper(*proxy, "static", imagePath, kMonitorSized);
    const bool sizedDrawn = signals.wait(kMonitorSized, 1);
    check(sizedDrawn, "a monitor whose screen reported a size is drawn at all");
    if (!sizedDrawn) return;

    // GOAL: S13.94 -- a monitor whose connector name has a reported size is drawn at that size, one with
    //       none at Config's default, and GetStatus's target fields and target_size_source say which.
    // WHY-THIS-SHAPE: the size is read from the frame GetFrame hands over, which is the extent the
    //       images were created at, as well as from GetStatus -- a status that reported the target
    //       while the images stayed at the default would satisfy the status half alone.
    // CONTROL: WallpaperManager::targetSizeLocked (backend/daemon/wallpaper_manager.cpp) made to return
    //       the Config size always -> "FAIL: a monitor whose screen reported a size is drawn at that
    //       size" printing 64x48, the GetStatus line for it red, and nearly every later size check in this
    //       file red with them, since no size can take effect. Reverted.
    // CONTROL: the `"reported" : "default"` operands swapped in DbusService::getStatus
    //       (backend/daemon/dbus_service.cpp) -> both GetStatus lines red and "after the refusals both
    //       monitors are drawn at what they were before them", which reads the same field; both frame
    //       lines green. Reverted.
    const Served sized = served(kMonitorSized);
    const Served unsized = served(kMonitorUnsized);
    check(sized.ok && sized.width == kReportedWidth && sized.height == kReportedHeight,
          "a monitor whose screen reported a size is drawn at that size");
    check(unsized.ok && unsized.width == config.width && unsized.height == config.height,
          "a monitor whose screen reported nothing is drawn at the configured default");
    if (!sized.ok || sized.width != kReportedWidth || !unsized.ok || unsized.width != config.width) {
        std::fprintf(stderr, "  sized monitor served %s, unsized monitor served %s\n", describeServed(sized).c_str(),
                     describeServed(unsized).c_str());
    }
    const auto statusSays = [&proxy](int32_t monitorId, uint32_t width, uint32_t height, const char* source) {
        const StatusReply status = getStatusOrExplain(*proxy);
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
        const bool says = entry != nullptr && entry->value("target_width", uint32_t{0}) == width &&
                          entry->value("target_height", uint32_t{0}) == height &&
                          entry->value("target_size_source", std::string()) == source;
        if (!says) {
            std::fprintf(stderr, "  GetStatus for monitor %d: %s\n", monitorId,
                         entry != nullptr ? entry->dump().c_str() : "(no entry)");
        }
        return says;
    };
    check(statusSays(kMonitorSized, kReportedWidth, kReportedHeight, "reported"),
          "GetStatus says the sized monitor is drawn at its reported size, and that a screen reported it");
    check(statusSays(kMonitorUnsized, config.width, config.height, "default"),
          "GetStatus says the unsized monitor is drawn at the default size, and that nothing reported one");

    // GOAL: S13.95 over the wire -- zero or a side past 8192 and an unusable name answer InvalidArgs, a
    //       name no monitor answers to UnknownOutput; 8192 is accepted; none of them draws anything or
    //       changes a target, and a size refused for an unbound name does not come with the name later.
    // WHY-THIS-SHAPE: "draws nothing" is read after a barrier render, never after a wait: an absent signal
    //       is only evidence once a later render on the same FIFO worker has been announced. It is read as
    //       "no FrameReady for the next generation", not as a signal count: releasing the image served()
    //       fetched re-announces the frame it frees, which is a FrameReady with no redraw behind it.
    // CONTROL: the `catch (const UnknownOutput& e)` arm deleted from DbusService::setOutputSize
    //       (backend/daemon/dbus_service.cpp), so it falls into the InvalidRequest arm -> "FAIL: a size
    //       for a name no monitor answers to is refused as io.wallpaperx.Daemon.Error.UnknownOutput",
    //       printing InvalidArgs, and nothing else in the phase. Reverted.
    struct WireRefusal {
        const char* what;
        std::string name;
        uint32_t width;
        uint32_t height;
        const char* error;
    };
    const std::vector<WireRefusal> refusals{
        {"a zero width", "DP-SIZED", 0, kReportedHeight, kInvalidArgs},
        {"a zero height", "DP-SIZED", kReportedWidth, 0, kInvalidArgs},
        {"a width past 8192", "DP-SIZED", 8193, kReportedHeight, kInvalidArgs},
        {"a height past 8192", "DP-SIZED", kReportedWidth, 8193, kInvalidArgs},
        {"an empty name", "", kReportedWidth, kReportedHeight, kInvalidArgs},
        {"a name past the cap", std::string(wallpaperx::kMaxOutputNameBytes + 1, 'D'), kReportedWidth,
         kReportedHeight, kInvalidArgs},
        {"a name no monitor answers to", "DP-NOBODY", kResizedWidth, kResizedHeight, kUnknownOutput},
    };
    for (const WireRefusal& refusal : refusals) {
        const std::string got = errorNameFrom([&] { setOutputSize(*proxy, refusal.name, refusal.width, refusal.height); });
        check(got == refusal.error,
              ("a size for " + std::string(refusal.what) + " is refused as " + refusal.error).c_str());
        if (got != refusal.error) std::fprintf(stderr, "  it answered '%s'\n", got.c_str());
    }
    check(errorNameFrom([&] { setMonitorOutput(*proxy, kMonitorSizeIdle, "DP-IDLE"); }).empty() &&
              errorNameFrom([&] { setOutputSize(*proxy, "DP-IDLE", 8192, 8192); }).empty(),
          "a size of 8192 on each side is accepted");
    check(errorNameFrom([&] { setMonitorOutput(*proxy, kMonitorUnsized, "DP-NOBODY"); }).empty(),
          "the name a size was refused for can be bound afterwards");
    if (!settle()) {
        check(false, "the barrier render after the refusals is announced");
        return;
    }
    check(signals.countOf(kMonitorSized, 2) == 0 && signals.countOf(kMonitorUnsized, 2) == 0,
          "no refused size, and no binding of the name one was refused for, draws anything");
    check(statusSays(kMonitorSized, kReportedWidth, kReportedHeight, "reported") &&
              statusSays(kMonitorUnsized, config.width, config.height, "default"),
          "after the refusals both monitors are drawn at what they were before them");

    // GOAL: S13.96 over the wire -- a new size redraws its monitor, announced, at the new size; the same
    //       size reported again draws nothing.
    // CONTROL: via control_batch.sh, --phase '^outputSizeWire$', 2026-09-29, each reverted. The
    //       `for (const int id : redraw) m_worker.post(id);` in DbusService::setOutputSize
    //       (backend/daemon/dbus_service.cpp) deleted -> "FAIL: a new size redraws its monitor, announced
    //       with FrameReady" after the signal timeout and "FAIL: the redraw is at the new size" printing
    //       "served 40x30", with the three lost-file checks below red since their redraw never runs.
    // CONTROL: for the repeat, it takes two mutations, since two mechanisms each stop it: the idempotent
    //       early return in WallpaperManager::setOutputSize AND the `== before` return in
    //       redrawIfResizedLocked both deleted (backend/daemon/wallpaper_manager.cpp) -> "FAIL: the same
    //       size reported again draws nothing", beside the refusal block's "no ... binding of the name
    //       one was refused for, draws anything". The early return alone -> the run passed; the
    //       `== before` deletion alone -> only that refusal-block line, whose bind changes no size.
    check(errorNameFrom([&] { setOutputSize(*proxy, "DP-SIZED", kResizedWidth, kResizedHeight); }).empty(),
          "a new size for a bound name is accepted");
    const bool redrawn = signals.wait(kMonitorSized, 2);
    check(redrawn, "a new size redraws its monitor, announced with FrameReady");
    const Served resized = served(kMonitorSized);
    check(resized.ok && resized.width == kResizedWidth && resized.height == kResizedHeight,
          "the redraw is at the new size");
    if (!resized.ok || resized.width != kResizedWidth) {
        std::fprintf(stderr, "  served %s\n", describeServed(resized).c_str());
    }
    check(errorNameFrom([&] { setOutputSize(*proxy, "DP-SIZED", kResizedWidth, kResizedHeight); }).empty(),
          "the same size reported again is accepted");
    if (!settle()) {
        check(false, "the barrier render after the repeated size is announced");
        return;
    }
    check(signals.countOf(kMonitorSized, 3) == 0, "the same size reported again draws nothing");

    // GOAL: S13.99 over the wire -- moving a sized name to another monitor redraws both, each at its new
    //       size.
    // CONTROL: the success path's `for (const int id : redraw) m_worker.post(id);` in
    //       DbusService::setMonitorOutput (backend/daemon/dbus_service.cpp) deleted, via
    //       control_batch.sh, --phase '^outputSizeWire$', 2026-09-29 -> "FAIL: moving a sized name to
    //       another monitor redraws both of them" after the signal timeout, alone. Reverted.
    check(errorNameFrom([&] { setMonitorOutput(*proxy, kMonitorUnsized, "DP-SIZED"); }).empty(),
          "a sized name can be moved to another monitor");
    const bool leftRedrawn = signals.wait(kMonitorSized, 3);
    const bool joinedRedrawn = signals.wait(kMonitorUnsized, 2);
    check(leftRedrawn && joinedRedrawn, "moving a sized name to another monitor redraws both of them");
    if (leftRedrawn && joinedRedrawn) {
        const Served left = served(kMonitorSized);
        const Served joined = served(kMonitorUnsized);
        check(left.ok && left.width == config.width && left.height == config.height && joined.ok &&
                  joined.width == kResizedWidth && joined.height == kResizedHeight,
              "the monitor the name left is drawn at the default, and the one it joined at the name's size");
        if (!left.ok || left.width != config.width || !joined.ok || joined.width != kResizedWidth) {
            std::fprintf(stderr, "  left %s, joined %s\n", describeServed(left).c_str(), describeServed(joined).c_str());
        }
    }

    // GOAL: S13.100's lost-file clause -- a resize redraw of a wallpaper whose file was deleted since it
    //       was drawn clears the frame and reports last_error and RenderFailed like any failed render,
    //       and is not retried.
    // WHY-THIS-SHAPE: read after a barrier render, when the failed pass has certainly finished -- polling
    //       for last_error could also catch the pass between recording the error and releasing the
    //       images, which the barrier phase below grades on its own.
    // CONTROL: it takes two mutations, both in WallpaperManager::renderPassLocked
    //       (backend/daemon/wallpaper_manager.cpp), because two things clear the frame: the
    //       `it->second.hasFrame = false;` in the commit block's error arm AND `releaseTarget(monitorId);`
    //       in the error tail. Either alone leaves this green, measured 2026-09-15. Both together -> "FAIL:
    //       and clears the frame rather than keep the old one" and "FAIL: and GetFrame answers
    //       RenderFailed", serving the old 64x48 frame, with the barrier phase's creation-failure pair and
    //       never-both pair red the same way. Reverted. Whether a failed monitor's images are released
    //       at all is not graded here, since the frame is withdrawn without it.
    const std::string lostPath = testAssetDir() + "/dbus_test_size_lost.png";
    {
        std::error_code ec;
        std::filesystem::copy_file(imagePath, lostPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            wallpaperx::testing::notVerified("S13.100's lost-file clause", "the wallpaper copy it deletes could not be made: " + ec.message());
            return;
        }
    }
    check(errorNameFrom([&] { setMonitorOutput(*proxy, kMonitorSizeLostFile, "DP-LOST"); }).empty() &&
              errorNameFrom([&] { setWallpaper(*proxy, "static", lostPath, kMonitorSizeLostFile); }).empty() &&
              signals.wait(kMonitorSizeLostFile, 1),
          "the wallpaper whose file is then deleted is drawn first");
    {
        std::error_code ec;
        std::filesystem::remove(lostPath, ec);
    }
    check(errorNameFrom([&] { setOutputSize(*proxy, "DP-LOST", kReportedWidth, kReportedHeight); }).empty(),
          "a size for the monitor whose file is gone is accepted");
    if (!settle()) {
        check(false, "the barrier render after the failed redraw is announced");
        return;
    }
    const StatusReply lostStatus = getStatusOrExplain(*proxy);
    const nlohmann::json* lostEntry = wallpaperStatus(lostStatus, std::to_string(kMonitorSizeLostFile));
    check(lostEntry != nullptr && lostEntry->contains("last_error"),
          "a resize redraw of a wallpaper whose file is gone reports last_error");
    check(lostEntry != nullptr && !lostEntry->value("has_frame", true), "and clears the frame rather than keep the old one");
    const Served lostServed = served(kMonitorSizeLostFile);
    check(!lostServed.ok && lostServed.error == kErrRenderFailed, "and GetFrame answers RenderFailed");
    if (lostServed.ok || lostServed.error != kErrRenderFailed) {
        std::fprintf(stderr, "  served %s\n", describeServed(lostServed).c_str());
    }
    if (!settle()) {
        check(false, "the second barrier render after the failed redraw is announced");
        return;
    }
    check(signals.countOf(kMonitorSizeLostFile, 2) == 0 &&
              wallpaperStatus(getStatusOrExplain(*proxy), std::to_string(kMonitorSizeLostFile)) != nullptr,
          "and the failed redraw is not retried");

    // GOAL: S13.161 over the wire -- SetOutputVisible takes the same name discipline as SetOutputSize
    //       (an unusable name is InvalidArgs, a name no monitor answers to is UnknownOutput), a bound
    //       name is accepted either way round, and GetStatus's `screen_visible` reports back what was
    //       last said about that screen. False for a daemon that accepts a name it cannot place --
    //       which would leave the desktop believing it had reported a dark screen that nothing acted
    //       on -- and false for one whose status cannot say why a wallpaper stopped moving.
    // WHY-THIS-SHAPE: the report is read back out of GetStatus rather than only checked for "no error",
    //       because an accepted call that recorded nothing answers identically. The pair false-then-true
    //       is asserted in both directions on one screen, so a status field wired to a constant fails
    //       on one of the two.
    // CONTROL: the `catch (const UnknownOutput& e)` arm deleted from DbusService::setOutputVisible
    //       (backend/daemon/dbus_service.cpp), so it falls into the InvalidRequest arm -> "FAIL: a
    //       visibility report for a name no monitor answers to is refused as
    //       io.wallpaperx.Daemon.Error.UnknownOutput", printing InvalidArgs. Observed 2026-09-20.
    //       Reverted.
    // CONTROL: `entry["screen_visible"] = monitor.screenVisible;` in DbusService::getStatus
    //       (backend/daemon/dbus_service.cpp) replaced by `entry["screen_visible"] = true;` -> "FAIL:
    //       GetStatus reports the screen as not drawn once its desktop says so". Observed 2026-09-20.
    //       Reverted.
    const std::vector<std::pair<const char*, std::string>> visibilityRefusals{
        {"an empty name", ""},
        {"a name past the cap", std::string(wallpaperx::kMaxOutputNameBytes + 1, 'D')},
    };
    for (const auto& [what, name] : visibilityRefusals) {
        const std::string got = errorNameFrom([&] { setOutputVisible(*proxy, name, false); });
        check(got == kInvalidArgs,
              ("a visibility report for " + std::string(what) + " is refused as InvalidArgs").c_str());
        if (got != kInvalidArgs) std::fprintf(stderr, "  it answered '%s'\n", got.c_str());
    }
    {
        const std::string got = errorNameFrom([&] { setOutputVisible(*proxy, "DP-NOSUCHSCREEN", false); });
        check(got == kUnknownOutput,
              "a visibility report for a name no monitor answers to is refused as "
              "io.wallpaperx.Daemon.Error.UnknownOutput");
        if (got != kUnknownOutput) std::fprintf(stderr, "  it answered '%s'\n", got.c_str());
    }
    const auto screenVisibleSays = [&proxy](int32_t monitorId) {
        const StatusReply status = getStatusOrExplain(*proxy);
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
        if (entry == nullptr) return std::string("(no entry: ") + status.error() + status.json().dump() + ")";
        if (!entry->contains("screen_visible")) return std::string("(no screen_visible field: ") + entry->dump() + ")";
        return std::string(entry->value("screen_visible", true) ? "true" : "false");
    };
    // DP-LOST, whose monitor has a wallpaper: GetStatus lists the wallpapers, so a screen named on a
    //       monitor nothing was ever set on has no entry to read the field out of.
    check(errorNameFrom([&] { setOutputVisible(*proxy, "DP-LOST", false); }).empty(),
          "a bound screen reporting that it is not being drawn is accepted");
    const std::string saidNotDrawn = screenVisibleSays(kMonitorSizeLostFile);
    check(saidNotDrawn == "false", "GetStatus reports the screen as not drawn once its desktop says so");
    if (saidNotDrawn != "false") std::fprintf(stderr, "  screen_visible read back as %s\n", saidNotDrawn.c_str());
    check(errorNameFrom([&] { setOutputVisible(*proxy, "DP-LOST", true); }).empty(),
          "and the same screen reporting that it is being drawn again is accepted");
    check(screenVisibleSays(kMonitorSizeLostFile) == "true", "and GetStatus reports it as drawn again");
}

// GOAL: S13.97's render half -- the first render after a restart is at the size saved with the screen's
//       name, before any screen reports.
// WHY-THIS-SHAPE: two managers on one config file, the second never told a size; the extent is read
//       from the committed frame and from the export. The daemon_system tier restarts a real process,
//       but what is at stake here is only which size restoreSaved() hands the first render.
// CONTROL: `m_outputSizes.insert_or_assign(entry.output, size);` deleted from the bound-name block of
//       WallpaperManager::restoreSaved() (backend/daemon/wallpaper_manager.cpp) via control_run.sh,
//       2026-09-29 -> "FAIL: the first render after a restart is at the size saved with the screen's
//       name", printing 64x48. Reverted.
void runOutputSizeRestartRenderChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                                      const Check& check) {
    const char* kGoal = "S13.97's render half (the first render after a restart uses the saved size)";
    const wallpaperx::testing::ScratchDir scratch("sizerestart");
    if (!scratch.ok()) {
        wallpaperx::testing::notVerified(kGoal, "a scratch directory could not be created");
        return;
    }
    wallpaperx::WallpaperManager::Config saving = config;
    saving.configFile = scratch.path() / "wallpaperx" / "config.json";
    {
        wallpaperx::WallpaperManager first(saving);
        wallpaperx::testing::nameScreen(first, kMonitorSized, "DP-SAVED");
        first.requestWallpaper("static", imagePath, kMonitorSized);
        std::vector<int> redraw;
        first.setOutputSize("DP-SAVED", kReportedWidth, kReportedHeight, redraw);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool saved = false;
        while (!saved && std::chrono::steady_clock::now() < deadline) {
            std::ifstream in(saving.configFile);
            const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
            if (!root.is_discarded() && root.is_object() && root.contains("monitors") && root["monitors"].is_array()) {
                for (const nlohmann::json& row : root["monitors"]) {
                    if (row.is_object() && row.value("output", std::string()) == "DP-SAVED" && row.contains("output_size") &&
                        row["output_size"] == nlohmann::json{{"width", kReportedWidth}, {"height", kReportedHeight}}) {
                        saved = true;
                    }
                }
            }
            if (!saved) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(saved, "the size reached the config file before the restart");
        if (!saved) return;
    }

    PhaseDeviceGrade grade("the output-size restart phase", check);
    wallpaperx::WallpaperManager second(saving);
    CaptureGradeOnExit captureGrade{grade, second};
    const std::vector<int> restored = second.restoreSaved();
    check(restored == std::vector<int>{kMonitorSized}, "the restart restores the sized monitor's wallpaper");
    if (!second.renderPending(kMonitorSized).rendered) {
        const wallpaperx::MonitorStatus failed = monitorStatus(second, kMonitorSized);
        check(false, "the restored wallpaper renders");
        std::fprintf(stderr, "  last_error: %s\n", failed.lastError.c_str());
        return;
    }
    const wallpaperx::MonitorStatus drawn = monitorStatus(second, kMonitorSized);
    uint32_t exportedWidth = 0;
    uint32_t exportedHeight = 0;
    wallpaperx::FrameExport exported = exportDelivered(second, kMonitorSized);
    if (exported.handle) {
        exportedWidth = exported.handle->frame.width;
        exportedHeight = exported.handle->frame.height;
        if (exported.handle->frame.fd >= 0) close(exported.handle->frame.fd);
        if (exported.handle->acquireFd >= 0) close(exported.handle->acquireFd);
    }
    const bool atSaved = drawn.width == kReportedWidth && drawn.height == kReportedHeight &&
                         exportedWidth == kReportedWidth && exportedHeight == kReportedHeight;
    check(atSaved, "the first render after a restart is at the size saved with the screen's name");
    if (!atSaved) {
        std::fprintf(stderr, "  committed %ux%u, exported %ux%u\n", drawn.width, drawn.height, exportedWidth,
                     exportedHeight);
    }
}

// GOAL: S13.104 -- an image released on a monitor that both owes a redraw and has a frame a consumer
//       can import queues the render AND announces the frame: the redraw at the new size is owed while
//       the old frame is still what a refused consumer can take.
// WHY-THIS-SHAPE: driven on the manager, on this thread, so the release's two answers are read directly
//       rather than inferred from which FrameReady arrives -- a re-announced generation and the
//       redraw's new one are both FrameReady on the wire.
// CONTROL: WallpaperManager::reportFreedImageLocked (backend/daemon/wallpaper_manager.cpp) put back to
//       `if (state.requestSeq != state.servicedSeq) { toRender.push_back(monitorId); return; }` before the
//       announcement, which is the code before this change -> "FAIL: and announces the frame it still
//       holds". Reverted.
void runResizeReleaseChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                            const Check& check) {
    const char* kGoal = "S13.104 (a release on a monitor owing a redraw queues it and announces the frame)";
    PhaseDeviceGrade grade("the resize-release phase", check);
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    const std::string alice = ":test.resize.alice";

    wallpaperx::testing::nameScreen(manager, kMonitorSized, "DP-HOLD");
    if (!renderOnce(manager, kMonitorSized, imagePath)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoal, "the first render on this host produced no frame to hold");
        return;
    }
    const Handover handed = fetchHandover(manager, kMonitorSized, alice);
    const uint64_t held = handed.bufferId;
    if (held == 0) {
        wallpaperx::testing::notVerified(kGoal, whyNoHandover(handed));
        return;
    }
    std::vector<int> redraw;
    manager.setOutputSize("DP-HOLD", kReportedWidth, kReportedHeight, redraw);
    const wallpaperx::MonitorStatus owing = monitorStatus(manager, kMonitorSized);
    const bool bothTrue = redraw == std::vector<int>{kMonitorSized} && owing.hasFrame && owing.renderOutstanding;
    check(bothTrue, "a new size on a monitor with a frame owes a redraw and keeps the frame until it runs");
    if (!bothTrue) return;

    std::vector<wallpaperx::FrameAvailable> announce;
    const std::vector<int> toRender = manager.releaseBuffer(alice, kMonitorSized, held, handed.number, &announce);
    check(toRender == std::vector<int>{kMonitorSized}, "releasing an image on a monitor that owes a redraw queues the render");
    const bool announced = std::any_of(announce.begin(), announce.end(), [&owing](const wallpaperx::FrameAvailable& a) {
        return a.monitorId == kMonitorSized && a.generation == owing.generation;
    });
    check(announced, "and announces the frame it still holds, so a consumer refused for want of an image can take it");
    check(manager.renderPending(kMonitorSized).rendered && monitorStatus(manager, kMonitorSized).width == kReportedWidth,
          "and the queued render then draws at the new size");
}

// Counts IN_CREATE events for one file name waiting on an inotify descriptor, without blocking.
int creationsOf(int inotifyFd, const std::string& name) {
    int count = 0;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t n = ::read(inotifyFd, buffer.data(), buffer.size());
        if (n <= 0) return count;
        for (ssize_t at = 0; at < n;) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + at);
            if ((event->mask & IN_CREATE) != 0 && event->len > 0 && name == event->name) ++count;
            at += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
        }
    }
}

void runResizeBarrierChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                            const Check& check) {
    using wallpaperx::RenderWorker;
    const char* kGoals = "S13.100's creation-failure and never-both clauses, S13.101 and S13.103 (a resize pass held "
                         "and failed from inside its own image rebuild)";
    constexpr int32_t kMonitorRecord = 0;
    constexpr int32_t kMonitorOvertaken = 1;
    constexpr int32_t kMonitorCreateFails = 2;
    constexpr int32_t kMonitorFileGone = 3;
    constexpr auto kGateLimit = std::chrono::seconds(30);

    const wallpaperx::testing::ScratchDir state("resizestate");
    if (!state.ok()) {
        wallpaperx::testing::notVerified(kGoals, "no scratch state directory could be created");
        return;
    }
    PhaseDeviceGrade grade("the resize barrier phase", check);
    wallpaperx::testing::FrameReadyLog signals;
    wallpaperx::WallpaperManager::Config withRecords = config;
    withRecords.stateDir = state.path();
    wallpaperx::WallpaperManager manager(withRecords);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager, [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    // Before the worker is destroyed, on every exit: a gate left armed or holding would park it.
    struct Unblock {
        RenderWorker& worker;
        ~Unblock() {
            g_framebufferGate.release();
            g_destroyFramebufferGate.release();
            worker.stop();
        }
    } unblock{worker};

    const auto firstFrame = [&](int32_t monitorId, const std::string& name, const std::string& path) {
        wallpaperx::testing::nameScreen(manager, monitorId, name);
        manager.requestWallpaper("static", path, monitorId);
        worker.post(monitorId);
        return signals.wait(monitorId, 1);
    };
    // What DbusService::setOutputSize does, minus the bus.
    const auto resize = [&](const std::string& name, uint32_t width, uint32_t height) {
        std::vector<int> redraw;
        manager.setOutputSize(name, width, height, redraw);
        for (const int id : redraw) worker.post(id);
    };
    struct Exported {
        bool served = false;
        uint32_t width = 0;
        uint32_t height = 0;
        wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
        std::string error;
    };
    // An export on the worker, which owns the Device, behind whatever is already queued there.
    const auto exportOnWorker = [&worker, &manager](int32_t monitorId) {
        auto answer = std::make_shared<std::promise<Exported>>();
        std::future<Exported> future = answer->get_future();
        worker.postTask([answer, &manager, monitorId](RenderWorker::TaskState taskState) {
            Exported out;
            if (taskState != RenderWorker::TaskState::Run) {
                out.error = "the worker did not run the export";
            } else {
                try {
                    wallpaperx::FrameExport e = exportDelivered(manager, monitorId);
                    if (e.handle) {
                        out.served = true;
                        out.width = e.handle->frame.width;
                        out.height = e.handle->frame.height;
                        if (e.handle->frame.fd >= 0) close(e.handle->frame.fd);
                        if (e.handle->acquireFd >= 0) close(e.handle->acquireFd);
                    } else {
                        out.reason = e.reason;
                    }
                } catch (const std::exception& ex) {
                    out.error = ex.what();
                }
            }
            answer->set_value(std::move(out));
        });
        return future;
    };
    const auto describeExport = [](const Exported& e) {
        if (e.served) return std::to_string(e.width) + "x" + std::to_string(e.height);
        if (!e.error.empty()) return "an exception: " + e.error;
        return "no frame, reason " + std::to_string(static_cast<int>(e.reason));
    };
    // True once everything queued on the worker before this call has run.
    const auto drained = [&worker]() {
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> future = done->get_future();
        worker.postTask([done](RenderWorker::TaskState) { done->set_value(); });
        return future.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
    };

    const int watch = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    struct CloseWatch {
        int fd;
        ~CloseWatch() {
            if (fd >= 0) ::close(fd);
        }
    } closeWatch{watch};
    // Deletions too, though only creations are counted: the kernel merges two identical unread events in
    // a row, so a watch on creations alone reports a record created, removed and created again as one.
    const bool watching = watch >= 0 && ::inotify_add_watch(watch, state.path().c_str(), IN_CREATE | IN_DELETE) >= 0;

    if (!firstFrame(kMonitorRecord, "DP-RECORD", imagePath)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the first render on this host produced no frame to resize");
        return;
    }

    // GOAL: S13.103 -- while the render thread creates a monitor's images at a new size, no crash record
    //       naming its wallpaper is on disk, so a driver dying on the size is not blamed on the picture.
    // WHY-THIS-SHAPE: the render thread is held inside the very call that finishes an image, and the
    //       record is looked for while it is held -- a check before or after the pass cannot see the
    //       span at all.
    // CONTROL: in WallpaperManager::renderPassLocked (backend/daemon/wallpaper_manager.cpp), the still's
    //       build given the pass's own record instead of the attempt's -- inside the
    //       retryOnShortageLocked(Relief::KeptStillsThenIdleVideo, ...) lambda, `const ApplyingRecord
    //       building(...)` replaced by `applying.emplace(m_config.stateDir, monitorId, type, path);`, so the
    //       record spans ensureTargets() -> exactly "FAIL: no crash record naming the wallpaper is on disk
    //       while its images are created at the new size". Re-run via control_batch.sh 2026-10-18, phase
    //       resizeBarrier, that line alone. Reverted.
    // COVERAGE-ASSERT: the same pass is required to have created the monitor's record at least twice --
    //       around the decode and around the draw -- so its absence at the hold is not a record this
    //       manager never writes.
    if (watching) creationsOf(watch, wallpaperx::crashRecordPath(state.path(), kMonitorRecord).filename().string());
    g_framebufferGate.arm(CallGate::Mode::Hold);
    resize("DP-RECORD", kReportedWidth, kReportedHeight);
    const bool rebuilding = g_framebufferGate.waitFired(kGateLimit);
    check(rebuilding, "a new size makes the render thread create the monitor's images again");
    if (rebuilding) {
        std::error_code ec;
        const bool onDisk = std::filesystem::exists(wallpaperx::crashRecordPath(state.path(), kMonitorRecord), ec);
        check(!ec && !onDisk, "no crash record naming the wallpaper is on disk while its images are created at the new size");
    }
    g_framebufferGate.release();
    check(signals.wait(kMonitorRecord, 2), "that redraw commits once image creation is let go");
    if (!watching) {
        wallpaperx::testing::notVerified("S13.103's coverage assertion", "inotify could not watch the state directory");
    } else {
        const int creations =
            creationsOf(watch, wallpaperx::crashRecordPath(state.path(), kMonitorRecord).filename().string());
        check(creations >= 2, "the resize pass wrote its crash record around the decode and the draw, so the absence "
                              "above was not a record never written");
        if (creations < 2) std::fprintf(stderr, "  the record was created %d time(s)\n", creations);
    }

    // GOAL: S13.101 -- a resize pass overtaken by a newer size after it replaced the images never leaves
    //       the monitor without a frame: an export queued behind it gets a frame, the old or the new, and
    //       never NotRendered.
    // WHY-THIS-SHAPE: the newer size lands while the first pass is held inside image creation, and the
    //       export is queued between that pass and the newer size's own post -- the one order in which
    //       the export runs before anything but the overtaken pass could draw.
    // CONTROL: WallpaperManager::renderPending's attempt loop (backend/daemon/wallpaper_manager.cpp)
    //       reduced to `return renderPassLocked(monitorId, rebuilt).outcome;` -> "FAIL: an export racing a
    //       resize pass overtaken mid-rebuild gets the old or the new frame, never none", printing
    //       "no frame, reason 1" (NotRendered). Observed 2026-09-18. Reverted.
    if (!firstFrame(kMonitorOvertaken, "DP-RACE", imagePath)) {
        check(false, "the monitor the overtaken pass is about is drawn first");
        return;
    }
    g_framebufferGate.arm(CallGate::Mode::Hold);
    resize("DP-RACE", kReportedWidth, kReportedHeight);
    const bool raceHeld = g_framebufferGate.waitFired(kGateLimit);
    check(raceHeld, "the pass to be overtaken is held inside its image rebuild");
    if (raceHeld) {
        std::future<Exported> racing = exportOnWorker(kMonitorOvertaken);
        resize("DP-RACE", kResizedWidth, kResizedHeight);
        g_framebufferGate.release();
        const bool answered = racing.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
        check(answered, "the export queued behind the overtaken pass is answered");
        if (answered) {
            const Exported got = racing.get();
            const bool framed = got.served && ((got.width == kResizedWidth && got.height == kResizedHeight) ||
                                               (got.width == config.width && got.height == config.height));
            check(framed, "an export racing a resize pass overtaken mid-rebuild gets the old or the new frame, never none");
            if (!framed) std::fprintf(stderr, "  the export got %s\n", describeExport(got).c_str());
        }
    }
    g_framebufferGate.release();

    if (!firstFrame(kMonitorCreateFails, "DP-NOIMAGE", imagePath)) {
        check(false, "the monitor whose image creation will fail is drawn first");
        return;
    }

    // GOAL: S15.781 -- a resize whose image creation runs out of memory once is retried after the kept
    //       still images are given back, and draws at the new size. False if one shortage fails the resize.
    // WHY-THIS-SHAPE: one injected failure in the real creation call; the pass's own still is kept by then,
    //       so there is something to give back. Graded by the served size, not "committed" alone.
    // CONTROL: control_batch.sh 2026-10-18, phase resizeBarrier, reverted: in renderPassLocked
    //       (backend/daemon/wallpaper_manager.cpp) `retryOnShortageLocked(Relief::KeptStills, [&] {` around
    //       ensureTargets() made a plain call -> this line, and the coverage line of S13.100 below.
    g_framebufferGate.arm(CallGate::Mode::Fail);
    resize("DP-NOIMAGE", kReportedWidth, kReportedHeight);
    const bool failedOnce = g_framebufferGate.waitFired(kGateLimit);
    check(failedOnce, "the image creation for the new size was reached, and failed once");
    if (failedOnce && drained()) {
        const wallpaperx::MonitorStatus once = monitorStatus(manager, kMonitorCreateFails);
        std::future<Exported> answer = exportOnWorker(kMonitorCreateFails);
        const bool exportAnswered = answer.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
        const Exported got = exportAnswered ? answer.get() : Exported{};
        const bool redrawn = once.hasFrame && once.lastError.empty() && got.served && got.width == kReportedWidth &&
                             got.height == kReportedHeight;
        check(redrawn, "a resize whose image creation runs out of memory once is retried and draws at the new size");
        if (!redrawn) {
            std::fprintf(stderr, "  has_frame %d, last_error \"%s\", the export got %s\n", once.hasFrame ? 1 : 0,
                         once.lastError.c_str(), exportAnswered ? describeExport(got).c_str() : "(no answer)");
        }
    }
    g_framebufferGate.release();

    // GOAL: S13.100's creation-failure clause -- a resize redraw whose image creation still fails after the
    //       kept stills are given back clears the frame, reports last_error and RenderFailed like any failed
    //       render, and is not retried again.
    // WHY-THIS-SHAPE: the failure is injected into the real creation call for exactly TWO calls -- the attempt
    //       and the one retry a shortage earns once kept stills are dropped -- so the daemon takes its own
    //       GpuFailure path; one injected failure is now relieved, which is S15.781 above. The state is read
    //       after the worker has drained.
    // CONTROL: control_batch.sh 2026-10-18, phase resizeBarrier, reverted: the `hasFrame = false;` clears deleted
    //       from both WallpaperManager::forgetImages and WallpaperManager::recordFailureLocked
    //       (backend/daemon/wallpaper_manager.cpp) -> "and has no frame, rather than keep the old one" and "and an
    //       export answers RenderFailed", with the never-both and lost-file lines red beside them.
    // COVERAGE-ASSERT: both armed failures are asserted spent, so the clause is graded on a pass that did retry.
    g_framebufferGate.arm(CallGate::Mode::Fail, {}, 2);
    resize("DP-NOIMAGE", kResizedWidth, kResizedHeight);
    const bool failedInside = g_framebufferGate.waitFired(kGateLimit);
    check(failedInside, "the image creation for the new size was reached, and failed");
    if (failedInside && drained()) {
        const int unspent = g_framebufferGate.remaining();
        check(unspent == 0, "both injected creation failures were spent: the shortage was retried once, and failed again");
        if (unspent != 0) std::fprintf(stderr, "  %d injected failure(s) unspent\n", unspent);
        const wallpaperx::MonitorStatus after = monitorStatus(manager, kMonitorCreateFails);
        check(!after.lastError.empty(), "a resize redraw whose image creation fails reports last_error");
        check(!after.hasFrame, "and has no frame, rather than keep the old one");
        std::future<Exported> answer = exportOnWorker(kMonitorCreateFails);
        const bool exportAnswered = answer.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
        const Exported got = exportAnswered ? answer.get() : Exported{};
        const bool refused = exportAnswered && !got.served && got.error.empty() &&
                             got.reason == wallpaperx::NoFrameReason::RenderFailed;
        check(refused, "and an export answers RenderFailed");
        if (!refused) std::fprintf(stderr, "  the export got %s\n", exportAnswered ? describeExport(got).c_str() : "(no answer)");
        const bool quiet = drained() && monitorStatus(manager, kMonitorCreateFails).generation == after.generation &&
                           !monitorStatus(manager, kMonitorCreateFails).renderOutstanding;
        check(quiet, "and nothing retries it");
    }
    g_framebufferGate.release();

    // GOAL: S13.100's never-both clause -- a failed resize redraw is never observable with a frame beside
    //       its error, not even between recording the error and releasing the images.
    // WHY-THIS-SHAPE: the render thread is held as the monitor's images are released -- the first
    //       framebuffer destroyed in the pass, inside releaseTarget(), which runs after the error is
    //       committed and before forgetImages() clears the flag a second time -- so only the clear made
    //       with the error can be what the read sees. A read after the pass cannot see that span. A hold
    //       at the unlink of the pass's crash record cannot be used: a still that fails to open has no
    //       record left by then (the build attempt's own is removed as it unwinds, before the error
    //       exists), and a hold there reads a state with no error yet.
    // CONTROL: `state.hasFrame = false;` deleted from WallpaperManager::recordFailureLocked
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a failed resize redraw is never observable
    //       with a frame beside its error", printing "has_frame is true beside last_error:
    //       StaticRenderer: cannot open image (No such file or directory)"; the check after it stays
    //       green, since releaseTarget() still clears the flag. Observed 2026-09-21 on the NVIDIA
    //       entry. Reverted.
    const std::filesystem::path gonePath = state.path() / "gone.png";
    {
        std::error_code ec;
        std::filesystem::copy_file(imagePath, gonePath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            wallpaperx::testing::notVerified("S13.100's never-both clause",
                                             "the wallpaper copy it deletes could not be made: " + ec.message());
            return;
        }
    }
    if (!firstFrame(kMonitorFileGone, "DP-GONE", gonePath.string())) {
        check(false, "the monitor whose file will be deleted is drawn first");
        return;
    }
    {
        std::error_code ec;
        std::filesystem::remove(gonePath, ec);
    }
    g_destroyFramebufferGate.arm(CallGate::Mode::Hold);
    resize("DP-GONE", kReportedWidth, kReportedHeight);
    const bool atRemoval = g_destroyFramebufferGate.waitFired(kGateLimit);
    check(atRemoval, "the failed redraw reaches the release of the monitor's images");
    if (atRemoval) {
        const wallpaperx::MonitorStatus during = monitorStatus(manager, kMonitorFileGone);
        // COVERAGE-ASSERT: the error must already be recorded when the thread is held, or "no frame
        //       beside an error" would be true of a read that came before the error existed.
        check(!during.lastError.empty(), "the failed redraw's error is already recorded when it is held");
        const bool both = during.hasFrame && !during.lastError.empty();
        check(!both, "a failed resize redraw is never observable with a frame beside its error");
        if (both) std::fprintf(stderr, "  has_frame is true beside last_error: %s\n", during.lastError.c_str());
    }
    g_destroyFramebufferGate.release();
    if (drained()) {
        const wallpaperx::MonitorStatus after = monitorStatus(manager, kMonitorFileGone);
        check(!after.lastError.empty() && !after.hasFrame, "once it has finished, the failed redraw has an error and no frame");
    }
}

// ---------------------------------------------------------------------------- //
// The acquire timeline: one per consumer connection, a value per picture
// ---------------------------------------------------------------------------- //

// Mirrored from WallpaperManager::kMaxAcquireTimelines rather than read from it: the limit a
// consumer meets on the wire is what is graded, and taking both sides from one symbol would let a
// change to it pass unnoticed.
constexpr size_t kExpectedMaxAcquireTimelines = 8;

// Whether the driver itself can hand a timeline semaphore across a process boundary, asked of the
// Vulkan API directly: the daemon's "Unsupported" is graded against this, so it must not come from
// the daemon's own AcquireTimeline::supported().
bool driverExportsTimelines(wallpaperx::Device& device) {
    if (!device.info().hasExternalSemaphoreFd || !device.timelineSemaphore()) return false;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    query.pNext = &type;
    query.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(device.physicalDevice(), &query, &properties);
    const VkExternalSemaphoreFeatureFlags both =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    return (properties.externalSemaphoreFeatures & both) == both;
}

// A consumer's timeline imported into a device of this test's own, so the value the daemon handed
// out can be compared with the count its payload has actually reached. The consumer's GPU cannot
// read the payload before it waits, so a value not yet reached is a wait that may never end.
class TimelineReader {
public:
    explicit TimelineReader(wallpaperx::Device& device) : m_device(device) {}
    ~TimelineReader() {
        if (m_semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_device.device(), m_semaphore, nullptr);
    }
    TimelineReader(const TimelineReader&) = delete;
    TimelineReader& operator=(const TimelineReader&) = delete;

    // Imports from a duplicate of `fd`, which stays the caller's.
    bool import(int fd, std::string& why) {
        auto importFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            vkGetDeviceProcAddr(m_device.device(), "vkImportSemaphoreFdKHR"));
        if (importFd == nullptr) {
            why = "vkImportSemaphoreFdKHR did not resolve on the test's device";
            return false;
        }
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        info.pNext = &type;
        if (vkCreateSemaphore(m_device.device(), &info, nullptr, &m_semaphore) != VK_SUCCESS) {
            m_semaphore = VK_NULL_HANDLE;
            why = "the test's device would not create a timeline semaphore";
            return false;
        }
        const int gift = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (gift < 0) {
            why = "the timeline descriptor could not be duplicated";
            return false;
        }
        VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        import.semaphore = m_semaphore;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        import.fd = gift;
        if (importFd(m_device.device(), &import) != VK_SUCCESS) {
            ::close(gift);  // not taken on failure
            why = "the daemon's timeline descriptor would not import into the test's device";
            return false;
        }
        return true;
    }

    bool counter(uint64_t& value, std::string& why) const {
        if (vkGetSemaphoreCounterValue(m_device.device(), m_semaphore, &value) != VK_SUCCESS) {
            why = "vkGetSemaphoreCounterValue failed";
            return false;
        }
        return true;
    }

private:
    wallpaperx::Device& m_device;
    VkSemaphore m_semaphore = VK_NULL_HANDLE;
};

// Waits until `name` has no owner on the bus. Asked of the bus itself, which emits the departure
// signal before it answers this, so a request sent to the daemon after a true return is read by
// the daemon after the departure. False when the name is still owned after five seconds.
bool waitNameGone(sdbus::IProxy& bus, const std::string& name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        bool owned = true;
        bus.callMethod("NameHasOwner").onInterface("org.freedesktop.DBus").withArguments(name).storeResultsTo(owned);
        if (!owned) return true;
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::string uniqueNameOf(sdbus::IProxy& proxy) { return proxy.getConnection().getUniqueName(); }

void runAcquireTimelineWireChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                                  const Check& check) {
    const char* kGoals = "S13.214, S13.217, S13.218 and S13.222's daemon halves (a value per picture on the caller's "
                         "own timeline, sync_fds for every other request, Unsupported and Busy)";
    constexpr int32_t kMonitor = 0;
    PhaseDeviceGrade grade("the acquire-timeline wire phase", check);
    wallpaperx::testing::FrameReadyLog signals;
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
    auto listener = wallpaperx::testing::subscribeFrameReady(signals, daemonName);
    auto bus = sdbus::createProxy(sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});

    // Each on a connection of its own, for the unique name the daemon keys a timeline on. alice asks
    // for a timeline; bob never does, which is what a plugin from before the timeline is.
    auto alice = sdbus::createProxy(daemonName, objectPath);
    auto bob = sdbus::createProxy(daemonName, objectPath);

    wallpaperx::testing::setWallpaper(*alice, "static", imagePath, kMonitor);
    if (!signals.wait(kMonitor, 1)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the daemon drew no frame on this host, so nothing could be handed over");
        return;
    }
    std::unique_ptr<wallpaperx::Device> probe;
    try {
        probe = std::make_unique<wallpaperx::Device>(/*enableValidation=*/false);
    } catch (const std::exception& e) {
        wallpaperx::testing::notVerified(kGoals, std::string("the test's own Vulkan device could not be made: ") + e.what());
        return;
    }
    const bool binaryAcquires = probe->info().hasExternalSemaphoreFd;
    const bool exportable = driverExportsTimelines(*probe);

    // One picture, given straight back so the per-consumer cap never answers in its place.
    const auto binaryPicture = [&](sdbus::IProxy& who) {
        wallpaperx::testing::Frame frame = wallpaperx::testing::getFrame(who, kMonitor);
        wallpaperx::testing::releaseFrame(who, kMonitor, frame);
        return frame;
    };
    const auto timelinePicture = [&](sdbus::IProxy& who) {
        wallpaperx::testing::Frame reply = wallpaperx::testing::getFrame(who, kMonitor, {.onTimeline = true});
        wallpaperx::testing::releaseFrame(who, kMonitor, reply);
        return reply;
    };
    const auto oneValidSync = [](const wallpaperx::testing::Frame& frame) {
        return frame.syncFds.size() == 1 && frame.syncFds.front().get() >= 0;
    };

    // GOAL: S13.222 -- a consumer that never asks for a timeline (a plugin from before it) is handed
    //       the binary semaphore on every picture, and asking GetFrame on the timeline without holding a
    //       timeline gets the same, with a value of 0. False if the daemon hands anyone a value it did
    //       not ask for on a timeline of its own.
    // WHY-THIS-SHAPE: asked before alice holds a timeline AND after, because the defect this guards
    //       is a timeline being given to whoever asks next or to everyone once one exists -- only the
    //       second reading can show that. Three pictures, so a value on the second or third is seen.
    // CONTROL: in WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp), the
    //       timeline the reply is signalled on made the first one registered, whoever asked and
    //       whether or not on the timeline -> "FAIL: a consumer that never asked for a timeline gets sync_fds on every
    //       GetFrame, while another consumer holds a timeline (0 of 3)" and "FAIL: and
    //       GetFrame on the timeline without a timeline answers value 0 with sync_fds, while another
    //       consumer holds a timeline (value 10, 0 sync descriptor(s))"; the before-reading stayed
    //       green. On the NVIDIA entry. Reverted.
    if (!binaryAcquires) {
        wallpaperx::testing::notVerified("S13.222's daemon half", "this GPU exports no binary semaphore, so no "
                                                                  "picture here can carry sync_fds");
    }
    const auto bobIsServedBinary = [&](const char* when) {
        int binary = 0;
        for (int i = 0; i < 3; ++i) binary += oneValidSync(binaryPicture(*bob)) ? 1 : 0;
        const wallpaperx::testing::Frame asked = timelinePicture(*bob);
        if (!binaryAcquires) return;
        check(binary == 3, (std::string("a consumer that never asked for a timeline gets sync_fds on every "
                                        "GetFrame, ") + when + " (" + std::to_string(binary) + " of 3)")
                               .c_str());
        check(asked.acquireValue == 0 && oneValidSync(asked),
              (std::string("and GetFrame on the timeline without a timeline answers value 0 with sync_fds, ") + when +
               " (value " + std::to_string(asked.acquireValue) + ", " + std::to_string(asked.syncFds.size()) +
               " sync descriptor(s))")
                  .c_str());
    };
    bobIsServedBinary("before anyone holds a timeline");

    // GOAL: S13.218 -- GetAcquireTimeline answers Unsupported only where the GPU can never export a
    //       timeline: on a driver that can, the first consumer is given one.
    // WHY-THIS-SHAPE: the driver is asked directly through the test's own device, not through the
    //       daemon's AcquireTimeline::supported(), which is part of what is graded.
    // CONTROL: in WallpaperManager::acquireTimelineFd (backend/daemon/wallpaper_manager.cpp),
    //       `!AcquireTimeline::supported(*m_device)` made `true` -> "FAIL: a GPU that can export a
    //       timeline gives the first consumer one (refused with
    //       'io.wallpaperx.Daemon.Error.Unsupported')" on the NVIDIA entry. The Unsupported direction
    //       runs only where the driver cannot export one: the same
    //       line made `false` -> "FAIL: a GPU that cannot export a timeline is answered Unsupported
    //       (answered 'io.wallpaperx.Daemon.Error.ExportFailed')" on dbus_service_lavapipe and
    //       dbus_service_classic_lavapipe, observed 2026-09-23. Reverted.
    sdbus::UnixFd aliceTimeline;
    std::string refusal;
    try {
        aliceTimeline = wallpaperx::testing::getAcquireTimeline(*alice);
    } catch (const sdbus::Error& e) {
        refusal = e.getName();
    }
    if (!exportable) {
        check(refusal == kErrUnsupported,
              ("a GPU that cannot export a timeline is answered Unsupported (answered '" + refusal + "')").c_str());
        wallpaperx::testing::notVerified(
            "S13.214, S13.217 and S13.222's daemon halves, and S13.218's Busy half (its Unsupported half ran above)",
            "this GPU cannot export a timeline semaphore, so no value can be handed over here");
        return;
    }
    check(refusal.empty() && aliceTimeline.get() >= 0,
          ("a GPU that can export a timeline gives the first consumer one (refused with '" + refusal + "')").c_str());
    if (!refusal.empty()) return;
    TimelineReader reader(*probe);
    std::string why;
    if (!reader.import(aliceTimeline.get(), why)) {
        wallpaperx::testing::notVerified("S13.214's reached-value clause", why);
        return;
    }

    // GOAL: S13.214 -- a picture asked for on the timeline carries a nonzero value and no sync_fds,
    //       each value is past the last, and every value has ALREADY been reached on the payload when
    //       the reply arrives. False for a value the payload has not reached: the consumer's GPU cannot
    //       read the count before it waits, so that wait may never end and stalls plasmashell.
    // WHY-THIS-SHAPE: the count is read from the consumer's own descriptor imported into a separate
    //       device, after the reply -- what a consumer can know, not what the daemon believes.
    // CONTROL: AcquireTimeline::signal (renderer/vulkan/frame_export.cpp) made to return `value + 1`
    //       -> "FAIL: every value has been reached on the payload when the reply arrives (0 of 4)",
    //       printing "value 2, the payload read 1" and so on. Observed 2026-09-23 on the NVIDIA
    //       entry. Reverted.
    uint64_t previous = 0;
    int reached = 0;
    int increasing = 0;
    int asValue = 0;
    constexpr int kPictures = 4;
    for (int i = 0; i < kPictures; ++i) {
        const wallpaperx::testing::Frame reply = timelinePicture(*alice);
        uint64_t count = 0;
        const bool read = reader.counter(count, why);
        if (reply.acquireValue != 0 && reply.syncFds.empty()) ++asValue;
        if (reply.acquireValue > previous) ++increasing;
        if (read && reply.acquireValue != 0 && count >= reply.acquireValue) ++reached;
        if (!read || count < reply.acquireValue) {
            std::fprintf(stderr, "  picture %d: value %llu, the payload read %s%llu\n", i,
                         static_cast<unsigned long long>(reply.acquireValue), read ? "" : "(unreadable) ",
                         static_cast<unsigned long long>(count));
        }
        previous = reply.acquireValue;
    }
    check(asValue == kPictures, ("a picture asked for on the caller's timeline carries a value and no sync_fds (" +
                                 std::to_string(asValue) + " of " + std::to_string(kPictures) + ")")
                                    .c_str());
    check(increasing == kPictures,
          ("each value is past the one before (" + std::to_string(increasing) + " of " + std::to_string(kPictures) + ")")
              .c_str());
    check(reached == kPictures, ("every value has been reached on the payload when the reply arrives (" +
                                 std::to_string(reached) + " of " + std::to_string(kPictures) + ")")
                                    .c_str());

    // GOAL: S13.217 -- holding a timeline switches nothing: the same consumer's GetFrame still gets
    //       sync_fds. The consumer chooses per request, which is what lets it fall back after a lost
    //       answer or a refused import without telling the daemon anything.
    // CONTROL: the `onTimeline &&` term dropped from the choice of timeline in
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a
    //       consumer holding a timeline still gets sync_fds from GetFrame (0 of 2)", and nothing
    //       else red, on the NVIDIA entry.
    int aliceBinary = 0;
    for (int i = 0; i < 2; ++i) aliceBinary += oneValidSync(binaryPicture(*alice)) ? 1 : 0;
    check(aliceBinary == 2, ("a consumer holding a timeline still gets sync_fds from GetFrame (" +
                             std::to_string(aliceBinary) + " of 2)")
                                .c_str());
    bobIsServedBinary("while another consumer holds a timeline");

    // GOAL: S13.218 -- at the limit of eight consumers the ninth is answered Busy, never Unsupported,
    //       and a holder leaving the bus frees its slot, so the ninth's next ask is granted.
    // WHY-THIS-SHAPE: the departure is waited for at the bus (NameHasOwner false), and a GetStatus
    //       round trip follows it: the daemon handles the departure signal and GetStatus on one
    //       connection, in order, so the ninth's retry is read after the departure. No sleep.
    // CONTROL: two, on the NVIDIA entry. (a) In
    //       DbusService::completeGetAcquireTimeline (backend/daemon/dbus_service.cpp) the
    //       TimelineLimitReached arm answered kErrorUnsupported -> "FAIL: the ninth consumer at the
    //       limit is answered Busy (answered 'io.wallpaperx.Daemon.Error.Unsupported')". (b)
    //       `m_manager.forgetAcquireTimeline(client);` deleted from DbusService::forgetClient ->
    //       "FAIL: once a holder leaves the bus, the ninth is given a timeline (answered
    //       'io.wallpaperx.Daemon.Error.Busy')". Also: the client test in liveAcquireTimeline
    //       (backend/daemon/wallpaper_manager.cpp) made `||` -> "FAIL: and GetFrame on the timeline
    //       without a timeline answers value 0 ... (value 5, 0 sync descriptor(s))" above, and this
    //       phase's Busy line red with "answered ''".
    std::vector<std::unique_ptr<sdbus::IProxy>> holders;
    int granted = 1;  // alice
    for (size_t i = 1; i < kExpectedMaxAcquireTimelines; ++i) {
        holders.push_back(sdbus::createProxy(daemonName, objectPath));
        try {
            (void)wallpaperx::testing::getAcquireTimeline(*holders.back());
            ++granted;
        } catch (const sdbus::Error& e) {
            std::fprintf(stderr, "  holder %zu refused: %s\n", i, e.getName().c_str());
        }
    }
    if (granted != static_cast<int>(kExpectedMaxAcquireTimelines)) {
        check(false, ("eight consumers are each given a timeline (" + std::to_string(granted) + ")").c_str());
        return;
    }
    auto ninth = sdbus::createProxy(daemonName, objectPath);
    const std::string atLimit = errorNameFrom([&] { (void)wallpaperx::testing::getAcquireTimeline(*ninth); });
    check(atLimit == "io.wallpaperx.Daemon.Error.Busy",
          ("the ninth consumer at the limit is answered Busy (answered '" + atLimit + "')").c_str());
    const std::string leaving = uniqueNameOf(*holders.front());
    holders.erase(holders.begin());
    const bool gone = waitNameGone(*bus, leaving);
    (void)wallpaperx::testing::getStatus(*alice);
    if (!gone) {
        wallpaperx::testing::harnessFailure("a holder's connection was closed and its name was still owned five "
                                            "seconds later, so no departure could be graded");
        return;
    }
    const std::string afterLeave = errorNameFrom([&] { (void)wallpaperx::testing::getAcquireTimeline(*ninth); });
    check(afterLeave.empty(), ("once a holder leaves the bus, the ninth is given a timeline (answered '" +
                               afterLeave + "')")
                                  .c_str());
}

// GOAL: S13.219 -- a consumer that leaves the bus with requests still queued behind the render
//       thread leaves nothing behind: neither the timeline nor the image those requests create after
//       its departure was handled. False for the leak the review found, where twelve such departures
//       left a fresh consumer refused both a timeline (Busy) and a frame (NoFreeImage).
// WHY-THIS-SHAPE: the render thread is held inside a render (the framebuffer gate) while twelve
//       consumers queue GetAcquireTimeline and GetFrame, prove the queueing with a GetStatus round
//       trip, and leave; each departure is waited for at the bus. Only then is the thread released,
//       so every request provably runs after its sender's departure -- a timing race cannot make it
//       run first and pass for the wrong reason. The simpler shape, a slow render in front, is what
//       the builder measured with, and whether it holds long enough is up to the host.
// CONTROL: WallpaperManager::noteRequestDone (backend/daemon/wallpaper_manager.cpp) made to return
//       false always, so no departure is applied again -> "FAIL: after twelve consumers left with
//       requests queued, a fresh consumer is given a timeline (answered
//       'io.wallpaperx.Daemon.Error.Busy')", "FAIL: and a frame (answered
//       'io.wallpaperx.Daemon.Error.NoFreeImage')" and "FAIL: no departed consumer still holds an
//       image (4 of 12 do)". Observed 2026-09-23 on the NVIDIA entry. Reverted.
// COVERAGE-ASSERT: the gate must be seen to fire, every departure seen at the bus, and afterwards
//       each victim is asked of the daemon's own hold ledger by name.
void runDepartedQueueChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                            const Check& check) {
    const char* kGoals = "S13.219 (a consumer that leaves with requests queued leaves no timeline and no hold)";
    constexpr int32_t kServed = 0;
    constexpr int32_t kHeld = 1;
    constexpr int kVictims = 12;
    PhaseDeviceGrade grade("the departed-queue phase", check);
    wallpaperx::testing::FrameReadyLog signals;
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    // Before the service is destroyed, on every exit: a held render would park its worker's stop.
    struct Unblock {
        ~Unblock() { g_framebufferGate.release(); }
    } unblock;
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
    auto listener = wallpaperx::testing::subscribeFrameReady(signals, daemonName);
    auto bus = sdbus::createProxy(sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
    auto control = sdbus::createProxy(daemonName, objectPath);

    wallpaperx::testing::setWallpaper(*control, "static", imagePath, kServed);
    if (!signals.wait(kServed, 1)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the daemon drew no frame on this host");
        return;
    }
    {
        wallpaperx::Device probe(/*enableValidation=*/false);
        if (!driverExportsTimelines(probe)) {
            wallpaperx::testing::notVerified(kGoals, "this GPU cannot export a timeline semaphore, so no consumer "
                                                     "here can be left holding one");
            return;
        }
    }

    g_framebufferGate.arm(CallGate::Mode::Hold);
    wallpaperx::testing::setWallpaper(*control, "static", imagePath, kHeld);
    if (!g_framebufferGate.waitFired(std::chrono::seconds(30))) {
        wallpaperx::testing::harnessFailure("the render thread never reached the held monitor's image creation, so "
                                            "nothing could be queued behind it");
        return;
    }
    std::vector<std::string> victims;
    for (int i = 0; i < kVictims; ++i) {
        auto victim = sdbus::createProxy(daemonName, objectPath);
        victims.push_back(uniqueNameOf(*victim));
        {
            // Scoped: a message outliving its proxy's connection crashes in its destructor.
            const auto noReply = [](sdbus::MethodReply, std::optional<sdbus::Error>) {};
            victim->callMethodAsync(victim->createMethodCall(sdbus::InterfaceName{DbusService::kInterface},
                                                             sdbus::MethodName{"GetAcquireTimeline"}),
                                    noReply);
            sdbus::MethodCall frame = victim->createMethodCall(sdbus::InterfaceName{DbusService::kInterface},
                                                               sdbus::MethodName{"GetFrame"});
            wallpaperx::testing::appendFrameArguments(frame, kServed);
            victim->callMethodAsync(frame, noReply);
        }
        // Answered on the bus thread in the order sent: once it returns, both above are queued.
        (void)wallpaperx::testing::getStatus(*victim);
        victim.reset();
        if (!waitNameGone(*bus, victims.back())) {
            wallpaperx::testing::harnessFailure("a departed consumer's name was still owned five seconds after its "
                                                "connection closed");
            return;
        }
    }
    (void)wallpaperx::testing::getStatus(*control);
    g_framebufferGate.release();

    auto fresh = sdbus::createProxy(daemonName, objectPath);
    const std::string timeline = errorNameFrom([&] { (void)wallpaperx::testing::getAcquireTimeline(*fresh); });
    std::string frameRefusal;
    try {
        const wallpaperx::testing::Frame frame = wallpaperx::testing::getFrame(*fresh, kServed);
        wallpaperx::testing::releaseFrame(*fresh, kServed, frame);
    } catch (const sdbus::Error& e) {
        frameRefusal = e.getName();
    }
    check(timeline.empty(), ("after twelve consumers left with requests queued, a fresh consumer is given a "
                             "timeline (answered '" + timeline + "')")
                                .c_str());
    check(frameRefusal.empty(), ("and a frame (answered '" + frameRefusal + "')").c_str());
    // Asked by ending it: a freed hold always either names a monitor to redraw or re-announces the
    // committed frame, so "neither" means there was nothing to free.
    int stillHolding = 0;
    for (const std::string& victim : victims) {
        std::vector<wallpaperx::FrameAvailable> announced;
        if (!manager.forgetHeldBuffers(victim, &announced).empty() || !announced.empty()) ++stillHolding;
    }
    check(stillHolding == 0, ("no departed consumer still holds an image (" + std::to_string(stillHolding) + " of " +
                              std::to_string(kVictims) + " do)")
                                 .c_str());
}

// GOAL: S13.219 on the throw path -- when answering a departed consumer's queued request throws
//       inside the worker task (here, sending the reply fails), its count still comes down and its
//       departure is still applied again, so the image the request recorded does not stay held.
// WHY-THIS-SHAPE: the consumer departs while two GetFrame requests of its own wait behind a held
//       render (the phase above's shape), and only the SECOND reply's send is failed. One request
//       is not enough any more: a reply that fails to send now withdraws its own hand-over, which
//       for a first hand-over ends the claim and would pass this check with the departure never
//       applied again. The first reply, sent to a name already gone, leaves a claim the withdrawal
//       of the second (a repeat of the same image) keeps. A later GetFrame from another consumer
//       runs behind both on the one worker; the hold ledger is then asked.
// CONTROL: in DbusService::postCall (backend/daemon/dbus_service.cpp), `if (failed)
//       std::rethrow_exception(failed);` moved above the count-down, so a throw skips it -> "FAIL: a
//       departed consumer whose queued request threw holds no image afterwards". The other throw
//       path -- postTask itself throwing before the task is queued -- is not reached: only a failed
//       allocation there can throw, and nothing here can make one fail at that call alone.
// COVERAGE-ASSERT: exactly one send must have been failed, and it must be the departed consumer's
//       second.
void runPostCallThrowChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                            const Check& check) {
    const char* kGoals = "S13.219's throw path (a request that throws still applies its sender's departure)";
    constexpr int32_t kServed = 0;
    constexpr int32_t kHeld = 1;
    PhaseDeviceGrade grade("the post-call throw phase", check);
    wallpaperx::testing::FrameReadyLog signals;
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    struct Unblock {
        ~Unblock() {
            g_framebufferGate.release();
            g_failReplyArmed.store(false);
        }
    } unblock;
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
    auto listener = wallpaperx::testing::subscribeFrameReady(signals, daemonName);
    auto bus = sdbus::createProxy(sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
    auto control = sdbus::createProxy(daemonName, objectPath);

    wallpaperx::testing::setWallpaper(*control, "static", imagePath, kServed);
    if (!signals.wait(kServed, 1)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the daemon drew no frame on this host");
        return;
    }
    g_framebufferGate.arm(CallGate::Mode::Hold);
    wallpaperx::testing::setWallpaper(*control, "static", imagePath, kHeld);
    if (!g_framebufferGate.waitFired(std::chrono::seconds(30))) {
        wallpaperx::testing::harnessFailure("the render thread never reached the held monitor's image creation");
        return;
    }
    std::string victimName;
    {
        auto victim = sdbus::createProxy(daemonName, objectPath);
        victimName = uniqueNameOf(*victim);
        for (int i = 0; i < 2; ++i) {
            sdbus::MethodCall frame = victim->createMethodCall(sdbus::InterfaceName{DbusService::kInterface},
                                                               sdbus::MethodName{"GetFrame"});
            wallpaperx::testing::appendFrameArguments(frame, kServed);
            victim->callMethodAsync(frame, [](sdbus::MethodReply, std::optional<sdbus::Error>) {});
        }
        (void)wallpaperx::testing::getStatus(*victim);
    }
    if (!waitNameGone(*bus, victimName)) {
        wallpaperx::testing::harnessFailure("the departed consumer's name was still owned five seconds later");
        return;
    }
    const int failed0 = g_repliesFailed.load();
    failReplyTo(victimName, /*letThrough=*/1);
    (void)wallpaperx::testing::getStatus(*control);
    g_framebufferGate.release();

    // Behind the victim's task on the one worker: once this is answered, that task has run.
    std::string after;
    try {
        const wallpaperx::testing::Frame frame = wallpaperx::testing::getFrame(*control, kServed);
        wallpaperx::testing::releaseFrame(*control, kServed, frame);
    } catch (const sdbus::Error& e) {
        after = e.getName();
    }
    g_failReplyArmed.store(false);
    if (g_repliesFailed.load() - failed0 != 1) {
        wallpaperx::testing::notVerified(kGoals, "the reply to the departed consumer was never sent through the "
                                                 "interposed sd_bus_send, so no throw reached the worker task");
        return;
    }
    std::vector<wallpaperx::FrameAvailable> announced;
    const bool stillHolds = !manager.forgetHeldBuffers(victimName, &announced).empty() || !announced.empty();
    check(after.empty(), ("the worker went on serving after the throw (answered '" + after + "')").c_str());
    check(!stillHolds, "a departed consumer whose queued request threw holds no image afterwards");
}

// Mirrored from WallpaperManager::kMaxDescriptorsPerClient and kMaxQueuedRequestsPerClient rather
// than read from them, for kExpectedHeldPerClient's reason. A reply's own charge is not mirrored:
// it is what the reply carried, which each check counts from the replies it received.
constexpr int kExpectedDescriptorCap = 64;
constexpr int kExpectedRequestShare = 16;
// How many replies of `perReply` descriptors each fit under the cap.
constexpr int framesUnderCap(int perReply) { return perReply > 0 ? kExpectedDescriptorCap / perReply : 0; }
constexpr const char* kErrTooManyDescriptors = "io.wallpaperx.Daemon.Error.TooManyDescriptors";
constexpr const char* kErrTooManyRequests = "io.wallpaperx.Daemon.Error.TooManyRequests";
constexpr const char* kErrExportFailed = "io.wallpaperx.Daemon.Error.ExportFailed";
constexpr const char* kErrTooManyTimelineCopies = "io.wallpaperx.Daemon.Error.TooManyTimelineCopies";
// Mirrored from WallpaperManager::kMaxTimelineCopiesPerClient, for kExpectedDescriptorCap's reason.
constexpr int kExpectedTimelineCopies = 4;

// A GetFrame that may be refused: the frame, or the error's name and message.
struct FetchResult {
    std::optional<Frame> frame;
    std::string error;
    std::string message;
};

FetchResult tryGetFrame(sdbus::IProxy& who, int32_t monitorId) {
    FetchResult out;
    try {
        out.frame = getFrame(who, monitorId);
    } catch (const sdbus::Error& e) {
        out.error = e.getName();
        out.message = e.getMessage();
    }
    return out;
}

using wallpaperx::testing::describeHolder;
using wallpaperx::testing::HolderEntry;
using wallpaperx::testing::holderEntry;

// Whether GetStatus lists `client` on the monitor, well formed, charged `descriptors`.
bool chargedExactly(const HolderEntry& holder, size_t descriptors) {
    return holder.listed && !holder.malformed && holder.descriptors == static_cast<int64_t>(descriptors);
}

std::string idList(const std::vector<uint64_t>& ids) {
    std::string out;
    for (const uint64_t id : ids) out += (out.empty() ? "" : ", ") + std::to_string(id);
    return "[" + out + "]";
}

// The descriptor-cap phase: one still frame on each of two monitors, fetched by callers that keep
// what they are given. Its own manager and service, so every charge read here is this phase's.
void runDescriptorCapChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                            const Check& check) {
    const char* kGoals = "S15.265-S15.271 (each caller's cap on descriptors it has not given back)";
    constexpr int32_t kMonitorA = 0;
    constexpr int32_t kMonitorB = 1;
    PhaseDeviceGrade grade("the descriptor-cap phase", check);
    wallpaperx::testing::FrameReadyLog signals;
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    struct Disarm {
        ~Disarm() { g_failNextMemoryExport.store(false); }
    } disarm;
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
    auto listener = wallpaperx::testing::subscribeFrameReady(signals, daemonName);
    auto alice = sdbus::createProxy(daemonName, objectPath);
    auto bob = sdbus::createProxy(daemonName, objectPath);
    const std::string aliceName = uniqueNameOf(*alice);
    const std::string bobName = uniqueNameOf(*bob);

    wallpaperx::testing::setWallpaper(*alice, "static", imagePath, kMonitorA);
    wallpaperx::testing::setWallpaper(*alice, "static", imagePath, kMonitorB);
    if (!signals.wait(kMonitorA, 1) || !signals.wait(kMonitorB, 1)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the daemon drew no frame on this host, so nothing could be handed over");
        return;
    }
    StdoutCapture log;
    if (!log.ok()) {
        wallpaperx::testing::harnessFailure("stdout could not be redirected, so the daemon's refusal lines cannot be "
                                            "counted (S15.267)");
    }
    const std::string aliceLine = "refused for \"" + aliceName + "\" until it gives descriptors back";

    // GOAL: S15.265 -- a caller fetching one still frame again and again, declaring nothing and
    //       giving nothing back, is served exactly as many replies as fit in 64 descriptors at what
    //       each reply carried (32 at two -- memory and sync_fds -- or 64 at one where the GPU exports
    //       no semaphore), and the next is refused as TooManyDescriptors with a message naming that
    //       caller; the cap is the caller's across monitors, so its first fetch on another monitor
    //       is refused too.
    // WHY-THIS-SHAPE: the same image every time, because a repeat of an image already held was the
    //       unbounded case -- one claim, a new reply each time. Fetching distinct images would be
    //       refused by the two-images-per-caller limit long before 64 and grade that limit instead.
    //       The per-reply figure is read off the first reply, not mirrored: it differs per driver.
    // CONTROL: `held->charges.insert(...)` deleted from withHandover's already-held branch
    //       (backend/daemon/wallpaper_manager.cpp), so a repeat is charged nothing -> "FAIL: and the
    //       next is refused as TooManyDescriptors, naming that caller (answered '': )".
    // COVERAGE-ASSERT: every reply must have carried the first one's count, or the expected figure
    //       would not be one number.
    int aliceServedOnA = 0;
    int served = 0;
    int otherImages = 0;
    int unlikeFirst = 0;
    int aliceSent = 0;  // descriptors alice's replies on monitor A carried
    uint64_t aliceBuffer = 0;
    uint64_t aliceNewest = 0;  // the newest hand-over of that image alice has read
    const FetchResult aliceFirst = tryGetFrame(*alice, kMonitorA);
    const int perReply = aliceFirst.frame ? static_cast<int>(aliceFirst.frame->descriptors()) : 0;
    const int underCap = framesUnderCap(perReply);
    if (aliceFirst.frame) {
        aliceBuffer = aliceFirst.frame->bufferId;
        aliceNewest = aliceFirst.frame->handover;
        aliceSent += perReply;
        ++served;
    }
    for (int i = served; aliceFirst.frame && i < underCap; ++i) {
        const FetchResult fetched = tryGetFrame(*alice, kMonitorA);
        if (!fetched.frame) {
            std::fprintf(stderr, "  fetch %d of %d was refused: %s\n", i + 1, underCap, fetched.error.c_str());
            break;
        }
        aliceNewest = std::max(aliceNewest, fetched.frame->handover);
        if (fetched.frame->bufferId != aliceBuffer) ++otherImages;
        if (static_cast<int>(fetched.frame->descriptors()) != perReply) ++unlikeFirst;
        aliceSent += static_cast<int>(fetched.frame->descriptors());
        ++served;
    }
    aliceServedOnA += served;
    const FetchResult over = tryGetFrame(*alice, kMonitorA);
    if (over.frame) {
        ++aliceServedOnA;
        aliceSent += static_cast<int>(over.frame->descriptors());
    }
    check(perReply > 0 && served == underCap && otherImages == 0 && unlikeFirst == 0,
          ("a caller is served every fetch of one still frame that fits under the cap, giving none back (" +
           std::to_string(served) + " served of " + std::to_string(underCap) + " at " + std::to_string(perReply) +
           " descriptor(s) each, " + std::to_string(otherImages) + " of another image, " +
           std::to_string(unlikeFirst) + " carrying another count)")
              .c_str());
    check(over.error == kErrTooManyDescriptors && over.message.find("caller \"" + aliceName + "\"") != std::string::npos,
          ("and the next is refused as TooManyDescriptors, naming that caller (answered '" + over.error + "': " +
           over.message + ")")
              .c_str());
    int refusedAgain = 0;
    for (int i = 0; i < 5; ++i) {
        const FetchResult again = tryGetFrame(*alice, kMonitorA);
        if (again.frame) {
            ++aliceServedOnA;
            aliceSent += static_cast<int>(again.frame->descriptors());
        }
        if (again.error == kErrTooManyDescriptors) ++refusedAgain;
    }
    const FetchResult aliceOnB = tryGetFrame(*alice, kMonitorB);
    check(aliceOnB.error == kErrTooManyDescriptors,
          ("the cap is the caller's across monitors: its first fetch on another monitor is refused too (answered '" +
           aliceOnB.error + "')")
              .c_str());

    // GOAL: S15.346 on the bus -- a refusal for descriptors says nothing about that monitor's ring:
    //       the monitor it was asked of, where nobody has been refused an image, still reports no
    //       refused hand-over.
    // WHY-THIS-SHAPE: read on monitor B, which nobody had asked anything of before alice's refused
    //       fetch, so the count can only have come from that refusal. That it is not collection either
    //       is graded where a monitor can be made stale, in video_wallpaper's watchEvidence phase.
    // CONTROL: `++it->second.refusedHandovers;` added to the `if (!admission.admitted)` branch of
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a
    //       refusal for descriptors is not a refused hand-over on the monitor asked (refused_handovers
    //       1)".
    {
        const StatusReply status = wallpaperx::testing::getStatus(*bob);
        const nlohmann::json* entryB = wallpaperStatus(status, std::to_string(kMonitorB));
        const int refusedOnB = entryB != nullptr ? entryB->value("refused_handovers", -1) : -1;
        check(aliceOnB.error == kErrTooManyDescriptors && refusedOnB == 0,
              ("a refusal for descriptors is not a refused hand-over on the monitor asked (refused_handovers " +
               std::to_string(refusedOnB) + ")")
                  .c_str());
    }

    // GOAL: S15.266 -- a caller at the cap costs no one else anything: another connection is served
    //       on the same monitor and on another one while the first is being refused.
    // CONTROL: the `hold.client == client` filter dropped from descriptorsInFlightLocked
    //       (backend/daemon/wallpaper_manager.cpp), so every holder's charge counts against every
    //       caller -> "FAIL: another caller is served on the same monitor and on another while the
    //       first is refused (same monitor answered 'io.wallpaperx.Daemon.Error.TooManyDescriptors',
    //       0 of 3 on the other)" and the later descriptor lines.
    const FetchResult bobA = tryGetFrame(*bob, kMonitorA);
    int bobServedOnB = 0;
    int bobSentOnB = 0;
    for (int i = 0; i < 3; ++i) {
        const FetchResult onB = tryGetFrame(*bob, kMonitorB);
        if (!onB.frame) continue;
        ++bobServedOnB;
        bobSentOnB += static_cast<int>(onB.frame->descriptors());
    }
    check(bobA.frame.has_value() && bobServedOnB == 3,
          ("another caller is served on the same monitor and on another while the first is refused (same monitor "
           "answered '" + bobA.error + "', " + std::to_string(bobServedOnB) + " of 3 on the other)")
              .c_str());

    // GOAL: S15.270 -- GetStatus reports, for each holder of each monitor, the descriptors its
    //       hand-overs there count toward the cap: exactly the descriptors those replies carried, a
    //       repeat of a held image included.
    // WHY-THIS-SHAPE: the expected values are the descriptors this phase counted in the replies it
    //       received, not a per-reply constant, so a daemon that charged a different amount from what
    //       it sent is caught whatever it admitted.
    // CONTROL: in WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp), the
    //       charge made `(sendMemory ? 1u : 0u) + 1u`, a sync descriptor charged whether or not one
    //       went -> on dbus_service_lavapipe, "FAIL: GetStatus reports each holder's descriptors as
    //       those its replies carried (...)".
    {
        const StatusReply status = wallpaperx::testing::getStatus(*bob);
        const HolderEntry aliceA = holderEntry(status, kMonitorA, aliceName);
        const HolderEntry bobOnA = holderEntry(status, kMonitorA, bobName);
        const HolderEntry bobOnB = holderEntry(status, kMonitorB, bobName);
        const size_t bobASent = bobA.frame ? bobA.frame->descriptors() : 0;
        check(status.ok() && chargedExactly(aliceA, static_cast<size_t>(aliceSent)) && chargedExactly(bobOnA, bobASent) &&
                  chargedExactly(bobOnB, static_cast<size_t>(bobSentOnB)),
              ("GetStatus reports each holder's descriptors as those its replies carried (first caller on monitor " +
               std::to_string(kMonitorA) + ": " + describeHolder(aliceA) + " for " + std::to_string(aliceServedOnA) +
               " replies carrying " + std::to_string(aliceSent) + "; second caller " + describeHolder(bobOnA) +
               " where " + std::to_string(bobASent) + " went, and " + describeHolder(bobOnB) + " where " +
               std::to_string(bobSentOnB) + " went)" + (status.ok() ? "" : "; GetStatus failed: " + status.error()))
                  .c_str());
    }
    if (bobA.frame) wallpaperx::testing::releaseFrame(*bob, kMonitorA, *bobA.frame);

    // GOAL: S15.267 -- a run of refusals of one caller is logged once, and a new run after the
    //       caller was served again is logged again: two runs, two lines.
    // WHY-THIS-SHAPE: counted in this process's stdout, where the service logs; the first run has
    //       seven refusals (the first past the cap, five more, one on the other monitor), so one line
    //       per refusal and one per run differ by six.
    // CONTROL: `m_descriptorRefusals.insert(client).second` made `true` in admitDescriptorsLocked
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a run of 7 refusals of one caller is
    //       logged once (7 line(s))".
    const int firstRunRefusals = (over.error == kErrTooManyDescriptors ? 1 : 0) + refusedAgain +
                                 (aliceOnB.error == kErrTooManyDescriptors ? 1 : 0);
    if (log.ok()) {
        const int lines = log.linesWith({aliceLine});
        check(lines == 1, ("a run of " + std::to_string(firstRunRefusals) + " refusals of one caller is logged once (" +
                           std::to_string(lines) + " line(s))")
                              .c_str());
    }

    // GOAL: S15.268 -- giving the image back ends the charge: the caller's next fetch is served, and
    //       it is again served exactly as many replies as before (S15.265's count) before the next
    //       refusal.
    // CONTROL: in descriptorsInFlightLocked (backend/daemon/wallpaper_manager.cpp), the total made
    //       to start at kMaxDescriptorsPerClient whenever m_descriptorRefusals holds the caller, so a
    //       refusal outlives the release -> "FAIL: after giving its image back, the caller is served
    //       again (answered 'io.wallpaperx.Daemon.Error.TooManyDescriptors')".
    // WHY-THIS-SHAPE: the release names the newest hand-over alice read, giving back every reply of
    //       the image (S15.304); only when she was served at all, or it names hand-over 0.
    if (aliceFirst.frame) wallpaperx::testing::releaseBuffer(*alice, kMonitorA, aliceBuffer, aliceNewest);
    const FetchResult back = tryGetFrame(*alice, kMonitorA);
    check(back.frame.has_value(),
          ("after giving its image back, the caller is served again (answered '" + back.error + "')").c_str());
    int servedSinceRelease = back.frame ? 1 : 0;
    std::string secondRefusal;
    for (int i = 0; i < underCap + 4 && secondRefusal.empty(); ++i) {
        const FetchResult fetched = tryGetFrame(*alice, kMonitorA);
        if (fetched.frame) {
            ++servedSinceRelease;
        } else {
            secondRefusal = fetched.error;
        }
    }
    check(underCap > 0 && servedSinceRelease == underCap && secondRefusal == kErrTooManyDescriptors,
          ("and is served the same " + std::to_string(underCap) + " replies again before the next refusal (" +
           std::to_string(servedSinceRelease) + " served, then '" + secondRefusal + "')")
              .c_str());
    if (log.ok()) {
        const int lines = log.linesWith({aliceLine});
        check(lines == 2, ("a second run of refusals, after the caller was served again, is logged again (" +
                           std::to_string(lines) + " line(s) for two runs)")
                              .c_str());
    }

    // GOAL: S15.269 -- a live timeline counts as ONE descriptor however many copies were sent: after
    //       four copies, 31 frames of two (1 + 62 = 63) and the 32nd refused; one copy and 31 frames
    //       refuse a frame (63 + 2) yet one more copy is sent, charged nothing. S15.343 -- the fifth
    //       GetAcquireTimeline is refused TooManyTimelineCopies naming the caller and saying to keep
    //       one copy; another connection is served meanwhile.
    // WHY-THIS-SHAPE: 31 frames separate "counts once" from "each copy counts" (30) and "counts
    //       nothing" (32). A departure clearing the count is graded at the manager (frameCharge),
    //       since the bus never reuses a unique name.
    // CONTROL: --phase '^descriptorCap$': in backend/daemon/wallpaper_manager.cpp, (a)
    //       descriptorsInFlightLocked adding `entry.copiesSent` per live timeline -> "FAIL: a timeline
    //       sent four times counts as one descriptor: ... (30 served ...)"; (b) WallpaperManager::
    //       acquireTimelineFd's `existing->copiesSent >= kMaxTimelineCopiesPerClient` made `false &&` ->
    //       "FAIL: a caller is sent four copies of its timeline and the fifth is refused
    //       TooManyTimelineCopies, ..." and the other-connection line; in DbusService::
    //       completeGetAcquireTimeline (backend/daemon/dbus_service.cpp), (c) the `catch (const
    //       TimelineCopyLimitReached&)` arm deleted -> the same two lines; (d)
    //       `m_manager.timelineCopyDelivered(client);` deleted -> the same caller line.
    // COVERAGE-ASSERT: each of the four copies must be an open descriptor.
    {
        std::unique_ptr<wallpaperx::Device> probe;
        try {
            probe = std::make_unique<wallpaperx::Device>(/*enableValidation=*/false);
        } catch (const std::exception& e) {
            wallpaperx::testing::notVerified("S15.269 and S15.343 over the bus", std::string("the test's own Vulkan device could not be made: ") +
                                                            e.what());
        }
        if (probe && !driverExportsTimelines(*probe)) {
            wallpaperx::testing::notVerified("S15.269 and S15.343 over the bus", "this GPU cannot export a timeline semaphore, so no timeline "
                                                        "reply can be charged here");
        } else if (probe && perReply != 2) {
            // The mixed case below tells a timeline reply's 1 from 2 only against frames charged 2.
            wallpaperx::testing::notVerified("S15.269 and S15.343 over the bus", "a plain frame reply here carried " + std::to_string(perReply) +
                                                            " descriptor(s), not the memory and a sync descriptor");
        } else if (probe) {
            {
                auto carol = sdbus::createProxy(daemonName, objectPath);
                const std::string carolName = uniqueNameOf(*carol);
                int granted = 0;
                int openCopies = 0;
                for (int i = 0; i < kExpectedTimelineCopies; ++i) {
                    try {
                        const sdbus::UnixFd copy = wallpaperx::testing::getAcquireTimeline(*carol);
                        ++granted;
                        if (copy.isValid() && ::fcntl(copy.get(), F_GETFD) != -1) ++openCopies;
                    } catch (const sdbus::Error& e) {
                        std::fprintf(stderr, "  timeline copy %d refused: %s\n", i + 1, e.getName().c_str());
                    }
                }
                std::string fifth;
                std::string fifthMessage;
                try {
                    (void)wallpaperx::testing::getAcquireTimeline(*carol);
                } catch (const sdbus::Error& e) {
                    fifth = e.getName();
                    fifthMessage = e.getMessage();
                }
                const bool namesCarol = fifthMessage.find(carolName) != std::string::npos;
                const bool saysKeepOne = fifthMessage.find("keep one") != std::string::npos;
                const bool saysNewConnection = fifthMessage.find("new connection") != std::string::npos;
                check(granted == kExpectedTimelineCopies && openCopies == kExpectedTimelineCopies &&
                          fifth == kErrTooManyTimelineCopies && namesCarol && saysKeepOne && saysNewConnection,
                      ("a caller is sent four copies of its timeline and the fifth is refused TooManyTimelineCopies, "
                       "naming it, telling it to keep one and that a new connection may ask (" +
                       std::to_string(granted) + " sent, " + std::to_string(openCopies) + " open, then '" + fifth +
                       "': " + fifthMessage + ")")
                          .c_str());

                int carolFrames = 0;
                std::string carolOver;
                for (int i = 0; i < framesUnderCap(perReply) + 2 && carolOver.empty(); ++i) {
                    const FetchResult r = tryGetFrame(*carol, kMonitorB);
                    if (r.frame) {
                        ++carolFrames;
                    } else {
                        carolOver = r.error;
                    }
                }
                check(carolFrames == (kExpectedDescriptorCap - 1) / perReply && carolOver == kErrTooManyDescriptors,
                      ("a timeline sent four times counts as one descriptor: the caller is then served " +
                       std::to_string((kExpectedDescriptorCap - 1) / perReply) + " frames before the cap (" +
                       std::to_string(carolFrames) + " served, then '" + carolOver + "')")
                          .c_str());

                // The other connection, asked while carol is refused.
                auto erika = sdbus::createProxy(daemonName, objectPath);
                const std::string erikaTimeline =
                    errorNameFrom([&] { (void)wallpaperx::testing::getAcquireTimeline(*erika); });
                const std::string carolStill =
                    errorNameFrom([&] { (void)wallpaperx::testing::getAcquireTimeline(*carol); });
                check(erikaTimeline.empty() && carolStill == kErrTooManyTimelineCopies,
                      ("another connection is sent a timeline while the first is refused copies (answered '" +
                       erikaTimeline + "', the first '" + carolStill + "')")
                          .c_str());
            }

            auto dave = sdbus::createProxy(daemonName, objectPath);
            const std::string daveTimeline =
                errorNameFrom([&] { (void)wallpaperx::testing::getAcquireTimeline(*dave); });
            int daveFrames = 0;
            for (int i = 0; i < framesUnderCap(perReply) - 1; ++i) {
                if (!tryGetFrame(*dave, kMonitorB).frame) break;
                ++daveFrames;
            }
            const FetchResult daveOver = tryGetFrame(*dave, kMonitorB);
            const std::string daveLastTimeline =
                errorNameFrom([&] { (void)wallpaperx::testing::getAcquireTimeline(*dave); });
            check(daveTimeline.empty() && daveFrames == framesUnderCap(perReply) - 1 &&
                      daveOver.error == kErrTooManyDescriptors && daveLastTimeline.empty(),
                  ("one timeline reply and 31 frames leave a caller refused a frame but served one more timeline "
                   "reply (timeline '" + daveTimeline + "', " + std::to_string(daveFrames) + " frames, then frame '" +
                   daveOver.error + "', timeline '" + daveLastTimeline + "')")
                      .c_str());
        }
    }

    // GOAL: S15.271 -- a failed export takes back only its own reply: after an export fails for an
    //       image the caller already held, the caller still holds that image and is charged exactly
    //       the descriptors of the replies it did receive; after the export of a first claim fails,
    //       the caller holds nothing.
    // WHY-THIS-SHAPE: failed at the driver boundary (the interposed vkGetMemoryFdKHR), so the daemon
    //       takes its real path; the ledger read back through GetStatus.
    // CONTROL: in WallpaperManager::undoHandoverLocked (backend/daemon/wallpaper_manager.cpp), (a)
    //       `record.previous <= held->released` made `true` -> "FAIL: after a failed export of an image
    //       it already held, the caller still holds it, charged for the two replies it received (not
    //       listed ...)"; (b) its `endClaim(...)` deleted -> "FAIL: after a failed export of its first
    //       image, the caller holds nothing on that monitor (...)".
    // COVERAGE-ASSERT: each export must fail at the interposer exactly once, or not verified.
    {
        auto erin = sdbus::createProxy(daemonName, objectPath);
        const std::string erinName = uniqueNameOf(*erin);
        const FetchResult first = tryGetFrame(*erin, kMonitorA);
        const FetchResult second = tryGetFrame(*erin, kMonitorA);
        const int failed0 = g_memoryExportsFailed.load();
        g_failNextMemoryExport.store(true);
        const FetchResult failed = tryGetFrame(*erin, kMonitorA);
        g_failNextMemoryExport.store(false);
        if (!first.frame || !second.frame) {
            check(false, ("a fresh caller is served its first two fetches (answered '" + first.error + "', '" +
                          second.error + "')")
                             .c_str());
        } else if (g_memoryExportsFailed.load() - failed0 != 1) {
            wallpaperx::testing::notVerified("S15.271", "the interposed vkGetMemoryFdKHR was not reached exactly once, "
                                                        "so no export was made to fail");
        } else {
            check(failed.error == kErrExportFailed,
                  ("an export made to fail is answered ExportFailed (answered '" + failed.error + "')").c_str());
            const HolderEntry charged = holderEntry(wallpaperx::testing::getStatus(*erin), kMonitorA, erinName);
            const size_t received = first.frame->descriptors() + second.frame->descriptors();
            check(chargedExactly(charged, received) && charged.buffers == std::vector<uint64_t>{first.frame->bufferId},
                  ("after a failed export of an image it already held, the caller still holds it, charged for the "
                   "two replies it received (" + describeHolder(charged) + " where " + std::to_string(received) +
                   " went)")
                      .c_str());
        }

        // Every announcement already caused on monitor A was sent before the release call that caused
        // it returned, so one signal sent after them -- a render on a third monitor -- arrives after
        // them all: A's count read then is settled.
        constexpr int32_t kMonitorBarrier = 2;
        wallpaperx::testing::setWallpaper(*bob, "static", imagePath, kMonitorBarrier);
        const bool settled = signals.wait(kMonitorBarrier, 1);
        const uint64_t generationA = monitorStatus(manager, kMonitorA).generation;
        const size_t announcedBefore = signals.countOf(kMonitorA, generationA);

        auto frank = sdbus::createProxy(daemonName, objectPath);
        const std::string frankName = uniqueNameOf(*frank);
        const int failed1 = g_memoryExportsFailed.load();
        g_failNextMemoryExport.store(true);
        const FetchResult firstFailed = tryGetFrame(*frank, kMonitorA);
        g_failNextMemoryExport.store(false);
        if (g_memoryExportsFailed.load() - failed1 != 1 || firstFailed.error != kErrExportFailed) {
            wallpaperx::testing::notVerified("S15.271's first-claim half", "the export of the first claim did not fail "
                                                                           "at the interposer (answered '" +
                                                                               firstFailed.error + "')");
        } else {
            const HolderEntry charged = holderEntry(wallpaperx::testing::getStatus(*frank), kMonitorA, frankName);
            check(!charged.listed, ("after a failed export of its first image, the caller holds nothing on that monitor "
                                    "(" + describeHolder(charged) + ")")
                                       .c_str());

            // GOAL: S15.347 on the bus -- an export that fails after taking the image out of the ring,
            //       and whose undo ends the claim, reports the image freed as a release does: the frame
            //       is announced again, so a consumer refused for want of an image learns it may ask.
            // WHY-THIS-SHAPE: counted after a barrier signal, the repeat waited for with a bound that
            //       fails. The owed render is graded at the manager (frameCharge): on the bus a second
            //       post is unobservable.
            // CONTROL: (a) `reportFreed();` deleted from DbusService::completeFrame's catch
            //       (backend/daemon/dbus_service.cpp); (b) the `reportFreedImageLocked(...)` in
            //       WallpaperManager::exportCurrentFrame's catch deleted. Each -> "FAIL: an export that
            //       fails and ends its caller's first claim announces the frame again".
            if (!settled) {
                wallpaperx::testing::harnessFailure("the barrier render on a third monitor was never announced, so "
                                                    "whether the failed export announced anything cannot be read");
            } else {
                const bool reannounced = signals.waitForRepeat(kMonitorA, generationA, announcedBefore + 1);
                check(reannounced, ("an export that fails and ends its caller's first claim announces the frame again ("
                                    + std::to_string(signals.countOf(kMonitorA, generationA) - announcedBefore) +
                                    " announcement(s) since)")
                                       .c_str());
            }
        }
    }
}

// Every answer to requests sent without waiting, collected off the proxies' own threads.
struct Answers {
    std::mutex mutex;
    std::condition_variable changed;
    int served = 0;
    std::map<std::string, int> errors;
    std::string lastMessage;

    void record(const std::optional<sdbus::Error>& error) {
        std::lock_guard<std::mutex> lock(mutex);
        if (error) {
            ++errors[error->getName()];
            lastMessage = error->getMessage();
        } else {
            ++served;
        }
        changed.notify_all();
    }
    // The Locked forms are for waitUntil's predicates, which already hold the mutex.
    int refusedLocked(const std::string& name) const {
        const auto it = errors.find(name);
        return it == errors.end() ? 0 : it->second;
    }
    int answeredLocked() const {
        int total = served;
        for (const auto& [name, count] : errors) total += count;
        return total;
    }
    int refused(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        return refusedLocked(name);
    }
    int answered() {
        std::lock_guard<std::mutex> lock(mutex);
        return answeredLocked();
    }
    int servedCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return served;
    }
    // False when `done` did not become true within `limit`.
    bool waitUntil(const std::function<bool(const Answers&)>& done, std::chrono::seconds limit) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, limit, [&] { return done(*this); });
    }
    std::string describe() {
        std::lock_guard<std::mutex> lock(mutex);
        std::string out = std::to_string(served) + " served";
        for (const auto& [name, count] : errors) out += ", " + std::to_string(count) + " " + name;
        return out;
    }
};

// `count` GetFrame calls from `who`, sent without waiting; each answer lands in `answers`.
void sendFrameRequests(sdbus::IProxy& who, int32_t monitorId, int count, const std::shared_ptr<Answers>& answers) {
    for (int i = 0; i < count; ++i) {
        // Scoped: a message outliving its proxy's connection crashes in its destructor.
        sdbus::MethodCall call =
            who.createMethodCall(sdbus::InterfaceName{DbusService::kInterface}, sdbus::MethodName{"GetFrame"});
        wallpaperx::testing::appendFrameArguments(call, monitorId);
        who.callMethodAsync(call, [answers](sdbus::MethodReply, std::optional<sdbus::Error> error) {
            answers->record(error);
        });
    }
}

// The fair-queue phase: the render thread is held inside a render (the framebuffer gate) while
// callers queue frame requests behind it, so how many are waiting is fixed by the test, not by how
// fast the worker happens to drain. Its own manager and service.
void runRequestShareChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                           const Check& check) {
    const char* kGoals = "S15.275-S15.278 (each caller's share of the render worker's queue)";
    constexpr int32_t kServed = 0;
    constexpr int32_t kHeldFirst = 1;
    constexpr int32_t kHeldSecond = 2;
    constexpr int32_t kHeldThird = 3;
    constexpr int kOver = 8;
    constexpr auto kLimit = std::chrono::seconds(30);
    PhaseDeviceGrade grade("the request-share phase", check);
    wallpaperx::testing::FrameReadyLog signals;
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    // Before the service is destroyed, on every exit: a held render would park its worker's stop.
    struct Unblock {
        ~Unblock() { g_framebufferGate.release(); }
    } unblock;
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
    auto listener = wallpaperx::testing::subscribeFrameReady(signals, daemonName);
    auto bus = sdbus::createProxy(sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
    auto control = sdbus::createProxy(daemonName, objectPath);

    wallpaperx::testing::setWallpaper(*control, "static", imagePath, kServed);
    if (!signals.wait(kServed, 1)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the daemon drew no frame on this host");
        return;
    }
    const auto holdWorker = [&](int32_t monitorId) {
        g_framebufferGate.arm(CallGate::Mode::Hold);
        wallpaperx::testing::setWallpaper(*control, "static", imagePath, monitorId);
        if (g_framebufferGate.waitFired(kLimit)) return true;
        wallpaperx::testing::harnessFailure("the render thread never reached the held monitor's image creation, so "
                                            "nothing could be queued behind it");
        return false;
    };
    StdoutCapture log;
    if (!log.ok()) {
        wallpaperx::testing::harnessFailure("stdout could not be redirected, so the daemon's refusal lines cannot be "
                                            "counted (S15.276)");
    }

    auto alice = sdbus::createProxy(daemonName, objectPath);
    auto bob = sdbus::createProxy(daemonName, objectPath);
    const std::string aliceName = uniqueNameOf(*alice);
    const std::string aliceLine = "refusing requests from \"" + aliceName + "\"";
    const auto aliceAnswers = std::make_shared<Answers>();
    const auto bobAnswers = std::make_shared<Answers>();

    // GOAL: S15.275 -- with the worker held, a caller that has 16 requests waiting has every
    //       further one refused at once as TooManyRequests, with a message naming it; none of the
    //       first 16 is refused.
    // WHY-THIS-SHAPE: the worker is held inside a render, so all 24 requests are provably waiting
    //       at once -- with a running worker, how many are queued at any instant is up to the
    //       scheduler and no exact count could be asserted. A GetStatus round trip, answered on the
    //       bus thread in order, proves every request was admitted or refused before the wait.
    // CONTROL: `>= kMaxQueuedRequestsPerClient` made `>= 1000` in WallpaperManager::admitRequest
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a caller with 16 requests waiting has
    //       every further one refused at once as TooManyRequests, naming it (0 served)" after the
    //       bounded wait.
    if (!holdWorker(kHeldFirst)) return;
    // No refusal of the first burst can be logged before this: S15.277's gap is measured from here.
    const auto firstBurstSent = std::chrono::steady_clock::now();
    sendFrameRequests(*alice, kServed, kExpectedRequestShare + kOver, aliceAnswers);
    (void)wallpaperx::testing::getStatus(*alice);
    (void)aliceAnswers->waitUntil([](const Answers& a) { return a.refusedLocked(kErrTooManyRequests) >= kOver; },
                                  std::chrono::seconds(5));
    sendFrameRequests(*bob, kServed, 1, bobAnswers);
    (void)wallpaperx::testing::getStatus(*bob);
    const std::string whileHeld = aliceAnswers->describe();
    const bool namesAlice = [&] {
        std::lock_guard<std::mutex> lock(aliceAnswers->mutex);
        return aliceAnswers->lastMessage.find("caller \"" + aliceName + "\"") != std::string::npos;
    }();
    check(aliceAnswers->refused(kErrTooManyRequests) == kOver && aliceAnswers->answered() == kOver && namesAlice,
          ("a caller with 16 requests waiting has every further one refused at once as TooManyRequests, naming it (" +
           whileHeld + ")")
              .c_str());
    g_framebufferGate.release();

    // GOAL: S15.276 -- another connection asking while the first is at its share is queued and
    //       served, never refused.
    // CONTROL: the count in admitRequest and noteRequestDone (backend/daemon/wallpaper_manager.cpp)
    //       keyed on the constant "all" instead of the caller -> "FAIL: another caller asking meanwhile
    //       is queued and served, never refused (0 served, 1 io.wallpaperx.Daemon.Error.TooManyRequests)";
    //       later in the phase the control connection's own fetch is refused too and ends it as an
    //       unexpected exception.
    const bool drained =
        aliceAnswers->waitUntil([](const Answers& a) { return a.answeredLocked() >= kExpectedRequestShare + kOver; },
                                kLimit) &&
        bobAnswers->waitUntil([](const Answers& a) { return a.answeredLocked() >= 1; }, kLimit);
    if (!drained) {
        wallpaperx::testing::harnessFailure("the queued requests were not all answered within 30 s of the render "
                                            "thread being let go (" + aliceAnswers->describe() + "; other caller " +
                                            bobAnswers->describe() + ")");
        return;
    }
    check(bobAnswers->servedCount() == 1, ("another caller asking meanwhile is queued and served, never refused (" +
                                           bobAnswers->describe() + ")")
                                              .c_str());

    // GOAL: S15.277 -- refusals of one caller are logged once per burst: a second burst inside the
    //       quiet period, after its queue fully emptied between the two, adds no line.
    // WHY-THIS-SHAPE: the queue is drained between the bursts on purpose -- ending a burst when the
    //       caller's queue empties was the design that logged 16,172 lines in 5 s. The 10 s premise is
    //       measured, not assumed: from before the first burst was sent to after the second burst's
    //       refusals were seen bounds the true gap from above, so a graded pass is sound, and a host
    //       too slow to fit inside the quiet period is reported not verified rather than failed.
    // CONTROL: two, on the NVIDIA entry, both reverted. (a) `const bool first = true;` in
    //       WallpaperManager::admitRequest (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a caller
    //       refused in two bursts within 10 s, its queue emptied between them, is logged once (16 line(s)
    //       for 16 refusals)"; (b) noteRequestDone erasing the entry whenever the count reaches zero -> the
    //       same line with "2 line(s) for 16 refusals". The guard: test-side, kExpectedRefusalQuiet made
    //       1 ms -> "NOT VERIFIED -- S15.277 ... 7 ms apart", the entry a skip with no FAIL.
    const int servedFirst = aliceAnswers->servedCount();
    const int refusedFirst = aliceAnswers->refused(kErrTooManyRequests);
    const int answeredFirst = aliceAnswers->answered();
    {
        // Back to no charge, so the second burst is served under the descriptor cap too.
        const FetchResult mine = tryGetFrame(*alice, kServed);
        if (!mine.frame) {
            check(false, ("between the bursts the caller is served a frame to give back (answered '" + mine.error +
                          "')")
                             .c_str());
            return;
        }
        wallpaperx::testing::releaseFrame(*alice, kServed, *mine.frame);
    }
    if (!holdWorker(kHeldSecond)) return;
    sendFrameRequests(*alice, kServed, kExpectedRequestShare + kOver, aliceAnswers);
    (void)wallpaperx::testing::getStatus(*alice);
    (void)aliceAnswers->waitUntil(
        [answeredFirst](const Answers& a) { return a.answeredLocked() >= answeredFirst + kOver; },
        std::chrono::seconds(5));
    const auto secondRefusedBy = std::chrono::steady_clock::now();
    g_framebufferGate.release();
    const bool secondDrained = aliceAnswers->waitUntil(
        [answeredFirst](const Answers& a) {
            return a.answeredLocked() >= answeredFirst + kExpectedRequestShare + kOver;
        },
        kLimit);
    if (!secondDrained) {
        wallpaperx::testing::harnessFailure("the second burst was not all answered within 30 s of the render thread "
                                            "being let go (" + aliceAnswers->describe() + ")");
        return;
    }
    // GOAL: S15.278's first half -- once its queue has emptied, a caller that was refused has its
    //       whole share again: the second burst queues 16 and refuses 8, exactly as the first did.
    // CONTROL: the S15.278 break below (a refusal counted) -> "FAIL: after its queue emptied, a
    //       refused caller has its whole share again (8 queued and 16 refused of 24)".
    const int servedSecond = aliceAnswers->servedCount() - servedFirst;
    const int refusedSecond = aliceAnswers->refused(kErrTooManyRequests) - refusedFirst;
    check(servedSecond == kExpectedRequestShare && refusedSecond == kOver,
          ("after its queue emptied, a refused caller has its whole share again (" + std::to_string(servedSecond) +
           " queued and " + std::to_string(refusedSecond) + " refused of " +
           std::to_string(kExpectedRequestShare + kOver) + ")")
              .c_str());
    // Mirrored from WallpaperManager's kRequestRefusalQuiet, which is private.
    constexpr auto kExpectedRefusalQuiet = std::chrono::seconds(10);
    const auto burstGap = secondRefusedBy - firstBurstSent;
    if (!log.ok()) {
        // Already reported as a harness fault where the capture failed.
    } else if (burstGap >= kExpectedRefusalQuiet) {
        wallpaperx::testing::notVerified(
            "S15.277", "the two bursts were up to " +
                           std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(burstGap).count()) +
                           " ms apart on this host, past the daemon's 10 s quiet period, so one line was not owed");
    } else {
        const int lines = log.linesWith({aliceLine});
        check(lines == 1, ("a caller refused in two bursts within 10 s, its queue emptied between them, is logged once (" +
                           std::to_string(lines) + " line(s) for " +
                           std::to_string(aliceAnswers->refused(kErrTooManyRequests)) + " refusals)")
                              .c_str());
    }

    // GOAL: S15.278 -- a refused request leaves no count behind: a caller refused past its share
    //       that then leaves the bus with requests still queued still has its departure applied again
    //       once they are answered, so no image it was granted after leaving stays held.
    // WHY-THIS-SHAPE: S13.219's shape with one request past the share: the departure is seen at the
    //       bus while the worker is held, so every queued request runs after it; the daemon's own
    //       hold ledger is then asked by name.
    // CONTROL: `++queued.count;` moved above the cap test in WallpaperManager::admitRequest
    //       (backend/daemon/wallpaper_manager.cpp), so a refusal is counted and never counted down ->
    //       "FAIL: a caller refused past its share that leaves with requests queued holds no image
    //       afterwards".
    // COVERAGE-ASSERT: the departing caller must have seen its refusal before leaving, or the goal is
    //       reported not verified.
    if (!holdWorker(kHeldThird)) return;
    std::string victimName;
    bool victimRefused = false;
    {
        auto victim = sdbus::createProxy(daemonName, objectPath);
        victimName = uniqueNameOf(*victim);
        const auto victimAnswers = std::make_shared<Answers>();
        sendFrameRequests(*victim, kServed, kExpectedRequestShare + 1, victimAnswers);
        (void)wallpaperx::testing::getStatus(*victim);
        victimRefused = victimAnswers->waitUntil(
            [](const Answers& a) { return a.refusedLocked(kErrTooManyRequests) >= 1; }, std::chrono::seconds(5));
    }
    if (!waitNameGone(*bus, victimName)) {
        wallpaperx::testing::harnessFailure("the departing caller's name was still owned five seconds after its "
                                            "connection closed");
        return;
    }
    (void)wallpaperx::testing::getStatus(*control);
    g_framebufferGate.release();
    // Behind the departed caller's requests on the one worker: once this is answered, they have run.
    {
        const FetchResult barrier = tryGetFrame(*control, kServed);
        if (!barrier.frame) {
            check(false, ("a caller's GetFrame behind a departed caller's queued requests is served (answered '" +
                          barrier.error + "')")
                             .c_str());
            return;
        }
        wallpaperx::testing::releaseFrame(*control, kServed, *barrier.frame);
    }
    if (!victimRefused) {
        wallpaperx::testing::notVerified("S15.278", "the departing caller was never refused past its share, so its "
                                                    "departure graded nothing about a refusal");
        return;
    }
    std::vector<wallpaperx::FrameAvailable> announced;
    const bool stillHolds = !manager.forgetHeldBuffers(victimName, &announced).empty() || !announced.empty();
    check(!stillHolds, "a caller refused past its share that leaves with requests queued holds no image afterwards");
}

// One in-process reply with its descriptors already closed: what it carried, what it was charged,
// and the handle withdrawHandover() takes back.
struct InProcessReply {
    bool served = false;
    wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
    bool memory = false;
    bool syncFd = false;
    uint64_t value = 0;
    wallpaperx::FrameHandle handle;

    uint64_t id() const { return handle.frame.bufferId; }
    uint32_t carried() const { return (memory ? 1u : 0u) + (syncFd ? 1u : 0u); }
};

InProcessReply closeReply(wallpaperx::FrameExport exported) {
    InProcessReply out;
    out.reason = exported.reason;
    if (!exported.handle) return out;
    out.served = true;
    out.handle = std::move(*exported.handle);
    out.memory = out.handle.frame.fd >= 0;
    out.syncFd = out.handle.acquireFd >= 0;
    out.value = out.handle.acquireValue;
    if (out.memory) ::close(std::exchange(out.handle.frame.fd, -1));
    if (out.syncFd) ::close(std::exchange(out.handle.acquireFd, -1));
    return out;
}

// A reply that went out: the export and the delivery DbusService::completeFrame records once it is sent.
InProcessReply exportClosed(wallpaperx::WallpaperManager& manager, int32_t monitorId, const std::string& client,
                            bool onTimeline = false, const std::vector<uint64_t>& known = {}) {
    return closeReply(exportDelivered(manager, monitorId, client, onTimeline, known));
}

// A reply that never went out, the only kind completeFrame withdraws: exported, never delivered.
InProcessReply exportUnsent(wallpaperx::WallpaperManager& manager, int32_t monitorId, const std::string& client,
                            bool onTimeline = false, const std::vector<uint64_t>& known = {}) {
    std::vector<int> toRender;
    return closeReply(manager.exportCurrentFrame(monitorId, client, onTimeline, known, toRender));
}

// One caller's claim on one monitor, as the manager's own ledger records it. `newest` and `released`
// are the last image's, which is the only one the phases reading them give this caller.
struct HoldView {
    bool present = false;
    uint32_t descriptors = 0;
    std::vector<uint64_t> charged;  // the hand-overs still charged, by number
    uint64_t newest = 0;
    uint64_t released = 0;
    std::vector<uint64_t> ids;
};

HoldView holdOf(const wallpaperx::WallpaperManager& manager, int32_t monitorId, const std::string& client) {
    HoldView view;
    for (const wallpaperx::BufferHold& hold : monitorStatus(manager, monitorId).holds) {
        if (hold.client != client) continue;
        view.present = true;
        for (const wallpaperx::HeldBuffer& buffer : hold.buffers) {
            for (const wallpaperx::HandoverCharge& charge : buffer.charges) {
                view.descriptors += charge.descriptors;
                view.charged.push_back(charge.handover);
            }
            view.newest = buffer.newest;
            view.released = buffer.released;
            view.ids.push_back(buffer.id);
        }
    }
    return view;
}

std::string describeHold(const HoldView& hold) {
    if (!hold.present) return "no claim";
    return std::to_string(hold.descriptors) + " descriptor(s) charged to hand-overs " + idList(hold.charged) +
           ", newest " + std::to_string(hold.newest) + ", released to " + std::to_string(hold.released) +
           ", images " + idList(hold.ids);
}

// The charge the manager's ledger holds for the hand-over `reply` made to `client`: the descriptors
// of its HandoverCharge, 0 when that hand-over is recorded and carries no charge (a reply that sent
// nothing is not entered), nullopt when the reply recorded no hand-over at all or its image is no
// longer held -- so "charged nothing" and "nothing to read" cannot be mistaken for each other.
std::optional<uint32_t> chargeOf(const wallpaperx::WallpaperManager& manager, int32_t monitorId,
                                 const std::string& client, const InProcessReply& reply) {
    if (!reply.handle.handover) return std::nullopt;
    const uint64_t number = reply.handle.handover->number;
    for (const wallpaperx::BufferHold& hold : monitorStatus(manager, monitorId).holds) {
        if (hold.client != client) continue;
        for (const wallpaperx::HeldBuffer& buffer : hold.buffers) {
            if (buffer.id != reply.id() || number > buffer.newest || number <= buffer.released) continue;
            for (const wallpaperx::HandoverCharge& charge : buffer.charges) {
                if (charge.handover == number) return charge.descriptors;
            }
            return 0u;
        }
    }
    return std::nullopt;
}

// The frame-charge phase: the manager and its Device on this thread, callers as plain names, so
// what each reply carried, what it was charged and what a withdrawal takes back are read off the
// manager's own ledger with nothing in between.
void runFrameChargeChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                          const Check& check) {
    const char* kGoals = "S15.284-S15.288, S15.291-S15.295 (what a GetFrame reply carries and is charged, and the "
                         "undo of a reply that never left)";
    constexpr int32_t kMonitor = 0;
    const auto expect = [&check](bool ok, const std::string& what) { check(ok, what.c_str()); };
    PhaseDeviceGrade grade("the frame-charge phase", check);
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    if (!renderOnce(manager, kMonitor, imagePath)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the daemon drew no frame on this host, so nothing could be handed over");
        return;
    }

    // GOAL: S15.286's ledger half -- each reply is charged on its own, one charge per reply, and the
    //       claim's charge is their sum. What a reply carries is graded on the wire (frameReply).
    // CONTROL: `held->charges.insert(...)` deleted from withHandover's already-held branch
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: the claim is charged the sum of what its
    //       replies carried, one charge per reply".
    const InProcessReply plain = exportClosed(manager, kMonitor, "frame.charge.a");
    // Never delivered: the undo below withdraws it, which production does only to a reply that never went out.
    const InProcessReply declared = exportUnsent(manager, kMonitor, "frame.charge.a", false, {plain.id()});
    const HoldView afterTwo = holdOf(manager, kMonitor, "frame.charge.a");
    const uint64_t plainNumber = wallpaperx::testing::handoverOf(plain.handle);
    const uint64_t declaredNumber = wallpaperx::testing::handoverOf(declared.handle);
    expect(afterTwo.descriptors == plain.carried() + declared.carried() &&
               afterTwo.charged == std::vector<uint64_t>{plainNumber, declaredNumber} &&
               afterTwo.newest == declaredNumber,
           "the claim is charged the sum of what its replies carried, one charge per reply (" + describeHold(afterTwo) +
               ")");

    // GOAL: S15.287 and S15.286's zero -- a reply on the caller's timeline for a declared image carries
    //       nothing and is charged nothing, and is served even to a caller at the cap of 64.
    // WHY-THIS-SHAPE: the caller is brought to exactly the cap -- one timeline descriptor and cap-1
    //       timeline replies carrying only the memory -- so the zero-charge reply is the one under test, and a
    //       plain reply after it is required refused, which proves the caller really was at the cap.
    // CONTROL: in WallpaperManager::admitDescriptorsLocked (backend/daemon/wallpaper_manager.cpp),
    //       `more` admitted as `std::max(more, 1u)` -> "FAIL: a reply carrying nothing is served to a
    //       caller at the cap (answered TooManyDescriptors, ...)" on the NVIDIA entry.
    std::string timelineWhy;
    int timelineFd = -1;
    try {
        timelineFd = manager.acquireTimelineFd("frame.charge.e");
    } catch (const std::exception& e) {
        timelineWhy = e.what();
    }
    if (timelineFd < 0) {
        wallpaperx::testing::notVerified("S15.287 and S15.286's zero charge",
                                         "this GPU gave no acquire timeline (" + timelineWhy + ")");
    } else {
        ::close(timelineFd);
        int memoryOnly = 0;
        int unlike = 0;
        uint64_t image = 0;
        for (int i = 0; i < kExpectedDescriptorCap - 1; ++i) {
            const InProcessReply r = exportClosed(manager, kMonitor, "frame.charge.e", true);
            if (!r.served) break;
            image = r.id();
            if (!(r.memory && !r.syncFd && r.value != 0 && r.carried() == 1)) ++unlike;
            ++memoryOnly;
        }
        const HoldView atCap = holdOf(manager, kMonitor, "frame.charge.e");
        const InProcessReply zero = exportClosed(manager, kMonitor, "frame.charge.e", true, {image});
        const std::optional<uint32_t> zeroCharge = chargeOf(manager, kMonitor, "frame.charge.e", zero);
        const InProcessReply over = exportClosed(manager, kMonitor, "frame.charge.e", true);
        expect(memoryOnly == kExpectedDescriptorCap - 1 && unlike == 0 &&
                   atCap.descriptors == static_cast<uint32_t>(kExpectedDescriptorCap - 1),
               std::to_string(kExpectedDescriptorCap - 1) +
                   " timeline replies carry the memory and a value only, charged one each (" +
                   std::to_string(memoryOnly) + " served, " + std::to_string(unlike) + " otherwise, " +
                   std::to_string(atCap.descriptors) + " descriptor(s) charged)");
        expect(zero.served && !zero.memory && !zero.syncFd && zero.value != 0 && zeroCharge == 0u &&
                  over.reason == wallpaperx::NoFrameReason::TooManyDescriptors,
              std::string("a reply carrying nothing is served to a caller at the cap (answered ") +
                  (zero.served ? "with a frame" : zero.reason == wallpaperx::NoFrameReason::TooManyDescriptors
                                                      ? "TooManyDescriptors"
                                                      : "another refusal") +
                  ", and the plain reply after it " +
                  (over.reason == wallpaperx::NoFrameReason::TooManyDescriptors ? "refused" : "not refused") + ")");
    }

    // GOAL: S15.343 at the manager -- a consumer is sent at most four copies of its timeline, each
    //       counted once timelineCopyDelivered() says it reached the caller; the fifth request throws
    //       TimelineCopyLimitReached; another consumer is sent its own four meanwhile; and once the
    //       consumer has left (forgetAcquireTimeline), the same name asking again is a new timeline
    //       with four copies of its own.
    // WHY-THIS-SHAPE: each copy through deliveredTimelineFd(), the export and the delivery report
    //       DbusService makes. The same name after a departure cannot be staged on the bus.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, (a) WallpaperManager::acquireTimelineFd's
    //       `existing->copiesSent >= kMaxTimelineCopiesPerClient` test made `false &&` -> "FAIL: a consumer
    //       is sent four copies of its timeline and the fifth is refused for copies (6 sent, then '')", the
    //       other two lines beside it; (b) WallpaperManager::forgetAcquireTimeline made a no-op -> "FAIL:
    //       after the consumer leaves, the same name is sent four copies of a new timeline (0 sent, then
    //       'TimelineCopyLimitReached')"; (c) WallpaperManager::timelineCopyDelivered made a no-op -> the
    //       first line, "(6 sent, then '')".
    if (timelineFd >= 0) {
        // How many copies `client` is sent before a refusal, up to six asks; the refusal's type in `why`.
        // `delivered` false exports each copy without reporting it delivered: a reply that never went out.
        const auto copiesFor = [&manager](const std::string& client, std::string& why, bool delivered = true) {
            int sent = 0;
            for (int i = 0; i < 6 && why.empty(); ++i) {
                try {
                    ::close(delivered ? wallpaperx::testing::deliveredTimelineFd(manager, client)
                                      : manager.acquireTimelineFd(client));
                    ++sent;
                } catch (const wallpaperx::TimelineCopyLimitReached&) {
                    why = "TimelineCopyLimitReached";
                } catch (const std::exception& e) {
                    why = e.what();
                }
            }
            return sent;
        };

        // GOAL: S15.358 at the manager -- a copy exported but never reported delivered uses up none of
        //       the four: six exports with no delivery, then the same consumer is still sent four
        //       delivered copies and refused the fifth.
        // WHY-THIS-SHAPE: six undelivered exports, more than the limit, so a count taken at export
        //       refuses the fifth of them and leaves none for the delivered run; the refusal after the
        //       four shows the limit is in force at all.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, copies counted at export -- the
        //       `++entry->copiesSent;` moved out of WallpaperManager::timelineCopyDelivered into
        //       acquireTimelineFd's repeat path and a new entry started at 1 -> "FAIL: a copy exported but
        //       never delivered uses up none of the four (4 exported, then
        //       'TimelineCopyLimitReached'; then 0 delivered ...)".
        {
            std::string undeliveredWhy, deliveredWhy;
            const int undelivered = copiesFor("frame.charge.v", undeliveredWhy, /*delivered=*/false);
            const int deliveredAfter = copiesFor("frame.charge.v", deliveredWhy);
            expect(undelivered == 6 && undeliveredWhy.empty() && deliveredAfter == kExpectedTimelineCopies &&
                       deliveredWhy == "TimelineCopyLimitReached",
                   "a copy exported but never delivered uses up none of the four (" + std::to_string(undelivered) +
                       " exported, then '" + undeliveredWhy + "'; then " + std::to_string(deliveredAfter) +
                       " delivered, then '" + deliveredWhy + "')");
            manager.forgetAcquireTimeline("frame.charge.v");
        }

        std::string firstWhy, otherWhy, againWhy;
        const int first = copiesFor("frame.charge.t", firstWhy);
        const int other = copiesFor("frame.charge.u", otherWhy);
        manager.forgetAcquireTimeline("frame.charge.t");
        const int again = copiesFor("frame.charge.t", againWhy);
        expect(first == kExpectedTimelineCopies && firstWhy == "TimelineCopyLimitReached",
               "a consumer is sent four copies of its timeline and the fifth is refused for copies (" +
                   std::to_string(first) + " sent, then '" + firstWhy + "')");
        expect(other == kExpectedTimelineCopies && otherWhy == "TimelineCopyLimitReached",
               "another consumer is sent four of its own meanwhile (" + std::to_string(other) + " sent, then '" +
                   otherWhy + "')");
        expect(again == kExpectedTimelineCopies && againWhy == "TimelineCopyLimitReached",
               "after the consumer leaves, the same name is sent four copies of a new timeline (" +
                   std::to_string(again) + " sent, then '" + againWhy + "')");
        manager.forgetAcquireTimeline("frame.charge.t");
        manager.forgetAcquireTimeline("frame.charge.u");
    } else {
        wallpaperx::testing::notVerified("S15.343 at the manager",
                                         "this GPU gave no acquire timeline (" + timelineWhy + ")");
    }

    // GOAL: S15.288 and S15.306's second state -- the undo of a repeat hand-over on top of one still
    //       outstanding takes back exactly that reply's charge, sets the claim's newest hand-over
    //       back to the one before it, and keeps the claim.
    // CONTROL: in WallpaperManager::undoHandoverLocked (backend/daemon/wallpaper_manager.cpp), each ->
    //       "FAIL: withdrawing the second reply takes back its charge alone, ...": (a) the
    //       `std::erase_if(held->charges, ...)` deleted; (b) `record.previous <= held->released` made
    //       `true` ("no claim"); (c) `held->newest = record.previous;` deleted.
    std::vector<wallpaperx::FrameAvailable> repeatAnnounced;
    const std::vector<int> repeatRender =
        manager.withdrawHandover(kMonitor, "frame.charge.a", declared.handle, &repeatAnnounced);
    const HoldView afterRepeat = holdOf(manager, kMonitor, "frame.charge.a");
    expect(afterRepeat.descriptors == plain.carried() && afterRepeat.charged == std::vector<uint64_t>{plainNumber} &&
               afterRepeat.newest == plainNumber && afterRepeat.ids == std::vector<uint64_t>{plain.id()} &&
               repeatRender.empty() && repeatAnnounced.empty(),
           "withdrawing the second reply takes back its charge alone, sets the newest hand-over back, and keeps the "
           "claim (" + describeHold(afterRepeat) + ", " + std::to_string(repeatAnnounced.size()) + " announced)");

    // GOAL: S15.291 and S15.294 -- withdrawing a first hand-over ends the claim and reports the
    //       image freed as a release does: the frame announced again, and a render owed posted.
    // WHY-THIS-SHAPE: two withdrawals, because the two reports exclude each other: with nothing
    //       outstanding the committed frame is announced again; with a new request recorded and not
    //       yet drawn, the render owed is posted and the old frame is no longer offered.
    // CONTROL: `reportFreedImageLocked(...)` deleted from WallpaperManager::withdrawHandover
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: withdrawing a first hand-over ends the
    //       claim and announces the frame again" and "FAIL: ... posts the render owed".
    const uint64_t committed = monitorStatus(manager, kMonitor).generation;
    const InProcessReply first = exportUnsent(manager, kMonitor, "frame.charge.b");
    std::vector<wallpaperx::FrameAvailable> firstAnnounced;
    const std::vector<int> firstRender = manager.withdrawHandover(kMonitor, "frame.charge.b", first.handle, &firstAnnounced);
    const HoldView afterFirst = holdOf(manager, kMonitor, "frame.charge.b");
    expect(first.served && !afterFirst.present && firstRender.empty() && firstAnnounced.size() == 1 &&
               firstAnnounced.front().monitorId == kMonitor && firstAnnounced.front().generation == committed,
           "withdrawing a first hand-over ends the claim and announces the frame again (" + describeHold(afterFirst) +
               ", " + std::to_string(firstAnnounced.size()) + " announced)");
    const InProcessReply owed = exportUnsent(manager, kMonitor, "frame.charge.b");
    manager.requestWallpaper("static", imagePath, kMonitor);
    std::vector<wallpaperx::FrameAvailable> owedAnnounced;
    const std::vector<int> owedRender = manager.withdrawHandover(kMonitor, "frame.charge.b", owed.handle, &owedAnnounced);
    const HoldView afterOwed = holdOf(manager, kMonitor, "frame.charge.b");
    expect(owed.served && !afterOwed.present && owedRender == std::vector<int>{kMonitor},
           "withdrawing a first hand-over while a render is owed ends the claim and posts the render owed (" +
               describeHold(afterOwed) + ", " + std::to_string(owedRender.size()) + " render(s))");
    (void)manager.renderPending(kMonitor);

    // GOAL: S15.293 -- a withdrawal finding the claim already ended, by a release or a departure,
    //       does nothing: no claim comes back and nothing is announced.
    // CONTROL: in WallpaperManager::withdrawHandover (backend/daemon/wallpaper_manager.cpp), the
    //       `if (!undoHandoverLocked(...)) return {};` made to fall through -> "FAIL: a withdrawal after
    //       a release or a departure changes nothing".
    const InProcessReply released = exportClosed(manager, kMonitor, "frame.charge.c");
    (void)wallpaperx::testing::releaseHandle(manager, "frame.charge.c", kMonitor, released.handle);
    std::vector<wallpaperx::FrameAvailable> lateAnnounced;
    const std::vector<int> lateRender = manager.withdrawHandover(kMonitor, "frame.charge.c", released.handle, &lateAnnounced);
    const InProcessReply departedFirst = exportClosed(manager, kMonitor, "frame.charge.d");
    const InProcessReply departedSecond = exportUnsent(manager, kMonitor, "frame.charge.d");
    (void)manager.forgetHeldBuffers("frame.charge.d");
    const std::vector<int> departedRender =
        manager.withdrawHandover(kMonitor, "frame.charge.d", departedSecond.handle, &lateAnnounced);
    const HoldView releasedHold = holdOf(manager, kMonitor, "frame.charge.c");
    const HoldView departedHold = holdOf(manager, kMonitor, "frame.charge.d");
    expect(released.served && departedFirst.served && departedSecond.served && !releasedHold.present &&
              !departedHold.present && lateRender.empty() && departedRender.empty() && lateAnnounced.empty(),
          "a withdrawal after a release or a departure changes nothing (" + describeHold(releasedHold) + "; " +
              describeHold(departedHold) + "; " + std::to_string(lateAnnounced.size()) + " announced)");

    // GOAL: S15.295 -- a reply to a caller with no bus name records no claim and no charge, so its
    //       handle carries no hand-over and withdrawing it changes nothing.
    // CONTROL: in WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp),
    //       `handle.handover = record;` made `handle.handover = HandoverRecord{1, 0};` -> "FAIL: a reply to
    //       a caller with no name carries no charge, and withdrawing it changes nothing", among other
    //       frameCharge lines reading the hand-over numbers.
    const size_t holdersBefore = monitorStatus(manager, kMonitor).holds.size();
    const InProcessReply unnamed = exportUnsent(manager, kMonitor, "");
    const std::vector<int> unnamedRender = manager.withdrawHandover(kMonitor, "", unnamed.handle);
    expect(unnamed.served && !unnamed.handle.handover && unnamedRender.empty() &&
              monitorStatus(manager, kMonitor).holds.size() == holdersBefore,
          "a reply to a caller with no name carries no charge, and withdrawing it changes nothing");

    // GOAL: S15.289's manager half -- at most kMaxKnownBuffers (8) declared ids: nine are refused as
    //       InvalidRequest before anything is recorded, eight are served.
    // CONTROL: in WallpaperManager::requireKnownBuffers (backend/daemon/wallpaper_manager.cpp), `>
    //       kMaxKnownBuffers` made `> kMaxKnownBuffers + 1` -> "FAIL: nine declared ids are refused
    //       before anything is recorded, eight are served".
    const std::vector<uint64_t> eight(8, plain.id());
    std::vector<uint64_t> nine = eight;
    nine.push_back(plain.id() + 1);
    bool nineRefused = false;
    try {
        (void)exportClosed(manager, kMonitor, "frame.charge.f", false, nine);
    } catch (const wallpaperx::InvalidRequest&) {
        nineRefused = true;
    }
    const bool nothingRecorded = !holdOf(manager, kMonitor, "frame.charge.f").present;
    const InProcessReply eightServed = exportClosed(manager, kMonitor, "frame.charge.f", false, eight);
    expect(nineRefused && nothingRecorded && eightServed.served,
          "nine declared ids are refused before anything is recorded, eight are served");

    // GOAL: S15.347 at the manager -- an export that fails after recording its caller's first claim,
    //       and whose undo therefore ends that claim, reports the image freed as a release does, before
    //       it throws: a render owed goes into `toRender` and the frame still served into `announce`.
    // WHY-THIS-SHAPE: a size change owes a render while the old frame is served, so both reports are
    //       due; the interposed vkGetMemoryFdKHR fails the export for real. Last, since that render
    //       rebuilds every image the claims above are on.
    // CONTROL: the `reportFreedImageLocked(...)` in WallpaperManager::exportCurrentFrame's catch
    //       (backend/daemon/wallpaper_manager.cpp) deleted -> "FAIL: an export that fails and ends its
    //       caller's first claim reports the render owed and announces the frame still served".
    // COVERAGE-ASSERT: the export must have failed at the interposer exactly once, and the render must
    //       be owed before it; otherwise the goal is reported not verified.
    {
        constexpr const char* kResized = "FRAME-CHARGE-RESIZED";
        (void)wallpaperx::testing::nameScreen(manager, kMonitor, kResized);
        std::vector<int> resized;
        manager.setOutputSize(kResized, kTargetWidth + 2, kTargetHeight + 2, resized);
        const uint64_t served = monitorStatus(manager, kMonitor).generation;
        std::vector<int> toRender;
        std::vector<wallpaperx::FrameAvailable> announce;
        const int failed0 = g_memoryExportsFailed.load();
        g_failNextMemoryExport.store(true);
        bool threw = false;
        try {
            (void)manager.exportCurrentFrame(kMonitor, "frame.charge.g", false, {}, toRender, &announce);
        } catch (const std::exception&) {
            threw = true;
        }
        g_failNextMemoryExport.store(false);
        if (resized != std::vector<int>{kMonitor} || g_memoryExportsFailed.load() - failed0 != 1 || !threw) {
            wallpaperx::testing::notVerified("S15.347 at the manager",
                                             "the size change owed no render, or the export did not fail at the "
                                             "interposer (" + std::to_string(resized.size()) + " owed, " +
                                                 std::to_string(g_memoryExportsFailed.load() - failed0) +
                                                 " failure(s), threw " + (threw ? "yes" : "no") + ")");
        } else {
            expect(toRender == std::vector<int>{kMonitor} && announce.size() == 1 &&
                       announce.front().monitorId == kMonitor && announce.front().generation == served &&
                       !holdOf(manager, kMonitor, "frame.charge.g").present,
                   "an export that fails and ends its caller's first claim reports the render owed and announces "
                   "the frame still served (" +
                       std::to_string(toRender.size()) + " render(s), " + std::to_string(announce.size()) +
                       " announced, " + describeHold(holdOf(manager, kMonitor, "frame.charge.g")) + ")");
        }
        (void)manager.renderPending(kMonitor);
    }
}

// The hand-over-release phase: numbered releases read off the manager's own ledger -- which
// hand-overs a release covers, what it takes off the caller's charge, which numbers it refuses, and
// the undo of a reply never sent in the states the numbering adds. Its own manager, callers as plain
// names, nothing between the export and the ledger.
void runHandoverReleaseChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                              const Check& check) {
    const char* kGoals = "S15.302-S15.306 at the manager (numbered releases, their charges and validation, and the "
                         "undo states)";
    constexpr int32_t kMonitor = 0;
    const auto expect = [&check](bool ok, const std::string& what) { check(ok, what.c_str()); };
    using wallpaperx::testing::handoverOf;
    PhaseDeviceGrade grade("the hand-over-release phase", check);
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    if (!renderOnce(manager, kMonitor, imagePath)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the daemon drew no frame on this host, so nothing could be handed over");
        return;
    }
    const auto release = [&manager](const std::string& client, uint64_t id, uint64_t number,
                                    std::vector<wallpaperx::FrameAvailable>* announced = nullptr) {
        return manager.releaseBuffer(client, kMonitor, id, number, announced);
    };

    // GOAL: S15.307's numbering -- every hand-over gets a number from one sequence that only rises,
    //       so three fetches of one image are three numbers, each above the last.
    // CONTROL: `const uint64_t number = m_lastHandover + 1;` made `= 1` in
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp) -> "FAIL: three
    //       hand-overs of one image are numbered upward from 1 (1, 1, 1)".
    const InProcessReply a1 = exportClosed(manager, kMonitor, "handover.a");
    const InProcessReply a2 = exportClosed(manager, kMonitor, "handover.a");
    const InProcessReply a3 = exportClosed(manager, kMonitor, "handover.a");
    const uint64_t n1 = handoverOf(a1.handle);
    const uint64_t n2 = handoverOf(a2.handle);
    const uint64_t n3 = handoverOf(a3.handle);
    const bool built = a1.served && a2.served && a3.served && a1.id() == a2.id() && a2.id() == a3.id();
    expect(built && n1 >= 1 && n2 > n1 && n3 > n2,
           "three hand-overs of one image are numbered upward from 1 (" + std::to_string(n1) + ", " +
               std::to_string(n2) + ", " + std::to_string(n3) + ")");
    if (!built) return;

    // GOAL: S15.302 and S15.304 -- a release naming a hand-over below the newest keeps the claim,
    //       raises what is released, and takes off exactly the charges it covers -- the one it names
    //       and every earlier one, a reply the caller never named included -- and nothing else; it
    //       reports no image freed. A release naming the newest ends the claim and its charges.
    // WHY-THIS-SHAPE: three replies and a release naming the middle one, so "covers earlier",
    //       "only up to the one named" and "keeps the claim" are different ledger fields.
    // CONTROL: in WallpaperManager::releaseHandoversLocked (backend/daemon/wallpaper_manager.cpp),
    //       each -> "FAIL: a release naming the second of three hand-overs keeps the claim, ...": (a)
    //       `handover >= held->newest` made `true || ...`; (b) `charge.handover <= handover` made `<`;
    //       (c) `held->released = std::max(...)` deleted. (d) `endClaim(...)` deleted from the newest
    //       branch -> "FAIL: a release naming the newest ends the claim and its charges".
    std::vector<wallpaperx::FrameAvailable> partialAnnounced;
    const std::vector<int> partialRender = release("handover.a", a1.id(), n2, &partialAnnounced);
    const HoldView partial = holdOf(manager, kMonitor, "handover.a");
    expect(partial.present && partial.newest == n3 && partial.released == n2 &&
               partial.charged == std::vector<uint64_t>{n3} && partial.descriptors == a3.carried() &&
               partialRender.empty() && partialAnnounced.empty(),
           "a release naming the second of three hand-overs keeps the claim, charged the third reply alone (" +
               describeHold(partial) + ", " + std::to_string(partialAnnounced.size()) + " announced)");
    std::vector<wallpaperx::FrameAvailable> fullAnnounced;
    (void)release("handover.a", a1.id(), n3, &fullAnnounced);
    const HoldView full = holdOf(manager, kMonitor, "handover.a");
    expect(!full.present && fullAnnounced.size() == 1,
           "a release naming the newest ends the claim and its charges, and the image is announced free (" +
               describeHold(full) + ", " + std::to_string(fullAnnounced.size()) + " announced)");

    // GOAL: S15.305 -- a release naming hand-over 0, or a number above the latest issued, is refused
    //       as InvalidRequest and changes nothing.
    // WHY-THIS-SHAPE: the claim is read before and after, field by field; "refused" alone would pass
    //       for a release that threw after changing the ledger. b's hand-over is the latest issued:
    //       nothing else exports between it and the release.
    // CONTROL: in WallpaperManager::releaseBuffer (backend/daemon/wallpaper_manager.cpp), (a) `handover
    //       == 0 ||` deleted -> "FAIL: a release naming hand-over 0 is refused and changes nothing
    //       (accepted, ...)"; (b) `|| handover > m_lastHandover` deleted -> "FAIL: a release naming a
    //       hand-over not yet made is refused and changes nothing (accepted, no claim)". Each alone,
    //       re-run 2026-10-24 via control_batch.sh, reverted.
    const InProcessReply b = exportClosed(manager, kMonitor, "handover.b");
    const uint64_t nb = handoverOf(b.handle);
    const HoldView beforeRefusal = holdOf(manager, kMonitor, "handover.b");
    const auto refused = [&](uint64_t number) {
        try {
            (void)release("handover.b", b.id(), number);
        } catch (const wallpaperx::InvalidRequest&) {
            return true;
        }
        return false;
    };
    const bool zeroRefused = refused(0);
    const HoldView afterZero = holdOf(manager, kMonitor, "handover.b");
    expect(b.served && zeroRefused && describeHold(afterZero) == describeHold(beforeRefusal),
           std::string("a release naming hand-over 0 is refused and changes nothing (") +
               (zeroRefused ? "refused" : "accepted") + ", " + describeHold(afterZero) + ")");
    const bool futureRefused = refused(nb + 1);
    const HoldView afterFuture = holdOf(manager, kMonitor, "handover.b");
    expect(b.served && futureRefused && describeHold(afterFuture) == describeHold(beforeRefusal),
           std::string("a release naming a hand-over not yet made is refused and changes nothing (") +
               (futureRefused ? "refused" : "accepted") + ", " + describeHold(afterFuture) + ")");
    if (b.served) (void)release("handover.b", b.id(), nb);

    // GOAL: S15.306's third state -- undoing a repeat hand-over after a release covering the one
    //       before it landed between the export and the undo ends the claim: nothing the caller has
    //       read is outstanding any more. And the image is reported freed.
    // CONTROL: in WallpaperManager::undoHandoverLocked (backend/daemon/wallpaper_manager.cpp),
    //       `record.previous <= held->released` made `<` -> "FAIL: undoing a repeat whose earlier
    //       hand-over was released meanwhile ends the claim (0 descriptor(s) charged to hand-overs [],
    //       ...)" -- the claim left standing with nothing outstanding.
    const InProcessReply c1 = exportClosed(manager, kMonitor, "handover.c");
    const InProcessReply c2 = exportUnsent(manager, kMonitor, "handover.c");
    (void)release("handover.c", c1.id(), handoverOf(c1.handle));
    std::vector<wallpaperx::FrameAvailable> undoAnnounced;
    (void)manager.withdrawHandover(kMonitor, "handover.c", c2.handle, &undoAnnounced);
    const HoldView afterUndo = holdOf(manager, kMonitor, "handover.c");
    expect(c1.served && c2.served && !afterUndo.present && undoAnnounced.size() == 1,
           "undoing a repeat whose earlier hand-over was released meanwhile ends the claim (" + describeHold(afterUndo) +
               ", " + std::to_string(undoAnnounced.size()) + " announced)");

    // GOAL: S15.306's fifth state -- an undo naming a hand-over that is no longer the claim's newest
    //       changes nothing: no claim ended, no newest moved, no charge taken.
    // WHY-THIS-SHAPE: the contract says an undo runs before the next export; this calls it after one
    //       on purpose, which is the only way to reach the guard from outside. The ledger is compared
    //       whole before and after.
    // CONTROL: `if (held->newest != record.number) return false;` deleted from
    //       WallpaperManager::undoHandoverLocked (backend/daemon/wallpaper_manager.cpp) -> "FAIL: an undo
    //       naming a hand-over that is no longer the newest changes nothing".
    const InProcessReply d1 = exportUnsent(manager, kMonitor, "handover.d");
    const InProcessReply d2 = exportClosed(manager, kMonitor, "handover.d");
    const HoldView beforeStale = holdOf(manager, kMonitor, "handover.d");
    std::vector<wallpaperx::FrameAvailable> staleAnnounced;
    (void)manager.withdrawHandover(kMonitor, "handover.d", d1.handle, &staleAnnounced);
    const HoldView afterStale = holdOf(manager, kMonitor, "handover.d");
    expect(d1.served && d2.served && beforeStale.present &&
               describeHold(afterStale) == describeHold(beforeStale) && staleAnnounced.empty(),
           "an undo naming a hand-over that is no longer the newest changes nothing (" + describeHold(afterStale) + ")");
    if (d2.served) (void)release("handover.d", d2.id(), handoverOf(d2.handle));
}

// The frame-reply phase: the wire end of the same rules, and the withdrawal of a reply that could
// not be sent. Its own manager and service.
void runFrameReplyChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                         const Check& check) {
    const char* kGoals = "S15.284, S15.289 and S15.291-S15.292 on the bus (the memory left out, the id limit "
                         "before queueing, and a reply that could not be sent)";
    constexpr int32_t kServed = 0;
    constexpr int32_t kHeld = 1;
    PhaseDeviceGrade grade("the frame-reply phase", check);
    wallpaperx::testing::FrameReadyLog signals;
    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    struct Unblock {
        ~Unblock() {
            g_framebufferGate.release();
            g_failReplyArmed.store(false);
        }
    } unblock;
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};
    auto listener = wallpaperx::testing::subscribeFrameReady(signals, daemonName);
    auto control = sdbus::createProxy(daemonName, objectPath);

    wallpaperx::testing::setWallpaper(*control, "static", imagePath, kServed);
    if (!signals.wait(kServed, 1)) {
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(kGoals, "the daemon drew no frame on this host");
        return;
    }
    const StatusReply status0 = getStatusOrExplain(*control);
    const nlohmann::json gpu0 = status0.json().contains("gpu") && status0.json()["gpu"].is_object()
                                    ? status0.json()["gpu"]
                                    : nlohmann::json::object();
    const std::string frameSync = gpu0.value("frame_sync", std::string());
    if (frameSync != "semaphore" && frameSync != "none") {
        wallpaperx::testing::harnessFailure("GetStatus named no frame hand-over mode, so what a reply should carry "
                                            "is unknown (gpu.frame_sync [" + frameSync + "])");
        return;
    }
    const bool semaphores = frameSync == "semaphore";

    // GOAL: S15.284 and S15.285 on the wire -- declaring the image just handed over brings a reply
    //       with no memory where an acquire comes with it, and with the memory where none does; and
    //       GetStatus charges the caller what the replies carried.
    // WHY-THIS-SHAPE: the expected shape follows GetStatus's frame_sync, read before any fetch, not
    //       the reply under test; and the charge is compared with what this client counted arriving.
    // CONTROL: `sendMemory` made `true` in WallpaperManager::exportCurrentFrame
    //       (backend/daemon/wallpaper_manager.cpp) -> on dbus_service, "FAIL: over the bus, declaring
    //       the image held brings a reply without the memory exactly when an acquire comes with it
    //       (1 memory descriptor(s), 1 sync)"; the charge there made `2u` -> "FAIL: GetStatus charges the
    //       caller what its two replies carried (4 descriptor(s) for images [1] for 3)".
    auto alice = sdbus::createProxy(daemonName, objectPath);
    const std::string aliceName = uniqueNameOf(*alice);
    const Frame plain = getFrame(*alice, kServed);
    const Frame declared = getFrame(*alice, kServed, {.knownBuffers = {plain.bufferId}});
    const size_t expectedMemory = semaphores ? 0 : 1;
    check(plain.memoryFds == 1 && declared.memoryFds == expectedMemory &&
              declared.syncFds.size() == (semaphores ? 1u : 0u) && declared.bufferId == plain.bufferId,
          (std::string("over the bus, declaring the image held brings a reply without the memory exactly when an "
                       "acquire comes with it (") +
           std::to_string(declared.memoryFds) + " memory descriptor(s), " +
           std::to_string(declared.syncFds.size()) + " sync)")
              .c_str());
    const HolderEntry charged = holderEntry(wallpaperx::testing::getStatus(*alice), kServed, aliceName);
    check(chargedExactly(charged, plain.descriptors() + declared.descriptors()),
          ("GetStatus charges the caller what its two replies carried (" + describeHolder(charged) + " for " +
           std::to_string(plain.descriptors() + declared.descriptors()) + ")")
              .c_str());
    // Each release's re-announcement is waited for, so S15.291's count below starts settled.
    const auto releaseSettled = [&](const Frame& frame) {
        const size_t before = signals.countOf(kServed, frame.generation);
        wallpaperx::testing::releaseFrame(*alice, kServed, frame);
        return signals.waitForRepeat(kServed, frame.generation, before + 1);
    };
    bool releasesSettled = releaseSettled(declared);

    // GOAL: S15.289's bus half -- a GetFrame declaring nine ids is answered InvalidArgs by the bus
    //       handler before anything is queued: it is answered while the render worker is held.
    // WHY-THIS-SHAPE: the worker is held inside a render (the framebuffer gate), so any request that
    //       reached its queue cannot be answered until the gate opens; a 5 s call timeout turns a
    //       queued request into a Timeout, never a pass. Eight are then served once the gate opens.
    // CONTROL: the `requireKnownBuffers` refusal in the GetFrame handler of DbusService::registerVTable
    //       (backend/daemon/dbus_service.cpp) deleted, so the list is refused only on the worker ->
    //       "FAIL: nine declared ids are refused as InvalidArgs while the render worker is held
    //       (answered 'org.freedesktop.DBus.Error.Timeout')".
    g_framebufferGate.arm(CallGate::Mode::Hold);
    wallpaperx::testing::setWallpaper(*control, "static", imagePath, kHeld);
    if (!g_framebufferGate.waitFired(std::chrono::seconds(30))) {
        wallpaperx::testing::harnessFailure("the render thread never reached the held monitor's image creation");
        return;
    }
    const std::vector<uint64_t> eight(8, plain.bufferId);
    std::vector<uint64_t> nine = eight;
    nine.push_back(plain.bufferId + 1);
    const std::string nineAnswer = errorNameFrom([&] {
        (void)getFrame(*alice, kServed, {.knownBuffers = nine, .timeout = std::chrono::seconds(5)});
    });
    g_framebufferGate.release();
    check(nineAnswer == "org.freedesktop.DBus.Error.InvalidArgs",
          ("nine declared ids are refused as InvalidArgs while the render worker is held (answered '" + nineAnswer +
           "')")
              .c_str());
    std::string eightAnswer;
    try {
        const Frame served = getFrame(*alice, kServed, {.knownBuffers = eight});
        releasesSettled = releaseSettled(served) && releasesSettled;
    } catch (const sdbus::Error& e) {
        eightAnswer = e.getName();
    }
    check(eightAnswer.empty(), ("eight declared ids are served (answered '" + eightAnswer + "')").c_str());

    // A GetFrame whose reply the interposed sd_bus_send fails, answered as the caller sees it.
    const auto failedSend = [&](sdbus::IProxy& who, const wallpaperx::testing::FrameRequest& request) {
        const int failed0 = g_repliesFailed.load();
        failReplyTo(uniqueNameOf(who));
        wallpaperx::testing::FrameRequest timed = request;
        timed.timeout = std::chrono::seconds(5);
        const std::string answer = errorNameFrom([&] { (void)getFrame(who, kServed, timed); });
        g_failReplyArmed.store(false);
        return std::make_pair(answer, g_repliesFailed.load() - failed0);
    };

    // GOAL: S15.291 on the bus -- a first hand-over whose reply cannot be sent leaves the caller
    //       holding nothing and charged nothing, the frame is announced again, and the caller is
    //       answered ExportFailed rather than left to time out.
    // WHY-THIS-SHAPE: the send is failed at sd_bus_send for this caller only (interposed above), so
    //       the export and the hold happen for real; a 5 s call timeout makes a missing answer a
    //       Timeout. The repeat announcement is waited for by count, bounded.
    // CONTROL: in DbusService::completeFrame (backend/daemon/dbus_service.cpp), (a) the
    //       withdrawHandover call deleted -> "FAIL: a first hand-over whose reply was not sent leaves
    //       no claim and the frame announced again (...)"; (b) the ExportFailed returnError deleted ->
    //       "FAIL: and the caller is answered ExportFailed rather than left to time out"; (c)
    //       WallpaperManager::withdrawHandover's `if (!undoHandoverLocked(...)) return {};` made
    //       `if (true) return {};` (backend/daemon/wallpaper_manager.cpp) -> the first line.
    // COVERAGE-ASSERT: exactly one send must have failed at the interposer.
    auto bob = sdbus::createProxy(daemonName, objectPath);
    const uint64_t generation = monitorStatus(manager, kServed).generation;
    const size_t announced0 = signals.countOf(kServed, generation);
    const auto [bobAnswer, bobFailed] = failedSend(*bob, {});
    if (!releasesSettled) {
        wallpaperx::testing::harnessFailure("alice's releases were never announced, so a repeat announcement could "
                                            "not be told apart from theirs (S15.291 on the bus)");
    } else if (bobFailed != 1) {
        wallpaperx::testing::notVerified("S15.291 on the bus", "the reply was not failed at the interposed sd_bus_send");
    } else {
        const bool reannounced = signals.waitForRepeat(kServed, generation, announced0 + 1);
        const HolderEntry bobCharged = holderEntry(wallpaperx::testing::getStatus(*bob), kServed, uniqueNameOf(*bob));
        check(!bobCharged.listed && reannounced,
              ("a first hand-over whose reply was not sent leaves no claim and the frame announced again (" +
               describeHolder(bobCharged) + ", announced " +
               std::to_string(signals.countOf(kServed, generation) - announced0) + " more)")
                  .c_str());
        check(bobAnswer == kErrExportFailed,
              ("and the caller is answered ExportFailed rather than left to time out (answered '" + bobAnswer + "')")
                  .c_str());
    }

    // GOAL: S15.292 -- a repeat hand-over whose reply cannot be sent takes back that reply's
    //       descriptors only: the claim stays, charged what the reply before it carried. Once with
    //       the memory sent and once, where an acquire comes, with it left out.
    // CONTROL: in WallpaperManager::undoHandoverLocked (backend/daemon/wallpaper_manager.cpp),
    //       `record.previous <= held->released` made `true`, so every undo ends the claim -> "FAIL: a
    //       repeat hand-over whose reply was not sent keeps the claim, charged what the earlier reply
    //       carried (memory sent: not listed ...)" and the same for "memory left out".
    const auto repeatCase = [&](const char* label, bool declare) {
        auto who = sdbus::createProxy(daemonName, objectPath);
        const Frame held = getFrame(*who, kServed);
        wallpaperx::testing::FrameRequest again;
        if (declare) again.knownBuffers = {held.bufferId};
        const auto [answer, failed] = failedSend(*who, again);
        if (failed != 1) {
            wallpaperx::testing::notVerified((std::string("S15.292, ") + label).c_str(),
                                             "the reply was not failed at the interposed sd_bus_send");
            return;
        }
        const HolderEntry stands = holderEntry(wallpaperx::testing::getStatus(*who), kServed, uniqueNameOf(*who));
        check(chargedExactly(stands, held.descriptors()) && stands.buffers == std::vector<uint64_t>{held.bufferId} &&
                  answer == kErrExportFailed,
              (std::string("a repeat hand-over whose reply was not sent keeps the claim, charged what the earlier reply "
                           "carried (") +
               label + ": " + describeHolder(stands) + " where " + std::to_string(held.descriptors()) +
               " stand, answered '" + answer + "')")
                  .c_str());
        wallpaperx::testing::releaseFrame(*who, kServed, held);
    };
    repeatCase("memory sent", false);
    if (semaphores) {
        repeatCase("memory left out", true);
    } else {
        wallpaperx::testing::notVerified("S15.292 with the memory left out",
                                         "this GPU sends no acquire, so no reply here leaves the memory out");
    }

    // GOAL: S15.318 -- a reply that could not be sent is not evidence that anyone is watching: a
    //       monitor nobody has collected from stays unwatched after it, and a sent reply in the same
    //       state makes it watched.
    // WHY-THIS-SHAPE: the monitor is driven stale first (bounded), and read after the ExportFailed the
    //       worker sends only after the undo; a delivered fetch then shows a delivery is visible.
    // CONTROL: in DbusService::completeFrame (backend/daemon/dbus_service.cpp),
    //       `m_manager.handoverDelivered(monitorId, client)` moved above `sendFrame(call, handle)` ->
    //       "FAIL: a GetFrame whose reply could not be sent leaves an unwatched monitor unwatched".
    {
        const uint64_t before = monitorStatus(manager, kServed).generation;
        wallpaperx::testing::setWallpaper(*control, "static", imagePath, kServed);
        bool stale = signals.waitAtLeast(kServed, before + 1);
        const auto staleBy = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (stale && monitorStatus(manager, kServed).watched) {
            if (std::chrono::steady_clock::now() > staleBy) {
                stale = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!stale) {
            wallpaperx::testing::notVerified("S15.318", "the republished monitor never went unwatched, so the state "
                                                        "a recorded delivery would change could not be built");
        } else {
            auto dave = sdbus::createProxy(daemonName, objectPath);
            const auto [answer, failed] = failedSend(*dave, {});
            if (failed != 1) {
                wallpaperx::testing::notVerified("S15.318", "the reply was not failed at the interposed sd_bus_send");
            } else {
                const bool watchedAfter = monitorStatus(manager, kServed).watched;
                check(!watchedAfter && answer == kErrExportFailed,
                      ("a GetFrame whose reply could not be sent leaves an unwatched monitor unwatched (watched " +
                       std::to_string(watchedAfter) + ", answered '" + answer + "')")
                          .c_str());
                const Frame delivered = getFrame(*dave, kServed);
                check(monitorStatus(manager, kServed).watched,
                      "and a GetFrame whose reply was sent, in the same state, makes it watched");
                wallpaperx::testing::releaseFrame(*dave, kServed, delivered);
            }
        }
    }

    // GOAL: S15.319 -- a GetFrame sent with the no-reply-expected flag records nothing: no claim and
    //       no descriptor charge for a reply the caller will never see, while the same caller's next
    //       ordinary GetFrame is served and charged only what it carried.
    // WHY-THIS-SHAPE: the ordinary call after the flagged one on one connection is the barrier;
    //       reading GetStatus straight after the flagged call would race the worker.
    // CONTROL: `if (call.doesntExpectReply()) return;` deleted from answeredOnly()
    //       (backend/daemon/dbus_service.cpp) -> "FAIL: a GetFrame sent expecting no reply records no
    //       claim or charge (...)".
    {
        auto erin = sdbus::createProxy(daemonName, objectPath);
        erin->callMethod("GetFrame")
            .onInterface(DbusService::kInterface)
            .withArguments(kServed, std::vector<uint64_t>{}, false)
            .dontExpectReply();
        std::string answer;
        Frame served;
        try {
            served = getFrame(*erin, kServed);
        } catch (const sdbus::Error& e) {
            answer = e.getName();
        }
        const HolderEntry charged = holderEntry(wallpaperx::testing::getStatus(*erin), kServed, uniqueNameOf(*erin));
        check(answer.empty() && chargedExactly(charged, served.descriptors()) &&
                  charged.buffers == std::vector<uint64_t>{served.bufferId},
              ("a GetFrame sent expecting no reply records no claim or charge, and the caller's next GetFrame is "
               "served and charged only what it carried (answered '" +
               answer + "', " + describeHolder(charged) + " for " + std::to_string(served.descriptors()) + ")")
                  .c_str());
        if (answer.empty()) wallpaperx::testing::releaseFrame(*erin, kServed, served);
    }

    // GOAL: S15.320, restated -- GetAcquireTimeline sent with the no-reply-expected flag sends the
    //       caller nothing, so it uses up none of its four copies of the timeline (S15.343): after
    //       sixteen such calls the caller is still sent four copies on ordinary calls, and refused the
    //       fifth.
    // WHY-THIS-SHAPE: read through the copy limit (a live timeline's charge is one however often
    //       sent); batches of four, each closed by an ordinary GetFrame as the barrier, keep the caller
    //       inside its queue share; the fifth ordinary call shows the limit in force.
    // CONTROL: `if (call.doesntExpectReply()) return;` deleted from answeredOnly()
    //       (backend/daemon/dbus_service.cpp) -> "FAIL: GetAcquireTimeline sent expecting no reply uses
    //       up no copy of the timeline (...)", S15.319's line beside it.
    // COVERAGE-ASSERT: every barrier must be served, or the flagged calls cannot be said to have run;
    //       that is reported as a harness fault, and a GPU with no timeline as not verified.
    {
        auto fay = sdbus::createProxy(daemonName, objectPath);
        constexpr int kBatches = 4;
        constexpr int kFlaggedPerBatch = 4;
        int barriers = 0;
        for (int batch = 0; batch < kBatches; ++batch) {
            for (int i = 0; i < kFlaggedPerBatch; ++i) {
                fay->callMethod("GetAcquireTimeline").onInterface(DbusService::kInterface).dontExpectReply();
            }
            const FetchResult barrier = tryGetFrame(*fay, kServed);
            if (!barrier.frame) break;
            ++barriers;
            wallpaperx::testing::releaseFrame(*fay, kServed, *barrier.frame);
        }
        int answered = 0;
        std::string refusal;
        for (int i = 0; i < kExpectedTimelineCopies + 1 && refusal.empty(); ++i) {
            try {
                const sdbus::UnixFd timeline = wallpaperx::testing::getAcquireTimeline(*fay);
                ++answered;
            } catch (const sdbus::Error& e) {
                refusal = e.getName();
            }
        }
        if (barriers != kBatches) {
            wallpaperx::testing::harnessFailure("a GetFrame closing a batch of flagged timeline calls was refused, so "
                                                "they cannot be said to have run (S15.320)");
        } else if (answered == 0 && refusal != kErrTooManyTimelineCopies) {
            wallpaperx::testing::notVerified("S15.320", "the first ordinary GetAcquireTimeline was refused ('" +
                                                            refusal + "'), so this GPU sends no timeline");
        } else {
            check(answered == kExpectedTimelineCopies && refusal == kErrTooManyTimelineCopies,
                  ("GetAcquireTimeline sent expecting no reply uses up no copy of the timeline (" +
                   std::to_string(answered) + " of " + std::to_string(kExpectedTimelineCopies) +
                   " ordinary requests answered after 16 flagged ones, then '" + refusal + "')")
                      .c_str());
        }
    }

    // GOAL: S15.358 over the bus -- a GetAcquireTimeline whose reply could not be sent uses up none of
    //       the caller's four copies: after one failed send the caller is still sent four copies on
    //       ordinary calls, and refused the fifth.
    // WHY-THIS-SHAPE: the reply is failed at sd_bus_send for this caller only, after the export, so the
    //       daemon's path from export to reply runs for real; the caller times out (2 s). One failed
    //       send suffices: counted, it would leave three, and the fourth ordinary call would be refused.
    // CONTROL: in DbusService::completeGetAcquireTimeline (backend/daemon/dbus_service.cpp),
    //       `m_manager.timelineCopyDelivered(client);` moved above `call.returnResults(timeline);` ->
    //       "FAIL: a GetAcquireTimeline whose reply could not be sent uses up no copy (3 of 4 ordinary
    //       requests answered after it, then '...TooManyTimelineCopies')".
    // COVERAGE-ASSERT: exactly one send must have failed at the interposer; a GPU with no timeline is
    //       reported not verified.
    {
        auto gil = sdbus::createProxy(daemonName, objectPath);
        const int failed0 = g_repliesFailed.load();
        failReplyTo(uniqueNameOf(*gil));
        const std::string lost = errorNameFrom([&] {
            sdbus::UnixFd timeline;
            gil->callMethod("GetAcquireTimeline")
                .onInterface(DbusService::kInterface)
                .withTimeout(std::chrono::seconds(2))
                .storeResultsTo(timeline);
        });
        g_failReplyArmed.store(false);
        const int failedSends = g_repliesFailed.load() - failed0;
        int answered = 0;
        std::string refusal;
        for (int i = 0; i < kExpectedTimelineCopies + 1 && refusal.empty(); ++i) {
            try {
                const sdbus::UnixFd timeline = wallpaperx::testing::getAcquireTimeline(*gil);
                ++answered;
            } catch (const sdbus::Error& e) {
                refusal = e.getName();
            }
        }
        if (failedSends != 1) {
            wallpaperx::testing::notVerified("S15.358 over the bus",
                                             "the reply was not failed at the interposed sd_bus_send");
        } else if (answered == 0 && refusal != kErrTooManyTimelineCopies) {
            wallpaperx::testing::notVerified("S15.358 over the bus",
                                             "the first ordinary GetAcquireTimeline was refused ('" + refusal +
                                                 "'), so this GPU sends no timeline");
        } else {
            check(answered == kExpectedTimelineCopies && refusal == kErrTooManyTimelineCopies && !lost.empty(),
                  ("a GetAcquireTimeline whose reply could not be sent uses up no copy (" + std::to_string(answered) +
                   " of " + std::to_string(kExpectedTimelineCopies) + " ordinary requests answered after it, then '" +
                   refusal + "'; the lost call answered '" + lost + "')")
                      .c_str());
        }
    }
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "dbus_service_test";

    // Points $XDG_STATE_HOME at a scratch directory for the whole process. Config::stateDir
    // defaults to empty now and no manager here sets one, so nothing should reach a state
    // directory at all -- and that is the point: this is the sandbox the check at the end of
    // main() reads to establish it, and it bounds the damage of the regression that check
    // exists to catch. Construct it before any Config: a manager already built has resolved
    // whatever it was going to resolve.
    const wallpaperx::testing::ScratchStateHome stateHome;
    if (!stateHome.ok()) {
        wallpaperx::testing::notVerified(
            "every goal in this file",
            "no scratch state directory could be created, and running without one would write this "
            "test's crash records into the developer's own state directory");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // Wrapped in a Check rather than called directly because every phase below takes one:
    // the std::function is what lets a phase be handed the grader without this file's
    // reporting internals.
    const Check check = wallpaperx::testing::check;

    // Scoped so the manager, and with it the render thread, the Vulkan Device, the
    // offscreen targets and Memory's leaked-allocation check, are all destroyed before
    // the validation counter is read below. Destroying an image still owned by an
    // external queue family, or leaking device memory, only raises validation errors
    // during teardown -- sampling the counter while the manager is still alive silently
    // exempts the whole destruction half of the code under test.
    // Filled as the manager scope closes; see CaptureOnExit below. Declared out here
    // because they are graded after the try block, once everything is destroyed.
    std::shared_ptr<const std::atomic<int>> validationCounter;
    bool validationLayerWasActive = false;
    bool sharedGpuPhaseRan = true;

    try {
        const std::string imagePath = writeTestImage();
        const std::string corruptPath =
            wallpaperx::testing::writeUndecodableFile(testAssetDir(), "dbus_test_corrupt.png");
        // A file that exists, is a regular file and is readable -- and nothing more.
        // Needed because SetWallpaper applies the readable-regular-file check to *every*
        // known type now (S6.38), so a path that was never created is rejected on the reply
        // and never reaches the recorded-but-unrenderable state these checks are about. The
        // type is `shader` rather than `video` because Step 13 makes video renderable and
        // these checks need one that still draws nothing. The shared helper's name says
        // "undecodable" and that is beside the point here: this build has no shader
        // renderer, so nothing ever opens the file and its contents are irrelevant.
        const std::string unrenderablePath =
            wallpaperx::testing::writeUndecodableFile(testAssetDir(), "dbus_test_unrenderable.frag");
        const std::vector<std::string> burstPaths = writeBurstImages();

        // Declared before everything it outlives. The signal callback below captures it,
        // and that callback runs on the proxy's dispatch thread -- so if the log were
        // declared after the proxy it would be destroyed first, and any FrameReady still
        // in flight during an unwinding stack would lock a destroyed mutex. Reverse
        // declaration order is what guarantees proxy-then-service-then-log teardown.
        FrameReadyLog signals;

        wallpaperx::WallpaperManager::Config config;
        config.width = kTargetWidth;
        config.height = kTargetHeight;
        // Validation on: it is the correctness oracle for the export path's barriers and
        // queue-family transfers, none of which this test can observe directly.
        config.enableValidation = !validationWaived();

        // The shared-manager phases, asked up front because the manager, service and proxy they
        // share are built only when one of them runs. They use monitors of their own, so a subset
        // run in this order sees nothing a skipped one would have left.
        using wallpaperx::testing::phaseSelected;
        const bool hostPhase = phaseSelected("host");
        const bool outputNamePhase = phaseSelected("outputName");
        const bool renderPhase = phaseSelected("render");
        const bool coalescingPhase = phaseSelected("coalescing");
        const bool modifierWirePhase = phaseSelected("importModifierWire");
        const bool withdrawalPhase = phaseSelected("modifierWithdrawalRender");
        sharedGpuPhaseRan = renderPhase || coalescingPhase || modifierWirePhase || withdrawalPhase;
        const bool sharedManager = hostPhase || outputNamePhase || sharedGpuPhaseRan;

        std::optional<wallpaperx::WallpaperManager> manager;
        if (sharedManager) manager.emplace(config);
        // Captured as this scope unwinds, in a destructor so it runs on the exception
        // path too -- a throw out of the phases below must not silently leave the
        // validation oracle reading "0 warnings" from a null handle.
        struct CaptureOnExit {
            const std::optional<wallpaperx::WallpaperManager>& manager;
            std::shared_ptr<const std::atomic<int>>& counter;
            bool& layerActive;
            ~CaptureOnExit() {
                if (!manager) return;
                counter = manager->validationCounter();
                layerActive = manager->validationLayerActive();
            }
        } captureOnExit{manager, validationCounter, validationLayerWasActive};
        // Taken while the manager is alive and graded after this scope closes. The
        // layer's teardown complaints -- a resource still in flight, an object destroyed
        // twice -- are emitted from ~WallpaperManager and ~Device, which is exactly what
        // "the render, export and teardown path produced zero warnings" claims to cover.
        // Reading a live accessor before the scope ends would silently drop that half.

        // Service side: requests the name and serves on a background event loop.
        std::unique_ptr<sdbus::IConnection> serviceConn;
        std::optional<DbusService> service;
        std::unique_ptr<sdbus::IProxy> proxy;
        if (sharedManager) {
            serviceConn = sdbus::createSessionBusConnection(sdbus::ServiceName{DbusService::kBusName});
            service.emplace(*serviceConn, *manager);
            serviceConn->enterEventLoopAsync();

            // Client side: its own connection + loop, managed by the proxy.
            // Subscribed before the first request, so no signal can be missed.
            proxy = wallpaperx::testing::subscribeFrameReady(signals);
        }

        // Every phase runs guarded: an exception escaping one fails that phase by name and the rest
        // still run (S15.328's sibling, test_phase.h). The shared-manager phases keep running after
        // one of them throws, so a later one may add knock-on lines; none can turn the run green.
        using wallpaperx::testing::guardedPhase;
        const auto phase = [](const char* name, auto&& body) {
            if (phaseSelected(name)) guardedPhase(name, body);
        };
        if (hostPhase) guardedPhase("host", [&] { runHostChecks(*proxy, imagePath, check); });
        // Before the render phases, and on the same manager: naming a screen needs no GPU, and running
        // it first keeps it reachable on a host whose Vulkan stack cannot come up.
        if (outputNamePhase) guardedPhase("outputName", [&] { runOutputNameChecks(*proxy, check); });
        if (renderPhase) {
            guardedPhase("render",
                         [&] { runRenderChecks(*proxy, signals, imagePath, corruptPath, unrenderablePath, check); });
        }
        if (coalescingPhase) {
            guardedPhase("coalescing",
                         [&] { runCoalescingChecks(*proxy, signals, imagePath, corruptPath, burstPaths, check); });
        }
        // Last two of the shared-manager phases, on purpose and in this order: an accepted
        // report rebuilds the images the set excludes, so anything after them would be reading
        // state it did not set, and the second re-reports a wider set than the first leaves.
        if (modifierWirePhase) {
            guardedPhase("importModifierWire", [&] { runImportModifierWireChecks(*proxy, signals, imagePath, check); });
        }
        if (withdrawalPhase) {
            guardedPhase("modifierWithdrawalRender",
                         [&] { runModifierWithdrawalRenderChecks(*proxy, signals, imagePath, check); });
        }

        // Each phase below has its own manager; those that draw nothing come first, so a host
        // whose Vulkan stack cannot come up still answers for them.
        phase("unservicedRequest", [&] { runUnservicedRequestChecks(config, imagePath, unrenderablePath, check); });
        phase("propertiesWire", [&] { runPropertiesWireChecks(config, imagePath, unrenderablePath, check); });
        phase("pauseWire", [&] { runPauseWireChecks(config, unrenderablePath, check); });
        phase("soundWire", [&] { runSoundWireChecks(config, unrenderablePath, check); });
        phase("frameRateWire", [&] { runFrameRateWireChecks(config, unrenderablePath, check); });
        phase("focusStateWire", [&] { runFocusStateWireChecks(config, unrenderablePath, check); });
        phase("importModifierClient", [&] { runImportModifierClientChecks(config, imagePath, check); });
        phase("importModifierClientWire", [&] { runImportModifierClientWireChecks(config, check); });
        phase("importModifierAnnouncement",
              [&] { runImportModifierAnnouncementChecks(config, imagePath, unrenderablePath, check); });
        phase("configChangedWire", [&] { runConfigChangedWireChecks(config, unrenderablePath, check); });
        phase("persistenceChangedWire", [&] { runPersistenceChangedWireChecks(config, unrenderablePath, check); });

        // These two drive renderPending() on this thread, so there is no worker for their Device to be
        // pinned to.
        phase("importModifier", [&] { runImportModifierChecks(config, imagePath, unrenderablePath, check); });
        phase("bufferHold", [&] { runBufferHoldChecks(config, imagePath, check); });
        phase("bufferReleaseWire", [&] { runBufferReleaseWireChecks(config, imagePath, check); });

        // A screen's reported size. The first over the bus, the next two on this thread, and the last
        // with a render worker and no bus, so this thread can look while the worker is held inside its
        // own image creation.
        phase("outputSizeWire", [&] { runOutputSizeWireChecks(config, imagePath, check); });
        phase("outputSizeRestartRender", [&] { runOutputSizeRestartRenderChecks(config, imagePath, check); });
        phase("resizeRelease", [&] { runResizeReleaseChecks(config, imagePath, check); });
        phase("resizeBarrier", [&] { runResizeBarrierChecks(config, imagePath, check); });

        // The acquire timeline. The second and third hold the render thread with the framebuffer gate.
        phase("acquireTimelineWire", [&] { runAcquireTimelineWireChecks(config, imagePath, check); });
        phase("departedQueue", [&] { runDepartedQueueChecks(config, imagePath, check); });
        phase("postCallThrow", [&] { runPostCallThrowChecks(config, imagePath, check); });
        // The second holds the render thread with the framebuffer gate.
        phase("descriptorCap", [&] { runDescriptorCapChecks(config, imagePath, check); });
        phase("requestShare", [&] { runRequestShareChecks(config, imagePath, check); });
        // The first two drive their manager on this thread, so there is no worker for a Device to be
        // pinned to; the third holds the render thread with the framebuffer gate.
        phase("frameCharge", [&] { runFrameChargeChecks(config, imagePath, check); });
        phase("handoverRelease", [&] { runHandoverReleaseChecks(config, imagePath, check); });
        phase("frameReply", [&] { runFrameReplyChecks(config, imagePath, check); });

        // Its own manager and Device, scoped inside the call so both are torn down
        // before the validation warning count is read below.
        phase("workerCoalescing", [&] { runWorkerCoalescingChecks(config, burstPaths, check); });
    } catch (const std::exception& e) {
        // Without this the first unhandled sdbus::Error -- which is what a host with no
        // usable Vulkan ICD produces -- would escape main and abort the process, so
        // ctest would report an opaque SIGABRT instead of a named failure, and none of
        // the FAIL lines above would ever be printed.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++wallpaperx::testing::g_failures;
    }

    // GOAL: a WallpaperManager told nothing about a state directory writes nothing into the
    //       user's -- the crash-record refusal is opt-in, like the config file. False if any
    //       manager in this file left anything under $XDG_STATE_HOME, which is what the
    //       earlier default did: stateDir came up as defaultStateDir(), and one measured run
    //       put 16 applying-*.json files in the developer's own state directory.
    // WHY-THIS-SHAPE: the obvious version -- assert stateDir is empty on a default-built
    //       Config -- grades a declaration and not a behaviour, and would still pass if a
    //       manager resolved the directory for itself somewhere else. This looks at the
    //       filesystem, and it looks for the *directory* rather than for record files:
    //       every completed render removes its own file, so files are gone by now and only
    //       the parent ApplyingRecord's create_directories() left behind survives.
    // COVERAGE-ASSERT: an empty directory proves nothing if this process's state directory
    //       was never the scratch one, so the probe below writes a record through the same
    //       constructor the manager uses and requires it to land inside the scratch home.
    //       An XDG_STATE_HOME that never took effect fails there instead of making the
    //       emptiness check vacuously true: ScratchStateHome made to export a differently
    //       named variable left the emptiness check passing and turned this one red.
    // CONTROL: in backend/daemon/wallpaper_manager.h, Config::stateDir declared
    //       `= defaultStateDir()`, which is the value backend/daemon/main.cpp assigns ->
    //       "FAIL: no manager in this file wrote anything into $XDG_STATE_HOME", printing
    //       the leftover wallpaperx directory. Reverted.
    {
        std::error_code ec;
        std::vector<std::filesystem::path> leftovers;
        const std::filesystem::recursive_directory_iterator last;
        auto it = std::filesystem::recursive_directory_iterator(stateHome.path(), ec);
        for (; !ec && it != last; it.increment(ec)) leftovers.push_back(it->path());
        // Graded, because the ec-taking iterator yields an end iterator on failure: an
        // unreadable scratch directory would otherwise read as an empty one and pass.
        check(!ec, "the scratch state directory could be walked");
        check(leftovers.empty(), "no manager in this file wrote anything into $XDG_STATE_HOME");
        for (const std::filesystem::path& left : leftovers) {
            std::fprintf(stderr, "  left behind: %s\n", left.c_str());
        }

        constexpr int kProbeMonitor = 4242;
        const std::filesystem::path resolved = wallpaperx::defaultStateDir();
        // parent_path(), not a string prefix: a prefix would also accept a sibling directory
        // whose name merely starts with the scratch one's.
        check(!resolved.empty() && resolved.parent_path() == stateHome.path(),
              "this process's default state directory is the scratch one, so the check above looked "
              "where a manager would have written");
        {
            const wallpaperx::ApplyingRecord probe(resolved, kProbeMonitor, "static", "/nonexistent/probe.png");
            std::error_code probeEc;
            check(std::filesystem::exists(wallpaperx::crashRecordPath(resolved, kProbeMonitor), probeEc),
                  "a record written into that directory does appear there, so an empty one means nobody wrote");
        }
    }

    // Asking for validation is not the same as getting it: Device degrades silently when
    // the layer package is absent, and the warning count then stays 0 no matter what the
    // GPU actually did. Without this check the suite's only GPU-side oracle reports green
    // on any machine without vulkan-validation-layers installed.
    if (validationWaived()) {
        std::printf(
            "dbus_service_test: WARNING -- running with WALLPAPERX_ALLOW_NO_VALIDATION=1. The GPU-side "
            "correctness oracle is DISABLED; this run proves only that the DBus and threading behaviour "
            "holds. Do not read a pass here as the suite being green.\n");
    } else if (!sharedGpuPhaseRan) {
        // Reachable only under WALLPAPERX_TEST_PHASE: no shared-manager phase that draws was
        // selected, so there is no Device for this oracle to read. The selected phases grade
        // their own.
        std::printf("dbus_service_test: the shared manager's validation oracle was not graded -- no phase that "
                    "draws on it was selected.\n");
    } else if (!validationCounter) {
        // Distinct from "the layer is missing". The manager builds its Device lazily on the render worker, so if every SetWallpaper
        // failed before ensureGpu committed there is no counter at all -- and grading a
        // null handle as 0 warnings would pass the oracle on a run where no GPU work happened.
        std::fprintf(stderr,
                     "FAIL: no Vulkan device was ever created, so the validation oracle graded "
                     "nothing -- see the render failures above for the cause.\n");
        ++wallpaperx::testing::g_failures;
    } else if (!validationLayerWasActive) {
        // Two causes, opposite verdicts. The package missing from the host says nothing about
        // this daemon; the package present while the daemon's own Device failed to activate it
        // is a defect here, and reporting that as a host fact retires the GPU-side oracle
        // silently. Either way this is not a ctest skip: the rest of the run was judged.
        if (wallpaperx::testing::validationLayerAvailable()) {
            std::fprintf(stderr,
                         "FAIL: VK_LAYER_KHRONOS_validation is installed but the daemon's Device did not activate "
                         "it -- the zero-warning check below would have validated nothing.\n");
        } else {
            std::fprintf(stderr,
                         "FAIL: VK_LAYER_KHRONOS_validation is not installed on this host, so the zero-warning "
                         "check below would have passed without validating anything. Install the Vulkan "
                         "validation layers, or set WALLPAPERX_ALLOW_NO_VALIDATION=1 to accept the loss.\n");
        }
        ++wallpaperx::testing::g_failures;
    } else {
        const int warnings = validationCounter->load();
        check(warnings == 0, "the render, export and teardown path produced zero Vulkan validation warnings");
        if (warnings > 0) {
            std::fprintf(stderr, "  %d validation warning(s)\n", warnings);
        }
    }

    // Asked before the pass/skip summary, so a run that is both broken and incomplete
    // reports as broken rather than hiding behind ctest's "Skipped".
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
