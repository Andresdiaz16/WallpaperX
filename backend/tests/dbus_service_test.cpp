// System-level test for Step 6: stands up the real DbusService on the session bus
// and drives it through a real sdbus-c++ proxy client (same process, separate
// connections -- messages still round-trip through the bus). A static wallpaper is
// rendered on the GPU and its frame comes back over DBus as a real Unix fd plus the
// metadata an importer needs.
//
// Run under a private bus, e.g. `dbus-run-session -- ./wallpaperx-dbus-service-test`.
//
// Two structural rules this file follows, both learned the hard way:
//
//  - Everything that needs no GPU runs first. This entry used to be
//    hardware-independent and no longer is, and the parts that still are -- the
//    wire-signature lock above all -- are too valuable to lose on a host whose Vulkan
//    stack cannot come up.
//  - Nothing waits on a sleep. Since task 3, SetWallpaper returns before the render
//    happens, so "the frame is ready" is knowable only from the FrameReady signal.
//    Negative cases ("no signal for this one") are settled by posting a later request
//    that *does* signal: the daemon has a single FIFO render thread, so that later
//    signal proves the earlier request was already processed.
#include "daemon/dbus_service.h"
#include "daemon/render_worker.h"
#include "daemon/wallpaper_manager.h"

#include "vulkan/device.h"
#include "vulkan/frame_export.h"

// Two support directories, spelled differently on purpose: the renderer's is on the
// include path as `support/...`, the backend's is reached through the backend source
// root, so the two never resolve to each other by accident.
#include "support/exported_frame_map.h"
#include "tests/support/daemon_client.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

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
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

using wallpaperx::DbusService;

