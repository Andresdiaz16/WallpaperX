// SPDX-License-Identifier: GPL-2.0-or-later

// Step 13 Stages E1 and E2: the daemon draws a `video` wallpaper's first picture, and keeps one
// decode session per monitor rather than building one inside every render pass.
//
// Each phase has a manager of its own, because they need the manager in three different shapes --
// with a bus, with a render worker and no bus, and with neither -- and the Device pins itself to
// the thread that created it.
//
//   over the bus        a video is drawn, served and reported (S13.114, S13.115)
//   no bus, no worker   an undecodable video fails like an undecodable image (S13.116)
//   a worker, no bus    a resize decodes nothing, a new wallpaper decodes before the rebuild, and
//                       an overtaken pass leaves the published frame reachable (S13.117, S13.118)
//   a worker, no bus    the crash record covers the decode and a session's teardown (S13.119)
//   no bus, no worker   which passes reuse, reopen, evict and drop a session (S13.123-S13.126,
//                       S13.132)
//   a worker, no bus    what a failed pass does with its session, and which teardowns are under a
//                       record (S13.127-S13.130)
//   a worker, no bus    shutdown frees sessions with no record, and the next start draws (S13.131)
//
// WHICH FILES ARE OPEN is read from ffmpeg's own I/O entry points, hooked below: a session holds
// its file open for exactly its lifetime, so the set of open wallpaper files is the set of
// resident sessions. Status cannot answer that -- a monitor reports the same frame whether its
// session was kept or rebuilt.
//
// THE VALIDATION ORACLE IS NARROWED HERE, AND ONLY HERE. ffmpeg's Vulkan decoder binds one
// reference slot per DPB picture where this driver's profile wants a single layered image, so
// every hardware decode raises VUID-VkVideoBeginCodingInfoKHR-flags-07244 -- reproducible with
// the stock `ffmpeg -hwaccel vulkan` command line, so a defect in a dependency. This process
// excuses that one message id through the validation layer's own filter and says so on every run.
// It is a separate binary from dbus_service_test for exactly that reason: a whole process is the
// smallest thing the filter can apply to, and dbus_service_test must keep grading every message.
//
// An excuse that stops working fails LOUDLY rather than quietly: if the filter is not applied the
// count is non-zero and each phase's grade goes red. There is no path where a broken excuse turns
// into a green run.
//
// WHY A GATE RATHER THAN A SLEEP. Several of these properties are about a span inside one render
// pass -- what has been decoded by the time the images are rebuilt, what is on disk while a
// decoder is torn down. A check before or after the pass cannot see that span at all, and a sleep
// would be an assertion on the scheduler. The daemon's own calls are hooked in this executable
// (tests/client/call_gate.h) and the render thread is held inside them.

#include "daemon/crash_record.h"
#include "daemon/dbus_service.h"
#include "daemon/render_worker.h"
#include "daemon/wallpaper_manager.h"

#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

#include "support/cover_expectation.h"
#include "support/decoder_message_id.h"
#include "support/exported_frame_map.h"
#include "support/test_image.h"
#include "support/validation_grade.h"
#include "tests/client/call_gate.h"
#include "tests/client/daemon_client.h"
#include "tests/client/scratch_dir.h"
#include "tests/manager_output.h"
#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>

using wallpaperx::DbusService;
using wallpaperx::RenderWorker;
using wallpaperx::WallpaperManager;
using wallpaperx::testing::CallGate;
using wallpaperx::testing::check;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::notVerified;

// The two ffmpeg entry points this file hooks. The header is needed, not just the declarations:
// telling the decoder's own codec context apart from the ones ffmpeg opens while probing the file
// means reading a field of it, and that distinction is load-bearing -- see isDecodersContext.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avio.h>
}

namespace {

// Where the render thread is held or failed, inside the daemon's own work. Unarmed, each costs the
// real call one atomic load.
// The two framebuffer gates are armed with an extent ("96x48") and fire only on a framebuffer of
// that size. Unfiltered they fire on the first framebuffer of the pass, which for a pass that opens a
// video is the frame converter's, built during the first decode -- not the monitor's images.
CallGate g_framebufferGate;      // the framebuffer that finishes a rebuilt image of the armed size
CallGate g_framebufferFailGate;  // the same call, failed; checked after the hold, so both can apply
CallGate g_decodeGate;           // a packet handed to the decoder: the span the picture is decoded in
CallGate g_freeCodecGate;        // the decoder's codec context being freed: a session's teardown
CallGate g_submitFailGate;       // a queue submission, failed
CallGate g_closeFileGate;        // a wallpaper file being closed, by path: the end of a session

// Packets sent to the wallpaper's decoder since the last reset. What makes "the decode happened
// before the images were rebuilt" an observation rather than an inference.
std::atomic<int> g_decodeCalls{0};
std::atomic<int> g_codecFrees{0};

// The wallpaper files ffmpeg has open, keyed by the I/O context it reads each through, and how
// often each path was opened and closed since the last reset.
class FileLedger {
public:
    void opened(const AVIOContext* io, const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_live[io] = path;
        ++m_opens[path];
    }

    // The path `io` was opened on, or empty for a context this ledger never saw.
    std::string closing(const AVIOContext* io) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_live.find(io);
        if (it == m_live.end()) return {};
        std::string path = it->second;
        m_live.erase(it);
        ++m_closes[path];
        return path;
    }

    void resetCounts() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_opens.clear();
        m_closes.clear();
    }

    int opens(const std::string& path) const { return count(m_opens, path); }
    int closes(const std::string& path) const { return count(m_closes, path); }
    int totalOpens() const { return total(m_opens); }
    int totalCloses() const { return total(m_closes); }

    std::multiset<std::string> live() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::multiset<std::string> paths;
        for (const auto& [io, path] : m_live) paths.insert(path);
        return paths;
    }

private:
    int count(const std::map<std::string, int>& from, const std::string& path) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = from.find(path);
        return it == from.end() ? 0 : it->second;
    }
    int total(const std::map<std::string, int>& from) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        int sum = 0;
        for (const auto& [path, n] : from) sum += n;
        return sum;
    }

    mutable std::mutex m_mutex;
    std::map<const AVIOContext*, std::string> m_live;
    std::map<std::string, int> m_opens;
    std::map<std::string, int> m_closes;
};

FileLedger g_files;

// VideoDecoder's own context, which it marks by setting `opaque`; avformat_find_stream_info opens
// contexts of its own and decodes through the same entry points to work out what is in the file.
//
// Counting those too is not a detail. They are sent while the VideoPipeline is being CONSTRUCTED,
// which happens before the images are rebuilt whichever order the decode is in -- so a counter
// that included them was non-zero at the rebuild either way, and the ordering check below passed
// with the production code deliberately reordered. Measured, not reasoned: that is what the
// control for S13.117 first produced.
bool isDecodersContext(const AVCodecContext* context) { return context != nullptr && context->opaque != nullptr; }

}  // namespace

