// System-level test for Step 6: stands up the real DbusService on the session bus
// and drives it through a real sdbus-c++ proxy client (same process, separate
// connections -- messages still round-trip through the bus). Task 2 made this an
// end-to-end test: a static wallpaper is rendered on the GPU and its frame comes
// back over DBus as a real Unix fd plus the metadata an importer needs.
//
// Run under a private bus, e.g. `dbus-run-session -- ./wallpaperx-dbus-service-test`.
//
// The checks are ordered so everything that needs no GPU runs first. This entry used
// to be hardware-independent and no longer is, and the parts that still are -- the
// wire-signature lock above all -- are too valuable to lose on a host whose Vulkan
// stack cannot come up.
#include "daemon/dbus_service.h"
#include "daemon/wallpaper_manager.h"

#include "vulkan/device.h"
#include "vulkan/frame_export.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

using wallpaperx::DbusService;

namespace {

// Deliberately not square. With equal dimensions a width/height swap anywhere between
// the target and the wire would satisfy every assertion below.
constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 48;

// Monitor ids are kept disjoint per scenario on purpose: the "nothing rendered yet"
// assertions have to run against an id that no other check has ever touched, or they
// silently start passing/failing for the wrong reason as the test grows.
constexpr int32_t kMonitorStatic = 0;
constexpr int32_t kMonitorVideo = 1;
constexpr int32_t kMonitorBadPath = 2;
constexpr int32_t kMonitorNeverSet = 9;

std::string writeTestImage() {
    std::filesystem::create_directories(WALLPAPERX_TEST_IMAGE_DIR);
    const std::string path = std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/dbus_test_blue.png";

    const int w = 8;
    const int h = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        pixels[i * 4 + 0] = 0;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 255;
        pixels[i * 4 + 3] = 255;
    }

    if (!stbi_write_png(path.c_str(), w, h, 4, pixels.data(), w * 4)) {
        throw std::runtime_error("dbus_service_test: failed to write test image");
    }
    return path;
}

// Descriptors currently open in this process. The service and the client share the
// process here, so a descriptor the daemon forgets to close is visible from the test
// -- which is the only vantage point from which GetFrameFd's fd ownership can actually
// be observed (see the leak check below).
size_t openFdCount() {
    size_t count = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++count;
    }
    return count;
}

// Every <arg> of `method` in introspection XML, in document order, as
// (direction, "type:name"). Attribute order and whitespace inside the tag don't
// matter -- only the values, which are the wire contract.
std::vector<std::pair<std::string, std::string>> parseMethodArgs(const std::string& xml, const std::string& method) {
    std::vector<std::pair<std::string, std::string>> args;
    // [\s\S] rather than . -- the XML is multi-line and . does not match newline.
    const std::regex methodRe("<method name=\"" + method + "\">([\\s\\S]*?)</method>");
    std::smatch methodMatch;
    if (!std::regex_search(xml, methodMatch, methodRe)) {
        return args;
    }
    const std::string body = methodMatch[1].str();

    const std::regex argRe("<arg\\b[^>]*/>");
    const std::regex attrRe("(\\w+)=\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), argRe); it != std::sregex_iterator(); ++it) {
        const std::string tag = it->str();
        std::string type, name, direction;
        for (auto a = std::sregex_iterator(tag.begin(), tag.end(), attrRe); a != std::sregex_iterator(); ++a) {
            const std::string key = (*a)[1].str(), value = (*a)[2].str();
            if (key == "type") type = value;
            else if (key == "name") name = value;
            else if (key == "direction") direction = value;
        }
        args.emplace_back(direction, type + ":" + name);
    }
    return args;
}

// Everything GetFrameFd hands back, in one place, so the assertions below read as
// claims about the frame rather than as tuple indices.
struct Frame {
    sdbus::UnixFd fd;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    int32_t handleType = 0;
    std::vector<sdbus::UnixFd> syncFds;
    uint64_t generation = 0;
    uint32_t drmFormat = 0;
    uint64_t drmModifier = 0;
    std::vector<sdbus::Struct<uint64_t, uint64_t>> planes;
};

Frame getFrameFd(sdbus::IProxy& proxy, int32_t monitorId) {
    Frame frame;
    proxy.callMethod("GetFrameFd")
        .onInterface(DbusService::kInterface)
        .withArguments(monitorId)
        .storeResultsTo(frame.fd, frame.width, frame.height, frame.memorySize, frame.handleType, frame.syncFds,
                        frame.generation, frame.drmFormat, frame.drmModifier, frame.planes);
    return frame;
}

// Returns the DBus error name a call failed with, or "" if it unexpectedly succeeded.
std::string errorNameFrom(const std::function<void()>& call) {
    try {
        call();
    } catch (const sdbus::Error& e) {
        return e.getName();
    }
    return "";
}