namespace {

// The client-side wire helpers now live in tests/support/daemon_client.h, shared with
// daemon_system_test so both tiers read the same reply layout. Pulled in one name at a
// time rather than with a blanket using-directive, so it stays visible which of them
// this file depends on.
using wallpaperx::testing::Check;
using wallpaperx::testing::errorNameFrom;
using wallpaperx::testing::Frame;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::getFrameFd;
using wallpaperx::testing::getStatus;
using wallpaperx::testing::kSignalTimeout;
using wallpaperx::testing::openFdCount;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::wallpaperStatus;

// Deliberately not square. With equal dimensions a width/height swap anywhere between
// the target and the wire would satisfy every assertion below.
constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 48;

// Monitor ids are kept disjoint per scenario on purpose: the "nothing rendered yet"
// assertions have to run against an id that no other check has ever touched, or they
// silently start passing/failing for the wrong reason as the test grows.
constexpr int32_t kMonitorStatic = 0;
constexpr int32_t kMonitorVideo = 1;
constexpr int32_t kMonitorCorrupt = 2;
constexpr int32_t kMonitorBarrier = 3;
constexpr int32_t kMonitorBurstFailLast = 4;
constexpr int32_t kMonitorBurstRenderLast = 5;
constexpr int32_t kMonitorBurstContent = 6;
constexpr int32_t kMonitorCoalesceBarrier = 7;
constexpr int32_t kMonitorErrorRecovery = 8;
constexpr int32_t kMonitorNeverSet = 9;
constexpr int32_t kMonitorWorkerCoalesce = 10;

// How many requests each coalescing burst fires at one monitor. Five is enough that a
// worker draining one per render still leaves several to collapse.
constexpr int kBurstSize = 5;

std::string testAssetDir() { return std::string(WALLPAPERX_TEST_IMAGE_DIR); }

// Whether to run without the validation layer. Exists for one caller: a ThreadSanitizer
// build, where the layer is uninstrumented, keeps its own lock-heavy state maps, and
// drowns the report in races inside code we do not ship -- while suppressing it produces
// its own artifacts, since TSan then sees the layer's unlocks without their locks.
//
// Compared against "1" exactly, matching WALLPAPERX_ALLOW_NO_HEAP_TIER: a plain getenv()
// truthiness check would make "=0" mean "waived", which is the opposite of what anyone
// typing it intends. Waiving is announced loudly below, because this deliberately
// disables the suite's only GPU-side oracle and a silent version of this switch is
// exactly the vacuous-green trap the validation check was added to close.
bool validationWaived() {
    const char* env = std::getenv("WALLPAPERX_ALLOW_NO_VALIDATION");
    return env != nullptr && std::strcmp(env, "1") == 0;
}

std::string writeTestImage() {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/dbus_test_blue.png";

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

// The colours the coalescing burst cycles through, as RGB. Solid fills, so the rendered
// frame is one colour end to end and identifying which request produced it needs no
// knowledge of how StaticRenderer scales its input.
//
// Chosen so the first and last differ in every channel: a daemon that served the first
// request of a burst instead of the last must be wrong about red, green *and* blue, not
// merely dim.
constexpr int kBurstColorCount = kBurstSize;
constexpr unsigned char kBurstColors[kBurstColorCount][3] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {32, 160, 224},
};

// Asserted here, at build time, rather than as a runtime check() inside the burst.
// Every operand is constexpr, so the compiler folds the whole expression -- checking it
// at run time meant an edit that made the palette non-discriminating was caught only
// after building, launching a daemon and running a burst, on whichever machine happened
// to run the suite, and was then reported as a test failure rather than as what it is:
// a bad edit to a constant.
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

// A file that exists but is not an image. SetWallpaper's synchronous check only proves
// the path is a regular file, so this is what reaches the render thread and fails
// there -- the only way to exercise the asynchronous failure path.
std::string writeCorruptImage() {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/dbus_test_corrupt.png";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "this is not a PNG";
    if (!out) {
        throw std::runtime_error("dbus_service_test: failed to write corrupt test image");
    }
    return path;
}

// Every <arg> of an introspection element, in document order, as
// (direction, "type:name"). Attribute order and whitespace inside the tag don't
// matter -- only the values, which are the wire contract. `element` is "method" or
// "signal"; a signal's args carry no direction, which shows up as an empty string.
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

// --------------------------------------------------------------------------- //
// Checks that need no working GPU. Kept first and separate so a host that cannot
// create a Vulkan device still verifies the wire contract.
// --------------------------------------------------------------------------- //
void runHostChecks(sdbus::IProxy& proxy, const std::string& imagePath, const Check& check) {
    nlohmann::json json = getStatus(proxy);
    check(!json.is_discarded(), "GetStatus returns parseable JSON");
    check(!json.is_discarded() && json.value("running", false), "GetStatus reports running=true");

    // Nothing has needed the GPU yet, so there is no tier to report. Reporting one here
    // would mean the daemon had guessed.
    check(!json.is_discarded() && json.contains("gpu") && json["gpu"]["initialized"] == false,
          "GetStatus reports the GPU as uninitialised before the first render");

    // A monitor nothing was ever set on has no frame -- a named, distinguishable error
    // rather than a bogus fd.
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorNeverSet); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "GetFrameFd reports NoFrame for a monitor that was never set");

    check(errorNameFrom([&] { setWallpaper(proxy, "banana", imagePath, kMonitorVideo); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "an unknown wallpaper type is rejected as InvalidArgs");

    check(errorNameFrom([&] { setWallpaper(proxy, "static", imagePath, -1); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a negative monitor id is rejected as InvalidArgs");

    const int32_t tooLarge = wallpaperx::WallpaperManager::kMaxMonitors;
    check(errorNameFrom([&] { setWallpaper(proxy, "static", imagePath, tooLarge); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a monitor id at or above the cap is rejected as InvalidArgs");

    // Both entry points must reject an id the same way. GetFrameFd answering NoFrame
    // for an out-of-range id would tell a polling consumer to retry forever on what is
    // actually a permanent client-side mistake.
    check(errorNameFrom([&] { getFrameFd(proxy, -1); }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "GetFrameFd rejects a negative monitor id as InvalidArgs, not NoFrame");
    check(errorNameFrom([&] { getFrameFd(proxy, tooLarge); }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "GetFrameFd rejects an out-of-range monitor id as InvalidArgs, not NoFrame");

    // A path that is not a file is the commonest user error, and rendering is
    // asynchronous -- so it must fail on the reply rather than becoming silence the
    // client can only discover by polling.
    check(errorNameFrom([&] { setWallpaper(proxy, "static", "/nonexistent/wallpaperx.png", kMonitorNeverSet); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a static path that is not a file is rejected synchronously as InvalidArgs");
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorNeverSet); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "a rejected request leaves the monitor with no frame");

    // Lock the whole wire contract: every argument of every method and signal, in
    // order. The KDE plugin (Step 12) is generated against it, so a reordering, a type
    // change or a dropped slot must fail here rather than at plugin-import time -- a
    // partial check would let width/height be swapped and still report green.
    //
    // Parsed rather than substring-matched: matching sd-bus's emitted XML verbatim
    // would couple this test to libsystemd's attribute order and spacing, so a
    // formatting change there would look like a wire-format regression.
    std::string introspection;
    proxy.callMethod("Introspect").onInterface("org.freedesktop.DBus.Introspectable").storeResultsTo(introspection);

    const std::vector<std::tuple<std::string, std::string, ArgList>> expected = {
        {"method", "SetWallpaper",
         ArgList{{"in", "s:type"}, {"in", "s:path"}, {"in", "i:monitor_id"}}},
        {"method", "GetFrameFd",
         ArgList{{"in", "i:monitor_id"},
                 {"out", "h:fd"},
                 {"out", "u:width"},
                 {"out", "u:height"},
                 {"out", "t:memory_size"},
                 {"out", "i:handle_type"},
                 {"out", "ah:sync_fds"},
                 {"out", "t:frame_generation"},
                 {"out", "u:drm_format"},
                 {"out", "t:drm_modifier"},
                 {"out", "a(tt):planes"}}},
        {"method", "GetStatus", ArgList{{"out", "s:status"}}},
        // A signal's args carry no direction attribute, hence the empty first field.
        {"signal", "FrameReady", ArgList{{"", "i:monitor_id"}, {"", "t:frame_generation"}}},
    };

    for (const auto& [element, name, expectedArgs] : expected) {
        const ArgList actualArgs = parseArgs(introspection, element, name);
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
}

// --------------------------------------------------------------------------- //
// Checks that render on the GPU and export a real frame.
// --------------------------------------------------------------------------- //
void runRenderChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& imagePath,
                     const std::string& corruptPath, const Check& check) {
    // Timed, because this is the one behaviour nothing else here can distinguish: every
    // other assertion in this function is satisfied just as well by rendering inside the
    // handler. This is also the most expensive request the daemon ever serves -- the
    // first one creates the Vulkan instance, loads the validation layers, builds the
    // device and the descriptor allocator, compiles the pipeline and only then renders.
    //
    // The assertion compares the reply against *this run's own* render duration rather
    // than against a fixed bound. A fixed bound was tried first and was useless: with
    // the render inside the handler the reply took 186 ms on NVIDIA and 29 ms on
    // Lavapipe, so any threshold loose enough to be safe on the slower machine also
    // passed the regression on the faster one. The two clauses cover each other -- a
    // reply that is essentially instant is proof on its own, and the ratio catches a
    // slow bus round trip on a loaded machine, where an absolute bound would flake.
    const auto requestedAt = std::chrono::steady_clock::now();
    setWallpaper(proxy, "static", imagePath, kMonitorStatic);
    const auto replyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - requestedAt).count();

    check(signals.wait(kMonitorStatic, 1), "FrameReady(monitor=0, generation=1) arrives after the first render");
    const auto frameMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - requestedAt).count();

    std::printf("dbus_service_test: SetWallpaper replied in %lld ms, FrameReady at %lld ms\n",
                static_cast<long long>(replyMs), static_cast<long long>(frameMs));
    check(replyMs <= 2 || replyMs * 4 < frameMs, "SetWallpaper replies without waiting for the render to finish");

    {
        const Frame frame = getFrameFd(proxy, kMonitorStatic);

        check(frame.fd.isValid(), "GetFrameFd returns a valid Unix fd after a static render");
        check(frame.width == kTargetWidth && frame.height == kTargetHeight,
              "GetFrameFd reports the target's dimensions");
        // The signal's generation must name the frame the export hands back, or a
        // consumer cannot tell whether the frame it holds is the announced one.
        check(frame.generation == 1, "the exported frame carries the generation FrameReady announced");
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

    // The GPU is up now, so GetStatus must say which one and which descriptor tier it
    // resolved to -- the first thing worth knowing when a bug reproduces on one GPU and
    // not another.
    {
        const nlohmann::json json = getStatus(proxy);
        const bool reported = !json.is_discarded() && json["gpu"]["initialized"] == true &&
                              json["gpu"].contains("name") && !json["gpu"]["name"].get<std::string>().empty() &&
                              json["gpu"].contains("descriptor_tier") &&
                              !json["gpu"]["descriptor_tier"].get<std::string>().empty();
        check(reported, "GetStatus reports the GPU name and resolved descriptor tier once initialised");
        if (!json.is_discarded() && json["gpu"]["initialized"] == true) {
            std::printf("dbus_service_test: gpu=%s tier=%s\n", json["gpu"].value("name", "?").c_str(),
                        json["gpu"].value("descriptor_tier", "?").c_str());
        }
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
    setWallpaper(proxy, "static", imagePath, kMonitorStatic);
    check(signals.wait(kMonitorStatic, 2), "FrameReady(generation=2) arrives after a re-render");
    {
        const Frame frame = getFrameFd(proxy, kMonitorStatic);
        check(frame.generation == 2, "frame_generation increments on re-render");
        check(frame.fd.isValid(), "a re-rendered frame still exports a valid fd");
    }

    // --- Requests that must produce no frame and no signal. ---

    // The failure case gets a working wallpaper first, so there is a frame to go stale.
    // Asserting "no frame after a failed render" on a monitor that never had one passes
    // no matter what the daemon does on failure -- which is exactly how a regression
    // that served the previous wallpaper's pixels under the new path would slip through.
    setWallpaper(proxy, "static", imagePath, kMonitorCorrupt);
    check(signals.wait(kMonitorCorrupt, 1), "the monitor used for the failure case renders a frame first");
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorCorrupt); }).empty(),
          "the monitor used for the failure case starts with a valid frame");

    // These three are posted before a request that does signal. The daemon does all GPU
    // work on one FIFO thread, so once the barrier's FrameReady arrives, they have
    // provably been processed -- no sleep, no flake.
    setWallpaper(proxy, "video", "/tmp/wallpaperx-test.mp4", kMonitorVideo);
    setWallpaper(proxy, "static", corruptPath, kMonitorCorrupt);
    setWallpaper(proxy, "static", imagePath, kMonitorBarrier);
    check(signals.wait(kMonitorBarrier, 1), "FrameReady arrives for the barrier request");

    check(signals.countFor(kMonitorVideo) == 0, "a video request emits no FrameReady");
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorVideo); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "a video wallpaper is recorded but yields no frame");

    // Still 1: the good render above, and nothing for the corrupt one that followed.
    check(signals.countFor(kMonitorCorrupt) == 1, "a render that fails emits no FrameReady");
    // RenderFailed, not NoFrame, and this is the whole point of the distinction: this
    // monitor and kMonitorVideo above are both frameless, but one will produce a frame
    // if the client waits and the other never will. Answering both with NoFrame told a
    // polling consumer to retry a wallpaper that cannot succeed. Asserted next to the
    // video case deliberately -- if the two ever collapse back to one error name, one of
    // these two lines fails.
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorCorrupt); }) ==
              "io.wallpaperx.Daemon.Error.RenderFailed",
          "a failed render is reported as RenderFailed, distinctly from a monitor with nothing rendered yet");

    // The reply to SetWallpaper was already sent by the time the render failed, so
    // GetStatus is the only place that failure is reported.
    {
        const nlohmann::json json = getStatus(proxy);
        const nlohmann::json* corrupt = wallpaperStatus(json, std::to_string(kMonitorCorrupt));
        const bool reported = corrupt != nullptr && !corrupt->value("last_error", std::string()).empty();
        check(reported, "GetStatus reports last_error for an asynchronous render failure");
    }

    // A monitor that rendered successfully must not carry a stale error.
    {
        const nlohmann::json json = getStatus(proxy);
        const nlohmann::json* still = wallpaperStatus(json, std::to_string(kMonitorStatic));
        const nlohmann::json* video = wallpaperStatus(json, std::to_string(kMonitorVideo));
        const bool reflected =
            still != nullptr && video != nullptr && still->value("type", std::string()) == "static" &&
            still->value("path", std::string()) == imagePath && still->value("has_frame", false) &&
            still->value("generation", uint64_t{0}) == 2 && still->value("width", uint32_t{0}) == kTargetWidth &&
            still->value("height", uint32_t{0}) == kTargetHeight && !still->contains("last_error") &&
            video->value("type", std::string()) == "video" && !video->value("has_frame", true);
        check(reflected, "GetStatus reflects per-monitor type, path, size, generation and frame availability");
    }
}