extern "C" VkResult vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
                                        const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer) {
    using Fn = VkResult (*)(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "vkCreateFramebuffer"));
    if (real == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    char extent[32] = "";
    if (info != nullptr) std::snprintf(extent, sizeof(extent), "%ux%u", info->width, info->height);
    if (g_framebufferGate.enter(extent)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (g_framebufferFailGate.enter(extent)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return real(device, info, allocator, framebuffer);
}

static std::string extentOf(uint32_t width, uint32_t height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

// The daemon's own submissions only: ffmpeg's decoder reaches the driver through its own table.
extern "C" VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
    using Fn = VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "vkQueueSubmit"));
    if (real == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (g_submitFailGate.enter()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return real(queue, count, infos, fence);
}

// VideoDecoder opens its file through avio_open2 and closes it through avio_closep, once each per
// session, and the close is the last step of the teardown.
extern "C" int avio_open2(AVIOContext** s, const char* url, int flags, const AVIOInterruptCB* callback,
                          AVDictionary** options) {
    using Fn = int (*)(AVIOContext**, const char*, int, const AVIOInterruptCB*, AVDictionary**);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avio_open2"));
    if (real == nullptr) return AVERROR(ENOSYS);
    const int result = real(s, url, flags, callback, options);
    if (result >= 0 && s != nullptr && *s != nullptr && url != nullptr) g_files.opened(*s, url);
    return result;
}

extern "C" int avio_closep(AVIOContext** s) {
    using Fn = int (*)(AVIOContext**);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avio_closep"));
    if (real == nullptr) return AVERROR(ENOSYS);
    if (s != nullptr && *s != nullptr) {
        const std::string path = g_files.closing(*s);
        g_closeFileGate.enter(path.c_str());
    }
    return real(s);
}

extern "C" int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    using Fn = int (*)(AVCodecContext*, const AVPacket*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avcodec_send_packet"));
    if (real == nullptr) return -1;
    if (isDecodersContext(context)) {
        g_decodeCalls.fetch_add(1, std::memory_order_relaxed);
        g_decodeGate.enter();
    }
    return real(context, packet);
}

extern "C" void avcodec_free_context(AVCodecContext** context) {
    using Fn = void (*)(AVCodecContext**);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avcodec_free_context"));
    if (real == nullptr) return;
    // The same filter, for the same reason: ffmpeg frees its probing contexts while the pipeline is
    // being built, so a gate that fired on the first free of any context would hold the thread
    // during construction and never see a session's teardown at all.
    if (context != nullptr && isDecodersContext(*context)) {
        g_codecFrees.fetch_add(1, std::memory_order_relaxed);
        g_freeCodecGate.enter();
    }
    real(context);
}

namespace {

using Clock = std::chrono::steady_clock;
using wallpaperx::testing::getFrameFd;
using wallpaperx::testing::getStatusOrExplain;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::StatusReply;
using wallpaperx::testing::validationWaived;
using wallpaperx::testing::wallpaperStatus;

// Deliberately not square, and deliberately a different aspect from the fixture's: a monitor whose
// shape matches the clip's is covered without cropping anything, and every wrong crop would then
// produce the right picture.
constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 24;

// What a screen reports for the resize phases. Different in both axes, so a rebuild is forced.
constexpr uint32_t kResizedWidth = 96;
constexpr uint32_t kResizedHeight = 48;
constexpr uint32_t kResizedAgainWidth = 80;
constexpr uint32_t kResizedAgainHeight = 40;

// The fixture: four horizontal bands over 320x240. Covering a 64x24 monitor keeps the middle half
// of the rows, so a correct draw shows green then yellow and none of the red or blue.
constexpr uint32_t kClipWidth = 320;
constexpr uint32_t kClipHeight = 240;

constexpr auto kGateLimit = std::chrono::seconds(30);

struct Band {
    uint32_t firstRow;
    uint32_t lastRow;  // exclusive
    int red;
    int green;
    int blue;
    const char* name;
};

// The bands the fixture is generated with, in backend/CMakeLists.txt. Stated once here and read by
// the pixel check, so a fixture recipe that changed without this changing is a failed check rather
// than a check that quietly grades a different picture.
constexpr Band kBands[] = {
    {0, 60, 255, 0, 0, "red"},
    {60, 120, 0, 255, 0, "green"},
    {120, 180, 255, 255, 0, "yellow"},
    {180, 240, 0, 0, 255, "blue"},
};

const Band* bandAtRow(double sourceRow) {
    for (const Band& band : kBands) {
        if (sourceRow >= band.firstRow && sourceRow < band.lastRow) return &band;
    }
    return nullptr;
}

std::string testAssetDir() { return std::string(WALLPAPERX_TEST_IMAGE_DIR); }
std::string clipPath() { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_bands.mp4"; }

// A file that exists, is a regular file and is readable, and is not a video. SetWallpaper applies
// the readable-regular-file check to every known type, so a path that was never created is
// rejected on the reply and never reaches the failed-render state S13.116 is about.
std::string writeUndecodable(const std::string& name) {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "this is not a video, and never was";
    return path;
}

std::optional<wallpaperx::MonitorStatus> monitorStatus(const WallpaperManager& manager, int monitorId) {
    const WallpaperManager::Snapshot snap = manager.snapshot();
    const auto it = snap.monitors.find(monitorId);
    if (it == snap.monitors.end()) return std::nullopt;
    return it->second;
}

// A phase's own Device graded for validation messages, in a destructor so an exception out of the
// phase cannot skip it, and with the counter snapshotted as the manager's scope unwinds -- the
// layer's teardown complaints come from ~WallpaperManager, and reading a live accessor before the
// manager is gone exempts the whole destruction half.
class PhaseGrade {
public:
    explicit PhaseGrade(std::string label) : m_label(std::move(label)) {}
    PhaseGrade(const PhaseGrade&) = delete;
    PhaseGrade& operator=(const PhaseGrade&) = delete;

    void capture(const WallpaperManager& manager) {
        m_counter = manager.validationCounter();
        m_layerActive = manager.validationLayerActive();
    }

    // For a phase that legitimately never built a Device and has already said so.
    void expectNoDevice() { m_deviceOptional = true; }

    ~PhaseGrade() {
        int failures = 0;
        int ungraded = 0;
        {
            wallpaperx::testing::ValidationGrade grade("video_wallpaper_test (" + m_label + ")", failures);
            if (validationWaived() || m_deviceOptional) grade.expectNoDevice();
            grade.reportUngradedTo(ungraded);
            grade.adopt(m_counter, m_layerActive);
        }
        if (ungraded > 0) {
            std::fprintf(stderr,
                         "video_wallpaper_test: %s's validation grade could not be made -- "
                         "VK_LAYER_KHRONOS_validation is not loaded. The phase itself is still judged.\n",
                         m_label.c_str());
        }
        check(failures == 0, ("video_wallpaper_test: " + m_label + "'s own device passed its validation grade").c_str());
    }

private:
    std::string m_label;
    std::shared_ptr<const std::atomic<int>> m_counter;
    bool m_layerActive = false;
    bool m_deviceOptional = false;
};

struct CaptureGradeOnExit {
    PhaseGrade& grade;
    const WallpaperManager& manager;
    ~CaptureGradeOnExit() { grade.capture(manager); }
};

// Points the validation layer at a settings file that excuses ffmpeg's one known message by id.
//
// Constructed before any Device: the layer reads its settings at vkCreateInstance, so an override
// set afterwards changes nothing for an instance already made.
class DecoderMessageExcuse {
public:
    explicit DecoderMessageExcuse(const std::filesystem::path& dir) {
        m_file = dir / "vk_layer_settings.txt";
        std::ofstream out(m_file);
        out << "khronos_validation.message_id_filter = " << wallpaperx::testing::kExcusedDecoderMessageId << "\n";
        out.close();
        if (!out) return;
        m_applied = ::setenv("VK_LAYER_SETTINGS_PATH", m_file.c_str(), 1) == 0;
    }

    bool applied() const { return m_applied; }
    const std::filesystem::path& file() const { return m_file; }

private:
    std::filesystem::path m_file;
    bool m_applied = false;
};

// --------------------------------------------------------------------------- //
// Over the bus: a video wallpaper is drawn, served and reported
// --------------------------------------------------------------------------- //

constexpr int32_t kMonitorClip = 0;
constexpr int32_t kMonitorStill = 1;

// What a hand-over's bytes said, or why they could not be read.
struct PixelVerdict {
    bool answerable = false;
    std::string reason;
    int compared = 0;
    int wrong = 0;
    std::string firstWrong;
};

// What the pixel check needs from a hand-over, whether it came over the bus or straight from the
// manager.
struct FrameLayout {
    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    int handleType = 0;  // the wire's numbering: 1 is a dma_buf
    uint64_t modifier = 0;
    bool hasPlane = false;
    uint64_t planeOffset = 0;
    uint64_t rowPitch = 0;
};

FrameLayout layoutOf(const wallpaperx::testing::Frame& frame) {
    FrameLayout layout{frame.fd.get(), frame.width, frame.height, frame.memorySize, frame.handleType,
                       frame.drmModifier};
    if (!frame.planes.empty()) {
        layout.hasPlane = true;
        layout.planeOffset = frame.planes[0].get<0>();
        layout.rowPitch = frame.planes[0].get<1>();
    }
    return layout;
}

FrameLayout layoutOf(const wallpaperx::ExportedFrame& frame) {
    FrameLayout layout{frame.fd,
                       frame.width,
                       frame.height,
                       frame.memorySize,
                       frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf ? 1 : 0,
                       frame.modifier};
    if (!frame.planes.empty()) {
        layout.hasPlane = true;
        layout.planeOffset = frame.planes[0].offset;
        layout.rowPitch = frame.planes[0].rowPitch;
    }
    return layout;
}

PixelVerdict gradeCoverCrop(const FrameLayout& frame) {
    PixelVerdict verdict;
    if (frame.handleType != 1 || frame.modifier != wallpaperx::testing::kDrmModifierLinear || !frame.hasPlane) {
        verdict.reason = "this driver's export is not a LINEAR dma_buf (handle type " +
                         std::to_string(frame.handleType) + ", modifier 0x" +
                         [&] {
                             char buf[32];
                             std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(frame.modifier));
                             return std::string(buf);
                         }() +
                         "), so no client can address its pixels";
        return verdict;
    }
    const wallpaperx::testing::MappedFrame mapped(frame.fd, frame.memorySize);
    if (!mapped.valid()) {
        verdict.reason = "the exported fd could not be mapped: " + mapped.error();
        return verdict;
    }
    const uint64_t planeOffset = frame.planeOffset;
    const uint64_t rowPitch = frame.rowPitch;
    const wallpaperx::testing::CropRect crop =
        wallpaperx::testing::expectedCover(kClipWidth, kClipHeight, frame.width, frame.height);

    for (uint32_t row = 0; row < frame.height; ++row) {
        // Where this monitor row samples the clip, from the cover rule in real arithmetic --
        // never from the renderer's own integer form, which would agree with itself however
        // both were wrong.
        const double sourceRow = crop.y0 + (row + 0.5) * crop.height / frame.height;
        const Band* band = bandAtRow(sourceRow);
        if (band == nullptr) continue;
        // A band boundary lands between two rows of chroma, so the two rows either side of one
        // are a blend of both colours and say nothing. Eight source rows is generous against a
        // 60-row band: what is skipped is the seam, not the band.
        const double toBoundary =
            std::min(sourceRow - static_cast<double>(band->firstRow), static_cast<double>(band->lastRow) - sourceRow);
        if (toBoundary < 8.0) continue;

        const unsigned char* pixels = mapped.row(planeOffset, rowPitch, row, static_cast<size_t>(frame.width) * 4);
        if (pixels == nullptr) {
            verdict.reason = "the frame's own published layout puts row " + std::to_string(row) +
                             " outside the memory it exported";
            return verdict;
        }
        const size_t column = frame.width / 2;
        const int blue = pixels[column * 4 + 0];
        const int green = pixels[column * 4 + 1];
        const int red = pixels[column * 4 + 2];
        ++verdict.compared;
        // 60 codes. The bands differ from each other by 255 in at least one channel, so a wrong band
        // cannot pass at any tolerance below 128; what this has to absorb is the 4:2:0 limited-range
        // round trip, which moves a primary by ~40 before the renderer's own conversion.
        const bool matches = std::abs(red - band->red) <= 60 && std::abs(green - band->green) <= 60 &&
                             std::abs(blue - band->blue) <= 60;
        if (!matches) {
            ++verdict.wrong;
            if (verdict.firstWrong.empty()) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "monitor row %u samples clip row %.1f, which is %s (%d,%d,%d), but reads (%d,%d,%d)",
                              row, sourceRow, band->name, band->red, band->green, band->blue, red, green, blue);
                verdict.firstWrong = buf;
            }
        }
    }
    verdict.answerable = true;
    return verdict;
}

void runBusChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& clip, const std::string& still) {
    // The still first, so it is already showing when the video is set: what "a still wallpaper on
    // another monitor is unaffected" is measured against.
    setWallpaper(proxy, "static", still, kMonitorStill);
    if (!signals.wait(kMonitorStill, 1)) {
        notVerified("S13.114 and S13.115",
                    "the still wallpaper this phase compares against never rendered on this host");
        return;
    }
    const StatusReply beforeClip = getStatusOrExplain(proxy);
    const nlohmann::json* stillBefore = wallpaperStatus(beforeClip, std::to_string(kMonitorStill));
    const uint64_t stillGeneration = stillBefore != nullptr ? stillBefore->value("generation", uint64_t{0}) : 0;

    // GOAL: S13.114 -- a `video` wallpaper is drawn. SetWallpaper("video", ...) commits a frame,
    //       GetFrameFd serves it, and GetStatus reports the monitor as renderable with a frame and
    //       no error. False for a build that accepts a video and then never draws one, which is
    //       exactly what this daemon did until Stage E1 and what every other type it knows still
    //       does.
    // WHY-THIS-SHAPE: the FrameReady signal is what says the frame exists, not a sleep and not a
    //       poll: SetWallpaper returns before the render is attempted, so any check made on the
    //       reply alone is equally true of a daemon that draws nothing. `unsupported` is asserted
    //       to be false as well as the frame being served, because those are two published
    //       surfaces derived from one predicate and a client branches on either.
    // CONTROL: drawableType() in backend/daemon/wallpaper_manager.h with its `video` arm removed, which
    //       is the daemon one commit before this stage -> "FAIL: a video wallpaper commits a frame" and
    //       five more, including the undecodable-video comparison and the decode gate; the phases that
    //       then had nothing to set up added no FAIL line. Observed 2026-09-15, and again 2026-09-16
    //       with the session phases present. Reverted.
    const bool drawn = [&] {
        setWallpaper(proxy, "video", clip, kMonitorClip);
        return signals.wait(kMonitorClip, 1);
    }();
    check(drawn, "a video wallpaper commits a frame");
    if (!drawn) {
        const StatusReply failed = getStatusOrExplain(proxy);
        if (const nlohmann::json* entry = wallpaperStatus(failed, std::to_string(kMonitorClip))) {
            std::fprintf(stderr, "  the video monitor reports: unsupported=%s last_error=%s\n",
                         entry->value("unsupported", false) ? "true" : "false",
                         entry->value("last_error", std::string("(none)")).c_str());
        }
        return;
    }

    const StatusReply after = getStatusOrExplain(proxy);
    const nlohmann::json* clipEntry = wallpaperStatus(after, std::to_string(kMonitorClip));
    check(clipEntry != nullptr, "GetStatus carries the video monitor");
    if (clipEntry != nullptr) {
        check(!clipEntry->value("unsupported", true), "GetStatus does not call a video wallpaper unsupported");
        check(clipEntry->value("has_frame", false), "GetStatus reports the video monitor as having a frame");
        check(!clipEntry->contains("last_error"), "a video wallpaper that drew leaves no last_error");
        check(clipEntry->value("type", std::string()) == "video", "and the recorded type is still the one asked for");
    }

    const wallpaperx::testing::Frame frame = getFrameFd(proxy, kMonitorClip);
    check(frame.fd.get() >= 0, "GetFrameFd serves the video monitor's frame");
    check(frame.generation >= 1, "the served frame names a generation");

    // GOAL: S13.115's first half -- the picture is drawn at the monitor's target size, not at the
    //       clip's. False for a daemon that sizes the frame from the decoded picture, which would
    //       hand the plugin a 320x240 frame for a 64x24 screen.
    // WHY-THIS-SHAPE: the frame's own published width and height, which is what an importer builds
    //       its texture from; the target size is deliberately a different aspect AND both axes
    //       differ from the clip's, so a swap or a passthrough cannot satisfy it.
    // CONTROL: no mutation of the crop reddens this one -- a stretched picture is still served at the
    //       target size -- so it is controlled by the same removal of drawableType()'s `video` arm as the
    //       check above, which leaves no frame to measure at all. What it forbids is a frame sized from
    //       the decoded picture, and nothing in this build produces one to mutate toward.
    const bool sized = frame.width == kTargetWidth && frame.height == kTargetHeight;
    check(sized, "the video frame is served at the monitor's target size, not the clip's");
    if (!sized) {
        std::fprintf(stderr, "  served %ux%u, target is %ux%u, the clip is %ux%u\n", frame.width, frame.height,
                     kTargetWidth, kTargetHeight, kClipWidth, kClipHeight);
    }

    // GOAL: S13.115's second half -- the picture is cover-cropped, as a still image is: scaled
    //       uniformly to cover the monitor, centred, the overflow cropped. False for a stretched,
    //       letterboxed, corner-anchored or mirrored draw.
    // WHY-THIS-SHAPE: the fixture is four bands, and a 64x24 monitor covered by a 320x240 clip
    //       shows only the middle two, so every wrong rule puts a band on screen that the right
    //       one crops away -- a stretch shows all four, a corner anchor shows red, a vertical
    //       mirror swaps green and yellow. The expected band per row comes from
    //       support/cover_expectation.h, the cover rule in real arithmetic, never from the
    //       renderer's own crop. Sampling one column is enough because the bands span the width.
    // CONTROL: renderer/renderers/cover_crop.h's coverCrop() made to return the whole source rectangle,
    //       which is the stretch commit 602281a replaced -> on the Lavapipe entry "FAIL: the video
    //       picture is cover-cropped to the monitor, like a still image", printing "8 of 16 sampled rows
    //       show the wrong band; first: monitor row 2 samples clip row 72.5, which is green (0,255,0),
    //       but reads (255,24,0)" -- the red band that a correct crop removes. On the NVIDIA entry the
    //       same mutation printed the abstention below instead of passing. Observed 2026-09-15. Reverted.
    // COVERAGE-ASSERT: the number of rows actually compared is asserted, so a crop that moved the
    //       sampled rows onto a band boundary (where the check abstains) cannot quietly empty it.
    const PixelVerdict pixels = gradeCoverCrop(layoutOf(frame));
    if (!pixels.answerable) {
        notVerified("S13.115's pixel half", pixels.reason);
    } else {
        check(pixels.compared >= 16,
              "enough monitor rows fell inside a band to grade the crop (the pixel check is not empty)");
        check(pixels.wrong == 0, "the video picture is cover-cropped to the monitor, like a still image");
        if (pixels.wrong > 0) {
            std::fprintf(stderr, "  %d of %d sampled rows show the wrong band; first: %s\n", pixels.wrong,
                         pixels.compared, pixels.firstWrong.c_str());
        } else {
            std::printf("video_wallpaper_test: %d monitor rows checked against the cover crop, all correct\n",
                        pixels.compared);
        }
    }

    // GOAL: S13.114's last clause -- drawing a video on one monitor leaves a still wallpaper on
    //       another untouched: same generation, still served. False if the new drawer disturbs
    //       another monitor's images or its published frame.
    // WHY-THIS-SHAPE: the generation is compared against the value read before the video was set,
    //       and the frame is re-fetched -- not just read from status. A monitor whose images were
    //       released still reports its old generation, so only asking for the frame back
    //       distinguishes "unaffected" from "still says it has one".
    const StatusReply stillAfter = getStatusOrExplain(proxy);
    const nlohmann::json* stillEntry = wallpaperStatus(stillAfter, std::to_string(kMonitorStill));
    const bool untouched = stillEntry != nullptr && stillEntry->value("has_frame", false) &&
                           stillEntry->value("generation", uint64_t{0}) == stillGeneration &&
                           !stillEntry->contains("last_error");
    check(untouched, "a still wallpaper on another monitor is unchanged by the video render");
    const wallpaperx::testing::Frame stillFrame = getFrameFd(proxy, kMonitorStill);
    check(stillFrame.fd.get() >= 0 && stillFrame.generation == stillGeneration,
          "and its frame is still served, at the generation it had before");
    check(signals.countFor(kMonitorStill) == 1, "and nothing re-rendered it");
}

