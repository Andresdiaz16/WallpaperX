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
#include "daemon/wallpaper_manager.h"

#include "vulkan/device.h"
#include "vulkan/frame_export.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
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
constexpr int32_t kMonitorNeverSet = 9;

// Generous: it bounds a GPU render on a software rasteriser under load, and is only
// ever reached when something is actually broken.
constexpr auto kSignalTimeout = std::chrono::seconds(30);

std::string testAssetDir() { return std::string(WALLPAPERX_TEST_IMAGE_DIR); }

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

// Records every FrameReady the daemon emits, and lets a test block until a specific
// (monitor, generation) arrives.
class FrameReadyLog {
public:
    void record(int32_t monitorId, uint64_t generation) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_events.emplace_back(monitorId, generation);
        }
        m_arrived.notify_all();
    }

    // Waits for FrameReady(monitorId, generation). Returns false on timeout.
    bool wait(int32_t monitorId, uint64_t generation) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_arrived.wait_for(lock, kSignalTimeout, [&] {
            for (const auto& [id, gen] : m_events) {
                if (id == monitorId && gen == generation) return true;
            }
            return false;
        });
    }

    size_t countFor(int32_t monitorId) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t n = 0;
        for (const auto& [id, gen] : m_events) {
            if (id == monitorId) ++n;
        }
        return n;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_arrived;
    std::vector<std::pair<int32_t, uint64_t>> m_events;
};

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

void setWallpaper(sdbus::IProxy& proxy, const std::string& type, const std::string& path, int32_t monitorId) {
    proxy.callMethod("SetWallpaper").onInterface(DbusService::kInterface).withArguments(type, path, monitorId);
}

nlohmann::json getStatus(sdbus::IProxy& proxy) {
    std::string status;
    proxy.callMethod("GetStatus").onInterface(DbusService::kInterface).storeResultsTo(status);
    return nlohmann::json::parse(status, nullptr, /*allow_exceptions=*/false);
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
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorCorrupt); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "a failed render leaves no frame, not the previous wallpaper's");

    // The reply to SetWallpaper was already sent by the time the render failed, so
    // GetStatus is the only place that failure is reported.
    {
        const nlohmann::json json = getStatus(proxy);
        const std::string corruptId = std::to_string(kMonitorCorrupt);
        const bool reported = !json.is_discarded() && json["wallpapers"].contains(corruptId) &&
                              json["wallpapers"][corruptId].contains("last_error") &&
                              !json["wallpapers"][corruptId]["last_error"].get<std::string>().empty();
        check(reported, "GetStatus reports last_error for an asynchronous render failure");
    }

    // A monitor that rendered successfully must not carry a stale error.
    {
        const nlohmann::json json = getStatus(proxy);
        const std::string staticId = std::to_string(kMonitorStatic);
        const std::string videoId = std::to_string(kMonitorVideo);
        const bool reflected = !json.is_discarded() && json["wallpapers"].contains(staticId) &&
                               json["wallpapers"][staticId]["type"] == "static" &&
                               json["wallpapers"][staticId]["path"] == imagePath &&
                               json["wallpapers"][staticId]["has_frame"] == true &&
                               json["wallpapers"][staticId]["generation"] == 2 &&
                               json["wallpapers"][staticId]["width"] == kTargetWidth &&
                               json["wallpapers"][staticId]["height"] == kTargetHeight &&
                               !json["wallpapers"][staticId].contains("last_error") &&
                               json["wallpapers"][videoId]["type"] == "video" &&
                               json["wallpapers"][videoId]["has_frame"] == false;
        check(reflected, "GetStatus reflects per-monitor type, path, size, generation and frame availability");
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
        config.enableValidation = true;
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
