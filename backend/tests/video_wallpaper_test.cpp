// SPDX-License-Identifier: GPL-2.0-or-later

// Step 13 Stage E1: the daemon draws a `video` wallpaper's first picture.
//
// Four phases, each with a manager of its own, because they need the manager in three different
// shapes -- with a bus, with a render worker and no bus, and with neither -- and the Device pins
// itself to the thread that created it.
//
//   over the bus        a video is drawn, served and reported (S13.114, S13.115)
//   no bus, no worker   an undecodable video fails like an undecodable image (S13.116)
//   a worker, no bus    the decode happens before the images are rebuilt, and an overtaken pass
//                       leaves the published frame reachable (S13.117, S13.118)
//   a worker, no bus    the crash record covers the decode and the decoder's teardown (S13.119)
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
// WHY A GATE RATHER THAN A SLEEP. Three of these properties are about a span inside one render
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
#include <memory>
#include <optional>
#include <string>
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
}

namespace {

// Where the render thread is held, inside the daemon's own work. Unarmed, each costs the real call
// one atomic load.
CallGate g_framebufferGate;  // the framebuffer that finishes every rebuilt image
CallGate g_decodeGate;       // a packet handed to the decoder: the span the picture is decoded in
CallGate g_freeCodecGate;    // the codec context being freed: the drawer's own teardown

// Packets sent to the wallpaper's decoder since the last reset. What makes "the decode happened
// before the images were rebuilt" an observation rather than an inference.
std::atomic<int> g_decodeCalls{0};
std::atomic<int> g_codecFrees{0};

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
    if (g_framebufferGate.enter()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return real(device, info, allocator, framebuffer);
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
    // during construction and never see the drawer's teardown at all.
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

PixelVerdict gradeCoverCrop(const wallpaperx::testing::Frame& frame) {
    PixelVerdict verdict;
    if (frame.handleType != 1 || frame.drmModifier != wallpaperx::testing::kDrmModifierLinear ||
        frame.planes.empty()) {
        verdict.reason = "this driver's export is not a LINEAR dma_buf (handle type " +
                         std::to_string(frame.handleType) + ", modifier 0x" +
                         [&] {
                             char buf[32];
                             std::snprintf(buf, sizeof(buf), "%llx",
                                           static_cast<unsigned long long>(frame.drmModifier));
                             return std::string(buf);
                         }() +
                         "), so no client can address its pixels";
        return verdict;
    }
    const wallpaperx::testing::MappedFrame mapped(frame.fd.get(), frame.memorySize);
    if (!mapped.valid()) {
        verdict.reason = "the exported fd could not be mapped: " + mapped.error();
        return verdict;
    }
    const uint64_t planeOffset = frame.planes[0].get<0>();
    const uint64_t rowPitch = frame.planes[0].get<1>();
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
    //       five more, including the undecodable-video comparison and the decode gate; the two phases
    //       that then had nothing to set up reported NOT VERIFIED rather than accusing the daemon.
    //       Observed 2026-09-15. Reverted.
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
    const PixelVerdict pixels = gradeCoverCrop(frame);
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
    const char* kGoals = "S13.117 (the decode happens before the images are rebuilt) and S13.118 (an overtaken "
                         "video pass leaves the published frame reachable)";
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

    // GOAL: S13.117 -- a redraw at a new size decodes the picture BEFORE it rebuilds the monitor's
    //       images, so the window in which the monitor has no images to show costs a blit and not
    //       a decode. False for the order a caller is pushed into by a single render call that
    //       needs its target up front: images rebuilt, then decode, then draw.
    // WHY-THIS-SHAPE: the render thread is held inside the framebuffer creation that finishes a
    //       rebuilt image, and what is read at that instant is whether any packet has been handed
    //       to a decoder during this pass. Ordering is the property, so it is observed as an
    //       ordering; the obvious alternative -- timing how long the monitor is frameless -- is an
    //       assertion on the scheduler, and a pass-level check cannot see inside the pass at all.
    //       The counter is reset immediately before the resize, so what it counts is this pass.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the decode moved below the
    //       `if (ensureTargets(monitorId, size)) rebuilt = true;` line -- the order a single
    //       decode-and-draw call forces -> "FAIL: the picture is decoded before the monitor's images are
    //       rebuilt", printing "no packet had been decoded when the image rebuild was reached", with the
    //       crash-record check below red beside it. Observed 2026-09-15. Reverted. The FIRST run of this
    //       control passed, which is why the counter is filtered to the decoder's own context: it had
    //       been counting the packets ffmpeg sends while probing the file, which precede the rebuild in
    //       either order.
    // COVERAGE-ASSERT: the gate is asserted to have fired, so "a packet was decoded first" is
    //       never concluded from a rebuild that never happened.
    g_decodeCalls.store(0);
    g_framebufferGate.arm(CallGate::Mode::Hold);
    resize("DP-ORDER", kResizedWidth, kResizedHeight);
    const bool rebuilding = g_framebufferGate.waitFired(kGateLimit);
    check(rebuilding, "a new size makes the render thread rebuild the video monitor's images");
    if (rebuilding) {
        const int decodedFirst = g_decodeCalls.load();
        check(decodedFirst > 0, "the picture is decoded before the monitor's images are rebuilt");
        if (decodedFirst == 0) {
            std::fprintf(stderr, "  no packet had been decoded when the image rebuild was reached\n");
        }
    }
    g_framebufferGate.release();
    check(signals.wait(kMonitorOrder, 2), "and the redraw commits once the rebuild is let go");

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
    g_framebufferGate.arm(CallGate::Mode::Hold);
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
    const char* kGoals = "S13.119 (the crash record covers a video's decode and its decoder's teardown)";
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the decode moved below the image rebuild, which
    //       also puts it after the `applying.reset()` that takes the record off disk for the rebuild ->
    //       "FAIL: a crash record is on disk while the video is being decoded". Observed 2026-09-15
    //       alongside the ordering control above. Reverted.
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

    // GOAL: S13.119's second half -- the record is STILL on disk while the video's drawer is torn
    //       down. Tearing a decode session down re-enters ffmpeg and the driver, so a fatal fault
    //       there has to be attributed to the file being applied, exactly as one during the decode
    //       is. False for a pass that takes its record off disk before destroying the drawer --
    //       which is what the obvious declaration order produces.
    // WHY-THIS-SHAPE: the thread is held inside the codec context being freed, which is inside
    //       ~VideoPipeline. The teardown happens before the pass returns and before FrameReady, so
    //       the gate has to be armed before the request; the first free of a codec in a pass is
    //       that teardown, because nothing here changes size or falls back mid-stream, and the
    //       count of frees is printed so a run where that stopped being true is visible.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, `drawer.emplace<std::monostate>();` moved below
    //       `applying.reset();` -- the declaration order that looks natural -> "FAIL: the crash record is
    //       still on disk while the video's decoder is torn down". Observed 2026-09-15. Reverted.
    g_codecFrees.store(0);
    g_freeCodecGate.arm(CallGate::Mode::Hold);
    manager.requestWallpaper("video", clip, kMonitorRecord);
    worker.post(kMonitorRecord);
    const bool tearingDown = g_freeCodecGate.waitFired(kGateLimit);
    check(tearingDown, "the render thread reaches the teardown of the video's decoder");
    if (tearingDown) {
        const bool onDisk = recordOnDisk(state.path(), kMonitorRecord);
        check(onDisk, "the crash record is still on disk while the video's decoder is torn down");
        std::printf("video_wallpaper_test: %d codec context(s) freed by the time the teardown was reached\n",
                    g_codecFrees.load());
    }
    g_freeCodecGate.release();
    check(signals.wait(kMonitorRecord, 2), "and the pass finishes once the teardown is let go");

    // The record does not outlive the pass that wrote it: a record left behind would refuse this
    // wallpaper at the next start, on a render that succeeded.
    check(!recordOnDisk(state.path(), kMonitorRecord), "and no record is left behind once the pass has finished");
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