// --------------------------------------------------------------------------- //
// No bus, no worker: an undecodable video fails like an undecodable image
// --------------------------------------------------------------------------- //

constexpr int kMonitorBadClip = 0;
constexpr int kMonitorBadImage = 1;

struct FailureShape {
    bool recorded = false;
    std::string lastError;
    bool daemonFault = false;
    bool hasFrame = true;
    wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
};

FailureShape failOnce(WallpaperManager& manager, int monitorId, const char* type, const std::string& path) {
    FailureShape shape;
    manager.requestWallpaper(type, path, monitorId);
    manager.renderPending(monitorId);
    const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, monitorId);
    if (!status) return shape;
    shape.recorded = true;
    shape.lastError = status->lastError;
    shape.daemonFault = status->lastErrorIsDaemonFault;
    shape.hasFrame = status->hasFrame;
    shape.reason = manager.exportCurrentFrame(monitorId).reason;
    return shape;
}

void runUndecodableChecks(const WallpaperManager::Config& config, const std::string& junk) {
    const char* kGoal = "S13.116 (an undecodable video fails like an undecodable image)";
    PhaseGrade grade("the undecodable phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    // GOAL: S13.116 -- a file that is not decodable as a video fails exactly as an undecodable
    //       image does: last_error is set, no frame, GetFrameFd answers RenderFailed, and the GPU
    //       is NOT marked failed. False for a daemon that reports a bad file as its own fault --
    //       which would make /health fail and a watchdog restart a daemon that is working -- and
    //       false for one that keeps a stale frame beside the error.
    // WHY-THIS-SHAPE: the two types are driven through the same call and their answers compared
    //       field by field, rather than each being asserted against a literal. The property is
    //       "the same as a still", so stating it as a comparison is what makes a video that fails
    //       in some *other* correct-looking way red. The GPU health is read from the manager's own
    //       snapshot: GetStatus publishes the device's name and tier but not its health, so over
    //       the bus this clause is not observable at all.
    // CONTROL: renderer/video/video_decoder.cpp's throwAvFailure() raising GpuFailure instead of
    //       WallpaperContentError, so the video fails as a fault while the still image still fails as
    //       content -> "FAIL: and is the file's fault, not the daemon's, exactly as an undecodable image
    //       is" and "FAIL: a video the daemon cannot decode does not mark the GPU failed", printing
    //       "gpu health: fail -- VideoDecoder: cannot open ...: Invalid data found when processing
    //       input". Observed 2026-09-15. Reverted.
    const FailureShape clip = failOnce(manager, kMonitorBadClip, "video", junk);
    const FailureShape image = failOnce(manager, kMonitorBadImage, "static", junk);
    if (!clip.recorded || !image.recorded) {
        notVerified(kGoal, "neither request was recorded, so there is nothing to compare");
        return;
    }
    if (image.lastError.empty()) {
        // The comparison is against the still image's behaviour, so a host where the still does
        // not fail either has nothing to compare with -- and that is a harness fact, not a video
        // defect.
        grade.expectNoDevice();
        notVerified(kGoal, "an undecodable image did not fail on this host, so the shape being compared to is absent");
        return;
    }

    check(!clip.lastError.empty(), "an undecodable video records a last_error");
    check(!clip.hasFrame, "and leaves no frame");
    check(clip.reason == wallpaperx::NoFrameReason::RenderFailed, "and a frame request answers RenderFailed");
    check(clip.reason == image.reason, "the same no-frame reason an undecodable image gives");
    check(clip.daemonFault == image.daemonFault && !clip.daemonFault,
          "and is the file's fault, not the daemon's, exactly as an undecodable image is");

    const WallpaperManager::Snapshot snap = manager.snapshot();
    check(snap.health.gpu.status != wallpaperx::HealthStatus::Fail,
          "a video the daemon cannot decode does not mark the GPU failed");
    if (snap.health.gpu.status == wallpaperx::HealthStatus::Fail) {
        std::fprintf(stderr, "  gpu health: fail -- %s\n", snap.health.gpu.detail.c_str());
    }
    std::printf("video_wallpaper_test: an undecodable video reports -- %s\n", clip.lastError.c_str());
}

// --------------------------------------------------------------------------- //
// A worker, no bus: what a resize costs, and what an overtaken pass leaves behind
// --------------------------------------------------------------------------- //

constexpr int kMonitorOrder = 0;
constexpr int kMonitorRace = 1;

struct Exported {
    bool served = false;
    uint32_t width = 0;
    uint32_t height = 0;
    wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
    std::string error;
};

std::string describe(const Exported& e) {
    if (e.served) return std::to_string(e.width) + "x" + std::to_string(e.height);
    if (!e.error.empty()) return "an exception: " + e.error;
    return "no frame, reason " + std::to_string(static_cast<int>(e.reason));
}

void runResizeChecks(const WallpaperManager::Config& config, const std::string& clip) {
    const char* kGoals = "S13.117 (a resize decodes nothing; a new wallpaper decodes before the images are "
                         "rebuilt), S13.118 (an overtaken video pass leaves the published frame reachable) and "
                         "S13.123's retry clause";
    PhaseGrade grade("the resize phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    // Before the worker is destroyed, on every exit: a gate left armed or holding would park it.
    struct Unblock {
        RenderWorker& worker;
        ~Unblock() {
            g_framebufferGate.release();
            g_decodeGate.release();
            worker.stop();
        }
    } unblock{worker};

    const auto firstFrame = [&](int monitorId, const std::string& name) {
        wallpaperx::testing::nameScreen(manager, monitorId, name);
        manager.requestWallpaper("video", clip, monitorId);
        worker.post(monitorId);
        return signals.wait(monitorId, 1);
    };
    const auto resize = [&](const std::string& name, uint32_t width, uint32_t height) {
        std::vector<int> redraw;
        manager.setOutputSize(name, width, height, redraw);
        for (const int id : redraw) worker.post(id);
    };
    const auto exportOnWorker = [&worker, &manager](int monitorId) {
        auto answer = std::make_shared<std::promise<Exported>>();
        std::future<Exported> future = answer->get_future();
        worker.postTask([answer, &manager, monitorId](RenderWorker::TaskState taskState) {
            Exported out;
            if (taskState != RenderWorker::TaskState::Run) {
                out.error = "the worker did not run the export";
            } else {
                try {
                    wallpaperx::FrameExport e = manager.exportCurrentFrame(monitorId, "");
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

    if (!firstFrame(kMonitorOrder, "DP-ORDER")) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame to resize");
        return;
    }

    // GOAL: S13.117's first half -- a redraw at a new size of a monitor whose session is resident
    //       opens no file and decodes nothing, before the rebuild or after it, and still commits.
    //       False for a daemon that builds a decode session inside every pass.
    // WHY-THIS-SHAPE: read at the rebuild AND after the commit, because "nothing before the rebuild"
    //       alone is also true of a pass that rebuilds first and decodes after. The render thread is
    //       held inside the creation of a framebuffer of the new size, which only the monitor's
    //       rebuilt images have, so the gate firing proves this pass really rebuilt.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, dropStaleVideoSessionLocked()'s keep-it
    //       `return` disabled (`&& false`), so every pass reopens its file -> "FAIL: a resize of a
    //       monitor with a resident session opens and decodes nothing", printing "1 open(s), 1
    //       packet(s) by the rebuild; 1 open(s), 1 packet(s) by the commit" (11 packets on Lavapipe).
    //       Observed 2026-09-16 on both driver entries. Reverted.
    // COVERAGE-ASSERT: the gate is asserted to have fired, so "nothing was decoded" is never
    //       concluded from a rebuild that never happened.
    g_decodeCalls.store(0);
    g_files.resetCounts();
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedWidth, kResizedHeight));
    resize("DP-ORDER", kResizedWidth, kResizedHeight);
    const bool rebuilding = g_framebufferGate.waitFired(kGateLimit);
    check(rebuilding, "a new size makes the render thread rebuild the video monitor's images");
    const int opensAtRebuild = g_files.totalOpens();
    const int decodesAtRebuild = g_decodeCalls.load();
    g_framebufferGate.release();
    const bool resizedCommit = signals.wait(kMonitorOrder, 2);
    check(resizedCommit, "and the redraw commits once the rebuild is let go");
    if (rebuilding && resizedCommit) {
        const bool nothing = opensAtRebuild == 0 && decodesAtRebuild == 0 && g_files.totalOpens() == 0 &&
                             g_decodeCalls.load() == 0;
        check(nothing, "a resize of a monitor with a resident session opens and decodes nothing");
        if (!nothing) {
            std::fprintf(stderr, "  %d open(s), %d packet(s) by the rebuild; %d open(s), %d packet(s) by the commit\n",
                         opensAtRebuild, decodesAtRebuild, g_files.totalOpens(), g_decodeCalls.load());
        }
    }

    // GOAL: S13.117's second half -- a pass that must open a session AND rebuild the images (a new
    //       wallpaper arriving with a new size) decodes before it rebuilds, so the frameless window
    //       costs a blit and not a decode. False for the order a single decode-and-draw call forces.
    // WHY-THIS-SHAPE: the new wallpaper and the new size are recorded before the one post, so one
    //       pass carries both. Two filters keep it honest: the gate fires only on a framebuffer of the
    //       new size, since the frame converter builds its own framebuffer during the first decode and
    //       an unfiltered gate stops there instead; and the packet counter takes only the decoder's own
    //       context, since ffmpeg's probing packets precede the rebuild in either order.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, `clip = &videoSessionLocked(...)` moved below
    //       `if (ensureTargets(monitorId, size)) rebuilt = true;` -> "FAIL: a new video wallpaper is
    //       decoded before the monitor's images are rebuilt", printing "no packet had been decoded when
    //       the image rebuild was reached". Observed 2026-09-16 on both driver entries. Reverted.
    // COVERAGE-ASSERT: the gate is asserted to have fired, as above.
    g_decodeCalls.store(0);
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedAgainWidth, kResizedAgainHeight));
    manager.requestWallpaper("video", clip, kMonitorOrder);
    resize("DP-ORDER", kResizedAgainWidth, kResizedAgainHeight);
    const bool rebuildingNew = g_framebufferGate.waitFired(kGateLimit);
    check(rebuildingNew, "a new wallpaper at a new size makes the render thread rebuild the images");
    if (rebuildingNew) {
        const int decodedFirst = g_decodeCalls.load();
        check(decodedFirst > 0, "a new video wallpaper is decoded before the monitor's images are rebuilt");
        if (decodedFirst == 0) {
            std::fprintf(stderr, "  no packet had been decoded when the image rebuild was reached\n");
        }
    }
    g_framebufferGate.release();
    check(signals.wait(kMonitorOrder, 3), "and that redraw commits once the rebuild is let go");

    // GOAL: S13.118 -- a video pass overtaken by a newer request after it replaced the monitor's
    //       images never leaves that monitor without a frame: an export queued behind it is
    //       answered with a frame, the old one or the new, and never with NotRendered.
    // WHY-THIS-SHAPE: the newer size lands while the first pass is held inside its image rebuild,
    //       and the export is queued between that pass and the newer size's own post -- the one
    //       order in which the export runs before anything but the overtaken pass could draw.
    //       Asserting on status instead would not do: has_frame is read from a different lock than
    //       the one the export takes, and it is the export that a client actually depends on.
    // CONTROL: WallpaperManager::renderPending's attempt loop (backend/daemon/wallpaper_manager.cpp)
    //       reduced to a single pass -> "FAIL: an export racing an overtaken video pass gets a frame,
    //       never none", printing "the export got no frame, reason 1" (NotRendered -- the answer this
    //       goal forbids). Observed 2026-09-15. Reverted.
    if (!firstFrame(kMonitorRace, "DP-RACE")) {
        check(false, "the monitor the overtaken video pass is about is drawn first");
        return;
    }
    g_decodeCalls.store(0);
    g_files.resetCounts();
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedWidth, kResizedHeight));
    resize("DP-RACE", kResizedWidth, kResizedHeight);
    const bool raceHeld = g_framebufferGate.waitFired(kGateLimit);
    check(raceHeld, "the video pass to be overtaken is held inside its image rebuild");
    if (raceHeld) {
        std::future<Exported> racing = exportOnWorker(kMonitorRace);
        resize("DP-RACE", kResizedAgainWidth, kResizedAgainHeight);
        g_framebufferGate.release();
        const bool answered = racing.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
        check(answered, "the export queued behind the overtaken video pass is answered");
        if (answered) {
            const Exported got = racing.get();
            check(got.served, "an export racing an overtaken video pass gets a frame, never none");
            if (!got.served) std::fprintf(stderr, "  the export got %s\n", describe(got).c_str());
        }

        // GOAL: S13.123's retry clause -- the passes renderPending() runs after an overtaken rebuild
        //       draw with the session already resident: no file opened, nothing decoded.
        // WHY-THIS-SHAPE: waits for generation 2, which only the retry inside the overtaken
        //       renderPending() call can commit -- the newer size's own post finds its request
        //       serviced -- so the counters cover exactly the overtaken pass and its retry.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, `m_videoSessions.erase(monitorId);` added
        //       before the superseded `return RenderPass{{}, true};` in the commit block -> "FAIL: the
        //       retry after an overtaken rebuild opens and decodes nothing", printing "1 open(s), 1
        //       packet(s) across the overtaken pass and its retry" (11 packets on Lavapipe).
        //       Observed 2026-09-16 on both driver entries. Reverted.
        const bool retried = signals.wait(kMonitorRace, 2);
        check(retried, "the retry after the overtaken rebuild commits");
        if (retried) {
            const bool nothing = g_files.totalOpens() == 0 && g_decodeCalls.load() == 0;
            check(nothing, "the retry after an overtaken rebuild opens and decodes nothing");
            if (!nothing) {
                std::fprintf(stderr, "  %d open(s), %d packet(s) across the overtaken pass and its retry\n",
                             g_files.totalOpens(), g_decodeCalls.load());
            }
        }
    }
    g_framebufferGate.release();
}