// --------------------------------------------------------------------------- //
// Coalescing, driven at the level the mechanisms actually live at.
//
// Two independent mechanisms collapse a burst, and both sit below DBus:
//
//   RenderWorker::m_queuedRenders  drops a monitor already sitting in the queue.
//   WallpaperManager::servicedSeq  makes renderPending a no-op for a request that has
//                                  already been serviced.
//
// So this drives the worker and the manager directly rather than through the bus. The
// point is not to avoid DBus for its own sake -- it is that over the bus the claim
// cannot be stated exactly. Counting the renders a burst produces measures how many
// completed inside a window whose length the OS scheduler sets, not the daemon: the
// worker erases a monitor from m_queuedRenders before rendering it, so every render that
// finishes mid-burst admits one more queue entry, and under `ctest -jN` the client's own
// calls are descheduled too. Any fixed bound is therefore either flaky or too loose to
// catch the thing it is bounding -- `<= 2` was reachable at 3.
//
// Holding the worker on a task that blocks removes the race instead of tolerating it:
// the worker runs one job at a time, so nothing can render until this thread releases
// the gate, and all five requests are provably recorded before the first render starts.
// The assertion becomes exact. Measured against the alternatives: with the gate held,
// either mechanism alone still yields 1, and both disabled yields 5.
//
// Uses its own WallpaperManager rather than the daemon's, and not for isolation's sake:
// Device pins itself to the thread that created it and aborts on a GPU call from any
// other, because Vulkan submission is not free-threaded and it shares one command pool.
// The daemon's Device belongs to the daemon's worker thread, so a second worker driving
// the same manager trips that guard by design. A second manager brings its own Device,
// created on and confined to the worker started below.
void runWorkerCoalescingChecks(const wallpaperx::WallpaperManager::Config& config,
                               const std::vector<std::string>& burstPaths, const Check& check) {
    wallpaperx::WallpaperManager manager(config);
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
    worker.postTask([gateOpen] { gateOpen.wait(); });

    for (const std::string& path : burstPaths) {
        manager.requestWallpaper("static", path, kMonitorWorkerCoalesce);
        worker.post(kMonitorWorkerCoalesce);
    }

    // Queued behind the burst on the same FIFO, so once it runs every render the burst
    // produced has completed. stop() deliberately does not drain the queue, so it cannot
    // be used as the barrier here.
    std::promise<void> drained;
    const std::future<void> drainedFuture = drained.get_future();
    worker.postTask([&drained] { drained.set_value(); });

    gate.release();
    drainedFuture.wait();

    const wallpaperx::WallpaperManager::Snapshot snapshot = manager.snapshot();
    const auto entry = snapshot.monitors.find(kMonitorWorkerCoalesce);
    const bool present = entry != snapshot.monitors.end();
    check(present, "the monitor the coalescing burst targeted appears in the manager's snapshot");
    if (!present) return;

    check(entry->second.generation == 1,
          "five requests posted while the render thread is held collapse into exactly one render");
    check(frameReadySignals.load() == 1, "exactly one FrameReady callback fires for the collapsed burst");
    check(entry->second.path == burstPaths.back(), "the one render that happens serves the LAST request of the burst");

    if (entry->second.generation != 1) {
        std::fprintf(stderr, "  generation is %llu after a burst of %d, expected 1\n",
                     static_cast<unsigned long long>(entry->second.generation), kBurstSize);
    }
}