using Check = std::function<void(bool, const char*)>;

// --------------------------------------------------------------------------- //
// Checks that need no working GPU. Kept first and separate so a host that cannot
// create a Vulkan device still verifies the wire contract.
// --------------------------------------------------------------------------- //
void runHostChecks(sdbus::IProxy& proxy, const std::string& imagePath, const Check& check) {
    std::string status;
    proxy.callMethod("GetStatus").onInterface(DbusService::kInterface).storeResultsTo(status);
    nlohmann::json json = nlohmann::json::parse(status, nullptr, /*allow_exceptions=*/false);
    check(!json.is_discarded(), "GetStatus returns parseable JSON");
    check(!json.is_discarded() && json.value("running", false), "GetStatus reports running=true");

    // A monitor nothing was ever set on has no frame -- a named, distinguishable error
    // rather than a bogus fd.
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorNeverSet); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "GetFrameFd reports NoFrame for a monitor that was never set");

    check(errorNameFrom([&] {
              proxy.callMethod("SetWallpaper")
                  .onInterface(DbusService::kInterface)
                  .withArguments(std::string("banana"), imagePath, kMonitorVideo);
          }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "an unknown wallpaper type is rejected as InvalidArgs");

    check(errorNameFrom([&] {
              proxy.callMethod("SetWallpaper")
                  .onInterface(DbusService::kInterface)
                  .withArguments(std::string("static"), imagePath, int32_t{-1});
          }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "a negative monitor id is rejected as InvalidArgs");

    const int32_t tooLarge = wallpaperx::WallpaperManager::kMaxMonitors;
    check(errorNameFrom([&] {
              proxy.callMethod("SetWallpaper")
                  .onInterface(DbusService::kInterface)
                  .withArguments(std::string("static"), imagePath, tooLarge);
          }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "a monitor id at or above the cap is rejected as InvalidArgs");

    // Both entry points must reject an id the same way. GetFrameFd answering NoFrame
    // for an out-of-range id would tell a polling consumer to retry forever on what is
    // actually a permanent client-side mistake.
    check(errorNameFrom([&] { getFrameFd(proxy, int32_t{-1}); }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "GetFrameFd rejects a negative monitor id as InvalidArgs, not NoFrame");
    check(errorNameFrom([&] { getFrameFd(proxy, tooLarge); }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "GetFrameFd rejects an out-of-range monitor id as InvalidArgs, not NoFrame");

    // Lock the whole wire signature, every arg, in order. The KDE plugin (Step 12) is
    // generated against it, so a reordering, a type change or a dropped slot must fail
    // here rather than at plugin-import time -- a partial check would let width/height
    // be swapped and still report green.
    //
    // Parsed rather than substring-matched: matching sd-bus's emitted XML verbatim
    // would couple this test to libsystemd's attribute order and spacing, so a
    // formatting change there would look like a wire-format regression.
    std::string introspection;
    proxy.callMethod("Introspect").onInterface("org.freedesktop.DBus.Introspectable").storeResultsTo(introspection);

    const std::vector<std::pair<std::string, std::string>> expectedArgs = {
        {"in", "i:monitor_id"},   {"out", "h:fd"},
        {"out", "u:width"},       {"out", "u:height"},
        {"out", "t:memory_size"}, {"out", "i:handle_type"},
        {"out", "ah:sync_fds"},   {"out", "t:frame_generation"},
        {"out", "u:drm_format"},  {"out", "t:drm_modifier"},
        {"out", "a(tt):planes"},
    };
    const std::vector<std::pair<std::string, std::string>> actualArgs = parseMethodArgs(introspection, "GetFrameFd");
    check(actualArgs == expectedArgs, "GetFrameFd's full wire signature and arg order are unchanged");
    if (actualArgs != expectedArgs) {
        std::fprintf(stderr, "  expected:");
        for (const auto& [dir, arg] : expectedArgs) std::fprintf(stderr, " %s(%s)", arg.c_str(), dir.c_str());
        std::fprintf(stderr, "\n  actual:  ");
        for (const auto& [dir, arg] : actualArgs) std::fprintf(stderr, " %s(%s)", arg.c_str(), dir.c_str());
        std::fprintf(stderr, "\n");
    }
}

// --------------------------------------------------------------------------- //
// Checks that render on the GPU and export a real frame.
// --------------------------------------------------------------------------- //
void runRenderChecks(sdbus::IProxy& proxy, const std::string& imagePath, const Check& check) {
    proxy.callMethod("SetWallpaper")
        .onInterface(DbusService::kInterface)
        .withArguments(std::string("static"), imagePath, kMonitorStatic);

    {
        const Frame frame = getFrameFd(proxy, kMonitorStatic);

        check(frame.fd.isValid(), "GetFrameFd returns a valid Unix fd after a static render");
        check(frame.width == kTargetWidth && frame.height == kTargetHeight,
              "GetFrameFd reports the target's dimensions");
        check(frame.generation == 1, "frame_generation is 1 after the first render");
        check(frame.syncFds.empty(), "sync_fds is empty (reserved slot)");
        check(frame.handleType == 0 || frame.handleType == 1, "handle_type is OPAQUE_FD (0) or DMA_BUF (1)");

        // memory_size must be the exact allocation size, not merely big enough:
        // glImportMemoryFdEXT and EGL both take it as *the* size of the imported object,
        // so a value that is too large fails the import and one that is too small is
        // read as a smaller buffer. A dma_buf reports its true size via SEEK_END, which
        // is an oracle independent of whatever the daemon chose to put on the wire.
        if (frame.handleType == 1) {
            const off_t seekSize = lseek(frame.fd.get(), 0, SEEK_END);
            check(seekSize > 0 && static_cast<uint64_t>(seekSize) == frame.memorySize,
                  "memory_size equals the dma_buf's actual size");
            if (seekSize > 0 && static_cast<uint64_t>(seekSize) != frame.memorySize) {
                std::fprintf(stderr, "  memory_size=%llu but the dma_buf is %lld bytes\n",
                             static_cast<unsigned long long>(frame.memorySize),
                             static_cast<long long>(seekSize));
            }
        }
        check(frame.memorySize >= static_cast<uint64_t>(kTargetWidth) * kTargetHeight * 4,
              "memory_size covers at least the frame's pixels");

        // The three layout fields are populated together or not at all. A half-populated
        // set is the failure that matters: an importer given a fourcc but no modifier,
        // or planes with no fourcc, builds a wrong image rather than failing.
        const bool populated = frame.drmModifier != wallpaperx::kDrmModifierInvalid;
        if (populated) {
            check(frame.handleType == 1, "layout metadata is only published on the DMA_BUF path");
            check(frame.drmFormat != 0, "a populated frame carries a DRM fourcc");
            check(!frame.planes.empty(), "a populated frame carries at least one plane");
            for (const auto& plane : frame.planes) {
                check(plane.get<1>() >= static_cast<uint64_t>(kTargetWidth) * 4,
                      "each plane's rowPitch covers a row of pixels");
            }
        } else {
            check(frame.drmFormat == 0, "an unpopulated frame reports no fourcc");
            check(frame.planes.empty(), "an unpopulated frame reports no planes");
        }
        std::printf("dbus_service_test: handle_type=%d drm_format=0x%08x modifier=0x%llx planes=%zu\n",
                    frame.handleType, frame.drmFormat, static_cast<unsigned long long>(frame.drmModifier),
                    frame.planes.size());
    }

    // Every GetFrameFd produces a fresh descriptor that the daemon must hand over
    // exactly once. Asserting the returned fd is open proves nothing -- sdbus-c++ dups
    // out of the received message, so the client's copy is always open and is not the
    // daemon's descriptor at all. What is observable is the process fd count: service
    // and client share this process, so a daemon that keeps its own copy (the
    // duplicating UnixFd constructor instead of adopt_fd) shows up as one leaked
    // descriptor per call. One warm-up call first, so one-time bus allocations are not
    // counted as growth.
    { const Frame warmup = getFrameFd(proxy, kMonitorStatic); }
    const size_t fdsBefore = openFdCount();
    constexpr int kPollCount = 32;
    for (int i = 0; i < kPollCount; ++i) {
        const Frame polled = getFrameFd(proxy, kMonitorStatic);
        check(polled.fd.isValid(), "each polled frame carries a valid fd");
    }
    const size_t fdsAfter = openFdCount();
    check(fdsAfter <= fdsBefore + 4, "repeated GetFrameFd calls do not leak descriptors");
    if (fdsAfter > fdsBefore + 4) {
        std::fprintf(stderr, "  fd count grew from %zu to %zu across %d GetFrameFd calls\n", fdsBefore, fdsAfter,
                     kPollCount);
    }

    // Re-rendering the same monitor exercises the queue-family acquire that pairs with
    // the release the exports above performed -- the path that only runs from the second
    // frame onward, and the one validation is silent about.
    proxy.callMethod("SetWallpaper")
        .onInterface(DbusService::kInterface)
        .withArguments(std::string("static"), imagePath, kMonitorStatic);
    {
        const Frame frame = getFrameFd(proxy, kMonitorStatic);
        check(frame.generation == 2, "frame_generation increments on re-render");
        check(frame.fd.isValid(), "a re-rendered frame still exports a valid fd");
    }

    // video is a recognised type but Step 13 renders it; it is recorded, not rendered.
    proxy.callMethod("SetWallpaper")
        .onInterface(DbusService::kInterface)
        .withArguments(std::string("video"), std::string("/tmp/wallpaperx-test.mp4"), kMonitorVideo);
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorVideo); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "a video wallpaper is recorded but yields no frame");

    // A failed render must leave the monitor with no frame, not with a stale one. The
    // monitor is given a working wallpaper first, so there is a frame to go stale --
    // asserting this on a monitor that never had one would pass no matter what
    // SetWallpaper does on failure.
    proxy.callMethod("SetWallpaper")
        .onInterface(DbusService::kInterface)
        .withArguments(std::string("static"), imagePath, kMonitorBadPath);
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorBadPath); }).empty(),
          "the monitor used for the failure case starts with a valid frame");

    check(errorNameFrom([&] {
              proxy.callMethod("SetWallpaper")
                  .onInterface(DbusService::kInterface)
                  .withArguments(std::string("static"), std::string("/nonexistent/wallpaperx.png"), kMonitorBadPath);
          }) == "io.wallpaperx.Daemon.Error.RenderFailed",
          "an unreadable image fails with RenderFailed");
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorBadPath); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "a monitor whose render failed has no frame");

    // GetStatus reflects the recorded requests, rendered or not.
    std::string status;
    proxy.callMethod("GetStatus").onInterface(DbusService::kInterface).storeResultsTo(status);
    nlohmann::json json = nlohmann::json::parse(status, nullptr, false);
    const std::string staticId = std::to_string(kMonitorStatic);
    const std::string videoId = std::to_string(kMonitorVideo);
    const bool reflected = !json.is_discarded() && json["wallpapers"].contains(staticId) &&
                           json["wallpapers"][staticId]["type"] == "static" &&
                           json["wallpapers"][staticId]["path"] == imagePath &&
                           json["wallpapers"][staticId]["has_frame"] == true &&
                           json["wallpapers"][staticId]["generation"] == 2 &&
                           json["wallpapers"][videoId]["type"] == "video" &&
                           json["wallpapers"][videoId]["has_frame"] == false;
    check(reflected, "GetStatus reflects per-monitor type, path, generation and frame availability");
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

    wallpaperx::resetValidationWarningCount();

    // Scoped so the manager, and with it the Vulkan Device, the offscreen targets and
    // Memory's leaked-allocation check, are all destroyed before the validation counter
    // is read below. Destroying an image still owned by an external queue family, or
    // leaking device memory, only raises validation errors during teardown -- sampling
    // the counter while the manager is still alive silently exempts the whole
    // destruction half of the code under test.
    try {
        const std::string imagePath = writeTestImage();

        wallpaperx::WallpaperManager::Config config;
        config.width = kTargetWidth;
        config.height = kTargetHeight;
        // Validation on: it is the correctness oracle for the export path's barriers and
        // queue-family transfers, none of which this test can observe directly.
        config.enableValidation = true;
        wallpaperx::WallpaperManager manager(config);

        // Service side: requests the name and serves on a background event loop.
        auto serviceConn = sdbus::createSessionBusConnection(sdbus::ServiceName{DbusService::kBusName});
        DbusService service(*serviceConn, manager);
        serviceConn->enterEventLoopAsync();

        // Client side: its own connection + loop, managed by the proxy.
        auto proxy =
            sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

        runHostChecks(*proxy, imagePath, check);
        runRenderChecks(*proxy, imagePath, check);
    } catch (const std::exception& e) {
        // Without this the first unhandled sdbus::Error -- which is what a host with no
        // usable Vulkan ICD produces on the very first SetWallpaper -- would escape main
        // and abort the process, so ctest would report an opaque SIGABRT instead of a
        // named failure, and none of the FAIL lines above would ever be printed.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++failures;
    }

    // Asking for validation is not the same as getting it: Device degrades silently when
    // the layer package is absent, and the warning count then stays 0 no matter what the
    // GPU actually did. Without this check the suite's only GPU-side oracle reports green
    // on any machine without vulkan-validation-layers installed.
    if (!wallpaperx::validationLayerActive()) {
        std::fprintf(stderr,
                     "FAIL: VK_LAYER_KHRONOS_validation was requested but is not loaded -- the "
                     "zero-warning check below would pass without validating anything. "
                     "Install the Vulkan validation layers.\n");
        ++failures;
    }

    const int warnings = wallpaperx::validationWarningCount();
    check(warnings == 0, "the render, export and teardown path produced zero Vulkan validation warnings");
    if (warnings > 0) {
        std::fprintf(stderr, "  %d validation warning(s)\n", warnings);
    }

    if (failures == 0) {
        std::printf("dbus_service_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "dbus_service_test: %d check(s) failed\n", failures);
    return 1;
}