// --------------------------------------------------------------------------- //
// A worker, no bus: what the crash record covers for a video
// --------------------------------------------------------------------------- //

constexpr int kMonitorRecord = 0;

bool recordOnDisk(const std::filesystem::path& stateDir, int monitorId) {
    std::error_code ec;
    return std::filesystem::exists(wallpaperx::crashRecordPath(stateDir, monitorId), ec) && !ec;
}

void runCrashRecordSpanChecks(const WallpaperManager::Config& base, const std::string& clip) {
    const char* kGoals = "S13.119 (the crash record covers a video's decode and a replaced session's teardown)";
    const wallpaperx::testing::ScratchDir state("videostate");
    if (!state.ok()) {
        notVerified(kGoals, "no scratch state directory could be created");
        return;
    }
    PhaseGrade grade("the crash record phase");
    FrameReadyLog signals;
    WallpaperManager::Config config = base;
    config.stateDir = state.path();
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    struct Unblock {
        RenderWorker& worker;
        ~Unblock() {
            g_decodeGate.release();
            g_freeCodecGate.release();
            worker.stop();
        }
    } unblock{worker};

    // GOAL: S13.119's first half -- while a video's picture is being decoded, a crash record naming
    //       that monitor, the type `video` and the file is on disk, so a start after a fatal fault
    //       inside ffmpeg refuses that wallpaper instead of applying it again. False if the record
    //       is written only around the draw, which is what leaves a decoder crash to repeat on
    //       every start -- the crash loop this feature exists to break.
    // WHY-THIS-SHAPE: the render thread is held inside the very call that hands a packet to the
    //       decoder, and the record is read from this thread while it is held. A check before or
    //       after the pass cannot see the span; the record is removed by the end of the pass, so
    //       "it was there afterwards" is not available as evidence. The record's contents are read
    //       too, not just its existence: a record naming the wrong type would refuse nothing.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `const ApplyingRecord applying(...)` line
    //       deleted from videoSessionLocked() -> "FAIL: a crash record is on disk while the video is
    //       being decoded", and no other line. Observed 2026-09-16 on both driver
    //       entries. Reverted.
    g_decodeGate.arm(CallGate::Mode::Hold);
    wallpaperx::testing::nameScreen(manager, kMonitorRecord, "DP-REC");
    manager.requestWallpaper("video", clip, kMonitorRecord);
    worker.post(kMonitorRecord);
    const bool decoding = g_decodeGate.waitFired(kGateLimit);
    check(decoding, "the render thread reaches the decode of a video wallpaper");
    if (decoding) {
        const bool onDisk = recordOnDisk(state.path(), kMonitorRecord);
        check(onDisk, "a crash record is on disk while the video is being decoded");
        if (onDisk) {
            const std::optional<wallpaperx::CrashRecord> record =
                wallpaperx::readCrashRecord(state.path(), kMonitorRecord);
            const bool names = record.has_value() && record->type == "video" && record->path == clip;
            check(names, "and it names the video and the file being applied");
            if (!names && record.has_value()) {
                std::fprintf(stderr, "  the record names type '%s' path '%s'\n", record->type.c_str(),
                             record->path.c_str());
            }
        }
    }
    g_decodeGate.release();
    const bool committed = signals.wait(kMonitorRecord, 1);
    check(committed, "the held decode goes on to commit its frame once it is let go");
    if (!committed) {
        grade.expectNoDevice();
        notVerified(kGoals, "the video render did not commit on this host");
        return;
    }

    // GOAL: S13.119's second half -- a session dropped because its monitor was given a wallpaper
    //       again, the same file included, is torn down with a record on disk naming the type
    //       `video` and the file that session opened. Tearing a decoder down re-enters ffmpeg and
    //       the driver, so a fatal fault there has to refuse that file at the next start, exactly
    //       as one during the decode does. False for a drop that frees the session with no record.
    // WHY-THIS-SHAPE: the thread is held inside the decoder's codec context being freed, inside
    //       ~VideoPipeline. The same path is requested again on purpose: a new wallpaper with the
    //       same path must still drop the session (S13.124), and the first decoder free in that pass
    //       is that drop, since it runs before the new session is opened. The count of frees is
    //       printed so a run where that stopped being true is visible. The record's contents are read
    //       too: a record naming the wrong type would refuse nothing.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `applying.emplace(...)` line deleted from
    //       closeVideoSessionLocked() -> "FAIL: a record naming the video is on disk while a replaced
    //       session's decoder is torn down", printing "the record on disk: none", beside the two
    //       S13.130 checks. Observed 2026-09-16 on both driver entries. Reverted.
    g_codecFrees.store(0);
    g_freeCodecGate.arm(CallGate::Mode::Hold);
    manager.requestWallpaper("video", clip, kMonitorRecord);
    worker.post(kMonitorRecord);
    const bool tearingDown = g_freeCodecGate.waitFired(kGateLimit);
    check(tearingDown, "the render thread reaches the teardown of the replaced session's decoder");
    if (tearingDown) {
        const std::optional<wallpaperx::CrashRecord> record = wallpaperx::readCrashRecord(state.path(), kMonitorRecord);
        const bool names = record.has_value() && record->type == "video" && record->path == clip;
        check(names, "a record naming the video is on disk while a replaced session's decoder is torn down");
        if (!names) {
            std::fprintf(stderr, "  the record on disk: %s\n",
                         record ? (record->type + " " + record->path).c_str() : "none");
        }
        std::printf("video_wallpaper_test: %d codec context(s) freed by the time the teardown was reached\n",
                    g_codecFrees.load());
    }
    g_freeCodecGate.release();
    check(signals.wait(kMonitorRecord, 2), "and the pass finishes once the teardown is let go");

    // The record does not outlive the pass that wrote it: a record left behind would refuse this
    // wallpaper at the next start, on a render that succeeded.
    check(!recordOnDisk(state.path(), kMonitorRecord), "and no record is left behind once the pass has finished");
}