// --------------------------------------------------------------------------- //
// Coalescing over the bus: when requests for one monitor arrive faster than the GPU can
// serve them, the daemon must render the LAST one.
//
// This exists because the claim was previously asserted on evidence that could not
// carry it. Five requests were fired at one monitor, the generation counter read 1, and
// that was taken as proof. All five named the *same image* -- so a daemon that served
// the first request and discarded the rest produces an identical observation, as does
// one that served the last. The counter distinguishes "how many renders committed",
// which was never the question; the question is "which request's pixels won".
//
// Two independent answers below. The outcome checks decide it from whether a frame
// exists at all, which needs no access to the frame's contents and therefore runs on
// every ICD. The content check decides it from the pixels, which needs a CPU-mappable
// export and therefore runs only on the DMA_BUF + LINEAR path.
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
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorBurstFailLast); }) ==
              "io.wallpaperx.Daemon.Error.RenderFailed",
          "a burst ending in an unrenderable request leaves no frame -- the last request is served, not the first");
    {
        const nlohmann::json json = getStatus(proxy);
        const nlohmann::json* monitor = wallpaperStatus(json, std::to_string(kMonitorBurstFailLast));
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

    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorBurstRenderLast); }).empty(),
          "a burst ending in a renderable request leaves a frame, even after four failures");
    {
        const nlohmann::json json = getStatus(proxy);
        const nlohmann::json* monitor = wallpaperStatus(json, std::to_string(kMonitorBurstRenderLast));
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
    // clearing code does. The check used to live there, passing for a monitor that had
    // never failed.
    //
    // Sequencing the two requests, each behind its own barrier, is what makes the
    // corrupt render actually execute and record an error for the next one to clear.
    setWallpaper(proxy, "static", corruptPath, kMonitorErrorRecovery);
    barrier("the barrier frame arrives after a standalone failing request");
    {
        const nlohmann::json json = getStatus(proxy);
        const nlohmann::json* monitor = wallpaperStatus(json, std::to_string(kMonitorErrorRecovery));
        const bool failed = monitor != nullptr && !monitor->value("has_frame", true) &&
                            !monitor->value("last_error", std::string()).empty();
        // Without this the clearing check below is unfalsifiable again, one step further
        // out: nothing else here proves the error was ever recorded in the first place.
        check(failed, "a standalone failing render records last_error, so there is an error to clear");
    }

    setWallpaper(proxy, "static", imagePath, kMonitorErrorRecovery);
    check(signals.wait(kMonitorErrorRecovery, 1), "the recovering render emits FrameReady");
    {
        const nlohmann::json json = getStatus(proxy);
        const nlohmann::json* monitor = wallpaperStatus(json, std::to_string(kMonitorErrorRecovery));
        const bool cleared = monitor != nullptr && monitor->value("path", std::string()) == imagePath &&
                             monitor->value("has_frame", false) && !monitor->contains("last_error");
        // Pins the outcome, not one particular line. WallpaperManager clears lastError
        // twice -- once in requestWallpaper() when the request is recorded, once in
        // renderPending() when a render commits -- and either alone is sufficient here,
        // so removing just one does not make this fail. Removing both does (verified).
        // That redundancy is worth knowing about: a future edit that deletes one of them
        // will not be caught by this check, and should not be assumed safe because it is.
        check(cleared, "a successful render leaves no last_error behind from an earlier failed one");
    }

    // --- Content: which request's pixels actually reached the exported frame. ---
    for (const std::string& path : burstPaths) {
        setWallpaper(proxy, "static", path, kMonitorBurstContent);
    }
    barrier("the barrier frame arrives after a burst of five different images");

    const Frame frame = getFrameFd(proxy, kMonitorBurstContent);
    {
        const nlohmann::json json = getStatus(proxy);
        const nlohmann::json* monitor = wallpaperStatus(json, std::to_string(kMonitorBurstContent));
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

    if (frame.handleType != 1) {
        std::printf(
            "dbus_service_test: content check skipped -- this device exports OPAQUE_FD, whose bytes are "
            "importable only by another Vulkan/GL context on the same device. The outcome checks above still "
            "ran, and they decide first-wins vs last-wins on their own.\n");
        return;
    }
    if (frame.drmModifier != wallpaperx::testing::kDrmModifierLinear) {
        std::printf(
            "dbus_service_test: content check skipped -- modifier 0x%llx is tiled, so its rows are not "
            "CPU-addressable and a byte comparison would fail a correct export.\n",
            static_cast<unsigned long long>(frame.drmModifier));
        return;
    }
    if (frame.planes.empty()) {
        check(false, "a LINEAR DMA_BUF frame carries a plane layout");
        return;
    }

    // The fd must really be at least memorySize bytes before it is mapped. MappedFrame
    // bounds-checks rows against memorySize -- the value the daemon *claims* -- not
    // against the object's real size, so a daemon publishing a too-large memorySize
    // yields a mapping whose tail has no backing pages and the pixel loop below takes a
    // SIGBUS. frame_export_probe gained exactly this gate; the equivalent check here did
    // not. The nearest existing assertion is a non-fatal check() on a different monitor,
    // which records a FAIL and then falls straight through into this mmap.
    const off_t fdSize = lseek(frame.fd.get(), 0, SEEK_END);
    if (fdSize < 0) {
        // "Could not determine the size" is not "the size is fine". Falling through here
        // is what the gate exists to prevent: mmap does not validate length against the
        // object, so a shorter object maps cleanly and the pixel loop below dies of
        // SIGBUS with no FAIL line at all. Losing the content check on a non-seekable
        // exporter is a stated gap; a crash is not.
        std::printf(
            "dbus_service_test: content check skipped -- lseek on the exported fd failed (%s), so its real size "
            "cannot be confirmed against the published memory_size of %llu before mapping it.\n",
            std::strerror(errno), static_cast<unsigned long long>(frame.memorySize));
        return;
    }
    if (static_cast<uint64_t>(fdSize) < frame.memorySize) {
        check(false, "the exported fd is at least as large as the memory_size the daemon published");
        std::fprintf(stderr, "  fd is %lld bytes but memory_size is %llu -- not mapping it\n",
                     static_cast<long long>(fdSize), static_cast<unsigned long long>(frame.memorySize));
        return;
    }

    const wallpaperx::testing::MappedFrame mapping(frame.fd.get(), frame.memorySize);
    if (!mapping.valid()) {
        std::printf("dbus_service_test: content check skipped -- mmap of the exported fd failed (%s).\n",
                    mapping.error().c_str());
        return;
    }

    // The target is a solid fill of the last burst colour, in B8G8R8A8 order.
    const unsigned char* rgb = kBurstColors[kBurstColorCount - 1];
    const unsigned char expectedB = rgb[2];
    const unsigned char expectedG = rgb[1];
    const unsigned char expectedR = rgb[0];
    constexpr int kTolerance = 6;

    const uint64_t offset = frame.planes[0].get<0>();
    const uint64_t pitch = frame.planes[0].get<1>();
    size_t badPixels = 0;
    unsigned char firstBad[4] = {0, 0, 0, 0};
    for (uint32_t y = 0; y < kTargetHeight; ++y) {
        const unsigned char* row = mapping.row(offset, pitch, y, static_cast<size_t>(kTargetWidth) * 4);
        if (row == nullptr) {
            badPixels += kTargetWidth;
            continue;
        }
        for (uint32_t x = 0; x < kTargetWidth; ++x) {
            const unsigned char* px = row + static_cast<size_t>(x) * 4;
            if (std::abs(int(px[0]) - int(expectedB)) > kTolerance ||
                std::abs(int(px[1]) - int(expectedG)) > kTolerance ||
                std::abs(int(px[2]) - int(expectedR)) > kTolerance) {
                if (badPixels == 0) std::memcpy(firstBad, px, 4);
                ++badPixels;
            }
        }
    }

    check(badPixels == 0,
          "the exported frame's pixels are the LAST image of the burst, read back through its own fd");
    if (badPixels != 0) {
        std::fprintf(stderr,
                     "  %zu of %u pixels differ; expected B,G,R = %d,%d,%d, first mismatch was %d,%d,%d "
                     "(the burst's first colour is B,G,R = %d,%d,%d)\n",
                     badPixels, kTargetWidth * kTargetHeight, expectedB, expectedG, expectedR, firstBad[0],
                     firstBad[1], firstBad[2], kBurstColors[0][2], kBurstColors[0][1], kBurstColors[0][0]);
    } else {
        std::printf("dbus_service_test: exported frame is solid B,G,R = %d,%d,%d -- the burst's last image\n",
                    expectedB, expectedG, expectedR);
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

    wallpaperx::resetValidationWarningCount();

    // Scoped so the manager, and with it the render thread, the Vulkan Device, the
    // offscreen targets and Memory's leaked-allocation check, are all destroyed before
    // the validation counter is read below. Destroying an image still owned by an
    // external queue family, or leaking device memory, only raises validation errors
    // during teardown -- sampling the counter while the manager is still alive silently
    // exempts the whole destruction half of the code under test.
    try {
        const std::string imagePath = writeTestImage();
        const std::string corruptPath = writeCorruptImage();
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
        wallpaperx::WallpaperManager manager(config);

        // Service side: requests the name and serves on a background event loop.
        auto serviceConn = sdbus::createSessionBusConnection(sdbus::ServiceName{DbusService::kBusName});
        DbusService service(*serviceConn, manager);
        serviceConn->enterEventLoopAsync();

        // Client side: its own connection + loop, managed by the proxy.
        auto proxy =
            sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

        // Subscribed before the first request, so no signal can be missed.
        proxy->uponSignal("FrameReady")
            .onInterface(DbusService::kInterface)
            .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

        runHostChecks(*proxy, imagePath, check);
        runRenderChecks(*proxy, signals, imagePath, corruptPath, check);
        runCoalescingChecks(*proxy, signals, imagePath, corruptPath, burstPaths, check);

        // Its own manager and Device, scoped inside the call so both are torn down
        // before the validation warning count is read below.
        runWorkerCoalescingChecks(config, burstPaths, check);
    } catch (const std::exception& e) {
        // Without this the first unhandled sdbus::Error -- which is what a host with no
        // usable Vulkan ICD produces -- would escape main and abort the process, so
        // ctest would report an opaque SIGABRT instead of a named failure, and none of
        // the FAIL lines above would ever be printed.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++failures;
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
    } else if (!wallpaperx::validationLayerActive()) {
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