// --------------------------------------------------------------------------- //
// Stage E2: one decode session per monitor
// --------------------------------------------------------------------------- //

std::string screenOf(int monitorId) { return "DP-S" + std::to_string(monitorId); }

std::string describeFiles(const std::multiset<std::string>& paths) {
    std::string out;
    for (const std::string& path : paths) {
        if (!out.empty()) out += ", ";
        out += std::filesystem::path(path).filename().string();
    }
    return out.empty() ? "none" : out;
}

bool contains(const std::vector<int>& ids, int id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

// What one pass cost, read from the hooks.
struct PassCost {
    wallpaperx::RenderOutcome outcome;
    int opens = 0;
    int closes = 0;
    int decodes = 0;
};

std::string describe(const PassCost& cost) {
    return std::string(cost.outcome.rendered ? "committed" : "did not commit") + ", " + std::to_string(cost.opens) +
           " open(s), " + std::to_string(cost.closes) + " close(s), " + std::to_string(cost.decodes) + " packet(s)";
}

// On the calling thread, which must be the one the manager's Device is pinned to.
PassCost renderCounted(WallpaperManager& manager, int monitorId) {
    g_files.resetCounts();
    g_decodeCalls.store(0);
    PassCost cost;
    cost.outcome = manager.renderPending(monitorId);
    cost.opens = g_files.totalOpens();
    cost.closes = g_files.totalCloses();
    cost.decodes = g_decodeCalls.load();
    return cost;
}

void reportUnless(bool ok, const char* what, const std::string& detail) {
    check(ok, what);
    if (!ok) std::fprintf(stderr, "  %s\n", detail.c_str());
}

// The monitor's current frame against the cover crop, where the export can be mapped.
void gradeServedPicture(WallpaperManager& manager, int monitorId, const char* goal, const char* what) {
    wallpaperx::FrameExport exported;
    try {
        exported = manager.exportCurrentFrame(monitorId);
    } catch (const std::exception& e) {
        reportUnless(false, what, std::string("the export threw: ") + e.what());
        return;
    }
    if (!exported.handle) {
        check(false, (std::string(what) + " -- the frame is not even served").c_str());
        return;
    }
    const PixelVerdict verdict = gradeCoverCrop(layoutOf(exported.handle->frame));
    if (exported.handle->frame.fd >= 0) close(exported.handle->frame.fd);
    if (exported.handle->acquireFd >= 0) close(exported.handle->acquireFd);
    if (!verdict.answerable) {
        notVerified(goal, verdict.reason);
        return;
    }
    check(verdict.compared >= 16, (std::string(what) + " -- enough rows were compared to grade it").c_str());
    reportUnless(verdict.wrong == 0, what,
                 std::to_string(verdict.wrong) + " of " + std::to_string(verdict.compared) +
                     " sampled rows show the wrong band; first: " + verdict.firstWrong);
}

constexpr int kSessionMonitors[] = {10, 11, 12, 13, 14};
constexpr int kMonitorStillOnly = 15;
constexpr const char* kConsumer = ":1.42";
// A layout no driver allocates: what a report excluding the committed one is made of.
constexpr uint64_t kUnallocatableModifier = 0x0f00000000000001ull;

// S13.123-S13.126 and S13.132, on the calling thread with no worker: every pass is run by this
// function, so the counters read after one cover that pass and nothing else.
void runSessionChecks(const WallpaperManager::Config& config, const std::vector<std::string>& clips,
                      const std::string& still, const std::string& shaderRow) {
    const char* kGoals = "S13.123-S13.126 and S13.132 (which passes reuse, reopen, evict and drop a session)";
    PhaseGrade grade("the session phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    for (const int id : kSessionMonitors) wallpaperx::testing::nameScreen(manager, id, screenOf(id));
    wallpaperx::testing::nameScreen(manager, kMonitorStillOnly, screenOf(kMonitorStillOnly));
    const auto resize = [&manager](int id, uint32_t width, uint32_t height) {
        std::vector<int> redraw;
        manager.setOutputSize(screenOf(id), width, height, redraw);
        return contains(redraw, id);
    };

    const int v0 = kSessionMonitors[0];
    manager.requestWallpaper("video", clips[0], v0);
    const PassCost first = renderCounted(manager, v0);
    if (!first.outcome.rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }
    // What gives every "opens nothing" below its meaning: the same hooks, on the same thread, see
    // this pass open and decode.
    reportUnless(first.opens == 1 && first.decodes > 0,
                 "a monitor's first video pass opens its file and decodes a picture (the counters work)",
                 describe(first));

    // GOAL: S13.123, a size report -- a re-post for a new size opens, decodes and closes nothing,
    //       and still commits the next generation showing the video.
    // WHY-THIS-SHAPE: counted per pass on the one thread that runs passes, rather than timed. The
    //       picture is graded because "committed" alone is also true of a pass that rebuilt the
    //       images and drew nothing into them.
    // CONTROL: (a) dropStaleVideoSessionLocked() in backend/daemon/wallpaper_manager.cpp made to
    //       drop every session (`&& false` on its keep-it test) -> "FAIL: a size report of a video
    //       monitor opens, decodes and closes nothing", printing "committed, 1 open(s), 1 close(s), 1
    //       packet(s)" (11 packets on Lavapipe). (b) the crop in renderer/renderers/video_renderer.cpp's
    //       render() made `static`, so a session keeps the crop of the first size it drew -> on
    //       Lavapipe "FAIL: after a size report the monitor shows the video, cover-cropped at the new
    //       size", printing "8 of 36 sampled rows show the wrong band"; NVIDIA abstains. Both observed
    //       2026-09-16; reverted.
    reportUnless(resize(v0, kResizedWidth, kResizedHeight), "a new size is owed to the video monitor",
                 "setOutputSize did not name it for a redraw");
    const PassCost resized = renderCounted(manager, v0);
    reportUnless(resized.outcome.rendered && resized.outcome.generation == first.outcome.generation + 1,
                 "a size report of a video monitor commits the next frame", describe(resized));
    reportUnless(resized.opens == 0 && resized.decodes == 0 && resized.closes == 0,
                 "a size report of a video monitor opens, decodes and closes nothing", describe(resized));
    gradeServedPicture(manager, v0, "S13.123's picture after a size report",
                       "after a size report the monitor shows the video, cover-cropped at the new size");
    uint64_t generation = resized.outcome.generation;

    // GOAL: S13.123, an import-modifier report -- a re-post a consumer's layout report causes opens
    //       and decodes nothing, and commits.
    // WHY-THIS-SHAPE: a report can condemn a frame only where the frame has a layout (a dma_buf),
    //       and on Lavapipe the only layout is LINEAR, so a report that excludes it leaves nothing to
    //       draw into. So the frame is condemned by one report -- which posts the monitor -- and made
    //       readable again by a second before the pass runs: the pass is still the report's, and it
    //       commits. An opaque-fd export carries no layout, so no report reaches it there.
    // CONTROL: the (a) mutation above -> on Lavapipe "FAIL: a layout report's re-post of a video
    //       monitor opens, decodes and closes nothing", printing "committed, 1 open(s), 1 close(s), 11
    //       packet(s)". Observed 2026-09-16; the NVIDIA entry abstains. Reverted.
    {
        wallpaperx::FrameExport probe = manager.exportCurrentFrame(v0);
        const bool served = probe.handle.has_value();
        uint64_t committedLayout = wallpaperx::kDrmModifierInvalid;
        bool dmaBuf = false;
        if (served) {
            committedLayout = probe.handle->frame.modifier;
            dmaBuf = probe.handle->frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf;
            if (probe.handle->frame.fd >= 0) close(probe.handle->frame.fd);
            if (probe.handle->acquireFd >= 0) close(probe.handle->acquireFd);
        }
        check(served, "the video monitor's frame is served before the layout report");
        if (served && (!dmaBuf || committedLayout == wallpaperx::kDrmModifierInvalid)) {
            notVerified("S13.123's import-modifier clause",
                        "this host exports opaque file descriptors, which carry no layout for a report to exclude, so "
                        "no report re-posts a monitor that has a frame");
        } else if (served) {
            const uint32_t format = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
            const std::vector<int> condemned =
                manager.setImportModifiers(kConsumer, format, {kUnallocatableModifier}).toRender;
            check(contains(condemned, v0), "a layout report excluding the frame's layout re-posts the video monitor");
            manager.setImportModifiers(kConsumer, format, {committedLayout, kUnallocatableModifier});
            const PassCost reported = renderCounted(manager, v0);
            reportUnless(reported.outcome.rendered && reported.outcome.generation == generation + 1,
                         "a layout report's re-post of a video monitor commits the next frame", describe(reported));
            reportUnless(reported.opens == 0 && reported.decodes == 0 && reported.closes == 0,
                         "a layout report's re-post of a video monitor opens, decodes and closes nothing",
                         describe(reported));
            if (reported.outcome.rendered) generation = reported.outcome.generation;
        }
    }

    // GOAL: S13.123, an image release -- the pass a consumer's release posts, for a render that was
    //       owed, opens and decodes nothing and commits.
    // WHY-THIS-SHAPE: a release posts a monitor only when a render is owed, so one is made owed by a
    //       size that changes and changes back. Neither size report is posted: the release is the
    //       only thing that runs the pass.
    // CONTROL: the (a) mutation above -> "FAIL: the release's pass of a video monitor opens, decodes
    //       and closes nothing", printing "committed, 1 open(s), 1 close(s), 1 packet(s)". Observed
    //       2026-09-16 on both driver entries. Reverted.
    {
        resize(v0, kResizedAgainWidth, kResizedAgainHeight);
        resize(v0, kResizedWidth, kResizedHeight);
        wallpaperx::FrameExport held = manager.exportCurrentFrame(v0, kConsumer);
        check(held.handle.has_value(), "a consumer is handed the video monitor's frame while a render is owed");
        if (held.handle) {
            const uint64_t bufferId = held.handle->frame.bufferId;
            if (held.handle->frame.fd >= 0) close(held.handle->frame.fd);
            if (held.handle->acquireFd >= 0) close(held.handle->acquireFd);
            const std::vector<int> retry = manager.releaseBuffer(kConsumer, v0, bufferId);
            check(contains(retry, v0), "releasing that image posts the video monitor");
            const PassCost released = renderCounted(manager, v0);
            reportUnless(released.outcome.rendered && released.outcome.generation == generation + 1,
                         "the release's pass of a video monitor commits the next frame", describe(released));
            reportUnless(released.opens == 0 && released.decodes == 0 && released.closes == 0,
                         "the release's pass of a video monitor opens, decodes and closes nothing", describe(released));
        }
    }

    // GOAL: S13.124 -- giving a monitor the same video path again opens a new session and closes the
    //       old one, leaving one. False for a daemon that keys the session on the path, which would
    //       keep decoding a file rewritten in place from bytes that are gone.
    // CONTROL: `++state.wallpaperSeq;` deleted from requestWallpaper() in
    //       backend/daemon/wallpaper_manager.cpp -> "FAIL: the same video path given again opens a new
    //       session and closes the old one", printing "committed, 0 open(s), 0 close(s), 0 packet(s)";
    //       S13.117's, S13.119's and S13.128's checks that need a new session went red with it. Observed
    //       2026-09-16 on both driver entries. Reverted.
    manager.requestWallpaper("video", clips[0], v0);
    const PassCost again = renderCounted(manager, v0);
    check(again.outcome.rendered, "the same video given again commits");
    reportUnless(again.opens == 1 && again.closes == 1 && again.decodes > 0,
                 "the same video path given again opens a new session and closes the old one", describe(again));
    reportUnless(g_files.live().count(clips[0]) == 1, "and one session for that monitor remains",
                 "open: " + describeFiles(g_files.live()));

    // GOAL: S13.126 -- at most four sessions are resident; the fifth evicts the least recently drawn
    //       -- not the least recently opened -- and the evicted monitor's next pass reopens its file
    //       and draws the video correctly.
    // WHY-THIS-SHAPE: every monitor plays a different copy of the fixture, so the set of open files
    //       says which monitors hold a session. Monitor 10 is opened first and redrawn after 11-13
    //       are opened, so least-recently-opened (10) and least-recently-drawn (11) differ.
    // CONTROL: (a) kMaxVideoSessions set to 5 in backend/daemon/wallpaper_manager.h -> "FAIL: a fifth
    //       session leaves four resident", printing all five files open, and three more. (b)
    //       `it->second.lastUsed = ++m_videoSessionUse;` deleted from videoSessionLocked()'s reuse
    //       branch in backend/daemon/wallpaper_manager.cpp -> "FAIL: the fifth session evicts the least
    //       recently drawn monitor's, not the first opened", printing video_session_0.mp4 gone. Both
    //       observed 2026-09-16 on both driver entries; reverted.
    // COVERAGE-ASSERT: each of the four opens is asserted to have happened, so "four are resident"
    //       is never read off a phase in which some never opened.
    {
        int opened = 0;
        for (int i = 1; i <= 3; ++i) {
            manager.requestWallpaper("video", clips[i], kSessionMonitors[i]);
            const PassCost cost = renderCounted(manager, kSessionMonitors[i]);
            if (cost.outcome.rendered && cost.opens == 1 && cost.closes == 0) ++opened;
            else std::fprintf(stderr, "  monitor %d: %s\n", kSessionMonitors[i], describe(cost).c_str());
        }
        check(opened == 3, "three more video monitors each open a session and close none");
        reportUnless(g_files.live().size() == 4, "four video monitors hold four sessions",
                     "open: " + describeFiles(g_files.live()));

        resize(v0, kResizedAgainWidth, kResizedAgainHeight);
        const PassCost touched = renderCounted(manager, v0);
        reportUnless(touched.outcome.rendered && touched.opens == 0,
                     "the first-opened monitor is redrawn from its session", describe(touched));

        manager.requestWallpaper("video", clips[4], kSessionMonitors[4]);
        const PassCost fifth = renderCounted(manager, kSessionMonitors[4]);
        reportUnless(fifth.outcome.rendered && fifth.opens == 1, "a fifth video monitor opens a session and commits",
                     describe(fifth));
        const std::multiset<std::string> afterFifth = g_files.live();
        reportUnless(afterFifth.size() == 4, "a fifth session leaves four resident", "open: " + describeFiles(afterFifth));
        reportUnless(g_files.closes(clips[1]) == 1 && afterFifth.count(clips[1]) == 0 && afterFifth.count(clips[0]) == 1,
                     "the fifth session evicts the least recently drawn monitor's, not the first opened",
                     "open: " + describeFiles(afterFifth));

        reportUnless(resize(kSessionMonitors[1], kResizedWidth, kResizedHeight),
                     "a new size is owed to the evicted monitor", "setOutputSize did not name it");
        const PassCost reopened = renderCounted(manager, kSessionMonitors[1]);
        reportUnless(reopened.outcome.rendered && g_files.opens(clips[1]) == 1 && reopened.decodes > 0,
                     "the evicted monitor's next pass reopens its file, decodes and commits", describe(reopened));
        reportUnless(g_files.closes(clips[2]) == 1 && g_files.live().size() == 4,
                     "and evicts the next least recently drawn in its place", "open: " + describeFiles(g_files.live()));
        gradeServedPicture(manager, kSessionMonitors[1], "S13.126's picture after eviction",
                           "the evicted monitor's redraw shows its video, cover-cropped");
    }

    // GOAL: S13.132 -- a still wallpaper's passes neither open, close nor evict a video session:
    //       with four resident, a still set and resized on another monitor leaves exactly the same
    //       files open and still commits.
    // WHY-THIS-SHAPE: run while the cap is full, so a still pass that counted against it, or that
    //       cleared sessions it does not own, changes the open set.
    // CONTROL: dropStaleVideoSessionLocked() in backend/daemon/wallpaper_manager.cpp given a first
    //       line that closes every session when `kind` is not a video -> "FAIL: setting a still beside
    //       four video sessions commits and opens or closes no video", printing "4 close(s)", and "FAIL:
    //       the four video sessions are all still resident". Observed 2026-09-16 on both driver
    //       entries. Reverted.
    {
        const std::multiset<std::string> resident = g_files.live();
        manager.requestWallpaper("static", still, kMonitorStillOnly);
        const PassCost set = renderCounted(manager, kMonitorStillOnly);
        reportUnless(set.outcome.rendered && set.opens == 0 && set.closes == 0,
                     "setting a still beside four video sessions commits and opens or closes no video", describe(set));
        resize(kMonitorStillOnly, kResizedWidth, kResizedHeight);
        const PassCost moved = renderCounted(manager, kMonitorStillOnly);
        reportUnless(moved.outcome.rendered && moved.opens == 0 && moved.closes == 0,
                     "resizing that still commits and opens or closes no video", describe(moved));
        reportUnless(g_files.live() == resident, "the four video sessions are all still resident",
                     "before: " + describeFiles(resident) + "; after: " + describeFiles(g_files.live()));
    }

    // GOAL: S13.125 -- a monitor whose wallpaper stops being a video, to a still or to a type this
    //       build cannot draw, holds no session after its pass.
    // WHY-THIS-SHAPE: both kinds of replacement, because they leave through different branches of
    //       the pass: the undrawable one returns before anything is drawn.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the dropStaleVideoSessionLocked() call moved
    //       from the top of renderPassLocked() into its video branch, beside videoSessionLocked() ->
    //       both checks below red, each printing the replaced file still open. Observed 2026-09-16 on
    //       both driver entries. Reverted. `kind` in dropStaleVideoSessionLocked()'s keep-it test has no
    //       control of its own: a type changes only through a request, which moves wallpaperSeq too.
    {
        manager.requestWallpaper("static", still, kSessionMonitors[3]);
        const PassCost toStill = renderCounted(manager, kSessionMonitors[3]);
        check(toStill.outcome.rendered, "a video monitor given a still commits it");
        reportUnless(g_files.closes(clips[3]) == 1 && g_files.live().count(clips[3]) == 0,
                     "a video monitor given a still no longer holds a session",
                     describe(toStill) + "; open: " + describeFiles(g_files.live()));

        manager.requestWallpaper("shader", shaderRow, kSessionMonitors[4]);
        const PassCost toShader = renderCounted(manager, kSessionMonitors[4]);
        reportUnless(g_files.closes(clips[4]) == 1 && g_files.live().count(clips[4]) == 0,
                     "a video monitor given a type this build cannot draw no longer holds a session",
                     describe(toShader) + "; open: " + describeFiles(g_files.live()));
    }
}

// What a start would decide had this process died at this instant: the config as it is now, and
// this monitor's crash record copied as it is now -- unstamped, which is what SIGKILL or the
// teardown alarm leaves.
struct RestartVerdict {
    bool graded = false;
    std::string why;
    bool refused = false;
    std::string lastError;
};

RestartVerdict startAfterDeathNow(const WallpaperManager::Config& running, int monitorId) {
    RestartVerdict verdict;
    const wallpaperx::testing::ScratchDir copy("videorestart");
    if (!copy.ok()) {
        verdict.why = "no scratch directory for the copied state";
        return verdict;
    }
    WallpaperManager::Config config;
    config.width = running.width;
    config.height = running.height;
    config.configFile = copy.path() / "config.json";
    config.stateDir = copy.path() / "state";
    std::error_code ec;
    std::filesystem::copy_file(running.configFile, config.configFile, ec);
    if (ec) {
        verdict.why = "the running config could not be copied: " + ec.message();
        return verdict;
    }
    const std::filesystem::path record = wallpaperx::crashRecordPath(running.stateDir, monitorId);
    if (std::filesystem::exists(record, ec)) {
        std::filesystem::create_directories(config.stateDir, ec);
        if (!ec) std::filesystem::copy_file(record, wallpaperx::crashRecordPath(config.stateDir, monitorId), ec);
        if (ec) {
            verdict.why = "the crash record could not be copied: " + ec.message();
            return verdict;
        }
    }
    try {
        WallpaperManager restarted(config);
        restarted.restoreSaved();
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(restarted, monitorId);
        if (!status) {
            verdict.why = "the copied config did not restore the monitor";
            return verdict;
        }
        verdict.graded = true;
        verdict.refused = !status->lastError.empty() && !status->renderOutstanding;
        verdict.lastError = status->lastError;
    } catch (const std::exception& e) {
        verdict.why = std::string("a manager on the copied state threw: ") + e.what();
    }
    return verdict;
}

bool restartGraded(const RestartVerdict& verdict) {
    if (!verdict.graded) {
        wallpaperx::testing::harnessFailure("a start from the copied state could not be graded: " + verdict.why);
    }
    return verdict.graded;
}

bool drainWorker(RenderWorker& worker) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> future = done->get_future();
    worker.postTask([done](RenderWorker::TaskState) { done->set_value(); });
    return future.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
}

std::string describeRecord(const std::optional<wallpaperx::CrashRecord>& record) {
    return record ? "type '" + record->type + "' path '" + record->path + "'" : "none";
}

// The render worker's thread, with every gate this file arms let go before the worker is stopped:
// a gate left armed or holding would park it.
struct WorkerUnblock {
    RenderWorker& worker;
    ~WorkerUnblock() {
        g_framebufferGate.release();
        g_framebufferFailGate.release();
        g_submitFailGate.release();
        g_closeFileGate.release();
        g_freeCodecGate.release();
        g_decodeGate.release();
        worker.stop();
    }
};

constexpr int kMonitorGpuFault = 20;
constexpr int kMonitorOvertaken = 21;
constexpr int kMonitorDrawFault = 22;
constexpr int kMonitorReplaced = 23;

// S13.127-S13.129 and S13.130's replacement half.
void runFailedPassChecks(const WallpaperManager::Config& base, const std::vector<std::string>& clips,
                         const std::string& still) {
    const char* kGoals = "S13.127-S13.130 (what a failed pass does with its session; which teardowns are recorded)";
    const wallpaperx::testing::ScratchDir scratch("videofail");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch directory could be created for the state and config");
        return;
    }
    WallpaperManager::Config config = base;
    config.stateDir = scratch.path() / "state";
    config.configFile = scratch.path() / "config.json";

    PhaseGrade grade("the failed-pass phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    const auto show = [&](int id, const std::string& path) {
        wallpaperx::testing::nameScreen(manager, id, screenOf(id));
        manager.requestWallpaper("video", path, id);
        worker.post(id);
        return signals.wait(id, 1);
    };
    const auto resize = [&](int id, uint32_t width, uint32_t height) {
        std::vector<int> redraw;
        manager.setOutputSize(screenOf(id), width, height, redraw);
        for (const int posted : redraw) worker.post(posted);
    };

    if (!show(kMonitorGpuFault, clips[0])) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }

    // GOAL: S13.127 -- a pass that fails before the draw for a reason that is not the file's (here
    //       the images failing to build) drops the monitor's session with NO crash record on disk,
    //       so a death inside that teardown does not refuse the video at the next start; and the
    //       next pass reopens the file rather than drawing with the dropped session.
    // WHY-THIS-SHAPE: the thread is held as the session's file is closed, the end of its teardown,
    //       and a start is built from the state copied at that instant -- the consequence, not just
    //       the file. S13.129 below grades the same copy going the other way, so this "not refused"
    //       cannot be the copy failing to refuse anything.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, renderPassLocked()'s
    //       `if (!error.empty()) m_videoSessions.erase(monitorId);` replaced with
    //       `if (!error.empty()) closeVideoSessionLocked(monitorId);` -- the record-writing close the
    //       last review found -> "FAIL: no crash record is on disk while a session is torn down for a
    //       fault that is not the file's", printing the record naming video_session_0.mp4, and "FAIL: a
    //       start after dying in that teardown applies the video instead of refusing it". Deleting that
    //       line instead reddens the same two, through the close after the commit block. Both observed
    //       2026-09-16 on both driver entries. Reverted.
    // COVERAGE-ASSERT: the injected failure is asserted to have fired, so the teardown seen is the
    //       one that failure caused.
    g_framebufferFailGate.arm(CallGate::Mode::Fail, extentOf(kResizedWidth, kResizedHeight));
    g_closeFileGate.arm(CallGate::Mode::Hold, clips[0]);
    resize(kMonitorGpuFault, kResizedWidth, kResizedHeight);
    const bool droppedForFault = g_closeFileGate.waitFired(kGateLimit);
    check(droppedForFault, "a pass whose images fail to build tears the monitor's session down");
    if (droppedForFault) {
        check(g_framebufferFailGate.waitFired(std::chrono::seconds(0)),
              "that teardown follows the injected image failure");
        const std::optional<wallpaperx::CrashRecord> record =
            wallpaperx::readCrashRecord(config.stateDir, kMonitorGpuFault);
        reportUnless(!record.has_value(),
                     "no crash record is on disk while a session is torn down for a fault that is not the file's",
                     "the record on disk: " + describeRecord(record));
        const RestartVerdict restart = startAfterDeathNow(config, kMonitorGpuFault);
        if (restartGraded(restart)) {
            reportUnless(!restart.refused, "a start after dying in that teardown applies the video instead of refusing it",
                         "the start refused it: " + restart.lastError);
        }
    }
    g_closeFileGate.release();
    g_framebufferFailGate.release();
    check(drainWorker(worker), "the failed pass finishes once its teardown is let go");
    {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMonitorGpuFault);
        check(status.has_value() && !status->lastError.empty() && status->lastErrorIsDaemonFault,
              "the pass whose images failed reports a failure that is the daemon's");
    }
    g_files.resetCounts();
    resize(kMonitorGpuFault, kResizedAgainWidth, kResizedAgainHeight);
    const bool recovered = signals.wait(kMonitorGpuFault, 2);
    check(recovered, "the monitor draws again at its next pass");
    if (recovered) {
        reportUnless(g_files.opens(clips[0]) == 1, "and that pass reopens the file rather than keeping the dropped session",
                     std::to_string(g_files.opens(clips[0])) + " open(s)");
    }

    // GOAL: S13.128 -- a pass that opened a session, then failed, and was overtaken by a newer
    //       request before it could commit, leaves no session: the newer request's pass opens its
    //       own. False for a daemon that drops a failed session only after the commit block, which
    //       an overtaken pass returns from inside.
    // WHY-THIS-SHAPE: the render thread is held inside the image rebuild of a pass that has just
    //       opened a new wallpaper's session, the rebuild is armed to fail when let go, and the newer
    //       size lands while it is held -- so the failure and the overtaking are both certain, in
    //       that order, with no timing involved.
    // CONTROL: `if (!error.empty()) m_videoSessions.erase(monitorId);` deleted from renderPassLocked()
    //       in backend/daemon/wallpaper_manager.cpp -> "FAIL: a failed, overtaken pass leaves no
    //       session: the newer pass opens its own", printing "0 open(s), 0 close(s) after the failure".
    //       Observed 2026-09-16 on both driver entries, after the gates were filtered by size: before
    //       that, the same mutation stayed green, because the hold landed in the frame converter
    //       during the open and the pass failed before any session existed. Reverted.
    // COVERAGE-ASSERT: the held pass is asserted to have opened the session, and the injected
    //       failure to have fired.
    if (!show(kMonitorOvertaken, clips[0])) {
        check(false, "the monitor the overtaken failing pass is about is drawn first");
        return;
    }
    g_files.resetCounts();
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedWidth, kResizedHeight));
    g_framebufferFailGate.arm(CallGate::Mode::Fail, extentOf(kResizedWidth, kResizedHeight));
    manager.requestWallpaper("video", clips[1], kMonitorOvertaken);
    resize(kMonitorOvertaken, kResizedWidth, kResizedHeight);
    const bool heldFailing = g_framebufferGate.waitFired(kGateLimit);
    check(heldFailing, "the pass to be overtaken is held inside its image rebuild");
    if (heldFailing) {
        const bool opened = g_files.opens(clips[1]) == 1;
        check(opened, "the pass to be overtaken opened its session before the rebuild");
        resize(kMonitorOvertaken, kResizedAgainWidth, kResizedAgainHeight);
        g_files.resetCounts();
        g_framebufferGate.release();
        const bool redrawn = signals.wait(kMonitorOvertaken, 2);
        check(redrawn, "the newer request's pass commits");
        check(g_framebufferFailGate.waitFired(std::chrono::seconds(0)), "the overtaken pass's rebuild was failed");
        if (opened && redrawn) {
            reportUnless(g_files.opens(clips[1]) == 1 && g_files.closes(clips[1]) == 1,
                         "a failed, overtaken pass leaves no session: the newer pass opens its own",
                         std::to_string(g_files.opens(clips[1])) + " open(s), " +
                             std::to_string(g_files.closes(clips[1])) + " close(s) after the failure");
        }
    }
    g_framebufferGate.release();
    g_framebufferFailGate.release();

    // GOAL: S13.129 -- a draw that throws inside the commit block tears the session down while a
    //       record naming that file is on disk, so a death in that teardown refuses the video at the
    //       next start.
    // WHY-THIS-SHAPE: the size changes and changes back before the one post, so the pass neither
    //       rebuilds nor decodes and the draw's submission is its only one -- failing the next
    //       submission fails the draw. The reported error is read to confirm that.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, renderPassLocked()'s
    //       `if (!error.empty()) closeVideoSessionLocked(monitorId);` replaced with
    //       `if (!error.empty()) m_videoSessions.erase(monitorId);` placed after `applying.reset();`
    //       -> "FAIL: a record naming the video is on disk while the session whose draw failed is torn
    //       down", printing "the record on disk: none", and "FAIL: a start after dying in that teardown
    //       refuses the video". Deleting the close outright -> "FAIL: a draw that fails tears the
    //       monitor's session down", after the gate's 30-second limit. Both observed 2026-09-16 on both
    //       driver entries. Reverted. Deleting the record from closeVideoSessionLocked() leaves this
    //       green, correctly: the pass's own record names the same file.
    if (!show(kMonitorDrawFault, clips[2])) {
        check(false, "the monitor whose draw is failed is drawn first");
        return;
    }
    {
        std::vector<int> notPosted;
        manager.setOutputSize(screenOf(kMonitorDrawFault), kResizedWidth, kResizedHeight, notPosted);
        manager.setOutputSize(screenOf(kMonitorDrawFault), kTargetWidth, kTargetHeight, notPosted);
    }
    g_submitFailGate.arm(CallGate::Mode::Fail);
    g_closeFileGate.arm(CallGate::Mode::Hold, clips[2]);
    worker.post(kMonitorDrawFault);
    const bool closingAfterDraw = g_closeFileGate.waitFired(kGateLimit);
    check(closingAfterDraw, "a draw that fails tears the monitor's session down");
    if (closingAfterDraw) {
        const std::optional<wallpaperx::CrashRecord> record =
            wallpaperx::readCrashRecord(config.stateDir, kMonitorDrawFault);
        reportUnless(record && record->type == "video" && record->path == clips[2],
                     "a record naming the video is on disk while the session whose draw failed is torn down",
                     "the record on disk: " + describeRecord(record));
        const RestartVerdict restart = startAfterDeathNow(config, kMonitorDrawFault);
        if (restartGraded(restart)) {
            check(restart.refused, "a start after dying in that teardown refuses the video");
        }
    }
    g_closeFileGate.release();
    g_submitFailGate.release();
    check(drainWorker(worker), "the pass whose draw failed finishes once its teardown is let go");
    {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMonitorDrawFault);
        const std::string error = status ? status->lastError : std::string();
        reportUnless(error.find("vkQueueSubmit") != std::string::npos,
                     "the failure the monitor reports is the draw's own submission", "last_error: " + error);
    }

    // GOAL: S13.130, a replaced video -- a session dropped because its monitor was given a still is
    //       torn down under a record naming the video that session opened, not the still now asked
    //       for.
    // WHY-THIS-SHAPE: a different file and type on purpose: S13.119's same-path form cannot tell a
    //       record naming the session's file from one naming the monitor's new request.
    // CONTROL: the `applying.emplace(...)` line deleted from closeVideoSessionLocked() in
    //       backend/daemon/wallpaper_manager.cpp -> "FAIL: a record naming the replaced video, not the
    //       still, is on disk while its session is torn down", printing "the record on disk: none".
    //       Observed 2026-09-16 on both driver entries. Reverted.
    if (!show(kMonitorReplaced, clips[3])) {
        check(false, "the monitor whose video is replaced is drawn first");
        return;
    }
    g_closeFileGate.arm(CallGate::Mode::Hold, clips[3]);
    manager.requestWallpaper("static", still, kMonitorReplaced);
    worker.post(kMonitorReplaced);
    const bool replacing = g_closeFileGate.waitFired(kGateLimit);
    check(replacing, "giving a video monitor a still tears its session down");
    if (replacing) {
        const std::optional<wallpaperx::CrashRecord> record = wallpaperx::readCrashRecord(config.stateDir, kMonitorReplaced);
        reportUnless(record && record->type == "video" && record->path == clips[3],
                     "a record naming the replaced video, not the still, is on disk while its session is torn down",
                     "the record on disk: " + describeRecord(record));
    }
    g_closeFileGate.release();
    check(signals.wait(kMonitorReplaced, 2), "and the still then commits");
}

constexpr int kEvictionMonitors[] = {24, 25, 26, 27, 28};

// S13.130's eviction half, on a manager of its own so which session is least recently drawn
// depends on nothing but this function.
void runEvictionRecordCheck(const WallpaperManager::Config& base, const std::vector<std::string>& clips) {
    const char* kGoal = "S13.130's eviction half (an evicted session is torn down under a record naming its file)";
    const wallpaperx::testing::ScratchDir scratch("videoevict");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch state directory could be created");
        return;
    }
    WallpaperManager::Config config = base;
    config.stateDir = scratch.path();

    PhaseGrade grade("the eviction phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    const auto post = [&](int i) {
        wallpaperx::testing::nameScreen(manager, kEvictionMonitors[i], screenOf(kEvictionMonitors[i]));
        manager.requestWallpaper("video", clips[i], kEvictionMonitors[i]);
        worker.post(kEvictionMonitors[i]);
    };
    post(0);
    if (!signals.wait(kEvictionMonitors[0], 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the first video render on this host produced no frame");
        return;
    }
    for (int i = 1; i <= 3; ++i) {
        post(i);
        if (!signals.wait(kEvictionMonitors[i], 1)) {
            check(false, "four video monitors are drawn before the fifth");
            return;
        }
    }

    // GOAL: S13.130, eviction -- the least recently drawn session, evicted to make room for a
    //       fifth, is torn down with a record on disk naming the EVICTED monitor and its file.
    // WHY-THIS-SHAPE: the thread is held as the evicted file is closed, and the evicted monitor's
    //       record is read -- not the drawing monitor's. The crash handler has one slot, so a record
    //       for the monitor being drawn, taken first, would leave the eviction's unwritten.
    // CONTROL: (a) in videoSessionLocked() (backend/daemon/wallpaper_manager.cpp), a copy of the
    //       `const ApplyingRecord applying(...)` line added above the eviction loop -> "FAIL: a record
    //       naming the evicted monitor's video is on disk while its session is torn down", printing
    //       "the evicted monitor's record: none". (b) the `applying.emplace(...)` line deleted from
    //       closeVideoSessionLocked() -> the same line. Both observed 2026-09-16 on both driver
    //       entries; reverted.
    g_closeFileGate.arm(CallGate::Mode::Hold, clips[0]);
    post(4);
    const bool evicting = g_closeFileGate.waitFired(kGateLimit);
    check(evicting, "drawing a fifth video monitor tears the least recently drawn session down");
    if (evicting) {
        const std::optional<wallpaperx::CrashRecord> record =
            wallpaperx::readCrashRecord(config.stateDir, kEvictionMonitors[0]);
        reportUnless(record && record->type == "video" && record->path == clips[0],
                     "a record naming the evicted monitor's video is on disk while its session is torn down",
                     "the evicted monitor's record: " + describeRecord(record));
    }
    g_closeFileGate.release();
    check(signals.wait(kEvictionMonitors[4], 1), "and the fifth monitor then commits");
}

constexpr int kMonitorShutdown = 29;

// S13.131: the manager is destroyed on a thread of this test's, so this thread can read the disk
// while the destruction is held.
void runShutdownChecks(const WallpaperManager::Config& base, const std::string& clip) {
    const char* kGoal = "S13.131 (shutdown frees a video session with no record, and the next start draws it)";
    const wallpaperx::testing::ScratchDir scratch("videoshutdown");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created for the state and config");
        return;
    }
    WallpaperManager::Config config = base;
    config.stateDir = scratch.path() / "state";
    config.configFile = scratch.path() / "config.json";

    {
        PhaseGrade grade("the shutdown phase");
        auto manager = std::make_unique<WallpaperManager>(config);
        struct CaptureIfAlive {
            PhaseGrade& grade;
            std::unique_ptr<WallpaperManager>& manager;
            ~CaptureIfAlive() {
                if (manager) grade.capture(*manager);
            }
        } captureGrade{grade, manager};

        FrameReadyLog signals;
        bool drawn = false;
        {
            RenderWorker worker(*manager, [&signals](int monitorId, uint64_t generation) {
                signals.record(monitorId, generation);
            });
            WorkerUnblock unblock{worker};
            wallpaperx::testing::nameScreen(*manager, kMonitorShutdown, screenOf(kMonitorShutdown));
            manager->requestWallpaper("video", clip, kMonitorShutdown);
            worker.post(kMonitorShutdown);
            drawn = signals.wait(kMonitorShutdown, 1);
        }
        if (!drawn) {
            grade.expectNoDevice();
            notVerified(kGoal, "the video render on this host produced no frame");
            return;
        }
        grade.capture(*manager);

        // GOAL: S13.131, the teardown -- destroying the manager frees a video session with NO crash
        //       record on disk, so a stop abandoned inside that teardown (the shutdown alarm) does
        //       not refuse, at the next start, a wallpaper that drew.
        // WHY-THIS-SHAPE: the destruction runs on a thread of this test's and is held as the file
        //       closes; the disk is read and a start is built from it at that instant.
        // CONTROL: ~WallpaperManager in backend/daemon/wallpaper_manager.cpp given a body that
        //       closes every session through closeVideoSessionLocked() under m_gpuMutex ->
        //       "FAIL: no crash record is on disk while shutdown tears a video session down", printing
        //       the record naming video_session_0.mp4, and "FAIL: a start after a shutdown abandoned in
        //       that teardown applies the video". Observed 2026-09-16 on both driver entries. Reverted.
        g_closeFileGate.arm(CallGate::Mode::Hold, clip);
        std::jthread teardown([&manager] { manager.reset(); });
        struct ReleaseFirst {
            ~ReleaseFirst() { g_closeFileGate.release(); }
        } releaseFirst;
        const bool closing = g_closeFileGate.waitFired(kGateLimit);
        check(closing, "shutting the manager down tears the video session down");
        if (closing) {
            const std::optional<wallpaperx::CrashRecord> record =
                wallpaperx::readCrashRecord(config.stateDir, kMonitorShutdown);
            reportUnless(!record.has_value(), "no crash record is on disk while shutdown tears a video session down",
                         "the record on disk: " + describeRecord(record));
            const RestartVerdict restart = startAfterDeathNow(config, kMonitorShutdown);
            if (restartGraded(restart)) {
                reportUnless(!restart.refused,
                             "a start after a shutdown abandoned in that teardown applies the video",
                             "the start refused it: " + restart.lastError);
            }
        }
        g_closeFileGate.release();
        teardown.join();
    }

    // GOAL: S13.131, the next start -- a daemon stopped while showing a video restores it at the
    //       next start owing a render, and that render opens the file and commits.
    // WHY-THIS-SHAPE: the same config and state directory the stopped manager used, so anything its
    //       shutdown left behind is read.
    // CONTROL: ~WallpaperManager in backend/daemon/wallpaper_manager.cpp given a body that writes a
    //       record file naming each session's video and never removes it -> "FAIL: the next start
    //       restores the video that was showing, owing a render rather than refusing it", printing the
    //       "not applied" refusal, and "FAIL: and that render opens the video and commits". Observed
    //       2026-09-16 on both driver entries. Reverted. Leaking an ApplyingRecord instead cannot
    //       control this: it keeps the process's one record slot, so no later record in the run is
    //       written at all, this phase's included.
    PhaseGrade grade("the start after shutdown");
    WallpaperManager restarted(config);
    CaptureGradeOnExit captureGrade{grade, restarted};
    const std::vector<int> posted = restarted.restoreSaved();
    const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(restarted, kMonitorShutdown);
    reportUnless(contains(posted, kMonitorShutdown) && status && status->lastError.empty() && status->renderOutstanding,
                 "the next start restores the video that was showing, owing a render rather than refusing it",
                 status ? "last_error: " + status->lastError : std::string("the monitor was not restored"));
    const PassCost drawn = renderCounted(restarted, kMonitorShutdown);
    reportUnless(drawn.outcome.rendered && g_files.opens(clip) == 1, "and that render opens the video and commits",
                 describe(drawn));
    // A refused monitor is never drawn, so no Device exists to grade; the checks above are the red.
    if (status && !status->lastError.empty()) grade.expectNoDevice();
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "video_wallpaper_test";

    const wallpaperx::testing::ScratchStateHome stateHome;
    if (!stateHome.ok()) {
        notVerified("every goal in this file",
                    "no scratch state directory could be created, and running without one would write this "
                    "test's crash records into the developer's own state directory");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // Before any Device is built: the layer reads its settings at instance creation.
    const DecoderMessageExcuse excuse(stateHome.path());
    if (!excuse.applied()) {
        // Not fatal and not a skip: without the filter the decoder's own messages are counted, so
        // the phases below go red with a reason that is visible rather than passing quietly.
        std::fprintf(stderr,
                     "FAIL (harness): the validation settings file that excuses %s could not be written to %s, so "
                     "ffmpeg's own decoder messages will be counted against this daemon\n",
                     wallpaperx::testing::kExcusedDecoderMessageId, excuse.file().c_str());
        ++wallpaperx::testing::g_harnessFaults;
    } else {
        std::printf("video_wallpaper_test: WARNING -- %s is excused for this process, by id, through the validation "
                    "layer's own filter (%s). It is ffmpeg's Vulkan decoder binding one reference slot per DPB "
                    "picture, reproducible with the stock ffmpeg command line. Every other validation message is "
                    "still graded, here and everywhere else.\n",
                    wallpaperx::testing::kExcusedDecoderMessageId, excuse.file().c_str());
    }
    std::fflush(stdout);

    const std::string clip = clipPath();
    if (!std::filesystem::exists(clip)) {
        notVerified("every goal in this file",
                    "the video fixture " + clip +
                        " was not generated, which means this build found no ffmpeg command line");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    try {
        std::filesystem::create_directories(testAssetDir());
        const std::string still = wallpaperx::testing::writeGradientPng(testAssetDir() + "/video_test_still.png", 32, 32);
        const std::string junk = writeUndecodable("video_test_not_a_clip.mp4");
        // Copies, one per monitor the session phases draw, so the set of open files names which
        // monitors hold a session.
        std::vector<std::string> clips;
        for (int i = 0; i < 5; ++i) {
            clips.push_back(testAssetDir() + "/video_session_" + std::to_string(i) + ".mp4");
            std::filesystem::copy_file(clip, clips.back(), std::filesystem::copy_options::overwrite_existing);
        }
        // A type this build records and cannot draw; requestWallpaper wants a readable file.
        const std::string shaderRow = writeUndecodable("video_test_undrawable.frag");

        WallpaperManager::Config config;
        config.width = kTargetWidth;
        config.height = kTargetHeight;
        // The GPU-side oracle for everything the render and export path does, with exactly one
        // message id excused above.
        config.enableValidation = !validationWaived();

        {
            PhaseGrade grade("the bus phase");
            FrameReadyLog signals;
            WallpaperManager manager(config);
            CaptureGradeOnExit captureGrade{grade, manager};

            auto serviceConn = sdbus::createSessionBusConnection(sdbus::ServiceName{DbusService::kBusName});
            DbusService service(*serviceConn, manager);
            serviceConn->enterEventLoopAsync();
            auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                            sdbus::ObjectPath{DbusService::kObjectPath});
            proxy->uponSignal("FrameReady")
                .onInterface(DbusService::kInterface)
                .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

            runBusChecks(*proxy, signals, clip, still);
        }

        runUndecodableChecks(config, junk);
        runResizeChecks(config, clip);
        runCrashRecordSpanChecks(config, clip);
        runSessionChecks(config, clips, still, shaderRow);
        runFailedPassChecks(config, clips, still);
        runEvictionRecordCheck(config, clips);
        runShutdownChecks(config, clips[0]);
    } catch (const std::exception& e) {
        // Without this an escaping sdbus::Error -- what a host with no session bus produces --
        // would abort the process, so ctest would report an opaque SIGABRT instead of the named
        // failures above.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++wallpaperx::testing::g_failures;
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
