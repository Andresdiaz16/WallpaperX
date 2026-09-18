// SPDX-License-Identifier: GPL-2.0-or-later
//
// VideoPipeline, playing once and looping, and VideoDecoder's resume after a mid-stream hardware
// failure: S13.56 onwards.
//
// PICTURES ARE IDENTIFIED BY WHAT IS DRAWN IN THEM. Each counter fixture draws its frame number into
// every picture; after each call the target is read back and the number decoded from its pixels
// (support/picture_counter.h). The sequence it must match comes from libavcodec driven directly. The
// decoder decides what to drop from timestamps and packet numbers, so identifying pictures by either
// would grade that decision with its own inputs.
//
// THE FAILURE IS INJECTED at avcodec_receive_frame: AVERROR_EXTERNAL, once, on the decoder's own
// context, during one chosen call -- what ffmpeg's hardware path returns for a failed decode. No file
// makes a working hardware decoder fail mid-stream. The device still answers, so the decoder restarts
// in software.
//
// A RESUME'S COST IS COUNTED IN PICTURES, not timed: the pictures the decoder receives during the call
// that meets the failure. A wall-clock bound would be an assertion on the scheduler.
//
// ONE ENTRY PER MODE: each needs a device with or without its video extensions, and one verdict would
// report unreached states as verified.

#include "video/video_decoder.h"
#include "video/video_pipeline.h"
#include "vulkan/device.h"
#include "vulkan/gpu_fault.h"
#include "vulkan/offscreen.h"

#include "support/check.h"
#include "support/forked_child.h"
#include "support/gpu_readback.h"
#include "support/picture_counter.h"
#include "support/validation_grade.h"
#include "support/video_probe_support.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
}

#include <dlfcn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

using wallpaperx::DecodedFrame;
using wallpaperx::Device;
using wallpaperx::OffscreenTarget;
using wallpaperx::OffscreenTargetManager;
using wallpaperx::VideoDecoder;
using wallpaperx::VideoPipeline;
using wallpaperx::WallpaperContentError;
using wallpaperx::testing::CounterReference;
using wallpaperx::testing::check;
using wallpaperx::testing::fail;

constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

constexpr const char* kShaderDir = WALLPAPERX_SHADER_DIR;
constexpr uint32_t kTargetWidth = 320;
constexpr uint32_t kTargetHeight = 240;

// ------------------------------------------------------------------------- //
// Injected conditions and counters. Each is inert until a check turns it on.
// ------------------------------------------------------------------------- //

// Read while a Device is built: strips VK_KHR_video*, so every stream decodes in software.
bool g_hideVideoExtensions = false;

// The decoder's own codec context is the one with `opaque` set; avformat_find_stream_info decodes
// through the same entry points on contexts of its own, which must neither be failed nor counted.
bool isDecodersContext(const AVCodecContext* context) { return context != nullptr && context->opaque != nullptr; }

int g_receiveExternalRemaining = 0;

// Pictures the decoder received, by where they live, while a run records them.
bool g_recordPictures = false;
int g_devicePictures = 0;
int g_hostPictures = 0;

// Everything below is counted, or applies, only during the call that meets the failure.
bool g_inFailingCall = false;
int g_failingPictures = 0;
struct SeekCall {
    int64_t target;  // as the decoder asked, before any shift below
    int result;
    bool byte = false;
};
std::vector<SeekCall> g_failingSeeks;
int g_containerOpens = 0;
int g_decoderOpens = 0;
// Added to the first seek's target: a demuxer that lands past what it was asked for.
int64_t g_seekShiftSeconds = 0;
enum class SeekFailure { None, AwayFromStart, All };
SeekFailure g_seekFailure = SeekFailure::None;
// avcodec_send_packet refuses every packet while the decoder opened during the failing call is the
// g_refuseFrom-th to g_refuseThrough-th; 0 is off.
int g_refuseFrom = 0;
int g_refuseThrough = 0;
// Pictures the decoder receives lose their timestamp.
bool g_stripTimestamps = false;

void clearFailingCallConditions() {
    g_stripTimestamps = false;
    g_receiveExternalRemaining = 0;
    g_inFailingCall = false;
    g_seekShiftSeconds = 0;
    g_seekFailure = SeekFailure::None;
    g_refuseFrom = 0;
    g_refuseThrough = 0;
}

// What one call of a looping run did, counted while g_tallying is set.
struct CallTally {
    int pictures = 0;
    int devicePictures = 0;
    int hostPictures = 0;
    std::vector<SeekCall> seeks;
    int containerOpens = 0;
    int decoderOpens = 0;
    int positionsCleared = 0;
};
bool g_tallying = false;
CallTally g_tally;
// What av_seek_frame does during a tallied call. LateFirst moves the call's first timestamp seek
// kLateSeconds past its target.
enum class LoopSeek { None, FailAll, FailFirst, FailTimestamps, LateFirst };
LoopSeek g_loopSeek = LoopSeek::None;
constexpr int kLateSeconds = 2;
// av_read_frame hands out packets without a byte position.
bool g_clearPositions = false;
// avcodec_send_packet refuses every packet on the decoder's context.
bool g_refuseAllPackets = false;
int g_packetsRefused = 0;

// Vulkan creations and waits made through this executable's symbols while g_countVulkan is set.
// ffmpeg's own calls go through dispatch pointers and are not seen, which is right: the claim is
// about the pipeline's work, not ffmpeg's.
#define WALLPAPERX_VULKAN_CALLS(X)                                                                          \
    X(vkCreateImage) X(vkCreateImageView) X(vkCreateBuffer) X(vkAllocateMemory) X(vkCreateFence)             \
    X(vkCreateSemaphore) X(vkAllocateCommandBuffers) X(vkCreateCommandPool) X(vkCreateSampler)                \
    X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout) X(vkAllocateDescriptorSets)                     \
    X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateRenderPass) X(vkCreateFramebuffer)       \
    X(vkCreateShaderModule) X(vkWaitForFences) X(vkWaitSemaphores) X(vkQueueWaitIdle) X(vkDeviceWaitIdle)
#define WALLPAPERX_ENUM(name) k_##name,
enum VulkanCall { WALLPAPERX_VULKAN_CALLS(WALLPAPERX_ENUM) kVulkanCallCount };
#undef WALLPAPERX_ENUM
#define WALLPAPERX_NAME(name) #name,
constexpr const char* kVulkanCallNames[] = {WALLPAPERX_VULKAN_CALLS(WALLPAPERX_NAME)};
#undef WALLPAPERX_NAME
constexpr int kFirstWait = k_vkWaitForFences;

bool g_countVulkan = false;
std::array<int, kVulkanCallCount> g_vulkanCalls{};

struct VulkanTally {
    std::array<int, kVulkanCallCount> calls{};
    int creations() const {
        int total = 0;
        for (int i = 0; i < kFirstWait; ++i) total += calls[i];
        return total;
    }
    int waits() const {
        int total = 0;
        for (int i = kFirstWait; i < kVulkanCallCount; ++i) total += calls[i];
        return total;
    }
    std::string describe() const {
        std::string text;
        for (int i = 0; i < kVulkanCallCount; ++i) {
            if (calls[i] == 0) continue;
            text += std::string(text.empty() ? "" : ", ") + kVulkanCallNames[i] + " x" + std::to_string(calls[i]);
        }
        return text.empty() ? "none" : text;
    }
};

// Image pools: each Vulkan frames context av_hwframe_ctx_init() is given while g_trackPools is set, and
// when ffmpeg frees it. AVHWFramesContext::free is ffmpeg's public hook, called once the context's image
// pool is gone. ffmpeg's Vulkan decoder installs one of its own for its profile data, so the probe's
// calls it on; user_opaque is ffmpeg's too and is left alone, so a pool is known by its address.
constexpr int kOutsideCalls = -1;
bool g_trackPools = false;
int g_poolCall = kOutsideCalls;  // the call in progress
bool g_decoderOpen = false;      // the pipeline or decoder under test still exists
struct PoolRecord {
    int width = 0;
    int height = 0;
    int createdDuring = kOutsideCalls;
    bool freed = false;
    int freedDuring = kOutsideCalls;
    bool freedWhileOpen = false;
    int pictures = 0;  // pictures the decoder received in it
    // A copy of a picture's reference, not a reference of its own: reads the pool's count while
    // `freed` is false, and adds nothing to it.
    AVBufferRef peek{};
};
std::vector<PoolRecord> g_pools;
// Every context whose free callback the probe replaced and that is not yet freed, kept across
// resetPools() so ffmpeg's own callback is always called. `record` is into g_pools of `generation`.
struct HookedPool {
    const AVHWFramesContext* frames;
    void (*ffmpegFree)(AVHWFramesContext*);
    size_t record;
    uint64_t generation;
};
std::vector<HookedPool> g_hookedPools;
uint64_t g_poolGeneration = 0;

void resetPools() {
    g_pools.clear();
    ++g_poolGeneration;
}

PoolRecord* poolRecord(const AVHWFramesContext* frames) {
    for (const HookedPool& hooked : g_hookedPools) {
        if (hooked.frames != frames) continue;
        return hooked.generation == g_poolGeneration && hooked.record < g_pools.size() ? &g_pools[hooked.record]
                                                                                       : nullptr;
    }
    return nullptr;
}

void onPoolFreed(AVHWFramesContext* frames) {
    const auto hooked = std::find_if(g_hookedPools.begin(), g_hookedPools.end(),
                                     [frames](const HookedPool& h) { return h.frames == frames; });
    if (hooked == g_hookedPools.end()) return;
    if (PoolRecord* pool = poolRecord(frames)) {
        pool->freed = true;
        pool->freedDuring = g_poolCall;
        pool->freedWhileOpen = g_decoderOpen;
    }
    const auto ffmpegFree = hooked->ffmpegFree;
    g_hookedPools.erase(hooked);
    if (ffmpegFree != nullptr) ffmpegFree(frames);
}

// Each picture the decoder received while g_logPictures is set: its width, and where it lives.
struct LoggedPicture {
    int width;
    bool device;
};
bool g_logPictures = false;
std::vector<LoggedPicture> g_pictureLog;

// Batches whose fences vkGetFenceStatus reports as still running while listed. The GPU has finished
// them, but the Device learns completion only from that call. A wait on one is counted and ends the
// hold first, so it returns rather than lasting the Device's bound.
bool g_holdNextSubmission = false;
std::vector<VkFence> g_heldFences;
int g_waitsOnHeld = 0;
// A wait on a held batch returns VK_TIMEOUT at once instead of ending the hold: a GPU that has stopped.
bool g_stallHeld = false;
int g_stalledWaits = 0;
// The next avcodec_receive_frame on the decoder's context ends the hold, so a batch finishes inside a
// call after next() has already polled it at the top.
bool g_releaseHoldAtReceive = false;
bool g_holdReleasedAtReceive = false;

// The decoder's own codec contexts, as avcodec_open2 opened them and avcodec_free_context freed them,
// while g_trackCodecs is set. `...During` is g_poolCall at the time.
constexpr int kDestroying = -3;
bool g_trackCodecs = false;
struct CodecRecord {
    const AVCodecContext* context;
    bool hardware;
    int openedDuring;
    bool freed = false;
    int freedDuring = kOutsideCalls;
};
std::vector<CodecRecord> g_codecs;

// Added to the next device picture avcodec_receive_frame returns on the decoder's context, once.
int g_inflateWidth = 0;
int g_inflateHeight = 0;
bool g_inflated = false;

// ------------------------------------------------------------------------- //
// Fixtures
// ------------------------------------------------------------------------- //

std::string asset(const char* name) { return (std::filesystem::path(WALLPAPERX_VIDEO_ASSET_DIR) / name).string(); }

bool haveAsset(const char* name, const std::string& label) {
    if (std::filesystem::exists(asset(name))) return true;
    std::printf("%s: fixture %s was not generated (no ffmpeg command line at configure time?) -- not answerable\n",
                label.c_str(), name);
    g_notVerified = true;
    return false;
}

// The independent sequence for a fixture. A fixture that cannot give one is the harness's fault.
std::optional<CounterReference> counterReference(const std::string& label, const char* name) {
    if (!haveAsset(name, label)) return std::nullopt;
    std::string why;
    std::optional<CounterReference> reference = wallpaperx::testing::decodeCounterReference(asset(name), why);
    if (!reference) {
        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), name, why.c_str());
        fail("a counter fixture could not be decoded independently, so there is no sequence to grade against");
        return std::nullopt;
    }
    if (!reference->distinctAndReadable()) {
        std::fprintf(stderr, "FAIL: %s: %s does not name each of its %zu pictures once\n", label.c_str(), name,
                     reference->ids.size());
        fail("a counter fixture cannot show a repeat or a skip, so it cannot grade one");
        return std::nullopt;
    }
    return reference;
}

bool hardwareDecoderPresent(const Device& device, const std::string& label) {
    if (device.info().videoDecodeOps != 0) return true;
    std::printf("%s: %s has no hardware video decoder, so nothing here reaches the hardware path -- not answerable\n",
                label.c_str(), device.info().gpuName.c_str());
    g_notVerified = true;
    return false;
}

bool videoExtensionsHidden(const Device& device, const std::string& label) {
    if (device.info().videoDecodeOps == 0) return true;
    std::fprintf(stderr, "FAIL: %s: the video extensions were not hidden from %s\n", label.c_str(),
                 device.info().gpuName.c_str());
    fail("the harness did not build the software-only device this entry needs");
    return false;
}

// ------------------------------------------------------------------------- //
// Running a pipeline to the end of its stream, reading back every call's picture
// ------------------------------------------------------------------------- //

struct FailurePlan {
    int after = -1;  // pictures shown before the call that meets the failure; -1 for no failure
    std::function<void()> arm;  // further conditions for that call, cleared when it returns
};

struct FailingCall {
    bool reached = false;
    int injectionsLeft = 0;
    int devicePicturesBefore = 0;
    int picturesDecoded = 0;
    std::vector<SeekCall> seeks;
    int containerOpens = 0;
    int decoderOpens = 0;
};

struct PipelineRun {
    std::vector<int> shown;  // the counter read back after each call that returned true
    bool ended = false;
    bool overran = false;
    std::string threw;
    bool threwContentError = false;
    std::string readbackFault;
    int devicePictures = 0;
    int hostPicturesAfterFailure = 0;
    FailingCall failing;
};

PipelineRun runPipeline(Device& device, const std::string& file, size_t expected, const FailurePlan& plan = {}) {
    PipelineRun run;
    struct Clear {
        ~Clear() {
            clearFailingCallConditions();
            g_recordPictures = false;
        }
    } clear;
    OffscreenTargetManager targets(device);
    const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
    int hostBeforeFailure = 0;
    auto endFailingCall = [&] {
        run.failing.reached = true;
        run.failing.injectionsLeft = g_receiveExternalRemaining;
        run.failing.picturesDecoded = g_failingPictures;
        run.failing.seeks = g_failingSeeks;
        run.failing.containerOpens = g_containerOpens;
        run.failing.decoderOpens = g_decoderOpens;
        clearFailingCallConditions();
    };
    try {
        VideoPipeline pipeline(device, file, kShaderDir);
        // Declared after the pipeline, so it is destroyed first: a pool freed by the pipeline's
        // destructor is recorded as freed at close.
        g_decoderOpen = true;
        struct Closing {
            ~Closing() {
                g_decoderOpen = false;
                g_poolCall = kOutsideCalls;
            }
        } closing;
        g_devicePictures = 0;
        g_hostPictures = 0;
        g_recordPictures = true;
        for (size_t call = 0;; ++call) {
            g_poolCall = kOutsideCalls;
            // A pipeline that never ends is reported, not waited out.
            if (call > expected + 8) {
                run.overran = true;
                break;
            }
            const bool failing = plan.after >= 0 && call == static_cast<size_t>(plan.after);
            if (failing) {
                g_failingPictures = 0;
                g_failingSeeks.clear();
                g_containerOpens = 0;
                g_decoderOpens = 0;
                run.failing.devicePicturesBefore = g_devicePictures;
                hostBeforeFailure = g_hostPictures;
                if (plan.arm) plan.arm();
                g_receiveExternalRemaining = 1;
                g_inFailingCall = true;
            }
            bool more = false;
            g_poolCall = static_cast<int>(call);
            try {
                more = pipeline.renderNext(targets, target);
            } catch (...) {
                if (failing) endFailingCall();
                throw;
            }
            g_poolCall = kOutsideCalls;
            if (failing) endFailingCall();
            if (!more) {
                run.ended = true;
                break;
            }
            try {
                run.shown.push_back(wallpaperx::testing::counterFromBgra(
                    wallpaperx::testing::reclaimAndReadback(device, targets, target), target.width, target.height));
            } catch (const std::exception& e) {
                run.readbackFault = e.what();
                break;
            }
        }
    } catch (const WallpaperContentError& e) {
        run.threw = e.what();
        run.threwContentError = true;
    } catch (const std::exception& e) {
        run.threw = e.what();
    }
    run.devicePictures = g_devicePictures;
    run.hostPicturesAfterFailure = g_hostPictures - hostBeforeFailure;
    return run;
}

// Whether the pictures shown are the reference's, each once, in order; prints why not.
bool gradeSequence(const std::string& label, const std::string& what, const PipelineRun& run,
                   const CounterReference& reference) {
    if (!run.readbackFault.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: reading back call %zu failed: %s\n", label.c_str(), what.c_str(),
                     run.shown.size() + 1, run.readbackFault.c_str());
        fail("the harness could not read back a shown picture");
        return false;
    }
    if (!run.threw.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: the pipeline threw after %zu pictures: %s\n", label.c_str(), what.c_str(),
                     run.shown.size(), run.threw.c_str());
        fail("the pipeline threw where the stream should have played to its end");
        return false;
    }
    if (run.overran) {
        std::fprintf(stderr, "FAIL: %s: %s: still returning pictures after %zu calls, for a stream of %zu\n",
                     label.c_str(), what.c_str(), run.shown.size(), reference.ids.size());
        fail("the pipeline did not end the stream");
        return false;
    }
    if (run.shown == reference.ids) return true;

    std::vector<int> seen(1 << wallpaperx::testing::kCounterBits, 0);
    int unreadable = 0;
    for (int id : run.shown) {
        if (id < 0) {
            ++unreadable;
        } else {
            ++seen[static_cast<size_t>(id)];
        }
    }
    int repeated = 0;
    for (int count : seen) repeated += count > 1 ? 1 : 0;
    int missing = 0;
    for (int id : reference.ids) missing += seen[static_cast<size_t>(id)] == 0 ? 1 : 0;
    size_t first = 0;
    while (first < run.shown.size() && first < reference.ids.size() && run.shown[first] == reference.ids[first]) {
        ++first;
    }
    std::fprintf(stderr,
                 "FAIL: %s: %s: %zu pictures shown against %zu in an independent decode; call %zu showed %d where %d "
                 "was due; %d shown more than once, %d never shown, %d unreadable\n",
                 label.c_str(), what.c_str(), run.shown.size(), reference.ids.size(), first + 1,
                 first < run.shown.size() ? run.shown[first] : -1,
                 first < reference.ids.size() ? reference.ids[first] : -1, repeated, missing, unreadable);
    fail("the pictures shown are not the stream's pictures, each once, in order");
    return false;
}

// Whether the injected failure met a hardware decode mid-stream and software took over.
bool failureExercised(const std::string& label, const std::string& what, const PipelineRun& run) {
    const FailingCall& failing = run.failing;
    if (!failing.reached || failing.injectionsLeft != 0) {
        std::fprintf(stderr, "FAIL: %s: %s: the injected hardware failure was %s\n", label.c_str(), what.c_str(),
                     failing.reached ? "never consumed" : "never armed: the stream ended first");
        fail("the injected hardware failure never reached the decoder, so no resume was exercised");
        return false;
    }
    if (failing.devicePicturesBefore == 0) {
        std::printf("%s: %s: no picture before the failure was decoded on the device -- not answerable\n",
                    label.c_str(), what.c_str());
        g_notVerified = true;
        return false;
    }
    return true;
}

// Coverage for a failure mid-stream: pictures after it can only come from software, so none means no
// resume was exercised. Checked after the sequence, so a skip is reported as a skip.
void checkSoftwareTookOver(const std::string& label, const std::string& what, const PipelineRun& run) {
    if (run.hostPicturesAfterFailure != 0) return;
    std::fprintf(stderr, "FAIL: %s: %s: no picture came from software after the failure\n", label.c_str(),
                 what.c_str());
    fail("the decoder did not restart in software after a hardware failure, so no resume was exercised");
}

std::string failurePoint(const char* file, int after) {
    return std::string(file) + ", hardware failing after " + std::to_string(after) + " picture(s)";
}

std::string describeSeeks(const std::vector<SeekCall>& seeks) {
    std::string text;
    for (const SeekCall& seek : seeks) {
        text += (text.empty() ? "" : ", ") + std::string(seek.byte ? "byte " : "") + std::to_string(seek.target) +
                (seek.result < 0 ? " (failed)" : "");
    }
    return text.empty() ? "no seek" : "seeks to " + text;
}

// ------------------------------------------------------------------------- //
// Ordinary playback: conversion before rendering, and timestamps that do not rise
// ------------------------------------------------------------------------- //

struct SequenceFixture {
    const char* file;
    const char* timestamps;
    bool irregular;
};
const SequenceFixture kSequenceFixtures[] = {
    {"counter_long.mp4", "rising", false},
    {"counter_bframes.h264", "absent", false},
    {"counter_jump.ts", "jumping back", true},
    {"counter_repeat.mkv", "repeating", true},
};
constexpr size_t kSequenceFixtureCount = 4;

void runSequenceChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;

    // GOAL: S13.56 -- each call shows the picture it decoded, not the one before: the conversion batch
    //       is submitted ahead of the blit. And S13.57 -- timestamps that repeat or jump back drop no
    //       picture in playback without a failure.
    // WHY-THIS-SHAPE: every call's target is read back and identified by its drawn counter, against a
    //       decode that never touches VideoDecoder. A blit submitted first shows the previous picture
    //       (black on the first), which only a per-call identity sees; a frame count cannot. Reading
    //       back adds a wait after the call, never inside it, so the order graded is the pipeline's.
    // CONTROL: each reverted. The convert now lives in renderer/video/video_pipeline.cpp decodeNext() and
    //       the blit in draw(), so "rendering before converting" is renderNext() calling draw() first.
    //       Re-run there 2026-09-15: all four fixtures red on the host path, "the pipeline threw after 0
    //       pictures: VideoPipeline::draw: no picture has been decoded" -- the split turned what used to
    //       show a stale picture into a throw, which is why this text no longer says "call 1 showed -1".
    //       renderer/video/video_decoder.cpp admit()
    //       dropping, outside a resume, a picture whose timestamp is not above the last returned:
    //       counter_jump.ts "60 pictures shown against 120", counter_repeat.mkv "30 pictures shown
    //       against 60"; the rising and untimestamped fixtures stayed green.
    // COVERAGE-ASSERT: the fixture list has its four entries, and each irregular fixture's independent
    //       decode shows a timestamp not above the one before; otherwise the drop is never offered.
    check(std::size(kSequenceFixtures) == kSequenceFixtureCount,
          "the playback fixture list no longer has the four entries this check is written for");
    for (const SequenceFixture& fixture : kSequenceFixtures) {
        const std::optional<CounterReference> reference = counterReference(label, fixture.file);
        if (!reference) continue;
        if (fixture.irregular && reference->nonIncreasing == 0) {
            std::fprintf(stderr, "FAIL: %s: %s decodes with every timestamp rising\n", label.c_str(), fixture.file);
            fail("a fixture meant to carry irregular timestamps does not, so their handling was not offered");
            continue;
        }
        const PipelineRun run = runPipeline(device, asset(fixture.file), reference->ids.size());
        if (onDevice && (run.devicePictures == 0 || run.hostPicturesAfterFailure != 0)) {
            std::printf("%s: %s did not decode on the device -- not answerable for it\n", label.c_str(),
                        fixture.file);
            g_notVerified = true;
            continue;
        }
        if (!onDevice && run.devicePictures != 0) {
            fail("a software-only device produced a device picture");
            continue;
        }
        const std::string what = std::string(fixture.file) + " (timestamps " + fixture.timestamps + ")";
        if (gradeSequence(label, what, run, *reference)) {
            std::printf("%s: %s: all %zu pictures shown once, in order (%d timestamps not rising, %d absent)\n",
                        label.c_str(), what.c_str(), run.shown.size(), reference->nonIncreasing,
                        reference->untimestamped);
        }
    }
}

// ------------------------------------------------------------------------- //
// Resume after a hardware failure, on a stream with rising timestamps
// ------------------------------------------------------------------------- //

void runResumeTimestampChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_long.mp4";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    if (reference->untimestamped != 0 || reference->nonIncreasing != 0) {
        fail("the long fixture's timestamps do not all rise, so the timestamp resume is not what it offers");
        return;
    }
    const int pictures = static_cast<int>(reference->ids.size());
    const int bound = reference->longestKeyframeGap + 1;
    const int late = pictures - 30;

    // GOAL: S13.58 -- across a hardware failure at the first picture, early, and late in the stream,
    //       every picture is shown once, in order. S13.59 -- the call that meets the failure decodes
    //       at most a keyframe interval's pictures plus one, wherever the failure falls.
    // WHY-THIS-SHAPE: the cost is the pictures the decoder receives during that call, not its time.
    //       The bound is the fixture's own longest keyframe gap, read off the independent decode, not
    //       the recipe's -g. Three points, because the first picture resumes by rewinding (no earlier
    //       keyframe to seek to) while the others seek.
    // CONTROL: renderer/video/video_decoder.cpp, each reverted. reopenInSoftware() with its seek
    //       disabled, so software rewinds and drops through -- the old replay: every picture still once,
    //       the bound red, "decoded 31 pictures" at 30 in and "292" at 291 in. admit()'s DropThrough
    //       test `<=` made `<`: "1 shown more than once" at all three points. DropThrough also dropping
    //       the first picture past the resume point: "1 never shown" at all three. The rewind() dropped
    //       from reopenInSoftware()'s no-seek branch: the first point alone red, "23 never shown".
    // COVERAGE-ASSERT: the late point is at least four bounds in -- red with a keyframe every 100
    //       substituted, "321 pictures with a keyframe gap of 100" -- and each run asserts the failure
    //       met device pictures and software took over.
    if (late < 4 * bound) {
        std::fprintf(stderr, "FAIL: %s: %s: %d pictures with a keyframe gap of %d\n", label.c_str(), file, pictures,
                     reference->longestKeyframeGap);
        fail("the long fixture is too short for its keyframe interval to tell a bounded resume from a replay");
        return;
    }
    for (const int after : {1, 30, late}) {
        const PipelineRun run = runPipeline(device, asset(file), reference->ids.size(), FailurePlan{after, {}});
        const std::string what = failurePoint(file, after);
        if (!failureExercised(label, what, run)) continue;
        gradeSequence(label, what, run, *reference);
        checkSoftwareTookOver(label, what, run);
        if (run.failing.picturesDecoded > bound) {
            std::fprintf(stderr,
                         "FAIL: %s: %s: the call that met the failure decoded %d pictures, more than the %d from a "
                         "keyframe through the picture after the last one shown\n",
                         label.c_str(), what.c_str(), run.failing.picturesDecoded, bound);
            fail("resuming after a hardware failure cost more than a keyframe interval");
        }
        std::printf("%s: %s: %d pictures decoded to resume (bound %d), %zu seek(s)\n", label.c_str(), what.c_str(),
                    run.failing.picturesDecoded, bound, run.failing.seeks.size());
    }
}

// ------------------------------------------------------------------------- //
// Resume on a stream without timestamps
// ------------------------------------------------------------------------- //

void runResumeNoTimestampChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_bframes.h264";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;

    // GOAL: S13.60 -- a raw bitstream with B-frames, whose pictures carry no timestamps, shows every
    //       picture once, in order, across a hardware failure at any of these points.
    // WHY-THIS-SHAPE: several points, because which picture a resume must stop dropping at depends on
    //       where the failure falls among reordered pictures: an ordered comparison of packet numbers
    //       instead of equality is right at some points and wrong at others.
    // CONTROL: renderer/video/video_decoder.cpp admit() ending DropThroughPacket at the first picture
    //       from a packet numbered at or after the last returned one's, reverted: red at 17 and 40,
    //       "1 shown more than once", and green at the other six -- which is why there are eight.
    // COVERAGE-ASSERT: the independent decode shows no picture with a timestamp and at least one B
    //       picture, so this grades the packet resume on reordered pictures. Red with a B-frame-free
    //       stream substituted: "90 of 90 pictures untimestamped, 0 B pictures".
    if (reference->untimestamped != static_cast<int>(reference->ids.size()) || reference->bidirectional == 0) {
        std::fprintf(stderr, "FAIL: %s: %s: %d of %zu pictures untimestamped, %d B pictures\n", label.c_str(), file,
                     reference->untimestamped, reference->ids.size(), reference->bidirectional);
        fail("the raw fixture has timestamps or no B pictures, so it does not offer what this check grades");
        return;
    }
    // GOAL: S13.60, cost -- the call that meets the failure decodes no more pictures than those already
    //       shown, plus a keyframe interval and one.
    // WHY-THIS-SHAPE: a raw stream resumes by decoding from its first packet, so the cost grows with the
    //       position by design; what must not happen is a whole extra pass. The bound is the S13.75 play-once
    //       bound, read off the independent decode's longest keyframe gap, and counted, never timed. The
    //       sequence alone cannot see an extra pass: the pictures it drops are the right ones.
    // CONTROL: renderer/video/video_decoder.cpp rewind() carrying packet numbers across the rewind instead of
    //       restarting them, reverted: "after 1 picture(s): the call that met the failure decoded 92 pictures,
    //       bound 26", every point red, each sequence still exact.
    for (const int after : {1, 5, 9, 17, 26, 40, 58, 77}) {
        const PipelineRun run = runPipeline(device, asset(file), reference->ids.size(), FailurePlan{after, {}});
        const std::string what = failurePoint(file, after);
        if (!failureExercised(label, what, run)) continue;
        checkSoftwareTookOver(label, what, run);
        const int bound = after + reference->longestKeyframeGap + 1;
        if (run.failing.picturesDecoded > bound) {
            std::fprintf(stderr, "FAIL: %s: %s: the call that met the failure decoded %d pictures, bound %d\n",
                         label.c_str(), what.c_str(), run.failing.picturesDecoded, bound);
            fail("resuming a raw stream cost more than the pictures shown and a keyframe interval");
        }
        if (gradeSequence(label, what, run, *reference)) {
            std::printf("%s: %s: every picture once, %d decoded to resume (bound %d)\n", label.c_str(), what.c_str(),
                        run.failing.picturesDecoded, bound);
        }
    }
}

// ------------------------------------------------------------------------- //
// A seek that lands late
// ------------------------------------------------------------------------- //

void runLateSeekChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_long.mp4";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    const int late = static_cast<int>(reference->ids.size()) - 30;

    // GOAL: S13.61 -- a resume seek that lands past the last picture shown is detected from the first
    //       timestamp decoded, and retried further back, so nothing between is skipped.
    // WHY-THIS-SHAPE: two sources of a late landing. On the MP4 the first seek's target is moved two
    //       seconds late, so the keyframe it lands on is past the resume point, and a second, earlier
    //       seek is asserted. The transport stream lands late by itself: its seeks after 40 and 100
    //       pictures were each followed by one a second earlier. Only its sequence is asserted, since
    //       how far the demuxer overshoots is ffmpeg's.
    // CONTROL: renderer/video/video_decoder.cpp admit() taking a Seeked picture past the resume point
    //       as landed (DropThrough) instead of retrying, reverted: MP4 "22 never shown" and "1 seek(s)
    //       made after the late landing"; transport stream after 40, "9 never shown". After 100 it stayed
    //       green with two seeks: that retry comes from another branch, so that point grades less.
    // COVERAGE-ASSERT: two seconds of the fixture's pictures exceed its keyframe gap -- red with a
    //       keyframe every 100 substituted, "the seek shift is within a keyframe gap".
    constexpr int kShiftSeconds = 2;
    constexpr int kFixturePicturesPerSecond = 30;
    if (kShiftSeconds * kFixturePicturesPerSecond <= reference->longestKeyframeGap) {
        fail("the seek shift is within a keyframe gap, so the shifted seek could land in time");
        return;
    }
    const PipelineRun run = runPipeline(device, asset(file), reference->ids.size(),
                                        FailurePlan{late, [] { g_seekShiftSeconds = kShiftSeconds; }});
    const std::string what = failurePoint(file, late) + ", first seek landing late";
    if (!failureExercised(label, what, run)) return;
    gradeSequence(label, what, run, *reference);
    checkSoftwareTookOver(label, what, run);
    const std::vector<SeekCall>& seeks = run.failing.seeks;
    if (seeks.size() < 2 || seeks[1].target >= seeks[0].target) {
        std::fprintf(stderr, "FAIL: %s: %s: %zu seek(s) made after the late landing\n", label.c_str(), what.c_str(),
                     seeks.size());
        fail("a seek that landed late was not retried further back");
    }
    std::printf("%s: %s: %s, %d pictures decoded to resume\n", label.c_str(), what.c_str(),
                describeSeeks(seeks).c_str(), run.failing.picturesDecoded);

    const char* const transport = "counter_seg.ts";
    const std::optional<CounterReference> transportReference = counterReference(label, transport);
    if (!transportReference) return;
    for (const int after : {40, 100}) {
        const PipelineRun natural =
            runPipeline(device, asset(transport), transportReference->ids.size(), FailurePlan{after, {}});
        const std::string naturalWhat = failurePoint(transport, after);
        if (!failureExercised(label, naturalWhat, natural)) continue;
        gradeSequence(label, naturalWhat, natural, *transportReference);
        checkSoftwareTookOver(label, naturalWhat, natural);
        std::printf("%s: %s: %s, %d pictures decoded to resume\n", label.c_str(), naturalWhat.c_str(),
                    describeSeeks(natural.failing.seeks).c_str(), natural.failing.picturesDecoded);
    }
}

// ------------------------------------------------------------------------- //
// A failure at the end of the stream
// ------------------------------------------------------------------------- //

void runEndChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;

    // GOAL: S13.62 -- end of stream during a resume ends playback only when the seek is known to have
    //       landed: a transport-stream seek past the last packet is retried, and a landed seek whose
    //       later pictures carry no timestamp ends the stream with that one seek.
    // WHY-THIS-SHAPE: two halves, because each wrong end-of-stream decision reddens only one. The
    //       second needs its pictures' timestamps stripped: on every fixture here a landed seek meets a
    //       timestamped picture first, and without the strip "always walk back" left all of this green.
    // CONTROL: renderer/video/video_decoder.cpp next(), each reverted. End of stream during a seek never
    //       walking back: "counter_seg.ts, hardware failing after 119 picture(s): 119 pictures shown
    //       against 120 ... 1 never shown". Always walking back: "pictures after the seek untimestamped:
    //       seeks to 163840, 148480, 133120, 102400, 40960, -1024", 822 pictures decoded -- the last being
    //       the rewind's seek to packet 1's decode timestamp.
    // COVERAGE-ASSERT: failureExercised() on every run, and one seek asserted on the stripped run.
    for (const char* file : {"counter_long.mp4", "counter_seg.ts"}) {
        const std::optional<CounterReference> reference = counterReference(label, file);
        if (!reference) continue;
        const int pictures = static_cast<int>(reference->ids.size());
        const int bound = reference->longestKeyframeGap + 1;
        for (const int after : {pictures - 1, pictures}) {
            const PipelineRun run = runPipeline(device, asset(file), reference->ids.size(), FailurePlan{after, {}});
            const std::string what = failurePoint(file, after);
            if (!failureExercised(label, what, run)) continue;
            gradeSequence(label, what, run, *reference);
            if (after < pictures) checkSoftwareTookOver(label, what, run);
            if (run.failing.picturesDecoded > bound) {
                std::fprintf(stderr, "FAIL: %s: %s: the call that met the failure decoded %d pictures, bound %d\n",
                             label.c_str(), what.c_str(), run.failing.picturesDecoded, bound);
                fail("a failure at the end of the stream walked back further than a keyframe interval");
            }
            std::printf("%s: %s: %d pictures decoded, %s\n", label.c_str(), what.c_str(),
                        run.failing.picturesDecoded, describeSeeks(run.failing.seeks).c_str());
        }
    }

    const char* const file = "counter_long.mp4";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    const int pictures = static_cast<int>(reference->ids.size());
    const PipelineRun run = runPipeline(device, asset(file), reference->ids.size(),
                                        FailurePlan{pictures, [] { g_stripTimestamps = true; }});
    const std::string what = failurePoint(file, pictures) + ", pictures after the seek untimestamped";
    if (!failureExercised(label, what, run)) return;
    gradeSequence(label, what, run, *reference);
    if (run.failing.seeks.size() != 1) {
        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(),
                     describeSeeks(run.failing.seeks).c_str());
        fail("end of stream after a seek that landed walked back instead of ending the stream");
    }
    std::printf("%s: %s: %d pictures decoded, %s\n", label.c_str(), what.c_str(), run.failing.picturesDecoded,
                describeSeeks(run.failing.seeks).c_str());
}

// ------------------------------------------------------------------------- //
// A seek that fails
// ------------------------------------------------------------------------- //

void runSeekFailureChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_long.mp4";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    const int late = static_cast<int>(reference->ids.size()) - 30;

    // GOAL: S13.63 -- when the resume seek fails the decoder decodes from the first packet instead,
    //       and when every seek fails it opens the file again; either way every picture is shown once.
    // WHY-THIS-SHAPE: av_seek_frame is failed from outside, because the fixture's container seeks.
    //       "Away from the start" fails the resume seek and lets the rewind's seek to packet 1 succeed.
    // CONTROL: renderer/video/video_decoder.cpp, each reverted. reopenInSoftware()'s failed-seek branch
    //       without its rewind(): both cases "21 never shown". rewind() without its reopen once every
    //       attempt fails: the every-seek case alone red, "4 seeks, some failed; container opened 0
    //       time(s)" -- the resume seek, then decode time, presentation time and byte position -- and "21
    //       never shown".
    // COVERAGE-ASSERT: a failed seek was recorded in each run, and the reopen only in the second -- red
    //       with the seek failure never applied, "1 seeks, none failed; container opened 0 time(s)".
    struct Case {
        const char* what;
        SeekFailure failure;
        bool reopens;
    };
    for (const Case& c : {Case{"the resume seek failing", SeekFailure::AwayFromStart, false},
                          Case{"every seek failing", SeekFailure::All, true}}) {
        const SeekFailure failure = c.failure;
        const PipelineRun run = runPipeline(device, asset(file), reference->ids.size(),
                                            FailurePlan{late, [failure] { g_seekFailure = failure; }});
        const std::string what = failurePoint(file, late) + ", " + c.what;
        if (!failureExercised(label, what, run)) continue;
        const bool seekFailed = std::any_of(run.failing.seeks.begin(), run.failing.seeks.end(),
                                            [](const SeekCall& seek) { return seek.result < 0; });
        if (!seekFailed || (run.failing.containerOpens > 0) != c.reopens) {
            std::fprintf(stderr, "FAIL: %s: %s: %zu seeks, %s failed; container opened %d time(s)\n", label.c_str(),
                         what.c_str(), run.failing.seeks.size(), seekFailed ? "some" : "none",
                         run.failing.containerOpens);
            fail("the injected seek failure did not take the path this case exists for");
        }
        checkSoftwareTookOver(label, what, run);
        if (gradeSequence(label, what, run, *reference)) {
            std::printf("%s: %s: every picture once; %d decoded to resume, %d container open(s)\n", label.c_str(),
                        what.c_str(), run.failing.picturesDecoded, run.failing.containerOpens);
        }
    }
}

// ------------------------------------------------------------------------- //
// Software never decodes the packet the last picture came from
// ------------------------------------------------------------------------- //

void runDropCountChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_bframes.h264";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    constexpr int kAfter = 40;

    // GOAL: S13.64 -- when a software pass ends without decoding the packet the last shown picture
    //       came from, the decoder starts once more and drops as many pictures as were shown: every
    //       picture once where the two decoders emit the same pictures. S13.65 -- when that pass too
    //       ends first, next() throws WallpaperContentError having shown nothing again.
    // WHY-THIS-SHAPE: the first software pass is made to decode nothing by refusing its packets as
    //       damaged, which ffmpeg reports for packets it cannot parse; the second pass is left alone,
    //       or refused too. The software decoder is told apart by the order it was opened in during
    //       the failing call.
    // CONTROL: renderer/video/video_decoder.cpp, each reverted. reopenInSoftware()'s DropCount one
    //       picture short: the first case "91 pictures shown against 90 ... 1 shown more than once".
    //       next() returning the end instead of throwing with DropCount pending: the second case "40
    //       pictures shown, then the stream ended".
    // COVERAGE-ASSERT: at least two software decoders opened in the failing call -- red with the
    //       refusal never applied, "1 software decoder(s) opened".
    struct Case {
        const char* what;
        int refuseThrough;
    };
    for (const Case& c : {Case{"the first software pass decoding nothing", 1},
                          Case{"every software pass decoding nothing", INT_MAX}}) {
        const int through = c.refuseThrough;
        const PipelineRun run = runPipeline(device, asset(file), reference->ids.size(), FailurePlan{kAfter, [through] {
                                                g_refuseFrom = 1;
                                                g_refuseThrough = through;
                                            }});
        const std::string what = failurePoint(file, kAfter) + ", " + c.what;
        if (!failureExercised(label, what, run)) continue;
        if (run.failing.decoderOpens < 2) {
            std::fprintf(stderr, "FAIL: %s: %s: %d software decoder(s) opened\n", label.c_str(), what.c_str(),
                         run.failing.decoderOpens);
            fail("the resume never started a second software pass, so counting was not exercised");
            continue;
        }
        if (c.refuseThrough == 1) {
            checkSoftwareTookOver(label, what, run);
            if (gradeSequence(label, what, run, *reference)) {
                std::printf("%s: %s: every picture once after %d software decoders\n", label.c_str(), what.c_str(),
                            run.failing.decoderOpens);
            }
            continue;
        }
        const std::vector<int> before(reference->ids.begin(), reference->ids.begin() + kAfter);
        if (!run.threwContentError || run.shown != before) {
            std::fprintf(stderr, "FAIL: %s: %s: %zu pictures shown, then %s\n", label.c_str(), what.c_str(),
                         run.shown.size(),
                         run.threw.empty() ? (run.ended ? "the stream ended" : "nothing") : run.threw.c_str());
            fail("a resume that could not reach the pictures already shown did not fail as bad content");
            continue;
        }
        std::printf("%s: %s: WallpaperContentError after the %d pictures already shown: %s\n", label.c_str(),
                    what.c_str(), kAfter, run.threw.c_str());
    }
}

// ------------------------------------------------------------------------- //
// A picture that changes size mid-stream
// ------------------------------------------------------------------------- //

void runShapeChecks(Device& device, const std::string& label) {
    if (!videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_resize.h264";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;

    // GOAL: S13.66 -- a picture of a new size mid-stream is shown in the call that decodes it, by a
    //       rebuilt stage: no call shows black or the previous picture, and nothing escapes the call.
    // WHY-THIS-SHAPE: a counter drawn at both sizes and read back after every call. Black reads as
    //       unreadable, so "never black" needs no separate check. This entry is the software path; the
    //       hardware path is the shape-device entry's, S13.80 onwards.
    // CONTROL: renderer/video/video_pipeline.cpp, each reverted. The stage build moved to decodeNext();
    //       built only for the first picture there, re-observed on this entry 2026-09-15: "the pipeline
    //       threw after 30 pictures: FrameConverter: the frame's picture differs from the one this
    //       converter was built for". The rendering-before-converting mutation is now renderNext()
    //       drawing before it decodes, which throws on the FIRST call rather than showing black; that was
    //       re-observed on the sequence entry, not re-run here.
    // COVERAGE-ASSERT: the independent decode shows two picture widths -- red with a single-size stream
    //       substituted, "the resize fixture does not change size".
    if (reference->widths.size() != 2) {
        fail("the resize fixture does not change size, so a rebuild is never needed");
        return;
    }
    const PipelineRun run = runPipeline(device, asset(file), reference->ids.size());
    if (gradeSequence(label, file, run, *reference)) {
        std::printf("%s: %s: %zu pictures at widths %u then %u, each shown once\n", label.c_str(), file,
                    run.shown.size(), reference->widths[0], reference->widths[1]);
    }
}

// Every stream whose size change crashed the process on the hardware path, and the codecs and
// containers beside them. Each runs in a child of its own, so one crash cannot hide the next verdict.
struct SizeChangeFixture {
    const char* file;
    const char* what;
    VkVideoCodecOperationFlagBitsKHR codec;
    std::vector<VkExtent2D> sizes;  // each part's picture size, in order
    // The image pools ffmpeg makes for it when the device accepts every size. 0: its release is not
    // graded -- a declined size, or AV1, which ffmpeg decodes into the first size's images throughout.
    size_t pools;
    // ffmpeg makes images for the first size only, so a larger picture is one they cannot hold. Asserted
    // of each run rather than trusted.
    bool keepsFirstPool = false;
};
const std::vector<SizeChangeFixture> kSizeChangeFixtures = {
    {"counter_resize.h264", "raw H.264, 320x240 to 160x120", VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
     {{320, 240}, {160, 120}}, 2},
    {"counter_grow.h264", "raw H.264, 160x120 to 320x240", VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
     {{160, 120}, {320, 240}}, 2},
    {"counter_shrink_grow.h264", "raw H.264, 320x240 to 160x120 and back", VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
     {{320, 240}, {160, 120}, {320, 240}}, 3},
    {"counter_big.h264", "raw H.264, 640x480 to 320x240 and back", VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
     {{640, 480}, {320, 240}, {640, 480}}, 3},
    {"counter_shrink_grow.ts", "H.264 transport stream, 320x240 to 160x120 and back",
     VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, {{320, 240}, {160, 120}, {320, 240}}, 3},
    {"counter_shrink_grow.mkv", "H.264 Matroska, 320x240 to 160x120 and back",
     VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, {{320, 240}, {160, 120}, {320, 240}}, 3},
    {"counter_shrink_grow.hevc", "raw HEVC, 320x240 to 160x120 and back", VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
     {{320, 240}, {160, 120}, {320, 240}}, 0},
    {"counter_big.hevc", "raw HEVC, 640x480 to 320x240 and back", VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
     {{640, 480}, {320, 240}, {640, 480}}, 3},
    {"counter_big.obu", "AV1, 640x480 to 320x240 and back", VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
     {{640, 480}, {320, 240}, {640, 480}}, 0, true},
    {"counter_tiny.obu", "AV1, 640x480 to 160x120 and back", VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
     {{640, 480}, {160, 120}, {640, 480}}, 0, true},
    {"counter_grow.obu", "AV1, 320x240 to 640x480", VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
     {{320, 240}, {640, 480}}, 0, true},
};
constexpr size_t kSizeChangeFixtureCount = 11;

// The coded sizes this device decodes a codec's 8-bit 4:2:0 main profile at, asked of Vulkan directly
// rather than of ffmpeg or the decoder, so which part of a fixture the device declines is known first.
struct DecodeRange {
    VkExtent2D min;
    VkExtent2D max;
    bool admits(VkExtent2D size) const {
        return size.width >= min.width && size.height >= min.height && size.width <= max.width &&
               size.height <= max.height;
    }
};
std::optional<DecodeRange> decodeRange(Device& device, VkVideoCodecOperationFlagBitsKHR codec) {
    VkVideoDecodeH264ProfileInfoKHR h264{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
    h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    VkVideoDecodeH265ProfileInfoKHR h265{VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR};
    h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
    VkVideoDecodeAV1ProfileInfoKHR av1{VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR};
    av1.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
    VkVideoDecodeH264CapabilitiesKHR h264Caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
    VkVideoDecodeH265CapabilitiesKHR h265Caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR};
    VkVideoDecodeAV1CapabilitiesKHR av1Caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR};
    VkVideoDecodeCapabilitiesKHR decodeCaps{VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
    VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
    caps.pNext = &decodeCaps;
    VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    profile.videoCodecOperation = codec;
    profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    switch (codec) {
        case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
            profile.pNext = &h264;
            decodeCaps.pNext = &h264Caps;
            break;
        case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
            profile.pNext = &h265;
            decodeCaps.pNext = &h265Caps;
            break;
        case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
            profile.pNext = &av1;
            decodeCaps.pNext = &av1Caps;
            break;
        default:
            return std::nullopt;
    }
    using CapabilitiesFn = VkResult (*)(VkPhysicalDevice, const VkVideoProfileInfoKHR*, VkVideoCapabilitiesKHR*);
    const auto query = reinterpret_cast<CapabilitiesFn>(
        vkGetInstanceProcAddr(device.instance(), "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
    if (query == nullptr || query(device.physicalDevice(), &profile, &caps) != VK_SUCCESS) return std::nullopt;
    return DecodeRange{caps.minCodedExtent, caps.maxCodedExtent};
}

std::string sizeName(uint32_t width, uint32_t height) { return std::to_string(width) + "x" + std::to_string(height); }

// The pools a run's pictures were decoded into, in the order they were made.
std::vector<size_t> outputPools() {
    std::vector<size_t> pools;
    for (size_t i = 0; i < g_pools.size(); ++i) {
        if (g_pools[i].pictures > 0) pools.push_back(i);
    }
    return pools;
}

std::string describePools(const std::vector<size_t>& pools) {
    std::string text;
    for (size_t i : pools) {
        const PoolRecord& pool = g_pools[i];
        text += (text.empty() ? "" : "; ") + sizeName(pool.width, pool.height) + " made in call " +
                std::to_string(pool.createdDuring + 1) + ", " +
                (!pool.freed                                           ? "never freed"
                 : !pool.freedWhileOpen                                ? "freed when the pipeline was destroyed"
                 : pool.freedDuring == kOutsideCalls ? "freed between calls"
                                                     : "freed in call " + std::to_string(pool.freedDuring + 1));
    }
    return text.empty() ? "none" : text;
}

// Whether the run's pools were freed as they were replaced: every pool but the last during a call, at
// most two alive after any call, and none outliving the pipeline. `expected` is how many the fixture's
// size changes make.
bool gradePools(const std::string& label, const std::string& what, size_t calls, size_t expected) {
    const std::vector<size_t> pools = outputPools();
    std::printf("%s: %s: image pools: %s\n", label.c_str(), what.c_str(), describePools(pools).c_str());
    // GOAL: S13.83 -- a replaced pool is freed during playback, not only when the pipeline is destroyed,
    //       and the pools alive at once stay bounded across repeated changes. S13.86 -- no pool outlives
    //       the pipeline.
    // WHY-THIS-SHAPE: seen through AVHWFramesContext::free, which ffmpeg calls after the pool's images
    //       are freed, so "freed" is ffmpeg's own event and not an inference from a count. The bound is
    //       asserted after each call, where a readback has already waited for every batch the call
    //       submitted, so how soon the GPU finished cannot move it. Counting live contexts at the end
    //       alone would pass a decoder that frees every pool at close. A replaced pool must go in a call
    //       that showed a picture: next() also releases on its end-of-stream return, so accepting any call
    //       passed a decoder that released nothing until the stream ended.
    // CONTROL: renderer/video/video_decoder.cpp, each reverted. next() without either
    //       releaseIdleFramesContexts() call: every accepted H.264 and HEVC stream "pool 1 of 3 (320x240) was
    //       freed only when the pipeline was destroyed" and "3 image pools alive after call 59". Without only
    //       the one at its top: all six "pool 1 of 2 (320x240) was freed only by the call that ended the
    //       stream" -- before the end-of-call bound, both two-pool streams stayed green under this one.
    //       closeDecoder() without its unref loop: "the pool in use at the end outlived the pipeline", with the
    //       validation reconcile also red.
    // COVERAGE-ASSERT: the run must make the pools its size changes imply; a fixture that stopped
    //       changing size, or an interposer never reached, fails here instead of grading nothing. Seen red
    //       both ways: "0 image pool(s) decoded into, where its size changes make 2" with ffmpeg's own free
    //       callback blocking the hook, and AV1 "1 ... make 3", which is why AV1's release is not graded.
    if (pools.size() != expected) {
        std::fprintf(stderr, "FAIL: %s: %s: %zu image pool(s) decoded into, where its size changes make %zu\n",
                     label.c_str(), what.c_str(), pools.size(), expected);
        fail("the run did not make the image pools its size changes imply, so their release is not graded");
        return false;
    }
    // `calls` counts the calls that showed a picture, so a call at or past it is the one that ended the
    // stream: a pool first freed there was held for the whole of playback.
    bool good = true;
    for (size_t n = 0; n + 1 < pools.size(); ++n) {
        const PoolRecord& pool = g_pools[pools[n]];
        if (pool.freed && pool.freedWhileOpen && pool.freedDuring != kOutsideCalls &&
            pool.freedDuring < static_cast<int>(calls)) {
            continue;
        }
        std::fprintf(stderr, "FAIL: %s: %s: pool %zu of %zu (%s) was %s\n", label.c_str(), what.c_str(), n + 1,
                     pools.size(), sizeName(pool.width, pool.height).c_str(),
                     !pool.freed                                                ? "never freed"
                     : !pool.freedWhileOpen || pool.freedDuring == kOutsideCalls ? "freed only when the pipeline was destroyed"
                                                                                 : "freed only by the call that ended the stream");
        fail("a replaced image pool was not freed during playback");
        good = false;
    }
    size_t mostAlive = 0;
    size_t mostAliveAfter = 0;
    for (size_t call = 0; call < calls; ++call) {
        size_t alive = 0;
        for (size_t i : pools) {
            const PoolRecord& pool = g_pools[i];
            if (pool.createdDuring > static_cast<int>(call)) continue;
            const bool gone = pool.freed && pool.freedWhileOpen && pool.freedDuring != kOutsideCalls &&
                              pool.freedDuring <= static_cast<int>(call);
            if (!gone) ++alive;
        }
        if (alive > mostAlive) {
            mostAlive = alive;
            mostAliveAfter = call;
        }
    }
    if (mostAlive > 2) {
        std::fprintf(stderr, "FAIL: %s: %s: %zu image pools alive after call %zu\n", label.c_str(), what.c_str(),
                     mostAlive, mostAliveAfter + 1);
        fail("the image pools alive at once grew past two across size changes");
        good = false;
    }
    if (!g_pools[pools.back()].freed) {
        std::fprintf(stderr, "FAIL: %s: %s: the pool in use at the end outlived the pipeline\n", label.c_str(),
                     what.c_str());
        fail("an image pool outlived the pipeline that decoded into it");
        good = false;
    }
    return good;
}

// The first size the device cannot decode into the images it has: outside its capability range, or, for a
// codec that keeps its first images, larger than them.
std::optional<VkExtent2D> firstDeclined(const SizeChangeFixture& fixture, const DecodeRange& range) {
    const VkExtent2D first = fixture.sizes.front();
    for (const VkExtent2D& size : fixture.sizes) {
        const bool held = !fixture.keepsFirstPool || (size.width <= first.width && size.height <= first.height);
        if (!range.admits(size) || !held) return size;
    }
    return std::nullopt;
}

// The largest amount a coded extent exceeds its visible picture by: one macroblock or coding block.
constexpr uint32_t kCodingBlock = 16;

// Every picture of `file` decoded directly and shown through a converter of the probe's own, so each
// extent() and visible() read is paired with the counter drawn in that same picture.
bool gradeReportedSizes(Device& device, const std::string& label, const std::string& what,
                        const SizeChangeFixture& fixture, bool shrinksOnDevice, const CounterReference& reference) {
    struct Shown {
        int id;
        VkExtent2D extent;
        VkRect2D visible;
        bool onDevice;
    };
    struct Stage {
        Stage(Device& d, const DecodedFrame& first)
            : converter(d, first, kShaderDir), renderer(d, converter.frame()) {}
        wallpaperx::FrameConverter converter;
        wallpaperx::VideoRenderer renderer;
    };
    std::vector<Shown> shown;
    std::string threw;
    try {
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
        VideoDecoder decoder(device, asset(fixture.file));
        std::unique_ptr<Stage> stage;
        while (std::optional<DecodedFrame> frame = decoder.next()) {
            if (shown.size() > reference.ids.size() + 8) {
                threw = "the stream did not end";
                break;
            }
            Shown entry{-1, frame->extent(), frame->visible(), frame->storage() == DecodedFrame::Storage::DeviceImage};
            if (!stage || wallpaperx::FrameConverter::pictureOf(*frame) != stage->converter.picture()) {
                stage = std::make_unique<Stage>(device, *frame);
            }
            static_cast<void>(stage->converter.convert(*frame));
            targets.render(stage->renderer, target);
            entry.id = wallpaperx::testing::counterFromBgra(
                wallpaperx::testing::reclaimAndReadback(device, targets, target), target.width, target.height);
            shown.push_back(entry);
        }
        device.waitForSubmissions();
    } catch (const std::exception& e) {
        threw = e.what();
    }

    // GOAL: S13.87 -- extent() and visible() report each picture's own size after a size change: visible()
    //       is the picture at the origin, and extent() exceeds it by less than one coding block.
    // WHY-THIS-SHAPE: the decoder is driven directly, since a pipeline hides the frame, and every picture is
    //       converted and read back so the size read is paired with that picture's counter, never with its
    //       position in the output. The expected size is libavcodec's own for that counter, cropping applied.
    //       Checking extent() against the first picture's alone would pass a pool-sized extent: that picture
    //       fills its pool.
    // CONTROL: renderer/video/video_decoder.cpp, each reverted. extent() returning the image pool's size on
    //       the device path: counter_big.obu alone, "picture 30 is 320x240, reported visible 320x240 at (0,0)
    //       with a coded extent of 640x480". visible() from the pool's size: counter_big.obu, "the direct
    //       decode threw after 30 pictures: FrameConverter: the stream's visible rectangle lies outside its
    //       coded picture". visible() two pixels narrower on device frames: all eleven, "picture 0 is
    //       320x240, reported visible 318x240".
    // COVERAGE-ASSERT: every picture compared, in the reference's order; and, where the device takes a
    //       smaller size into images kept from the first, at least one device picture is smaller than them.
    std::string fault;
    size_t smallerThanFirstImages = 0;
    if (!threw.empty()) {
        fault = "the direct decode threw after " + std::to_string(shown.size()) + " pictures: " + threw;
    } else if (shown.size() != reference.ids.size()) {
        fault = std::to_string(shown.size()) + " pictures decoded against " + std::to_string(reference.ids.size());
    } else {
        for (size_t i = 0; i < shown.size() && fault.empty(); ++i) {
            const Shown& s = shown[i];
            if (s.id != reference.ids[i]) {
                fault = "call " + std::to_string(i + 1) + " showed " + std::to_string(s.id) + " where " +
                        std::to_string(reference.ids[i]) + " was due, so no size can be paired with its picture";
                break;
            }
            const auto& due = reference.sizes[i];
            const bool visibleRight = s.visible.offset.x == 0 && s.visible.offset.y == 0 &&
                                      static_cast<int>(s.visible.extent.width) == due.width &&
                                      static_cast<int>(s.visible.extent.height) == due.height;
            const bool extentRight = s.extent.width >= s.visible.extent.width &&
                                     s.extent.height >= s.visible.extent.height &&
                                     s.extent.width - s.visible.extent.width < kCodingBlock &&
                                     s.extent.height - s.visible.extent.height < kCodingBlock;
            if (!visibleRight || !extentRight) {
                fault = "picture " + std::to_string(s.id) + " is " + sizeName(due.width, due.height) +
                        ", reported visible " + sizeName(s.visible.extent.width, s.visible.extent.height) + " at (" +
                        std::to_string(s.visible.offset.x) + "," + std::to_string(s.visible.offset.y) +
                        ") with a coded extent of " + sizeName(s.extent.width, s.extent.height) + " on the " +
                        (s.onDevice ? "device" : "host");
            }
            const VkExtent2D first = fixture.sizes.front();
            if (s.onDevice && (s.visible.extent.width < first.width || s.visible.extent.height < first.height)) {
                ++smallerThanFirstImages;
            }
        }
    }
    if (fault.empty() && fixture.keepsFirstPool && shrinksOnDevice && smallerThanFirstImages == 0) {
        fault = "no device picture was smaller than the images made for the first size";
    }
    if (!fault.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(), fault.c_str());
        fail("a decoded picture did not report its own size");
        return false;
    }
    std::printf("%s: %s: every picture reports its own size; %zu device picture(s) smaller than the first size\n",
                label.c_str(), what.c_str(), smallerThanFirstImages);
    return true;
}

void runShapeDeviceFixture(Device& device, const std::string& label, const SizeChangeFixture& fixture) {
    if (!hardwareDecoderPresent(device, label)) return;
    const std::string what = std::string(fixture.file) + " (" + fixture.what + ")";
    const std::optional<CounterReference> reference = counterReference(label, fixture.file);
    if (!reference) return;
    std::vector<uint32_t> widths;
    for (const VkExtent2D& size : fixture.sizes) {
        if (std::find(widths.begin(), widths.end(), size.width) == widths.end()) widths.push_back(size.width);
    }
    if (reference->widths != widths) {
        std::fprintf(stderr, "FAIL: %s: %s: the independent decode shows %zu picture width(s), not the %zu declared\n",
                     label.c_str(), what.c_str(), reference->widths.size(), widths.size());
        fail("a size-change fixture does not change size as declared, so it grades no change");
        return;
    }
    const std::optional<DecodeRange> range = decodeRange(device, fixture.codec);
    if (!range) {
        std::printf("%s: %s: %s reports no decode capabilities for this codec -- not answerable\n", label.c_str(),
                    what.c_str(), device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }
    if (!range->admits(fixture.sizes.front())) {
        std::printf("%s: %s: %s declines the first size, so no change happens on the hardware path -- not "
                    "answerable\n",
                    label.c_str(), what.c_str(), device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }
    const std::optional<VkExtent2D> declined = firstDeclined(fixture, *range);

    resetPools();
    g_pictureLog.clear();
    g_trackPools = true;
    g_logPictures = true;
    const PipelineRun run = runPipeline(device, asset(fixture.file), reference->ids.size());
    g_trackPools = false;
    g_logPictures = false;

    // COVERAGE-ASSERT: S13.88 -- a fixture declared to keep its first images made exactly one image pool, of
    //       the first size, so a larger picture is one those images cannot hold rather than one this probe
    //       assumed. Seen red with counter_big.h264 declared to keep them: "its image pools were: 640x480 made
    //       in call 1, freed in call 32; 320x240 made in call 29, ...; 640x480 made in call 59, ...".
    if (fixture.keepsFirstPool && run.threw.empty()) {
        const std::vector<size_t> pools = outputPools();
        const VkExtent2D first = fixture.sizes.front();
        if (pools.size() != 1 || g_pools[pools[0]].width != static_cast<int>(first.width) ||
            g_pools[pools[0]].height != static_cast<int>(first.height)) {
            std::fprintf(stderr, "FAIL: %s: %s: declared to keep its first images, but its image pools were: %s\n",
                         label.c_str(), what.c_str(), describePools(pools).c_str());
            fail("ffmpeg did not keep the first size's images, so which size they cannot hold is not known");
            return;
        }
    }

    // GOAL: S13.80 -- a size change on the hardware path does not kill the process, and every picture is
    //       shown once, in order, across it: the process surviving is graded by the parent that forked
    //       this child. S13.81 -- when the device accepts every size, every picture is decoded on the
    //       device. S13.82 -- a size the device declines continues in software from that size, with no
    //       exception, and was declined by the device's own capability answer, not after a yes.
    // WHY-THIS-SHAPE: the drawn counter read back after every call, against libavcodec decoding the file
    //       directly. Where each picture was decoded is logged at avcodec_receive_frame with its width.
    //       Which size is declined comes from vkGetPhysicalDeviceVideoCapabilitiesKHR, never from what
    //       the decoder did, so a decoder that wrongly refused an accepted size cannot grade itself right.
    // CONTROL: each in renderer/video/video_decoder.cpp attachHardwareFrames(), reverted. The crash is the
    //       parent's (superviseChild). Declining every size once the path is Hardware: all seven streams the
    //       device accepts at every size, "60 picture(s) decoded in software and 28 on the device, where the
    //       device accepts every size" (90 or 66 for the three-part ones), with "1 image pool(s) decoded
    //       into"; the three AV1 streams and the declined HEVC stayed green, as none asks again. The
    //       failed-parameters refusal throwing WallpaperContentError: the declined HEVC alone, "the pipeline
    //       threw after 28 pictures" and "raised WallpaperContentError"; the AV1 declines, made in
    //       allocatePicture(), stayed green.
    //       renderer/video/video_pipeline.cpp's rendering-before-converting mutation is now renderNext()
    //       calling draw() before decodeNext(); since the split, draw() with nothing decoded throws
    //       instead of showing a stale picture, so the recorded "call 1 showed -1" no longer describes it.
    //       Re-observed 2026-09-15 on the software sequence entry ("the pipeline threw after 0 pictures:
    //       VideoPipeline::draw: no picture has been decoded"); these forked per-fixture device runs were
    //       not repeated for it.
    // CONTROL (S13.88): renderer/video/video_decoder.cpp holds(), each reverted. Without its minimum-size
    //       test: counter_tiny.obu "no picture at the declined size was decoded in software" and "20 Vulkan
    //       validation warning(s)", VUID-VkVideoBeginCodingInfoKHR-pPictureResource-07243 and
    //       VUID-vkCmdDecodeVideoKHR-pDecodeInfo-07145. Without its test against the pool's size:
    //       counter_grow.obu "the pipeline threw after 30 pictures: DecodedFrame: ffmpeg decoded a picture
    //       larger than the image it is in", with no validation message.
    bool good = gradeSequence(label, what, run, *reference);
    if (!run.threw.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: a stream that changes size raised %s\n", label.c_str(), what.c_str(),
                     run.threwContentError ? "WallpaperContentError" : "an exception other than bad content");
        fail("a size change on the hardware path was reported as a fault");
    }
    int devicePictures = 0;
    int hostPictures = 0;
    for (const LoggedPicture& picture : g_pictureLog) ++(picture.device ? devicePictures : hostPictures);
    if (!declined) {
        if (hostPictures != 0 || devicePictures == 0) {
            std::fprintf(stderr, "FAIL: %s: %s: %d picture(s) decoded in software and %d on the device, where the "
                                 "device accepts every size\n",
                         label.c_str(), what.c_str(), hostPictures, devicePictures);
            fail("the hardware path did not continue after a size change the device accepts");
            good = false;
        }
        if (fixture.pools == 0) {
            std::printf("%s: %s: image pools, not graded: %s\n", label.c_str(), what.c_str(),
                        describePools(outputPools()).c_str());
        } else {
            good = gradePools(label, what, run.shown.size(), fixture.pools) && good;
        }
    } else {
        int deviceBefore = 0;
        int hostAtDeclined = 0;
        bool sawHost = false;
        for (const LoggedPicture& picture : g_pictureLog) {
            if (!picture.device && picture.width == static_cast<int>(declined->width)) ++hostAtDeclined;
            if (picture.device && !sawHost) ++deviceBefore;
            if (!picture.device) sawHost = true;
        }
        std::printf("%s: %s: %s declines %s (decodes %s to %s); %d picture(s) on the device before it, %d in "
                    "software at it\n",
                    label.c_str(), what.c_str(), device.info().gpuName.c_str(),
                    sizeName(declined->width, declined->height).c_str(), sizeName(range->min.width, range->min.height).c_str(),
                    sizeName(range->max.width, range->max.height).c_str(), deviceBefore, hostAtDeclined);
        // COVERAGE-ASSERT: the device decoded before the declined size and software decoded at it, so the
        //       refusal was met mid-stream rather than at the start or never. Red under the refusal that
        //       throws, above: "no picture at the declined size was decoded in software".
        if (deviceBefore == 0) {
            std::printf("%s: %s: nothing was decoded on the device before the declined size -- not answerable\n",
                        label.c_str(), what.c_str());
            g_notVerified = true;
            return;
        }
        if (hostAtDeclined == 0) {
            std::fprintf(stderr, "FAIL: %s: %s: no picture at the declined size was decoded in software\n",
                         label.c_str(), what.c_str());
            fail("the declined size was never decoded in software, so the refusal mid-stream was not exercised");
            good = false;
        }
    }
    good = gradeReportedSizes(device, label, what, fixture, !declined, *reference) && good;
    if (good) {
        std::printf("%s: %s: %zu pictures, each shown once; %d on the device, %d in software\n", label.c_str(),
                    what.c_str(), run.shown.size(), devicePictures, hostPictures);
    }
}

// ------------------------------------------------------------------------- //
// A device picture larger than its image
// ------------------------------------------------------------------------- //

void runOversizeChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_resize.h264";
    if (!haveAsset(file, label)) return;

    // GOAL: S13.89 -- a device picture larger than the image it is in cannot be constructed: next() throws
    //       std::logic_error for one a column wider, and for one a row taller.
    // WHY-THIS-SHAPE: no stream makes ffmpeg hand out such a picture now that the decoder declines them, so
    //       the size ffmpeg reports is enlarged at avcodec_receive_frame, on a picture that otherwise fills
    //       its image exactly. Two cases, because the guard tests the dimensions separately.
    // CONTROL: renderer/video/video_decoder.cpp DecodedFrame's constructor, each reverted. Its width half
    //       deleted: the wider case "next() returned a picture reporting 321x240". Its height half deleted:
    //       the taller case "next() returned a picture reporting 320x241".
    // COVERAGE-ASSERT: the enlargement landed on a device picture, and the picture before it filled its
    //       image -- red with the enlargement never armed, "the enlargement was never applied".
    struct Case {
        const char* what;
        int wider;
        int taller;
    };
    for (const Case& c : {Case{"a column wider", 1, 0}, Case{"a row taller", 0, 1}}) {
        const std::string what = std::string(file) + ", a device picture " + c.what + " than its image";
        std::string caught;
        std::string other;
        std::optional<VkExtent2D> returned;
        VkExtent2D filled{};
        bool firstOnDevice = false;
        resetPools();
        g_inflated = false;
        try {
            g_trackPools = true;
            VideoDecoder decoder(device, asset(file));
            std::optional<DecodedFrame> first = decoder.next();
            firstOnDevice = first && first->storage() == DecodedFrame::Storage::DeviceImage;
            if (firstOnDevice) {
                filled = first->extent();
                first.reset();
                g_inflateWidth = c.wider;
                g_inflateHeight = c.taller;
                try {
                    std::optional<DecodedFrame> enlarged = decoder.next();
                    if (enlarged) returned = enlarged->extent();
                } catch (const std::logic_error& e) {
                    caught = e.what();
                }
                g_inflateWidth = 0;
                g_inflateHeight = 0;
            }
            g_trackPools = false;
        } catch (const std::exception& e) {
            g_trackPools = false;
            g_inflateWidth = 0;
            g_inflateHeight = 0;
            other = e.what();
        }
        if (!firstOnDevice) {
            std::printf("%s: %s: the first picture was not decoded on the device -- not answerable\n", label.c_str(),
                        what.c_str());
            g_notVerified = true;
            continue;
        }
        if (!other.empty()) {
            std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(), other.c_str());
            fail("the decoder threw something other than std::logic_error around an enlarged picture");
            continue;
        }
        const std::vector<size_t> pools = outputPools();
        if (!g_inflated || pools.empty() || g_pools[pools[0]].width != static_cast<int>(filled.width) ||
            g_pools[pools[0]].height != static_cast<int>(filled.height)) {
            std::fprintf(stderr, "FAIL: %s: %s: the enlargement was %s; image pools: %s\n", label.c_str(),
                         what.c_str(), g_inflated ? "applied" : "never applied", describePools(pools).c_str());
            fail("the enlarged picture was not one a column or row larger than its image");
            continue;
        }
        if (caught.find("larger than the image") == std::string::npos) {
            std::fprintf(stderr, "FAIL: %s: %s: next() %s\n", label.c_str(), what.c_str(),
                         returned ? ("returned a picture reporting " + sizeName(returned->width, returned->height)).c_str()
                         : caught.empty() ? "returned nothing"
                                          : ("threw another logic_error: " + caught).c_str());
            fail("a device picture larger than its image was constructed");
            continue;
        }
        std::printf("%s: %s: next() threw std::logic_error: %s\n", label.c_str(), what.c_str(), caught.c_str());
    }
}

// ------------------------------------------------------------------------- //
// Giving up a hardware decoder for software: what waits, and when it is freed
// ------------------------------------------------------------------------- //

// A decoder driven directly, one call at a time, with the batch submitted before each call held until the
// call that falls back to software. What every call did is kept.
struct DirectCall {
    bool returned = false;
    bool onDevice = false;
    bool openedSoftware = false;
    int waits = 0;
};

class RetireHarness {
public:
    RetireHarness(Device& device, const std::string& file) : m_device(device) {
        resetPools();
        g_codecs.clear();
        g_waitsOnHeld = 0;
        g_stalledWaits = 0;
        g_holdReleasedAtReceive = false;
        g_trackPools = true;
        g_trackCodecs = true;
        decoder.emplace(device, file);
        g_decoderOpen = true;
    }
    ~RetireHarness() {
        g_countVulkan = false;
        g_stallHeld = false;
        g_releaseHoldAtReceive = false;
        g_holdNextSubmission = false;
        g_heldFences.clear();
        g_poolCall = kDestroying;
        decoder.reset();
        g_poolCall = kOutsideCalls;
        g_decoderOpen = false;
        g_trackPools = false;
        g_trackCodecs = false;
    }
    RetireHarness(const RetireHarness&) = delete;
    RetireHarness& operator=(const RetireHarness&) = delete;

    // Holds a fresh batch in place of the last one.
    void holdNewBatch() {
        g_heldFences.clear();
        g_holdNextSubmission = true;
        held = m_device.submit([](VkCommandBuffer) {});
        g_holdNextSubmission = false;
    }

    DirectCall call() {
        // Before the call, so a call that waits on the held batch -- which ends the hold -- is still known to
        // have begun with it running.
        heldBeforeCall = holdSeen();
        const size_t codecsBefore = g_codecs.size();
        g_vulkanCalls.fill(0);
        g_poolCall = static_cast<int>(calls.size());
        g_countVulkan = true;
        std::optional<DecodedFrame> frame;
        try {
            frame = decoder->next();
        } catch (...) {
            g_countVulkan = false;
            g_poolCall = kOutsideCalls;
            throw;
        }
        g_countVulkan = false;
        g_poolCall = kOutsideCalls;
        DirectCall result;
        VulkanTally tally;
        tally.calls = g_vulkanCalls;
        result.waits = tally.waits();
        result.returned = frame.has_value();
        result.onDevice = frame && frame->storage() == DecodedFrame::Storage::DeviceImage;
        frame.reset();
        for (size_t i = codecsBefore; i < g_codecs.size(); ++i) result.openedSoftware |= !g_codecs[i].hardware;
        calls.push_back(result);
        return result;
    }

    // Tears the decoder down, counting its Vulkan waits.
    int destroy() {
        g_vulkanCalls.fill(0);
        g_poolCall = kDestroying;
        g_countVulkan = true;
        decoder.reset();
        g_countVulkan = false;
        g_poolCall = kOutsideCalls;
        g_decoderOpen = false;
        VulkanTally tally;
        tally.calls = g_vulkanCalls;
        return tally.waits();
    }

    // The interposer's own record first: a batch that was never held can still be running a moment after it
    // was submitted, and asking the Device alone would grade how fast the GPU was.
    bool holdSeen() { return held && !g_heldFences.empty() && !m_device.hasCompleted(*held); }

    // The hardware codec the decoder opened at construction, or null.
    static const CodecRecord* hardwareCodec() {
        for (const CodecRecord& codec : g_codecs) {
            if (codec.hardware && codec.openedDuring == kOutsideCalls) return &codec;
        }
        return nullptr;
    }
    static const CodecRecord* softwareCodec() {
        for (const CodecRecord& codec : g_codecs) {
            if (!codec.hardware) return &codec;
        }
        return nullptr;
    }

    std::optional<VideoDecoder> decoder;
    std::optional<Device::SubmitTicket> held;
    std::vector<DirectCall> calls;
    bool heldBeforeCall = false;

private:
    Device& m_device;
};

std::string codecFate(const CodecRecord* codec) {
    if (codec == nullptr) return "never opened";
    if (!codec->freed) return "never freed";
    if (codec->freedDuring == kDestroying) return "freed at teardown";
    if (codec->freedDuring == kOutsideCalls) return "freed outside a call";
    return "freed in call " + std::to_string(codec->freedDuring + 1);
}

// The fixture's declined size on this device, or a skip with a reason.
std::optional<VkExtent2D> declinedHere(Device& device, const std::string& label, const SizeChangeFixture& fixture) {
    if (!hardwareDecoderPresent(device, label) || !haveAsset(fixture.file, label)) return std::nullopt;
    const std::optional<DecodeRange> range = decodeRange(device, fixture.codec);
    if (!range || !range->admits(fixture.sizes.front())) {
        std::printf("%s: %s: this device does not decode the first size -- not answerable\n", label.c_str(),
                    fixture.file);
        g_notVerified = true;
        return std::nullopt;
    }
    std::optional<VkExtent2D> declined = firstDeclined(fixture, *range);
    if (!declined) {
        std::printf("%s: %s: this device declines no size of it, so nothing falls back -- not answerable\n",
                    label.c_str(), fixture.file);
        g_notVerified = true;
    }
    return declined;
}

// Calls with a fresh batch held before each until one opens a software decoder. The index of that call, or
// a reason in `stopped`.
std::optional<size_t> callUntilFallback(RetireHarness& harness, std::string& stopped) {
    for (size_t n = 0; n < 400; ++n) {
        harness.holdNewBatch();
        const DirectCall call = harness.call();
        if (call.openedSoftware) return harness.calls.size() - 1;
        if (!call.returned) {
            stopped = "the stream ended on the device after " + std::to_string(harness.calls.size() - 1) + " pictures";
            return std::nullopt;
        }
    }
    stopped = "no fallback in 400 calls";
    return std::nullopt;
}

int devicePicturesBefore(const RetireHarness& harness, size_t call) {
    int n = 0;
    for (size_t i = 0; i < call; ++i) n += harness.calls[i].onDevice ? 1 : 0;
    return n;
}

void runRetireWaitChecks(Device& device, const std::string& label, const SizeChangeFixture& fixture) {
    if (!declinedHere(device, label, fixture)) return;
    const std::string what = std::string(fixture.file) + ", the fallback with a batch held";

    // GOAL: S13.90 -- falling back to software after a declined size makes no Device wait on the playback
    //       path with the last batch before it held unfinished, frees the hardware codec in none of those
    //       calls, and frees it in the first call after that batch completes, with no wait.
    // WHY-THIS-SHAPE: VideoDecoder driven directly, a fresh batch held before every call until the one that
    //       opens a software decoder, so the codec it retires names a batch the Device reports running. Codec
    //       frees are seen at avcodec_free_context and waits at this executable's Vulkan symbols, never
    //       timed; a wait on a held batch ends the hold, so a decoder that waits is counted rather than stalled.
    //       Grading the outcome alone (every picture once) passed the blocking fallback: it waited, then played.
    //       The fallback call's waits are graded before anything that needs the hold to have survived it: a
    //       wait ends the hold, and a check that first demanded a running batch reported that instead.
    // CONTROL: renderer/video/video_decoder.cpp, each reverted. reopenInSoftware() calling closeDecoder()
    //       instead of retireDecoder(): all three streams, "the fallback call 29 made 1 wait(s), 1 on the held
    //       batch" (call 31 for both AV1 streams). releaseRetiredCodec() without its hasCompleted() poll: "the
    //       hardware codec was freed in call 30, while its last reader was still running" (call 32 for AV1).
    //       next() without its first releaseRetiredCodec(): "the hardware codec was never freed by call 33,
    //       the first after its last reader completed" (35 for AV1); the end-of-stream cases stayed green.
    // COVERAGE-ASSERT: the fallback call follows device pictures and began with the batch held, and the
    //       Device still reports it unfinished after that call. "Held" needs the interposer's record as well
    //       as the Device's answer: with the hold never applied, asking the Device alone passed the first
    //       assert, the empty batch still running; with the record, "the batch was not held when the fallback
    //       call began" on all three streams.
    constexpr int kHeldCalls = 3;
    std::string stopped;
    std::string threw;
    std::optional<size_t> fallback;
    int deviceBefore = 0;
    bool holdSeen = false;
    bool heldBeforeFallback = false;
    int fallbackWaits = -1;
    int fallbackWaitsOnHeld = 0;
    int heldWaits = 0;
    size_t heldCalls = 0;
    int releaseWaits = -1;
    size_t releaseCall = 0;
    std::optional<CodecRecord> hardwareAtRelease;
    try {
        RetireHarness harness(device, asset(fixture.file));
        fallback = callUntilFallback(harness, stopped);
        if (fallback) {
            deviceBefore = devicePicturesBefore(harness, *fallback);
            fallbackWaits = harness.calls[*fallback].waits;
            fallbackWaitsOnHeld = g_waitsOnHeld;
            heldBeforeFallback = harness.heldBeforeCall;
            holdSeen = harness.holdSeen();
            for (; heldCalls < kHeldCalls; ++heldCalls) {
                const DirectCall call = harness.call();
                heldWaits += call.waits;
                if (!call.returned) break;
            }
            g_heldFences.clear();
            device.waitForSubmissions();
            releaseCall = harness.calls.size();
            releaseWaits = harness.call().waits;
            if (const CodecRecord* hardware = RetireHarness::hardwareCodec()) hardwareAtRelease = *hardware;
        }
    } catch (const std::exception& e) {
        threw = e.what();
    }
    if (!threw.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: the decoder threw: %s\n", label.c_str(), what.c_str(), threw.c_str());
        fail("a fallback run threw");
        return;
    }
    if (!fallback) {
        std::fprintf(stderr, "FAIL: %s: %s: no fallback was reached: %s\n", label.c_str(), what.c_str(), stopped.c_str());
        fail("the declined size did not open a software decoder, so the fallback was not exercised");
        return;
    }
    if (deviceBefore == 0) {
        std::printf("%s: %s: nothing was decoded on the device before the fallback -- not answerable\n",
                    label.c_str(), what.c_str());
        g_notVerified = true;
        return;
    }
    if (!heldBeforeFallback) {
        std::fprintf(stderr, "FAIL: %s: %s: the batch was not held when the fallback call began\n", label.c_str(),
                     what.c_str());
        fail("the fallback was not reached with a batch held, so no wait could be seen");
        return;
    }
    // A wait on the held batch ends the hold, so after one the held calls below would grade nothing.
    if (fallbackWaits != 0 || fallbackWaitsOnHeld != 0) {
        std::fprintf(stderr, "FAIL: %s: %s: the fallback call %zu made %d wait(s), %d on the held batch; the calls "
                             "after it are not graded, since that wait ended the hold\n",
                     label.c_str(), what.c_str(), *fallback + 1, fallbackWaits, fallbackWaitsOnHeld);
        fail("giving up a hardware decoder for software waited on the playback path");
        return;
    }
    if (!holdSeen || heldCalls != kHeldCalls || !hardwareAtRelease) {
        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(),
                     !holdSeen                ? "the Device reported the held batch complete after the fallback call"
                     : heldCalls != kHeldCalls ? "the stream ended within the held calls"
                                               : "no hardware codec was opened");
        fail("the held calls after the fallback were not reached with the batch held");
        return;
    }
    const CodecRecord& hardware = *hardwareAtRelease;
    std::printf("%s: %s: fallback in call %zu after %d device picture(s); the fallback call made %d wait(s), %zu "
                "held calls %d, release call %zu %d, %d on a held batch; the hardware codec was %s\n",
                label.c_str(), what.c_str(), *fallback + 1, deviceBefore, fallbackWaits, heldCalls, heldWaits,
                releaseCall + 1, releaseWaits, g_waitsOnHeld, codecFate(&hardware).c_str());
    if (hardware.freed && hardware.freedDuring != static_cast<int>(releaseCall)) {
        std::fprintf(stderr, "FAIL: %s: %s: the hardware codec was %s, while its last reader was still running\n",
                     label.c_str(), what.c_str(), codecFate(&hardware).c_str());
        fail("a retired hardware codec was freed before the last batch that could read its images completed");
    } else if (!hardware.freed) {
        std::fprintf(stderr, "FAIL: %s: %s: the hardware codec was never freed by call %zu, the first after its last "
                             "reader completed\n",
                     label.c_str(), what.c_str(), releaseCall + 1);
        fail("a retired hardware codec was not freed once its last reader had completed");
    }
    if (fallbackWaits != 0 || heldWaits != 0 || releaseWaits != 0 || g_waitsOnHeld != 0) {
        std::fprintf(stderr, "FAIL: %s: %s: waits on the playback path, as above\n", label.c_str(), what.c_str());
        fail("giving up a hardware decoder for software waited on the playback path");
    }
}

void runRetireEndChecks(Device& device, const std::string& label, const SizeChangeFixture& fixture) {
    if (!declinedHere(device, label, fixture)) return;
    const std::optional<CounterReference> reference = counterReference(label, fixture.file);
    if (!reference) return;
    const std::string what = std::string(fixture.file) + ", played once, its last reader finishing in the ending call";

    // GOAL: S13.93 -- a retired hardware decoder whose last reader finishes during the end-of-stream call is
    //       freed by that call, and so are the image pools it held, rather than left to the decoder's teardown.
    // WHY-THIS-SHAPE: the batch held at the fallback stays held through the rest of the stream, and is let go
    //       at the ending call's first avcodec_receive_frame -- after next() has polled it at the top, so only
    //       a release made on the way out of that call can free the codec. Releasing it between calls instead
    //       would let the top-of-call poll free it and grade nothing about the end.
    // CONTROL: renderer/video/video_decoder.cpp next(), each reverted. Without the releaseRetiredCodec() on
    //       its end-of-stream return: all three streams, "the hardware codec was freed at teardown, not in
    //       ending call 91", and every pool "was freed at teardown", since the codec still held them. Without
    //       the releaseIdleFramesContexts() there: the codec still freed in the ending call, and "pool 1 of 1
    //       (320x240) was freed at teardown" on HEVC, "pool 1 of 2" on both AV1 streams.
    // COVERAGE-ASSERT: the ending call is the one after the last picture, returned nothing, and released the
    //       hold inside itself; and the codec and pools were still alive when it began.
    const size_t pictures = reference->ids.size();
    std::string stopped;
    std::string threw;
    std::optional<size_t> fallback;
    int deviceBefore = 0;
    bool holdSeen = false;
    size_t returned = 0;
    std::optional<size_t> endingCall;
    bool aliveBeforeEnd = false;
    try {
        RetireHarness harness(device, asset(fixture.file));
        fallback = callUntilFallback(harness, stopped);
        if (fallback) {
            deviceBefore = devicePicturesBefore(harness, *fallback);
            holdSeen = harness.holdSeen();
            for (const DirectCall& call : harness.calls) returned += call.returned ? 1 : 0;
            while (returned < pictures && harness.calls.size() < pictures + 8) {
                if (!harness.call().returned) break;
                ++returned;
            }
            const CodecRecord* hardware = RetireHarness::hardwareCodec();
            aliveBeforeEnd = hardware != nullptr && !hardware->freed && harness.holdSeen();
            for (const PoolRecord& pool : g_pools) aliveBeforeEnd = aliveBeforeEnd && !pool.freed;
            endingCall = harness.calls.size();
            g_releaseHoldAtReceive = true;
            const DirectCall ending = harness.call();
            g_releaseHoldAtReceive = false;
            if (ending.returned) endingCall.reset();
            device.waitForSubmissions();
        }
    } catch (const std::exception& e) {
        threw = e.what();
    }
    if (!threw.empty() || !fallback) {
        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(),
                     threw.empty() ? ("no fallback was reached: " + stopped).c_str() : ("threw: " + threw).c_str());
        fail("the end-of-stream run did not reach a retired decoder at its end");
        return;
    }
    if (deviceBefore == 0) {
        std::printf("%s: %s: nothing was decoded on the device before the fallback -- not answerable\n",
                    label.c_str(), what.c_str());
        g_notVerified = true;
        return;
    }
    const CodecRecord* hardware = RetireHarness::hardwareCodec();
    if (!holdSeen || returned != pictures || !endingCall || !g_holdReleasedAtReceive || !aliveBeforeEnd ||
        hardware == nullptr || g_pools.empty()) {
        std::fprintf(stderr,
                     "FAIL: %s: %s: held batch seen %s after the fallback (%d wait(s) on it), %zu of %zu pictures "
                     "returned, ending call %s, hold %s released inside it, codec and pools %s alive before it, %zu "
                     "pool(s)\n",
                     label.c_str(), what.c_str(), holdSeen ? "running" : "complete", g_waitsOnHeld, returned, pictures,
                     endingCall ? "returned nothing" : "returned a picture or was not made",
                     g_holdReleasedAtReceive ? "was" : "was not", aliveBeforeEnd ? "were" : "were not", g_pools.size());
        fail("the ending call was not reached with a retired decoder whose last reader was still running");
        return;
    }
    const int ending = static_cast<int>(*endingCall);
    std::printf("%s: %s: fallback in call %zu; ending call %d; the hardware codec was %s\n", label.c_str(),
                what.c_str(), *fallback + 1, ending + 1, codecFate(hardware).c_str());
    if (!hardware->freed || hardware->freedDuring != ending) {
        std::fprintf(stderr, "FAIL: %s: %s: the hardware codec was %s, not in ending call %d\n", label.c_str(),
                     what.c_str(), codecFate(hardware).c_str(), ending + 1);
        fail("a retired hardware decoder whose last reader finished was not freed by the end-of-stream call");
    }
    for (size_t i = 0; i < g_pools.size(); ++i) {
        const PoolRecord& pool = g_pools[i];
        if (pool.freed && pool.freedDuring == ending) continue;
        std::fprintf(stderr, "FAIL: %s: %s: pool %zu of %zu (%s) was %s\n", label.c_str(), what.c_str(), i + 1,
                     g_pools.size(), sizeName(pool.width, pool.height).c_str(),
                     !pool.freed                        ? "never freed"
                     : pool.freedDuring == kDestroying ? "freed at teardown"
                                                       : ("freed in call " + std::to_string(pool.freedDuring + 1)).c_str());
        fail("an image pool of a retired decoder was not freed by the end-of-stream call");
    }
}

void runStalledTeardownChecks(Device& device, const std::string& label, const SizeChangeFixture& fixture) {
    if (!declinedHere(device, label, fixture)) return;
    const std::string what = std::string(fixture.file) + ", torn down after the fallback on a stalled GPU";

    // GOAL: S13.91 -- tearing a decoder down after a fallback, on a GPU whose wait times out, frees the
    //       software codec while leaving the image pools unfreed.
    // WHY-THIS-SHAPE: the batch held at the fallback is still held at teardown, and a wait on it returns
    //       VK_TIMEOUT at once, which is what the Device's bounded wait reports for a GPU that stopped -- so the
    //       leak branch runs without waiting out the bound. What happens to the retired hardware codec is
    //       printed, not graded: leaking it there is a known limit, not a property.
    // CONTROL: renderer/video/video_decoder.cpp closeDecoder()'s leak branch dropping m_codec whatever it is,
    //       reverted: both streams, "the software codec was never freed", with 1 timed-out wait each. Its
    //       answer ignored, so teardown frees everything after the timed-out wait, reverted: "pool 1 of 1
    //       (320x240) was freed at teardown", both pools on counter_tiny.obu. Removing only its
    //       m_framesContexts.clear() stayed green: the leaked retired codec still references each pool.
    // COVERAGE-ASSERT: teardown waited on the held batch and was told it timed out -- red with the stall never
    //       applied, "0 timed-out wait(s)" -- and a software codec was open.
    std::string stopped;
    std::string threw;
    std::optional<size_t> fallback;
    int deviceBefore = 0;
    bool heldAtTeardown = false;
    try {
        RetireHarness harness(device, asset(fixture.file));
        fallback = callUntilFallback(harness, stopped);
        if (fallback) {
            deviceBefore = devicePicturesBefore(harness, *fallback);
            for (int i = 0; i < 2; ++i) static_cast<void>(harness.call());
            heldAtTeardown = harness.holdSeen();
            g_stallHeld = true;
            static_cast<void>(harness.destroy());
            g_stallHeld = false;
        }
    } catch (const std::exception& e) {
        g_stallHeld = false;
        threw = e.what();
    }
    // Lets the Device retire the batch, so the entry's closing wait does not report the hold as a stall.
    g_heldFences.clear();
    if (!threw.empty() || !fallback) {
        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(),
                     threw.empty() ? ("no fallback was reached: " + stopped).c_str() : ("threw: " + threw).c_str());
        fail("the stalled teardown run did not reach a fallback");
        return;
    }
    if (deviceBefore == 0) {
        std::printf("%s: %s: nothing was decoded on the device before the fallback -- not answerable\n",
                    label.c_str(), what.c_str());
        g_notVerified = true;
        return;
    }
    const CodecRecord* software = RetireHarness::softwareCodec();
    const CodecRecord* hardware = RetireHarness::hardwareCodec();
    std::printf("%s: %s: %d timed-out wait(s); software codec %s; retired hardware codec %s (a known limit, not "
                "graded); %zu pool(s)\n",
                label.c_str(), what.c_str(), g_stalledWaits, codecFate(software).c_str(), codecFate(hardware).c_str(),
                g_pools.size());
    if (!heldAtTeardown || g_stalledWaits == 0 || software == nullptr || g_pools.empty()) {
        std::fprintf(stderr,
                     "FAIL: %s: %s: held batch %s at teardown (%d wait(s) on it in the run), %d timed-out wait(s), %zu "
                     "pool(s)\n",
                     label.c_str(), what.c_str(), heldAtTeardown ? "running" : "complete", g_waitsOnHeld,
                     g_stalledWaits, g_pools.size());
        fail("teardown was not reached on a stalled GPU with a software codec open, so its leak branch was not graded");
        return;
    }
    if (!software->freed || software->freedDuring != kDestroying) {
        std::fprintf(stderr, "FAIL: %s: %s: the software codec was %s\n", label.c_str(), what.c_str(),
                     codecFate(software).c_str());
        fail("a software codec was not freed at teardown on a stalled GPU");
    }
    // Only what teardown freed is graded here; a pool freed during playback is S13.84's question.
    size_t aliveAtTeardown = 0;
    for (size_t i = 0; i < g_pools.size(); ++i) {
        const PoolRecord& pool = g_pools[i];
        if (!pool.freed || pool.freedDuring == kDestroying) ++aliveAtTeardown;
        if (!pool.freed || pool.freedDuring != kDestroying) continue;
        std::fprintf(stderr, "FAIL: %s: %s: pool %zu of %zu (%s) was freed at teardown\n", label.c_str(), what.c_str(),
                     i + 1, g_pools.size(), sizeName(pool.width, pool.height).c_str());
        fail("an image pool was freed on a GPU whose teardown wait timed out");
    }
    if (aliveAtTeardown == 0) {
        std::fprintf(stderr, "FAIL: %s: %s: every image pool was freed before teardown\n", label.c_str(), what.c_str());
        fail("no image pool was alive at teardown, so what a stalled teardown leaks was not graded");
    }
}

void runRefusedTeardownChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_sg_30.hevc";
    if (!haveAsset(file, label)) return;
    constexpr VkExtent2D kSize{160, 120};
    const std::optional<DecodeRange> range = decodeRange(device, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR);
    if (!range || range->admits(kSize)) {
        std::printf("%s: %s: this device %s 160x120 HEVC, so nothing is refused before decoding -- not answerable\n",
                    label.c_str(), file, range ? "decodes" : "reports no range for");
        g_notVerified = true;
        return;
    }
    const std::string what = std::string(file) + ", hardware refused before decoding, torn down with a batch held";

    // GOAL: S13.92 -- tearing down a decoder whose hardware was refused before it decoded anything, which holds
    //       a retired hardware codec and no image pool, makes no Device wait and frees that codec.
    // WHY-THIS-SHAPE: a stream entirely below the device's minimum, one call, then teardown with a batch held,
    //       so a wait at teardown has something to wait on and is counted; with nothing pending a drain makes no
    //       Vulkan call and would stay green. The size is known to be refused from the capability query itself.
    // CONTROL: renderer/video/video_decoder.cpp closeDecoder() also asking safeToFree() when a retired codec is
    //       held, reverted: "teardown made 1 wait(s), 1 on a held batch".
    // COVERAGE-ASSERT: the first call opened a software decoder and returned a host picture, no image pool was
    //       made, and the hardware codec was still open with the batch held when teardown began.
    std::string threw;
    bool hostPicture = false;
    bool openedSoftware = false;
    bool aliveAtTeardown = false;
    int teardownWaits = -1;
    try {
        RetireHarness harness(device, asset(file));
        harness.holdNewBatch();
        const DirectCall first = harness.call();
        hostPicture = first.returned && !first.onDevice;
        openedSoftware = first.openedSoftware;
        const CodecRecord* hardware = RetireHarness::hardwareCodec();
        aliveAtTeardown = hardware != nullptr && !hardware->freed && harness.holdSeen();
        teardownWaits = harness.destroy();
    } catch (const std::exception& e) {
        threw = e.what();
    }
    if (!threw.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: threw: %s\n", label.c_str(), what.c_str(), threw.c_str());
        fail("the refused-before-decoding run threw");
        return;
    }
    const CodecRecord* hardware = RetireHarness::hardwareCodec();
    if (!hostPicture || !openedSoftware || !g_pools.empty() || !aliveAtTeardown) {
        std::fprintf(stderr,
                     "FAIL: %s: %s: first call %s, %s a software decoder; %zu pool(s) made; hardware codec and held batch "
                     "%s alive at teardown\n",
                     label.c_str(), what.c_str(), hostPicture ? "returned a host picture" : "did not return a host picture",
                     openedSoftware ? "opened" : "did not open", g_pools.size(), aliveAtTeardown ? "were" : "were not");
        fail("teardown was not reached holding a retired codec refused before decoding, so its wait was not graded");
        return;
    }
    std::printf("%s: %s: teardown made %d wait(s), %d on a held batch; the hardware codec was %s\n", label.c_str(),
                what.c_str(), teardownWaits, g_waitsOnHeld, codecFate(hardware).c_str());
    if (teardownWaits != 0 || g_waitsOnHeld != 0) {
        fail("tearing down a decoder refused before decoding waited on the Device");
    }
    if (!hardware->freed || hardware->freedDuring != kDestroying) {
        fail("the hardware codec refused before decoding was not freed at teardown");
    }
}

// ------------------------------------------------------------------------- //
// Releasing a replaced pool never waits
// ------------------------------------------------------------------------- //

void runReleaseChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_resize.h264";
    if (!haveAsset(file, label)) return;

    // GOAL: S13.84 -- a replaced pool is not freed while the last batch submitted before it went idle is
    //       still running, is freed in the first call after that batch completes, and no call waits for it.
    // WHY-THIS-SHAPE: VideoDecoder driven directly, so the pipeline's own stage rebuild -- which waits by
    //       design -- is not in the counted calls. A batch is submitted before each call from the pool's
    //       replacement on, and vkGetFenceStatus reports those batches running until the probe lets them
    //       go: the Device learns completion only there, so to the decoder they are running, and nothing
    //       real is held on a queue ffmpeg also waits on. Waits are counted at this executable's Vulkan
    //       symbols, never timed; a wait on a held batch ends the hold, so a decoder that waits is counted
    //       rather than stalled for the Device's bound. The idle point is read from the pool's own
    //       reference count, so the held calls are known to follow it rather than assumed to.
    // CONTROL: renderer/video/video_decoder.cpp releaseIdleFramesContexts(), each reverted. The poll made
    //       Device::waitForSubmissions(): "3 held calls made 1 wait(s) ... 1 wait(s) on a held batch", freed
    //       in held call 32. The poll deleted: freed in held call 32 with 0 waits, and "not freed in call 35".
    //       Harness: vkGetFenceStatus never holding -> "the Device reported a held batch complete".
    // COVERAGE-ASSERT: the run must reach the release call, with three held calls after the idle point,
    //       and the Device must report a held batch as not complete.
    constexpr int kHeldCalls = 3;
    constexpr int kWatchCalls = 8;
    enum class Phase { Replacing, Watching, Holding, Released, Done };
    Phase phase = Phase::Replacing;
    std::optional<size_t> oldPool;
    int watched = 0;
    int held = 0;
    int calls = 0;
    int idleAfter = -1;
    int waitsWhileHeld = 0;
    int waitsAtRelease = -1;
    bool freedWhileHeld = false;
    bool freedAtRelease = false;
    bool holdSeenByDevice = false;
    std::string stopped;
    struct Clear {
        ~Clear() {
            g_trackPools = false;
            g_countVulkan = false;
            g_heldFences.clear();
            g_holdNextSubmission = false;
            g_poolCall = kOutsideCalls;
            g_decoderOpen = false;
        }
    } clear;
    resetPools();
    g_waitsOnHeld = 0;
    try {
        g_trackPools = true;
        std::optional<VideoDecoder> decoder;
        decoder.emplace(device, asset(file));
        g_decoderOpen = true;
        for (;; ++calls) {
            if (calls > 200) {
                stopped = "the stream did not end";
                break;
            }
            const Phase during = phase;
            std::optional<Device::SubmitTicket> heldTicket;
            if (during == Phase::Watching || during == Phase::Holding) {
                g_holdNextSubmission = true;
                heldTicket = device.submit([](VkCommandBuffer) {});
                g_holdNextSubmission = false;
            }
            g_vulkanCalls.fill(0);
            g_poolCall = calls;
            g_countVulkan = true;
            std::optional<DecodedFrame> frame;
            try {
                frame = decoder->next();
            } catch (...) {
                g_countVulkan = false;
                throw;
            }
            g_countVulkan = false;
            g_poolCall = kOutsideCalls;
            VulkanTally tally;
            tally.calls = g_vulkanCalls;
            if (!frame) {
                stopped = "the stream ended first";
                break;
            }
            const bool onDevice = frame->storage() == DecodedFrame::Storage::DeviceImage;
            frame.reset();
            if (!onDevice) {
                stopped = "call " + std::to_string(calls + 1) + " decoded in software";
                break;
            }
            if (!oldPool) {
                const std::vector<size_t> pools = outputPools();
                if (pools.empty()) {
                    stopped = "the first picture's image pool was not seen";
                    break;
                }
                oldPool = pools.front();
            }
            PoolRecord& old = g_pools[*oldPool];
            if (during == Phase::Holding) {
                if (heldTicket && !device.hasCompleted(*heldTicket)) holdSeenByDevice = true;
                waitsWhileHeld += tally.waits();
                if (old.freed) freedWhileHeld = true;
                if (++held == kHeldCalls) {
                    g_heldFences.clear();
                    device.waitForSubmissions();
                    phase = Phase::Released;
                }
                continue;
            }
            if (during == Phase::Released) {
                waitsAtRelease = tally.waits();
                freedAtRelease = old.freed && old.freedWhileOpen && old.freedDuring == calls;
                phase = Phase::Done;
                break;
            }
            if (phase == Phase::Replacing && g_pools.size() > *oldPool + 1) phase = Phase::Watching;
            if (phase == Phase::Watching) {
                if (old.freed) {
                    stopped = "the replaced pool was freed before this probe saw it idle, in call " +
                              std::to_string(old.freedDuring + 1);
                    break;
                }
                if (av_buffer_get_ref_count(&old.peek) == 1) {
                    idleAfter = calls;
                    phase = Phase::Holding;
                } else if (++watched > kWatchCalls) {
                    stopped = "the replaced pool still had " + std::to_string(av_buffer_get_ref_count(&old.peek)) +
                              " references " + std::to_string(kWatchCalls) + " calls after it was replaced";
                    break;
                }
            }
        }
        g_heldFences.clear();
        device.waitForSubmissions();
        g_decoderOpen = false;
        decoder.reset();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s: %s: the decoder threw during call %d: %s\n", label.c_str(), file, calls + 1,
                     e.what());
        fail("the release run threw");
        return;
    }
    if (phase != Phase::Done) {
        std::fprintf(stderr, "FAIL: %s: %s: the release was never reached: %s\n", label.c_str(), file,
                     stopped.c_str());
        fail("the run did not reach a replaced pool's release, so nothing was graded");
        return;
    }
    if (!holdSeenByDevice) {
        std::fprintf(stderr, "FAIL: %s: %s: the Device reported a held batch complete\n", label.c_str(), file);
        fail("the held batches were not running as far as the Device could tell, so the hold graded nothing");
        return;
    }
    std::printf("%s: %s: replaced pool idle after call %d; %d held calls made %d wait(s); release call %d made %d "
                "wait(s), %d wait(s) on a held batch\n",
                label.c_str(), file, idleAfter + 1, kHeldCalls, waitsWhileHeld, calls + 1, waitsAtRelease,
                g_waitsOnHeld);
    if (freedWhileHeld) {
        std::fprintf(stderr, "FAIL: %s: %s: the replaced pool was freed in a held call (call %d)\n", label.c_str(),
                     file, g_pools[*oldPool].freedDuring + 1);
        fail("a replaced pool was freed while the last batch before it went idle was still running");
    }
    if (!freedAtRelease) {
        std::fprintf(stderr, "FAIL: %s: %s: the replaced pool was not freed in call %d, the first after its last "
                             "reader completed\n",
                     label.c_str(), file, calls + 1);
        fail("a replaced pool was not freed once its last reader had completed");
    }
    if (waitsWhileHeld != 0 || waitsAtRelease != 0 || g_waitsOnHeld != 0) {
        fail("releasing a replaced pool waited on the playback path");
    }
}

// ------------------------------------------------------------------------- //
// What a steady picture costs
// ------------------------------------------------------------------------- //

void runSteadyChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_long.mp4";
    if (!haveAsset(file, label)) return;

    // GOAL: S13.67 -- once its stage exists, a call creates no Vulkan object and waits on nothing.
    // WHY-THIS-SHAPE: counted at this executable's Vulkan symbols during renderNext() alone -- which since
    //       the split is decodeNext() and draw() together, so both halves are inside the count. The
    //       queue is drained before each counted call, so a wait inside it is the pipeline's and not
    //       one a slow GPU forced: FrameConverter's host path waits when two uploads are still in
    //       flight, and counting across an undrained queue would grade the scheduler. The first calls
    //       are left out because the Device's fence and command buffer pool grows to its most batches.
    // CONTROL: renderer/video/video_pipeline.cpp, each reverted. Both mutations moved with the split and
    //       were re-run at their new homes 2026-09-15, on the host path; the device and Lavapipe runs were
    //       not repeated this round. Device::waitForSubmissions() after the render, now in draw(): "318
    //       steady calls made vkWaitForFences x318". The stage rebuilt on every call, now in decodeNext():
    //       "318 steady calls made vkCreateImage x1272, vkCreateImageView x1272, vkCreateBuffer x636,
    //       vkCreateSampler x318, ... vkWaitForFences x317".
    // COVERAGE-ASSERT: the first call, which builds the stage, must be seen creating and waiting -- red
    //       with counting never switched on, "the first call was seen making none" -- and 300 calls
    //       counted, red on the 20-picture fixture, "only 17 calls counted".
    constexpr int kUncountedCalls = 3;
    constexpr int kMinCountedCalls = 300;
    VulkanTally first;
    VulkanTally steady;
    int calls = 0;
    int counted = 0;
    int devicePictures = 0;
    int hostPictures = 0;
    try {
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
        VideoPipeline pipeline(device, asset(file), kShaderDir);
        g_devicePictures = 0;
        g_hostPictures = 0;
        g_recordPictures = true;
        for (;;) {
            device.waitForSubmissions();
            g_vulkanCalls.fill(0);
            g_countVulkan = true;
            bool more = false;
            try {
                more = pipeline.renderNext(targets, target);
            } catch (...) {
                g_countVulkan = false;
                throw;
            }
            g_countVulkan = false;
            if (!more) break;
            if (calls == 0) first.calls = g_vulkanCalls;
            if (calls >= kUncountedCalls) {
                for (int i = 0; i < kVulkanCallCount; ++i) steady.calls[i] += g_vulkanCalls[i];
                ++counted;
            }
            if (++calls > 2000) {
                fail("the steady run did not end");
                break;
            }
        }
        device.waitForSubmissions();
        devicePictures = g_devicePictures;
        hostPictures = g_hostPictures;
    } catch (const std::exception& e) {
        g_countVulkan = false;
        g_recordPictures = false;
        std::fprintf(stderr, "FAIL: %s: the steady run threw: %s\n", label.c_str(), e.what());
        fail("the steady run threw");
        return;
    }
    g_recordPictures = false;
    if (onDevice && (devicePictures == 0 || hostPictures != 0)) {
        std::printf("%s: %s did not decode on the device -- not answerable\n", label.c_str(), file);
        g_notVerified = true;
        return;
    }
    if (first.creations() == 0 || first.waits() == 0) {
        std::fprintf(stderr, "FAIL: %s: the first call was seen making %s\n", label.c_str(), first.describe().c_str());
        fail("the Vulkan interposers did not see the stage being built, so a count of zero would mean nothing");
        return;
    }
    if (counted < kMinCountedCalls) {
        std::fprintf(stderr, "FAIL: %s: only %d calls counted\n", label.c_str(), counted);
        fail("too few steady calls were counted to grade");
        return;
    }
    if (steady.creations() != 0 || steady.waits() != 0) {
        std::fprintf(stderr, "FAIL: %s: %d steady calls made %s\n", label.c_str(), counted, steady.describe().c_str());
        fail("a steady picture created a Vulkan object or waited");
    }
    std::printf("%s: first call made %s; %d steady calls made %s\n", label.c_str(), first.describe().c_str(), counted,
                steady.describe().c_str());
}

// ------------------------------------------------------------------------- //
// Looping: a run whose every call is tallied on its own
// ------------------------------------------------------------------------- //

constexpr int kNotRead = -2;
constexpr size_t kPasses = 3;
// A landing is decided at one boundary; the second pass is read back whole.
constexpr size_t kLandingPasses = 2;

struct LoopCall {
    bool returned = false;
    int shown = kNotRead;
    CallTally tally;
    VulkanTally vulkan;
};

struct LoopPlan {
    size_t calls = 0;
    std::optional<VideoPipeline::Playback> playback = VideoPipeline::Playback::Loop;  // unset: the default
    std::function<bool(size_t)> readBack;  // the calls read back; unset reads back every one
    // The calls made with the queue drained first; unset drains before every one. Needed only where
    // Vulkan waits are graded.
    std::function<bool(size_t)> drainBefore;
    int failAt = -1;  // the call during which the hardware decoder fails, once
    std::function<void(size_t)> beforeCall;  // conditions for one call, cleared when it returns
    bool clearPositions = false;  // for the whole run
    size_t callsAfterEnd = 0;
};

struct LoopRun {
    std::vector<LoopCall> calls;
    std::optional<size_t> endedAt;
    std::string threw;
    std::string readbackFault;
    bool failureConsumed = false;
};

void clearLoopConditions() {
    g_tallying = false;
    g_countVulkan = false;
    g_loopSeek = LoopSeek::None;
    g_receiveExternalRemaining = 0;
    g_refuseAllPackets = false;
}

// A call made with the queue drained first counts only waits that are the pipeline's own.
LoopRun runLoop(Device& device, const std::string& file, const LoopPlan& plan) {
    LoopRun run;
    struct Clear {
        ~Clear() {
            clearLoopConditions();
            g_clearPositions = false;
        }
    } clear;
    try {
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
        g_clearPositions = plan.clearPositions;
        std::optional<VideoPipeline> pipeline;
        // Declared after the pipeline, so a pool its destructor frees is recorded as freed at close.
        struct Closing {
            ~Closing() {
                g_decoderOpen = false;
                g_poolCall = kOutsideCalls;
            }
        } closing;
        if (plan.playback) {
            pipeline.emplace(device, file, kShaderDir, *plan.playback);
        } else {
            pipeline.emplace(device, file, kShaderDir);
        }
        g_decoderOpen = true;
        size_t afterEnd = 0;
        for (size_t index = 0; index < plan.calls; ++index) {
            if (!plan.drainBefore || plan.drainBefore(index)) device.waitForSubmissions();
            g_tally = CallTally{};
            g_vulkanCalls.fill(0);
            if (plan.beforeCall) plan.beforeCall(index);
            const bool failing = static_cast<int>(index) == plan.failAt;
            if (failing) g_receiveExternalRemaining = 1;
            g_tallying = true;
            g_countVulkan = true;
            LoopCall call;
            g_poolCall = static_cast<int>(index);
            call.returned = pipeline->renderNext(targets, target);
            g_poolCall = kOutsideCalls;
            if (failing) run.failureConsumed = g_receiveExternalRemaining == 0;
            clearLoopConditions();
            call.tally = g_tally;
            call.vulkan.calls = g_vulkanCalls;
            run.calls.push_back(std::move(call));
            if (!run.calls.back().returned) {
                if (!run.endedAt) run.endedAt = index;
                if (afterEnd++ == plan.callsAfterEnd) break;
                continue;
            }
            if (plan.readBack && !plan.readBack(index)) continue;
            try {
                run.calls.back().shown = wallpaperx::testing::counterFromBgra(
                    wallpaperx::testing::reclaimAndReadback(device, targets, target), target.width, target.height);
            } catch (const std::exception& e) {
                run.readbackFault = e.what();
                break;
            }
        }
        device.waitForSubmissions();
    } catch (const std::exception& e) {
        run.threw = e.what();
    }
    return run;
}

// Whether each call read back showed picture (call mod N). `calls` and `compared` are what the plan
// made and read back, checked so a plan that stops early or skips its boundaries cannot pass.
bool gradeLoop(const std::string& label, const std::string& what, const LoopRun& run,
               const CounterReference& reference, size_t calls, size_t compared) {
    if (!run.readbackFault.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: reading back call %zu failed: %s\n", label.c_str(), what.c_str(),
                     run.calls.size(), run.readbackFault.c_str());
        fail("the harness could not read back a shown picture");
        return false;
    }
    if (!run.threw.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: the pipeline threw during call %zu: %s\n", label.c_str(), what.c_str(),
                     run.calls.size() + 1, run.threw.c_str());
        fail("a looping pipeline threw");
        return false;
    }
    if (run.endedAt) {
        std::fprintf(stderr, "FAIL: %s: %s: call %zu returned false, for a stream of %zu pictures\n", label.c_str(),
                     what.c_str(), *run.endedAt + 1, reference.ids.size());
        fail("a looping pipeline stopped");
        return false;
    }
    const size_t pictures = reference.ids.size();
    size_t seen = 0;
    size_t wrong = 0;
    size_t unreadable = 0;
    std::optional<size_t> first;
    for (size_t i = 0; i < run.calls.size(); ++i) {
        const int shown = run.calls[i].shown;
        if (shown == kNotRead) continue;
        ++seen;
        if (shown == reference.ids[i % pictures]) continue;
        ++wrong;
        if (shown < 0) ++unreadable;
        if (!first) first = i;
    }
    if (run.calls.size() != calls || seen != compared) {
        std::fprintf(stderr, "FAIL: %s: %s: %zu calls made and %zu read back, where %zu and %zu were planned\n",
                     label.c_str(), what.c_str(), run.calls.size(), seen, calls, compared);
        fail("the loop check did not compare the calls it was written for");
        return false;
    }
    if (wrong == 0) return true;
    const size_t at = *first;
    std::fprintf(stderr,
                 "FAIL: %s: %s: call %zu (pass %zu, picture %zu of it) showed %d where %d was due; %zu of %zu calls "
                 "compared were wrong, %zu unreadable\n",
                 label.c_str(), what.c_str(), at + 1, at / pictures + 1, at % pictures, run.calls[at].shown,
                 reference.ids[at % pictures], wrong, seen, unreadable);
    fail("a looping pipeline did not show each picture once per pass, in order");
    return false;
}

// Whether a run's pictures came from the path its entry grades. A throw is left to gradeLoop to report.
bool loopOnPath(const std::string& label, const std::string& what, const LoopRun& run, bool onDevice) {
    if (!run.threw.empty()) return true;
    int devicePictures = 0;
    int hostPictures = 0;
    for (const LoopCall& call : run.calls) {
        devicePictures += call.tally.devicePictures;
        hostPictures += call.tally.hostPictures;
    }
    if (onDevice && (devicePictures == 0 || hostPictures != 0)) {
        std::printf("%s: %s did not decode on the device (%d device, %d host pictures) -- not answerable for it\n",
                    label.c_str(), what.c_str(), devicePictures, hostPictures);
        g_notVerified = true;
        return false;
    }
    if (!onDevice && devicePictures != 0) {
        std::fprintf(stderr, "FAIL: %s: %s: %d device pictures\n", label.c_str(), what.c_str(), devicePictures);
        fail("a software-only device produced a device picture");
        return false;
    }
    return true;
}

std::string describeCall(const LoopCall& call) {
    const CallTally& tally = call.tally;
    return "decoded " + std::to_string(tally.pictures) + " picture(s), " + describeSeeks(tally.seeks) + ", " +
           std::to_string(tally.containerOpens) + " container open(s), " + std::to_string(tally.decoderOpens) +
           " decoder open(s)";
}

enum class SeekAttempt { DecodeTime, PresentationTime, BytePosition };

// One of the rewind's seeks made on `file` by libavformat alone, never through VideoDecoder: whether it
// reported success, whether a video packet could be read after it, and that packet's number from 1 by
// byte position (0 for none). `firstDts` is packet 1's decode timestamp.
struct Landing {
    bool opened = false;
    int64_t firstDts = AV_NOPTS_VALUE;
    bool seeked = false;
    bool read = false;
    int packet = 0;
};
Landing measureLanding(const std::string& file, SeekAttempt attempt) {
    Landing landing;
    AVFormatContext* format = nullptr;
    AVPacket* packet = av_packet_alloc();
    struct Free {
        AVFormatContext*& format;
        AVPacket*& packet;
        ~Free() {
            av_packet_free(&packet);
            avformat_close_input(&format);
        }
    } guard{format, packet};
    if (packet == nullptr || avformat_open_input(&format, file.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(format, nullptr) < 0) {
        return landing;
    }
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) return landing;
    auto readVideo = [&] {
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == stream) return true;
            av_packet_unref(packet);
        }
        return false;
    };
    std::vector<int64_t> positions;
    int64_t firstPts = AV_NOPTS_VALUE;
    while (readVideo()) {
        if (positions.empty()) {
            landing.firstDts = packet->dts;
            firstPts = packet->pts;
        }
        positions.push_back(packet->pos);
        av_packet_unref(packet);
    }
    if (positions.empty()) return landing;
    landing.opened = true;
    const int64_t start = format->streams[stream]->start_time == AV_NOPTS_VALUE ? 0 : format->streams[stream]->start_time;
    int result = -1;
    if (attempt == SeekAttempt::DecodeTime && landing.firstDts != AV_NOPTS_VALUE) {
        result = av_seek_frame(format, stream, landing.firstDts, AVSEEK_FLAG_BACKWARD);
    } else if (attempt == SeekAttempt::PresentationTime) {
        result = av_seek_frame(format, stream, firstPts != AV_NOPTS_VALUE ? firstPts : start, AVSEEK_FLAG_BACKWARD);
    } else if (attempt == SeekAttempt::BytePosition) {
        result = av_seek_frame(format, -1, positions.front(), AVSEEK_FLAG_BYTE);
    }
    landing.seeked = result >= 0;
    if (!landing.seeked) return landing;
    landing.read = readVideo();
    if (!landing.read) return landing;
    const auto found = std::find(positions.begin(), positions.end(), packet->pos);
    if (packet->pos >= 0 && found != positions.end()) landing.packet = static_cast<int>(found - positions.begin()) + 1;
    av_packet_unref(packet);
    return landing;
}

// ------------------------------------------------------------------------- //
// Looping: seamless passes, and what a boundary does
// ------------------------------------------------------------------------- //

struct LoopFixture {
    const char* file;
    const char* what;
    bool changesSize;
    bool firstPacketHasNoDecodeTime;
};
const LoopFixture kLoopFixtures[] = {
    {"counter_short.mp4", "MP4 with B-frames", false, false},
    {"counter_bframes.h264", "raw H.264 with B-frames", false, false},
    {"counter_seg.ts", "transport stream", false, false},
    {"counter_bf2.mkv", "Matroska with B-frames", false, true},
    {"counter_repeat.mkv", "Matroska with repeating timestamps", false, false},
    {"counter_jump.ts", "transport stream whose timestamps jump back", false, false},
    {"counter_resize.h264", "raw H.264 that halves in size", true, false},
};
constexpr size_t kLoopFixtureCount = 7;

void runLoopChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;

    // GOAL: S13.69 -- a looping pipeline shows every picture once per pass, in order, over three passes,
    //       with no black or repeated call at a boundary. S13.71 -- no boundary opens the container or
    //       the decoder again, the Matroska file whose packet 1 has no decode timestamp included.
    //       S13.79 -- a boundary on the same stage creates no Vulkan object and waits on nothing.
    //       S13.85 -- on the device, the stream that changes size loops too, and its image pools stay
    //       bounded across the changes of three passes.
    // WHY-THIS-SHAPE: every call read back and compared with picture (call mod N) of an independent
    //       decode; comparing the boundary call alone would miss a pass that starts right and repeats
    //       a picture two calls later. Opens and Vulkan work are counted in the boundary call alone,
    //       with the queue drained before it. The resize fixture is exempt from the Vulkan count, since
    //       its boundary changes size and must rebuild. The loop_device entry runs in a forked child,
    //       so a crash on that fixture is reported by the parent.
    // CONTROL (S13.85): renderer/video/video_decoder.cpp, each reverted. The decoder before the fix --
    //       attachHardwareFrames() without its pool hold, and allocatePicture() skipping holds() when nothing
    //       is held: the child died by signal 11, reported by the parent. The first site alone sends every
    //       picture to software, and this entry reports not answerable. next()
    //       without either releaseIdleFramesContexts() call: "pool 1 of 6 (320x240) was freed only when the
    //       pipeline was destroyed" through pool 5, and "6 image pools alive after call 149". Without only the
    //       one at its top: pools 1-3 freed at the boundaries by the end-of-stream call, "pool 4 of 6 (160x128)
    //       was freed only when the pipeline was destroyed", pool 5, and "3 image pools alive after call 89".
    // CONTROL: each reverted, both paths. renderer/video/video_decoder.cpp restart() without its flush:
    //       "call 21 returned false, for a stream of 20 pictures". rewind() dropping packet 1 after a
    //       landing: "call 91 (pass 2, picture 0 of it) showed 24 where 0 was due". seekToFirstPacket()
    //       without the presentation-time seek: counter_bf2.mkv alone, "seeks to byte 617, 1 container
    //       open(s)". renderer/video/video_pipeline.cpp resetting the stage at the restart: "boundary 1
    //       made vkCreateImage x3, ..."; waiting for submissions after that call's render: "vkWaitForFences
    //       x1". The wait placed before the render stayed green: nothing is pending, no Vulkan call is made.
    // COVERAGE-ASSERT: the calls made and read back match the plan -- red with every seventh skipped, "60
    //       calls made and 51 read back" -- and the first call is seen building the stage, red with
    //       counting off, "the first call was seen making none". The list's length and counter_bf2.mkv's
    //       missing decode timestamp are asserted, red with six expected and with the MP4 claimed instead.
    check(std::size(kLoopFixtures) == kLoopFixtureCount,
          "the loop fixture list no longer has the seven entries this check is written for");
    for (const LoopFixture& fixture : kLoopFixtures) {
        const std::optional<CounterReference> reference = counterReference(label, fixture.file);
        if (!reference) continue;
        if (fixture.firstPacketHasNoDecodeTime) {
            const Landing landing = measureLanding(asset(fixture.file), SeekAttempt::PresentationTime);
            if (!landing.opened || landing.firstDts != AV_NOPTS_VALUE) {
                std::fprintf(stderr, "FAIL: %s: %s: packet 1 has a decode timestamp\n", label.c_str(), fixture.file);
                fail("the fixture meant to give packet 1 no decode timestamp gives it one, so that rewind is not offered");
                continue;
            }
        }
        const size_t pictures = reference->ids.size();
        LoopPlan plan;
        plan.calls = kPasses * pictures;
        const bool poolsGraded = onDevice && fixture.changesSize;
        resetPools();
        g_trackPools = poolsGraded;
        const LoopRun run = runLoop(device, asset(fixture.file), plan);
        g_trackPools = false;
        const std::string what = std::string(fixture.file) + " (" + fixture.what + "), three passes";
        if (!loopOnPath(label, what, run, onDevice)) continue;
        bool good = gradeLoop(label, what, run, *reference, plan.calls, plan.calls);
        // Each pass makes a pool per size, the boundary's return to the first size included.
        if (poolsGraded && run.threw.empty()) {
            good = gradePools(label, what, run.calls.size(), kPasses * reference->widths.size()) && good;
        }
        if (!run.calls.empty() && (run.calls[0].vulkan.creations() == 0 || run.calls[0].vulkan.waits() == 0)) {
            std::fprintf(stderr, "FAIL: %s: %s: the first call was seen making %s\n", label.c_str(), what.c_str(),
                         run.calls[0].vulkan.describe().c_str());
            fail("the Vulkan interposers did not see the stage being built, so a boundary count of zero would mean "
                 "nothing");
            good = false;
        }
        for (size_t pass = 1; pass < kPasses && pass * pictures < run.calls.size(); ++pass) {
            const LoopCall& boundary = run.calls[pass * pictures];
            if (!boundary.returned) break;
            if (boundary.tally.containerOpens != 0 || boundary.tally.decoderOpens != 0) {
                std::fprintf(stderr, "FAIL: %s: %s: boundary %zu %s\n", label.c_str(), what.c_str(), pass,
                             describeCall(boundary).c_str());
                fail("a loop boundary opened the container or the decoder again");
                good = false;
            }
            if (!fixture.changesSize && (boundary.vulkan.creations() != 0 || boundary.vulkan.waits() != 0)) {
                std::fprintf(stderr, "FAIL: %s: %s: boundary %zu made %s\n", label.c_str(), what.c_str(), pass,
                             boundary.vulkan.describe().c_str());
                fail("a loop boundary created a Vulkan object or waited");
                good = false;
            }
            std::printf("%s: %s: boundary %zu %s; Vulkan %s\n", label.c_str(), what.c_str(), pass,
                        describeCall(boundary).c_str(), boundary.vulkan.describe().c_str());
        }
        if (good) {
            std::printf("%s: %s: every picture once per pass, in order (%zu calls)\n", label.c_str(), what.c_str(),
                        run.calls.size());
        }
    }
}

// ------------------------------------------------------------------------- //
// Looping: where the rewind lands
// ------------------------------------------------------------------------- //

void runLoopLandingChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;

    // GOAL: S13.72 -- a rewind seek that lands on a packet other than packet 1 is not taken as the
    //       start: it falls through to the next seek, and when none lands, to opening the container again.
    // WHY-THIS-SHAPE: two outcomes libavformat really produces, measured on the fixture with libavformat
    //       alone before the run. The transport stream's presentation-time seek lands on packet 3, a
    //       non-keyframe, so its decode-time seek is failed and the byte seek must follow. The Matroska
    //       byte seek reports success and then reads no packet, so its timestamp seeks are failed and a
    //       reopen must follow. A shifted target alone would grade only a landing this harness invented.
    // CONTROL: renderer/video/video_decoder.cpp, each reverted, both paths. isFirstPacket() true for any
    //       packet: the transport stream red on both, each with "call 121 (pass 2, picture 0 of it) showed 24
    //       where 0 was due" and "no seek that succeeded was followed by another attempt", and Matroska green:
    //       nothing is read for it to judge. A failed read after a seek taken as landed:
    //       Matroska "call 61 returned false". rewind() without its reopen: Matroska "call 61 returned false".
    // COVERAGE-ASSERT: a successful seek is followed by another attempt at each boundary -- red with the
    //       Matroska seek failures not applied, "no seek that succeeded was followed by another attempt" --
    //       and the premise is measured: measuring the decode-time seek instead reports "not answerable".
    struct Case {
        const char* file;
        LoopSeek seek;
        SeekAttempt wrong;
        bool readsNothing;
        const char* what;
        bool reopens;
    };
    for (const Case& c :
         {Case{"counter_seg.ts", LoopSeek::FailFirst, SeekAttempt::PresentationTime, false,
               "its decode-time seek failing, leaving a presentation-time seek that lands past packet 1", false},
          Case{"counter_bf2.mkv", LoopSeek::FailTimestamps, SeekAttempt::BytePosition, true,
               "its timestamp seeks failing, leaving a byte seek that succeeds and reads no packet", true}}) {
        const std::optional<CounterReference> reference = counterReference(label, c.file);
        if (!reference) continue;
        const Landing wrong = measureLanding(asset(c.file), c.wrong);
        const bool premise =
            wrong.opened && wrong.seeked && (c.readsNothing ? !wrong.read : wrong.read && wrong.packet != 1);
        if (!premise) {
            const std::string seen = !wrong.seeked ? "fails"
                                     : !wrong.read ? "reads no packet"
                                                   : "lands on packet " + std::to_string(wrong.packet);
            std::printf("%s: %s: libavformat's seek here %s, which is not the outcome this case grades -- not "
                        "answerable\n",
                        label.c_str(), c.file, seen.c_str());
            g_notVerified = true;
            continue;
        }
        const size_t pictures = reference->ids.size();
        const LoopSeek seek = c.seek;
        LoopPlan plan;
        plan.calls = kLandingPasses * pictures;
        plan.drainBefore = [](size_t) { return false; };
        plan.beforeCall = [pictures, seek](size_t index) {
            if (index > 0 && index % pictures == 0) g_loopSeek = seek;
        };
        const LoopRun run = runLoop(device, asset(c.file), plan);
        const std::string what = std::string(c.file) + " looping with " + c.what;
        if (!loopOnPath(label, what, run, onDevice)) continue;
        bool good = gradeLoop(label, what, run, *reference, plan.calls, plan.calls);
        for (size_t pass = 1; pass < kLandingPasses && pass * pictures < run.calls.size(); ++pass) {
            const LoopCall& boundary = run.calls[pass * pictures];
            if (!boundary.returned) break;
            const CallTally& tally = boundary.tally;
            bool offered = false;
            for (size_t s = 0; s < tally.seeks.size(); ++s) {
                if (tally.seeks[s].result >= 0 && (s + 1 < tally.seeks.size() || tally.containerOpens > 0)) offered = true;
            }
            if (!offered) {
                std::fprintf(stderr, "FAIL: %s: %s: boundary %zu %s: no seek that succeeded was followed by another "
                                     "attempt\n",
                             label.c_str(), what.c_str(), pass, describeCall(boundary).c_str());
                fail("no rewind landing was rejected at a loop boundary, so the proof of packet 1 was not exercised");
                good = false;
            }
            if ((tally.containerOpens > 0) != c.reopens) {
                std::fprintf(stderr, "FAIL: %s: %s: boundary %zu %s\n", label.c_str(), what.c_str(), pass,
                             describeCall(boundary).c_str());
                fail(c.reopens ? "a rewind whose every landing missed packet 1 did not open the container again"
                               : "a rewind that could still land on packet 1 opened the container again");
                good = false;
            }
            std::printf("%s: %s: boundary %zu %s\n", label.c_str(), what.c_str(), pass, describeCall(boundary).c_str());
        }
        if (good) std::printf("%s: %s: every picture once per pass, in order\n", label.c_str(), what.c_str());
    }

    // GOAL: S13.73 -- a packet 1 without a byte position is still recognised by its timestamps and
    //       size, so its container is not opened again at a boundary; a raw bitstream, which has
    //       neither, is opened again; both show every picture once per pass.
    // WHY-THIS-SHAPE: no container here gives a packet no position, so av_read_frame's packets lose
    //       theirs for the whole run, packet 1 included. The late-landing half moves the first seek
    //       past packet 1 on the long MP4: without a position only timestamps and size can reject that
    //       landing, and a match on "has no position" alone would take it.
    // CONTROL: renderer/video/video_decoder.cpp isFirstPacket(), each reverted, both paths. The match
    //       without a position always false: MP4, transport stream and Matroska "1 container open(s)" at
    //       the boundary. That match on "no position" alone: "call 322 (pass 2, picture 0 of it) showed 48
    //       where 0 was due" on the late landing; the other runs stayed green.
    // COVERAGE-ASSERT: positions were cleared at each boundary -- red with clearing off, "read no packet
    //       with a position to clear", and the raw stream "was not opened again" -- and the shift exceeds a
    //       keyframe gap, red with a zero shift.
    struct NoPosition {
        const char* file;
        bool reopens;
    };
    for (const NoPosition& c : {NoPosition{"counter_short.mp4", false}, NoPosition{"counter_seg.ts", false},
                                NoPosition{"counter_bf2.mkv", false}, NoPosition{"counter_bframes.h264", true}}) {
        const std::optional<CounterReference> reference = counterReference(label, c.file);
        if (!reference) continue;
        const size_t pictures = reference->ids.size();
        LoopPlan plan;
        plan.calls = kLandingPasses * pictures;
        plan.drainBefore = [](size_t) { return false; };
        plan.clearPositions = true;
        const LoopRun run = runLoop(device, asset(c.file), plan);
        const std::string what = std::string(c.file) + " looping with no packet positions";
        if (!loopOnPath(label, what, run, onDevice)) continue;
        bool good = gradeLoop(label, what, run, *reference, plan.calls, plan.calls);
        for (size_t pass = 1; pass < kLandingPasses && pass * pictures < run.calls.size(); ++pass) {
            const LoopCall& boundary = run.calls[pass * pictures];
            if (!boundary.returned) break;
            if (boundary.tally.positionsCleared == 0) {
                std::fprintf(stderr, "FAIL: %s: %s: boundary %zu read no packet with a position to clear\n",
                             label.c_str(), what.c_str(), pass);
                fail("the packets read at a loop boundary kept their byte positions, so the match without one was not "
                     "exercised");
                good = false;
            }
            if ((boundary.tally.containerOpens > 0) != c.reopens) {
                std::fprintf(stderr, "FAIL: %s: %s: boundary %zu %s\n", label.c_str(), what.c_str(), pass,
                             describeCall(boundary).c_str());
                fail(c.reopens ? "a raw bitstream without positions was not opened again, so the reopen it needs was "
                                 "not exercised"
                               : "a packet 1 without a byte position forced the container open again, though its "
                                 "timestamps identify it");
                good = false;
            }
            std::printf("%s: %s: boundary %zu %s\n", label.c_str(), what.c_str(), pass, describeCall(boundary).c_str());
        }
        if (good) std::printf("%s: %s: every picture once per pass, in order\n", label.c_str(), what.c_str());
    }

    const char* const file = "counter_long.mp4";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    constexpr int kFixturePicturesPerSecond = 30;
    if (kLateSeconds * kFixturePicturesPerSecond <= reference->longestKeyframeGap) {
        fail("the late seek shift is within a keyframe gap, so the shifted seek could still land on packet 1");
        return;
    }
    const size_t pictures = reference->ids.size();
    LoopPlan plan;
    plan.calls = pictures + 3;
    plan.drainBefore = [](size_t) { return false; };
    plan.clearPositions = true;
    plan.readBack = [pictures](size_t index) { return index + 2 >= pictures; };
    plan.beforeCall = [pictures](size_t index) {
        if (index == pictures) g_loopSeek = LoopSeek::LateFirst;
    };
    const LoopRun run = runLoop(device, asset(file), plan);
    const std::string what = std::string(file) + " looping with no packet positions and its first rewind seek late";
    if (!loopOnPath(label, what, run, onDevice)) return;
    bool good = gradeLoop(label, what, run, *reference, plan.calls, 5);
    if (run.calls.size() > pictures && run.calls[pictures].returned) {
        const LoopCall& boundary = run.calls[pictures];
        const std::vector<SeekCall>& seeks = boundary.tally.seeks;
        if (seeks.size() < 2 || seeks[0].result < 0 || boundary.tally.containerOpens != 0) {
            std::fprintf(stderr, "FAIL: %s: %s: boundary %s\n", label.c_str(), what.c_str(), describeCall(boundary).c_str());
            fail("a late landing without a position was not rejected for the next seek, which lands");
            good = false;
        }
        std::printf("%s: %s: boundary %s\n", label.c_str(), what.c_str(), describeCall(boundary).c_str());
    }
    if (good) std::printf("%s: %s: the boundary showed the right pictures\n", label.c_str(), what.c_str());
}

// ------------------------------------------------------------------------- //
// Looping: a file deleted or renamed over while it plays
// ------------------------------------------------------------------------- //

void runLoopFileChecks(Device& device, const std::string& label) {
    if (!videoExtensionsHidden(device, label)) return;
    const char* const original = "counter_short.mp4";
    const char* const other = "counter_jump_b.ts";
    const std::optional<CounterReference> reference = counterReference(label, original);
    if (!reference) return;
    const std::optional<CounterReference> otherReference = counterReference(label, other);
    if (!otherReference) return;

    // GOAL: S13.74 -- a looping pipeline whose file is deleted, or renamed over by another video, while
    //       it plays keeps showing the original, each picture once per pass, even when every boundary
    //       opens the container again.
    // WHY-THIS-SHAPE: every seek fails from the change on, because a seek that lands never reopens and
    //       would leave the path unread. The other video's counter starts at 1000, so a picture read
    //       from it by path cannot be mistaken for the original's.
    // CONTROL: renderer/video/video_decoder.cpp openContainer() opening the path at every open, reverted:
    //       deletion "threw during call 21: VideoDecoder: cannot open .../deleted.mp4: No such file or
    //       directory"; rename "call 21 (pass 2, picture 0 of it) showed 1000 where 0 was due".
    // COVERAGE-ASSERT: each boundary opens the container once -- red with the seek failures not applied --
    //       and the two videos share no counter, red with the original as its own replacement.
    for (int id : otherReference->ids) {
        if (std::find(reference->ids.begin(), reference->ids.end(), id) != reference->ids.end()) {
            fail("the replacement video shares a counter with the original, so a picture read from it is not visible");
            return;
        }
    }
    namespace fs = std::filesystem;
    std::string pattern = (fs::temp_directory_path() / "wallpaperx-loop-XXXXXX").string();
    if (mkdtemp(pattern.data()) == nullptr) {
        std::fprintf(stderr, "FAIL: %s: cannot create a scratch directory from %s\n", label.c_str(), pattern.c_str());
        fail("the harness could not create a scratch directory for the file it changes");
        return;
    }
    struct Scratch {
        fs::path path;
        ~Scratch() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } scratch{pattern};

    const size_t pictures = reference->ids.size();
    for (const bool rename : {false, true}) {
        const fs::path playing = scratch.path / (rename ? "renamed-over.mp4" : "deleted.mp4");
        const fs::path staged = scratch.path / "staged.ts";
        std::error_code error;
        fs::copy_file(asset(original), playing, fs::copy_options::overwrite_existing, error);
        if (error) {
            std::fprintf(stderr, "FAIL: %s: cannot copy %s: %s\n", label.c_str(), original, error.message().c_str());
            fail("the harness could not copy the fixture it changes");
            continue;
        }
        const size_t changeAt = pictures / 2;
        std::string changeFault;
        bool changed = false;
        LoopPlan plan;
        plan.calls = kPasses * pictures;
        plan.beforeCall = [&](size_t index) {
            if (index == changeAt) {
                std::error_code e;
                if (rename) {
                    fs::copy_file(asset(other), staged, fs::copy_options::overwrite_existing, e);
                    if (!e) fs::rename(staged, playing, e);
                    changed = !e && fs::file_size(playing, e) == fs::file_size(asset(other), e) && !e;
                } else {
                    changed = fs::remove(playing, e) && !fs::exists(playing);
                }
                if (e) changeFault = e.message();
            }
            if (index >= changeAt) g_loopSeek = LoopSeek::FailAll;
        };
        const LoopRun run = runLoop(device, playing.string(), plan);
        const std::string what = std::string(original) + (rename ? " renamed over by " + std::string(other) : " deleted") +
                                 " after " + std::to_string(changeAt) + " pictures, every seek failing from then";
        if (!changed) {
            std::fprintf(stderr, "FAIL: %s: %s: the change was not made: %s\n", label.c_str(), what.c_str(),
                         changeFault.c_str());
            fail("the harness could not change the file under the pipeline");
            continue;
        }
        bool good = gradeLoop(label, what, run, *reference, plan.calls, plan.calls);
        int fromOther = 0;
        for (const LoopCall& call : run.calls) {
            if (std::find(otherReference->ids.begin(), otherReference->ids.end(), call.shown) != otherReference->ids.end()) {
                ++fromOther;
            }
        }
        for (size_t pass = 1; pass < kPasses && pass * pictures < run.calls.size(); ++pass) {
            const LoopCall& boundary = run.calls[pass * pictures];
            if (!boundary.returned) break;
            if (boundary.tally.containerOpens != 1) {
                std::fprintf(stderr, "FAIL: %s: %s: boundary %zu %s\n", label.c_str(), what.c_str(), pass,
                             describeCall(boundary).c_str());
                fail("a boundary with every seek failing did not open the container once, so reading the changed path "
                     "was not exercised");
                good = false;
            }
        }
        std::printf("%s: %s: %zu calls, %d picture(s) from the other video%s\n", label.c_str(), what.c_str(),
                    run.calls.size(), fromOther, good ? ", every picture of the original once per pass" : "");
    }
}

// ------------------------------------------------------------------------- //
// Where each picture falls on the stream's own timeline
// ------------------------------------------------------------------------- //

void runTimingChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_short.mp4";
    // The fixture's own recipe in renderer/CMakeLists.txt: twenty pictures at ten a second. Stated
    // here so a recipe that changed without this changing fails a check rather than grading a
    // different file.
    constexpr std::chrono::nanoseconds kInterval{100'000'000};
    constexpr size_t kPictures = 20;
    constexpr size_t kTimingPasses = 2;

    // GOAL: S13.145 -- every picture says where it belongs on the stream's own timeline: a
    //       presentation time that advances by the file's interval from one picture to the next, its
    //       own duration, and -- for the first picture of the file and the first after a loop -- a
    //       mark saying the picture before it was not its predecessor. False for timings read off the
    //       wrong time base (a presentation time in stream ticks rather than nanoseconds is off by
    //       four orders of magnitude here), for a pipeline that reports the first picture's timing for
    //       every picture, and for a loop boundary that is not marked -- which is the case that
    //       matters, because the difference across it is the whole file's length backwards.
    // WHY-THIS-SHAPE: two full passes, so the boundary is inside the run and the mark can be asserted
    //       to be true EXACTLY twice. Asserting "the boundary is marked" alone would also hold for a
    //       pipeline that marks every picture, which would make a caller restart its schedule on every
    //       frame. The timings are compared against the fixture's own recipe rather than against each
    //       other alone: a pipeline reporting a constant would satisfy "consecutive differences are
    //       equal" perfectly.
    // CONTROL: renderer/video/video_pipeline.cpp, decodeNext()'s `startsPass = true;` after the
    //       restart deleted -> "FAIL: the first picture after a loop is marked as starting a pass",
    //       printing "1 mark(s), where 2 were due", and the spacing check with it ("-1900000000 ns at
    //       20" -- the whole file backwards, which is what the mark exists to keep a caller from
    //       using). renderer/video/video_decoder.cpp, next()'s `frame.m_presentation` given
    //       AVRational{1, 1} as its base -> "FAIL: each picture is due one file interval after the one
    //       before it", printing "38 of 38 steps are not 100000000 ns; first: 1024000000000 ns at 1".
    //       Both observed 2026-09-18; reverted.
    // COVERAGE-ASSERT: the number of pictures decoded and the number of successive pairs actually
    //       compared are both asserted, so a run that decoded two pictures cannot report the same
    //       verdict as one that decoded forty.
    struct Shown {
        std::optional<std::chrono::nanoseconds> presentation;
        std::optional<std::chrono::nanoseconds> duration;
        bool startsPass = false;
    };
    std::vector<Shown> shown;
    std::string threw;
    bool timingBeforeDecodeThrew = false;
    std::string otherThrow;
    try {
        VideoPipeline pipeline(device, asset(file), kShaderDir, VideoPipeline::Playback::Loop);
        try {
            static_cast<void>(pipeline.timing());
        } catch (const std::logic_error&) {
            timingBeforeDecodeThrew = true;
        } catch (const std::exception& e) {
            otherThrow = e.what();
        }
        for (size_t i = 0; i < kTimingPasses * kPictures && pipeline.decodeNext(); ++i) {
            const VideoPipeline::PictureTiming timing = pipeline.timing();
            shown.push_back(Shown{timing.presentation, timing.duration, timing.startsPass});
        }
    } catch (const std::exception& e) {
        threw = e.what();
    }
    if (!threw.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: the timing run threw: %s\n", label.c_str(), file, threw.c_str());
        fail("decoding the fixture to read its timings threw");
        return;
    }
    check(timingBeforeDecodeThrew, "timing() before any picture has been decoded throws std::logic_error");
    if (!timingBeforeDecodeThrew) {
        std::fprintf(stderr, "  it threw %s instead\n", otherThrow.empty() ? "nothing" : otherThrow.c_str());
    }
    if (shown.size() != kTimingPasses * kPictures) {
        std::fprintf(stderr, "FAIL: %s: %s: %zu picture(s) decoded, where %zu were planned\n", label.c_str(), file,
                     shown.size(), kTimingPasses * kPictures);
        fail("the fixture did not yield the two passes this check grades, so nothing below was exercised");
        return;
    }

    size_t withoutTiming = 0;
    size_t wrongDuration = 0;
    for (const Shown& s : shown) {
        if (!s.presentation || !s.duration) ++withoutTiming;
        else if (*s.duration != kInterval) ++wrongDuration;
    }
    check(withoutTiming == 0, "every picture of a file that carries timestamps reports a presentation time and a "
                              "duration");
    if (withoutTiming != 0) std::fprintf(stderr, "  %zu of %zu carried neither\n", withoutTiming, shown.size());
    check(wrongDuration == 0, "and the duration each reports is the file's own picture interval");
    if (wrongDuration != 0) std::fprintf(stderr, "  %zu of %zu report another\n", wrongDuration, shown.size());

    size_t compared = 0;
    size_t wrongStep = 0;
    std::string firstWrong;
    for (size_t i = 1; i < shown.size(); ++i) {
        // The boundary is excluded on purpose: the picture after it restarts the file, so the step
        // across it is the whole file backwards -- which is exactly what startsPass exists to say.
        if (shown[i].startsPass || !shown[i].presentation || !shown[i - 1].presentation) continue;
        ++compared;
        if (*shown[i].presentation - *shown[i - 1].presentation == kInterval) continue;
        ++wrongStep;
        if (firstWrong.empty()) {
            firstWrong = std::to_string((*shown[i].presentation - *shown[i - 1].presentation).count()) + " ns at " +
                         std::to_string(i);
        }
    }
    check(compared == kTimingPasses * (kPictures - 1), "enough successive pictures were compared to grade their spacing");
    if (compared != kTimingPasses * (kPictures - 1)) {
        std::fprintf(stderr, "  %zu pair(s) compared, where %zu were due\n", compared, kTimingPasses * (kPictures - 1));
    }
    check(wrongStep == 0, "each picture is due one file interval after the one before it");
    if (wrongStep != 0) {
        std::fprintf(stderr, "  %zu of %zu steps are not %lld ns; first: %s\n", wrongStep, compared,
                     static_cast<long long>(kInterval.count()), firstWrong.c_str());
    }

    size_t marks = 0;
    bool boundaryMarked = false;
    for (size_t i = 0; i < shown.size(); ++i) {
        if (!shown[i].startsPass) continue;
        ++marks;
        if (i == kPictures) boundaryMarked = true;
        if (i != 0 && i != kPictures) {
            std::fprintf(stderr, "  picture %zu is marked as starting a pass, and it does not\n", i);
        }
    }
    check(boundaryMarked, "the first picture after a loop is marked as starting a pass");
    check(marks == kTimingPasses, "and nothing else is: the mark is on the file's first picture and the loop's, only");
    if (marks != kTimingPasses) std::fprintf(stderr, "  %zu mark(s), where %zu were due\n", marks, kTimingPasses);
    if (marks == kTimingPasses && boundaryMarked && wrongStep == 0 && withoutTiming == 0) {
        std::printf("%s: %s: %zu pictures over %zu passes, %lld ns apart, each carrying its own duration\n",
                    label.c_str(), file, shown.size(), kTimingPasses, static_cast<long long>(kInterval.count()));
    }
}

// ------------------------------------------------------------------------- //
// The two halves the daemon drives: decode, then draw
// ------------------------------------------------------------------------- //

void runHalvesChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_short.mp4";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    if (reference->ids.size() < 3) {
        fail("the halves fixture has fewer than three pictures, so the target cannot be watched across a decode");
        return;
    }

    // GOAL: S13.120 -- the sequence the daemon uses. decodeNext() then draw() shows the picture that
    //       decodeNext() decoded; decodeNext() on its own writes to NO target, so a caller that decides
    //       not to show the picture leaves what is on screen untouched; draw() before anything has been
    //       decoded throws std::logic_error rather than drawing whatever the target held; and
    //       renderNext() is still exactly the two composed.
    // WHY-THIS-SHAPE: what is watched across the lone decode is whether the target is PUBLISHED, not
    //       what its pixels hold. Reading the pixels is the obvious version and it cannot be written:
    //       the readback reclaims the published frame, and a target may be reclaimed once per render,
    //       so a second read with no draw between throws rather than answering. Publication is the
    //       property anyway -- only render() publishes, so a decodeNext() that drew anywhere would leave
    //       the target published, and one that did not cannot. renderNext() is run over the same fixture
    //       in a pipeline of its own and its sequence compared, rather than trusting that it delegates:
    //       "composed of the two" is a claim about output, not about code.
    // CONTROL: renderer/video/video_pipeline.cpp, each reverted -- observed below. The untouched-target
    //       check has no production mutation that expresses it: decodeNext() takes no target and so
    //       cannot be made to draw into one. Its control is in this harness -- renderNext() substituted
    //       for that one decodeNext() call, which publishes and reddens it -- and that is recorded here
    //       rather than left to look like an unreddened check.
    // COVERAGE-ASSERT: the picture drawn after the lone decode is asserted to be the NEXT one, so a
    //       readback that had stopped distinguishing pictures (which would make the sequence checks
    //       trivially true) fails here instead of passing there.
    std::string threw;
    bool drawBeforeDecodeThrew = false;
    std::string otherThrow;
    bool decodedFirst = false;
    bool decodedSecond = false;
    bool publishedAfterLoneDecode = true;
    bool publishedAfterDraw = false;
    int afterFirstDraw = -2;
    int afterSecondDraw = -2;
    std::vector<int> halves;
    std::vector<int> composed;
    try {
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
        const auto shown = [&] {
            return wallpaperx::testing::counterFromBgra(
                wallpaperx::testing::reclaimAndReadback(device, targets, target), target.width, target.height);
        };
        {
            VideoPipeline pipeline(device, asset(file), kShaderDir, VideoPipeline::Playback::Once);
            try {
                pipeline.draw(targets, target);
            } catch (const std::logic_error&) {
                drawBeforeDecodeThrew = true;
            } catch (const std::exception& e) {
                otherThrow = e.what();
            }

            decodedFirst = pipeline.decodeNext();
            if (decodedFirst) {
                pipeline.draw(targets, target);
                afterFirstDraw = shown();
                halves.push_back(afterFirstDraw);

                // Decoded and converted, deliberately not drawn: what the daemon does for a pass it may
                // still abandon. shown() above reclaimed the frame the draw published, so the target is
                // unpublished here and only a draw can publish it again.
                decodedSecond = pipeline.decodeNext();
                if (decodedSecond) {
                    publishedAfterLoneDecode = target.published();
                    pipeline.draw(targets, target);
                    publishedAfterDraw = target.published();
                    afterSecondDraw = shown();
                    halves.push_back(afterSecondDraw);
                    while (pipeline.decodeNext()) {
                        pipeline.draw(targets, target);
                        halves.push_back(shown());
                    }
                }
            }
        }
        {
            VideoPipeline composedPipeline(device, asset(file), kShaderDir, VideoPipeline::Playback::Once);
            while (composedPipeline.renderNext(targets, target)) composed.push_back(shown());
        }
    } catch (const std::exception& e) {
        threw = e.what();
    }
    if (!threw.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: the two-halves run threw: %s\n", label.c_str(), file, threw.c_str());
        fail("driving the pipeline through decodeNext() and draw() threw");
        return;
    }
    if (!decodedFirst) {
        std::fprintf(stderr, "FAIL: %s: %s: decodeNext() returned no picture at all\n", label.c_str(), file);
        fail("the halves run decoded nothing, so nothing below was exercised");
        return;
    }

    check(drawBeforeDecodeThrew, "draw() before any picture has been decoded throws std::logic_error");
    if (!drawBeforeDecodeThrew) {
        std::fprintf(stderr, "  it threw %s instead\n", otherThrow.empty() ? "nothing" : otherThrow.c_str());
    }
    if (!decodedSecond) {
        std::fprintf(stderr, "FAIL: %s: %s: only one picture was decoded, so the lone decode was never made\n",
                     label.c_str(), file);
        fail("the halves run never reached a decode with no draw after it");
        return;
    }
    check(!publishedAfterLoneDecode, "decodeNext() publishes no frame: it writes to no target at all");
    // The other half of that claim, and what keeps it from being true of a decode that did nothing:
    // the very next draw does publish.
    check(publishedAfterDraw, "and the draw that follows it does publish one");
    const bool thenShown = afterSecondDraw == reference->ids[1];
    check(thenShown, "and the draw that follows shows the picture that decode had decoded");
    if (!thenShown) {
        std::fprintf(stderr, "  the draw after it showed %d where picture %d was due\n", afterSecondDraw,
                     reference->ids[1]);
    }
    const bool everyPicture = halves == reference->ids;
    check(everyPicture, "decodeNext() then draw() shows every picture once, in order");
    if (!everyPicture) {
        std::fprintf(stderr, "  %zu pictures shown against %zu\n", halves.size(), reference->ids.size());
    }
    const bool sameAsComposed = composed == halves;
    check(sameAsComposed, "renderNext() shows exactly what the two halves show, in the same order");
    if (!sameAsComposed) {
        std::fprintf(stderr, "  renderNext() showed %zu pictures, the halves %zu\n", composed.size(), halves.size());
    }
    if (everyPicture && sameAsComposed) {
        std::printf("%s: %s: %zu pictures through decodeNext()+draw() and the same %zu through renderNext(); the "
                    "target held picture %d across a decode\n",
                    label.c_str(), file, halves.size(), composed.size(), afterFirstDraw);
    }
}

// ------------------------------------------------------------------------- //
// Playing once, and a looping stream with nothing to show
// ------------------------------------------------------------------------- //

void runPlayOnceChecks(Device& device, const std::string& label) {
    if (!videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_short.mp4";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    const size_t pictures = reference->ids.size();

    // GOAL: S13.77 -- a pipeline that plays once, asked explicitly or by default, shows every picture
    //       once, returns false at the end, and keeps returning false without seeking or opening
    //       anything again.
    // WHY-THIS-SHAPE: two calls are made after the first false, because a pipeline that restarts on the
    //       false call itself would still return false there and show picture 0 only on the next.
    // CONTROL: each reverted. renderer/video/video_pipeline.h defaulting to Playback::Loop: the default
    //       case alone, "23 pictures shown against 20", and every fixture of sequence-host "still
    //       returning pictures". renderer/video/video_pipeline.cpp restarting whatever the playback: both.
    constexpr size_t kCallsAfterEnd = 2;
    for (const bool explicitOnce : {true, false}) {
        LoopPlan plan;
        plan.calls = pictures + 1 + kCallsAfterEnd;
        plan.callsAfterEnd = kCallsAfterEnd;
        if (explicitOnce) {
            plan.playback = VideoPipeline::Playback::Once;
        } else {
            plan.playback.reset();
        }
        const LoopRun run = runLoop(device, asset(file), plan);
        const std::string what = std::string(file) + (explicitOnce ? " with Playback::Once" : " with the default playback");
        std::vector<int> shown;
        bool quietAfterEnd = true;
        for (size_t i = 0; i < run.calls.size(); ++i) {
            const LoopCall& call = run.calls[i];
            if (run.endedAt && i >= *run.endedAt) {
                quietAfterEnd = quietAfterEnd && !call.returned && call.tally.seeks.empty() &&
                                call.tally.containerOpens == 0 && call.tally.decoderOpens == 0;
            } else {
                shown.push_back(call.shown);
            }
        }
        if (!run.threw.empty() || !run.readbackFault.empty() || run.endedAt != pictures || shown != reference->ids ||
            run.calls.size() != plan.calls || !quietAfterEnd) {
            std::fprintf(stderr,
                         "FAIL: %s: %s: %zu pictures shown against %zu, ended at call %zd, %zu calls made, the calls from "
                         "the end %s%s%s\n",
                         label.c_str(), what.c_str(), shown.size(), pictures,
                         run.endedAt ? static_cast<ssize_t>(*run.endedAt) + 1 : -1, run.calls.size(),
                         !run.endedAt   ? "never came"
                         : quietAfterEnd ? "quiet"
                                         : "returned a picture, seeked or opened something",
                         run.threw.empty() ? "" : "; threw: ", run.threw.c_str());
            fail("a pipeline that plays once did not stop at the end of the stream and stay stopped");
            continue;
        }
        std::printf("%s: %s: %zu pictures once, then %zu calls returning false with no seek or open\n", label.c_str(),
                    what.c_str(), pictures, kCallsAfterEnd + 1);
    }

    // GOAL: S13.78 -- a looping pipeline that has never shown a picture returns false at the end of the
    //       stream without starting it again.
    // WHY-THIS-SHAPE: every packet is refused as damaged, which the decoder skips, so the stream ends
    //       with nothing shown. What is graded is that no seek or open follows: returning false is not
    //       enough, since a restarted empty stream returns false too.
    // CONTROL: the restart moved to renderer/video/video_pipeline.cpp decodeNext(); with its `&& m_stage`
    //       dropped there, re-observed 2026-09-15: "a looping pipeline that has shown nothing started its
    //       stream again instead of reporting the end", printing "first call decoded 0 picture(s), seeks
    //       to -2048". Reverted.
    // COVERAGE-ASSERT: packets were refused -- red with the refusal never switched on.
    g_packetsRefused = 0;
    LoopPlan plan;
    plan.calls = 2;
    plan.callsAfterEnd = 1;
    plan.beforeCall = [](size_t) { g_refuseAllPackets = true; };
    const LoopRun run = runLoop(device, asset(file), plan);
    const std::string what = std::string(file) + " looping with every packet refused";
    if (g_packetsRefused == 0) {
        fail("no packet was refused, so the stream had pictures to show and nothing here was exercised");
        return;
    }
    bool quiet = run.calls.size() == 2;
    for (const LoopCall& call : run.calls) {
        quiet = quiet && !call.returned && call.tally.seeks.empty() && call.tally.containerOpens == 0;
    }
    if (!run.threw.empty() || run.endedAt != 0 || !quiet) {
        std::fprintf(stderr, "FAIL: %s: %s: %zu calls, ended at %zd%s%s; first call %s\n", label.c_str(), what.c_str(),
                     run.calls.size(), run.endedAt ? static_cast<ssize_t>(*run.endedAt) + 1 : -1,
                     run.threw.empty() ? "" : ", threw: ", run.threw.c_str(),
                     run.calls.empty() ? "not made" : describeCall(run.calls[0]).c_str());
        fail("a looping pipeline that has shown nothing started its stream again instead of reporting the end");
        return;
    }
    std::printf("%s: %s: %d packets refused, both calls false with no seek or open\n", label.c_str(), what.c_str(),
                g_packetsRefused);
}

// ------------------------------------------------------------------------- //
// Looping: a boundary's cost against the length of the file
// ------------------------------------------------------------------------- //

void runLoopCostChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;

    // GOAL: S13.70 -- a loop boundary decodes one picture and opens neither the container nor the
    //       decoder, and makes as many seeks for a 3000-picture file as for a 20-picture one of the same
    //       encoding, in MP4, transport stream and raw H.264.
    // WHY-THIS-SHAPE: counted, never timed: pictures decoded, seeks and opens in the boundary call alone.
    //       The seek count is compared between the two lengths rather than asserted, since how many
    //       attempts a container needs is ffmpeg's; it is printed. Only calls around the boundary are
    //       read back, so 3000 pictures cost what decoding them costs.
    // CONTROL: renderer/video/video_decoder.cpp, each reverted, both paths, all six files. rewind() always
    //       reopening: "no seek, 1 container open(s)". restart() reopening the decoder instead of flushing:
    //       "1 decoder open(s)" -- on the device only when the reopen clears the path first, since a plain
    //       reopen lands in software and that entry reports not answerable. restart() seeking once more
    //       after 100 pictures: each boundary green, the
    //       comparison red, "the 3000-picture boundary decoded 1 picture(s), seeks to 0, -1024".
    // COVERAGE-ASSERT: the long file is a hundred times the short -- red with the short one in its place.
    struct Pair {
        const char* container;
        const char* shortFile;
        const char* longFile;
    };
    constexpr size_t kLengthRatio = 100;
    for (const Pair& pair : {Pair{"MP4", "counter_loop_short.mp4", "counter_loop_long.mp4"},
                             Pair{"transport stream", "counter_loop_short.ts", "counter_loop_long.ts"},
                             Pair{"raw H.264", "counter_loop_short.h264", "counter_loop_long.h264"}}) {
        const std::optional<CounterReference> shortReference = counterReference(label, pair.shortFile);
        const std::optional<CounterReference> longReference = counterReference(label, pair.longFile);
        if (!shortReference || !longReference) continue;
        if (longReference->ids.size() < kLengthRatio * shortReference->ids.size()) {
            std::fprintf(stderr, "FAIL: %s: %s: %zu and %zu pictures\n", label.c_str(), pair.container,
                         shortReference->ids.size(), longReference->ids.size());
            fail("the long loop fixture is not a hundred times the short one, so a cost growing with length could hide");
            continue;
        }
        std::vector<std::pair<size_t, LoopCall>> boundaries;
        for (const auto& [file, reference] : {std::pair{pair.shortFile, &*shortReference},
                                              std::pair{pair.longFile, &*longReference}}) {
            const size_t pictures = reference->ids.size();
            LoopPlan plan;
            plan.calls = pictures + 2;
            plan.readBack = [pictures](size_t index) { return index + 2 >= pictures; };
            plan.drainBefore = [](size_t) { return false; };
            const LoopRun run = runLoop(device, asset(file), plan);
            const std::string what = std::string(file) + ", one boundary";
            if (!loopOnPath(label, what, run, onDevice)) continue;
            if (!gradeLoop(label, what, run, *reference, plan.calls, 4)) continue;
            const LoopCall& boundary = run.calls[pictures];
            if (boundary.tally.pictures != 1 || boundary.tally.containerOpens != 0 || boundary.tally.decoderOpens != 0) {
                std::fprintf(stderr, "FAIL: %s: %s: the boundary %s\n", label.c_str(), what.c_str(),
                             describeCall(boundary).c_str());
                fail("a loop boundary did more than decode the first picture again on the container and decoder it had");
            }
            std::printf("%s: %s (%zu pictures): boundary %s\n", label.c_str(), what.c_str(), pictures,
                        describeCall(boundary).c_str());
            boundaries.emplace_back(pictures, boundary);
        }
        if (boundaries.size() != 2) continue;
        const CallTally& a = boundaries[0].second.tally;
        const CallTally& b = boundaries[1].second.tally;
        if (a.pictures != b.pictures || a.seeks.size() != b.seeks.size() || a.containerOpens != b.containerOpens ||
            a.decoderOpens != b.decoderOpens) {
            std::fprintf(stderr, "FAIL: %s: %s: the %zu-picture boundary %s; the %zu-picture boundary %s\n",
                         label.c_str(), pair.container, boundaries[0].first, describeCall(boundaries[0].second).c_str(),
                         boundaries[1].first, describeCall(boundaries[1].second).c_str());
            fail("a loop boundary's cost depends on the length of the file");
        }
    }
}

// ------------------------------------------------------------------------- //
// Looping: a hardware failure in a later pass
// ------------------------------------------------------------------------- //

void runLoopResumeChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;

    // GOAL: S13.75 -- a hardware failure in pass 2 resumes within pass 2: every picture once per pass,
    //       and the failing call decodes no more pictures than a play-once pipeline decodes for a
    //       failure at the same point in the file (at the boundary: at the end of the file, plus the
    //       first picture). S13.76 -- after that fallback the next pass is decoded in software too.
    // WHY-THIS-SHAPE: compared with play-once, not with a keyframe bound: a transport stream's seek
    //       lands late and is retried a second back, which costs 29 pictures after 100 against a gap
    //       of 24 in play-once too, and a raw bitstream decodes from its start by design. S13.59-S13.62
    //       grade what play-once costs; this grades that a later pass costs no more. Failure points at
    //       the boundary, just after it, mid-pass and late, because each resumes differently.
    // CONTROL: renderer/video/video_decoder.cpp restart(), each reverted. Keeping m_returned: "after 30
    //       picture(s): the failing call decoded 31 pictures, where playing once cost 7" (raw H.264 green:
    //       it resumes from the pass start either way). Carrying packet numbers over: raw H.264 "decoded
    //       108 pictures, where playing once cost 18". Keeping m_resume: on the optimised build, counter_bf2.mkv
    //       failing at the boundary, "call 61 (pass 2, picture 0 of it) showed 1 where 0 was due" and "decoded
    //       14 pictures, where playing once cost 12" (an assertion build aborted on an unset std::optional). Reopening a software decoder for the hardware: "2 device pictures from the failure on".
    // COVERAGE-ASSERT: each failing call opens a software decoder, looping and playing once -- red with
    //       either left unarmed -- and playing once costs at most the pictures up to the failure plus a
    //       keyframe gap: numbers carried across every rewind inflate both alike, "playing once, a failure
    //       after 1 picture(s) decoded 92", while resume-no-timestamps stays green.
    struct Fixture {
        const char* file;
        std::vector<size_t> offsets;
    };
    const Fixture fixtures[] = {
        {"counter_long.mp4", {1, 30, 291}},
        {"counter_bf2.mkv", {0, 1, 25, 50}},
        {"counter_bframes.h264", {1, 17, 40, 77}},
        {"counter_seg.ts", {1, 40, 100}},
    };
    for (const Fixture& fixture : fixtures) {
        const std::optional<CounterReference> reference = counterReference(label, fixture.file);
        if (!reference) continue;
        const size_t pictures = reference->ids.size();
        for (const size_t offset : fixture.offsets) {
            LoopPlan once;
            once.playback = VideoPipeline::Playback::Once;
            once.calls = pictures + 1;
            once.failAt = static_cast<int>(offset == 0 ? pictures : offset);
            once.readBack = [](size_t) { return false; };
            once.drainBefore = [](size_t) { return false; };
            const LoopRun onceRun = runLoop(device, asset(fixture.file), once);
            if (!onceRun.threw.empty() || !onceRun.failureConsumed ||
                onceRun.calls.size() <= static_cast<size_t>(once.failAt) ||
                onceRun.calls[static_cast<size_t>(once.failAt)].tally.decoderOpens == 0) {
                std::fprintf(stderr, "FAIL: %s: %s: the play-once run failing after %d picture(s) %s%s\n", label.c_str(),
                             fixture.file, once.failAt,
                             !onceRun.threw.empty()      ? "threw: "
                             : onceRun.failureConsumed ? "opened no software decoder"
                                                       : "never met its failure",
                             onceRun.threw.c_str());
                fail("the play-once run a later pass's resume is compared with did not resume");
                continue;
            }
            const size_t onceCost = static_cast<size_t>(onceRun.calls[static_cast<size_t>(once.failAt)].tally.pictures);
            const size_t gap = static_cast<size_t>(reference->longestKeyframeGap);
            if (onceCost > static_cast<size_t>(once.failAt) + gap + 1) {
                std::fprintf(stderr, "FAIL: %s: %s: playing once, a failure after %d picture(s) decoded %zu to resume\n",
                             label.c_str(), fixture.file, once.failAt, onceCost);
                fail("playing once, a resume decoded more than the stream up to the failure and a keyframe interval, so "
                     "it is no baseline for a later pass");
                continue;
            }
            const size_t failAt = pictures + offset;
            LoopPlan plan;
            plan.calls = 2 * pictures + 2;
            plan.failAt = static_cast<int>(failAt);
            plan.readBack = [pictures](size_t index) { return index + 1 >= pictures; };
            plan.drainBefore = [](size_t) { return false; };
            const LoopRun run = runLoop(device, asset(fixture.file), plan);
            const std::string what = std::string(fixture.file) + ", hardware failing in pass 2 after " +
                                     std::to_string(offset) + " picture(s)";
            if (run.calls.size() > failAt && (!run.failureConsumed || run.calls[failAt].tally.decoderOpens == 0)) {
                std::fprintf(stderr, "FAIL: %s: %s: the injected failure was %s\n", label.c_str(), what.c_str(),
                             run.failureConsumed ? "met, but the call opened no software decoder" : "never consumed");
                fail("the injected hardware failure never reached the decoder, so no resume was exercised");
                continue;
            }
            int deviceBefore = 0;
            for (size_t i = 0; i < failAt && i < run.calls.size(); ++i) deviceBefore += run.calls[i].tally.devicePictures;
            if (run.threw.empty() && deviceBefore == 0) {
                std::printf("%s: %s: nothing before the failure decoded on the device -- not answerable\n",
                            label.c_str(), what.c_str());
                g_notVerified = true;
                continue;
            }
            bool good = gradeLoop(label, what, run, *reference, plan.calls, pictures + 3);
            if (run.calls.size() <= 2 * pictures) continue;
            const size_t decoded = static_cast<size_t>(run.calls[failAt].tally.pictures);
            const size_t bound = onceCost + (offset == 0 ? 1 : 0);
            if (decoded > bound) {
                std::fprintf(stderr, "FAIL: %s: %s: the failing call decoded %zu pictures, where playing once cost %zu\n",
                             label.c_str(), what.c_str(), decoded, onceCost);
                fail("the call that met a hardware failure in a later pass decoded more than the same failure costs "
                     "when playing once");
                good = false;
            }
            int deviceAfter = 0;
            for (size_t i = failAt; i < run.calls.size(); ++i) deviceAfter += run.calls[i].tally.devicePictures;
            const int nextPassHost = run.calls[2 * pictures].tally.hostPictures;
            if (deviceAfter != 0 || nextPassHost == 0) {
                std::fprintf(stderr, "FAIL: %s: %s: %d device pictures from the failure on; pass 3 began with %d host "
                                     "pictures\n",
                             label.c_str(), what.c_str(), deviceAfter, nextPassHost);
                fail("a decoder that fell back to software went back to the hardware in a later pass");
                good = false;
            }
            std::printf("%s: %s: %zu decoded to resume (playing once: %zu), %s%s\n", label.c_str(), what.c_str(),
                        decoded, onceCost, describeCall(run.calls[failAt]).c_str(), good ? "; every picture once" : "");
        }
    }
}

}  // namespace

// ------------------------------------------------------------------------- //
// Interposers. The executable's definitions preempt libvulkan's and ffmpeg's for every call site
// linked into it, wallpaperx-renderer.a included; the real entry point is reached through RTLD_NEXT.
// ------------------------------------------------------------------------- //

namespace {
template <class Fn>
Fn realSymbol(const char* name) {
    const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
    if (real == nullptr) {
        std::fprintf(stderr, "video_pipeline_probe: cannot resolve the real %s\n", name);
        std::abort();
    }
    return real;
}
}  // namespace

extern "C" VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* layerName,
                                                         uint32_t* count, VkExtensionProperties* properties) {
    static const auto real =
        realSymbol<wallpaperx::testing::EnumerateDeviceExtensionsFn>("vkEnumerateDeviceExtensionProperties");
    if (!g_hideVideoExtensions) return real(physicalDevice, layerName, count, properties);
    return wallpaperx::testing::enumerateWithoutVideoExtensions(real, physicalDevice, layerName, count, properties);
}

#define WALLPAPERX_COUNTED(name, params, args)                   \
    extern "C" VkResult name params {                            \
        static const auto real = realSymbol<VkResult(*) params>(#name); \
        if (g_countVulkan) ++g_vulkanCalls[k_##name];            \
        return real args;                                        \
    }
WALLPAPERX_COUNTED(vkCreateImage, (VkDevice d, const VkImageCreateInfo* i, const VkAllocationCallbacks* a, VkImage* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateImageView,
                   (VkDevice d, const VkImageViewCreateInfo* i, const VkAllocationCallbacks* a, VkImageView* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateBuffer, (VkDevice d, const VkBufferCreateInfo* i, const VkAllocationCallbacks* a, VkBuffer* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkAllocateMemory,
                   (VkDevice d, const VkMemoryAllocateInfo* i, const VkAllocationCallbacks* a, VkDeviceMemory* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateFence, (VkDevice d, const VkFenceCreateInfo* i, const VkAllocationCallbacks* a, VkFence* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateSemaphore,
                   (VkDevice d, const VkSemaphoreCreateInfo* i, const VkAllocationCallbacks* a, VkSemaphore* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkAllocateCommandBuffers, (VkDevice d, const VkCommandBufferAllocateInfo* i, VkCommandBuffer* o),
                   (d, i, o))
WALLPAPERX_COUNTED(vkCreateCommandPool,
                   (VkDevice d, const VkCommandPoolCreateInfo* i, const VkAllocationCallbacks* a, VkCommandPool* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateSampler, (VkDevice d, const VkSamplerCreateInfo* i, const VkAllocationCallbacks* a, VkSampler* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateDescriptorPool,
                   (VkDevice d, const VkDescriptorPoolCreateInfo* i, const VkAllocationCallbacks* a, VkDescriptorPool* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateDescriptorSetLayout,
                   (VkDevice d, const VkDescriptorSetLayoutCreateInfo* i, const VkAllocationCallbacks* a,
                    VkDescriptorSetLayout* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkAllocateDescriptorSets, (VkDevice d, const VkDescriptorSetAllocateInfo* i, VkDescriptorSet* o),
                   (d, i, o))
WALLPAPERX_COUNTED(vkCreatePipelineLayout,
                   (VkDevice d, const VkPipelineLayoutCreateInfo* i, const VkAllocationCallbacks* a, VkPipelineLayout* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateGraphicsPipelines,
                   (VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* i,
                    const VkAllocationCallbacks* a, VkPipeline* o),
                   (d, c, n, i, a, o))
WALLPAPERX_COUNTED(vkCreateRenderPass,
                   (VkDevice d, const VkRenderPassCreateInfo* i, const VkAllocationCallbacks* a, VkRenderPass* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateFramebuffer,
                   (VkDevice d, const VkFramebufferCreateInfo* i, const VkAllocationCallbacks* a, VkFramebuffer* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkCreateShaderModule,
                   (VkDevice d, const VkShaderModuleCreateInfo* i, const VkAllocationCallbacks* a, VkShaderModule* o),
                   (d, i, a, o))
WALLPAPERX_COUNTED(vkWaitSemaphores, (VkDevice d, const VkSemaphoreWaitInfo* i, uint64_t t), (d, i, t))
WALLPAPERX_COUNTED(vkQueueWaitIdle, (VkQueue q), (q))
WALLPAPERX_COUNTED(vkDeviceWaitIdle, (VkDevice d), (d))
#undef WALLPAPERX_COUNTED

namespace {
bool isHeld(VkFence fence) { return std::find(g_heldFences.begin(), g_heldFences.end(), fence) != g_heldFences.end(); }
}  // namespace

extern "C" VkResult vkWaitForFences(VkDevice d, uint32_t n, const VkFence* f, VkBool32 all, uint64_t t) {
    static const auto real = realSymbol<VkResult (*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)>(
        "vkWaitForFences");
    if (g_countVulkan) ++g_vulkanCalls[k_vkWaitForFences];
    for (uint32_t i = 0; i < n; ++i) {
        if (!isHeld(f[i])) continue;
        if (g_stallHeld) {
            ++g_stalledWaits;
            return VK_TIMEOUT;
        }
        ++g_waitsOnHeld;
        g_heldFences.clear();
        break;
    }
    return real(d, n, f, all, t);
}

extern "C" VkResult vkGetFenceStatus(VkDevice d, VkFence fence) {
    static const auto real = realSymbol<VkResult (*)(VkDevice, VkFence)>("vkGetFenceStatus");
    if (isHeld(fence)) return VK_NOT_READY;
    return real(d, fence);
}

extern "C" VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    static const auto real = realSymbol<VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)>("vkQueueSubmit");
    const VkResult result = real(queue, count, submits, fence);
    if (g_holdNextSubmission && result == VK_SUCCESS && fence != VK_NULL_HANDLE) {
        g_heldFences.push_back(fence);
        g_holdNextSubmission = false;
    }
    return result;
}

extern "C" int av_hwframe_ctx_init(AVBufferRef* ref) {
    static const auto real = realSymbol<int (*)(AVBufferRef*)>("av_hwframe_ctx_init");
    auto* frames = reinterpret_cast<AVHWFramesContext*>(ref->data);
    if (g_trackPools && frames->format == AV_PIX_FMT_VULKAN && frames->free != &onPoolFreed) {
        PoolRecord pool;
        pool.width = frames->width;
        pool.height = frames->height;
        pool.createdDuring = g_poolCall;
        g_pools.push_back(pool);
        g_hookedPools.push_back(HookedPool{frames, frames->free, g_pools.size() - 1, g_poolGeneration});
        frames->free = &onPoolFreed;
    }
    return real(ref);
}

extern "C" int avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    static const auto real = realSymbol<int (*)(AVCodecContext*, AVFrame*)>("avcodec_receive_frame");
    const bool decoders = isDecodersContext(context);
    if (decoders && g_receiveExternalRemaining > 0) {
        --g_receiveExternalRemaining;
        return AVERROR_EXTERNAL;
    }
    if (decoders && g_releaseHoldAtReceive) {
        g_heldFences.clear();
        g_releaseHoldAtReceive = false;
        g_holdReleasedAtReceive = true;
    }
    const int err = real(context, frame);
    if (err == 0 && decoders && (g_inflateWidth != 0 || g_inflateHeight != 0) && frame->format == AV_PIX_FMT_VULKAN) {
        frame->width += g_inflateWidth;
        frame->height += g_inflateHeight;
        g_inflateWidth = 0;
        g_inflateHeight = 0;
        g_inflated = true;
    }
    if (err == 0 && decoders && g_logPictures) {
        g_pictureLog.push_back(LoggedPicture{frame->width, frame->format == AV_PIX_FMT_VULKAN});
    }
    if (err == 0 && decoders && g_trackPools && frame->format == AV_PIX_FMT_VULKAN && frame->hw_frames_ctx != nullptr) {
        if (PoolRecord* pool = poolRecord(reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data))) {
            ++pool->pictures;
            pool->peek = *frame->hw_frames_ctx;
        }
    }
    if (err == 0 && decoders && g_inFailingCall && g_stripTimestamps) frame->best_effort_timestamp = AV_NOPTS_VALUE;
    if (err == 0 && decoders && g_recordPictures) {
        ++(frame->format == AV_PIX_FMT_VULKAN ? g_devicePictures : g_hostPictures);
        if (g_inFailingCall) ++g_failingPictures;
    }
    if (err == 0 && decoders && g_tallying) {
        ++g_tally.pictures;
        ++(frame->format == AV_PIX_FMT_VULKAN ? g_tally.devicePictures : g_tally.hostPictures);
    }
    return err;
}

extern "C" int av_read_frame(AVFormatContext* format, AVPacket* packet) {
    static const auto real = realSymbol<int (*)(AVFormatContext*, AVPacket*)>("av_read_frame");
    const int err = real(format, packet);
    if (err >= 0 && g_clearPositions && packet->pos >= 0) {
        packet->pos = -1;
        if (g_tallying) ++g_tally.positionsCleared;
    }
    return err;
}

extern "C" int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    static const auto real = realSymbol<int (*)(AVCodecContext*, const AVPacket*)>("avcodec_send_packet");
    if (g_refuseAllPackets && packet != nullptr && isDecodersContext(context)) {
        ++g_packetsRefused;
        return AVERROR_INVALIDDATA;
    }
    if (g_inFailingCall && packet != nullptr && isDecodersContext(context) && g_refuseFrom > 0 &&
        g_decoderOpens >= g_refuseFrom && g_decoderOpens <= g_refuseThrough) {
        return AVERROR_INVALIDDATA;
    }
    return real(context, packet);
}

extern "C" int avcodec_open2(AVCodecContext* context, const AVCodec* codec, AVDictionary** options) {
    static const auto real = realSymbol<int (*)(AVCodecContext*, const AVCodec*, AVDictionary**)>("avcodec_open2");
    const int err = real(context, codec, options);
    if (err >= 0 && g_inFailingCall && isDecodersContext(context)) ++g_decoderOpens;
    if (err >= 0 && g_tallying && isDecodersContext(context)) ++g_tally.decoderOpens;
    if (err >= 0 && g_trackCodecs && isDecodersContext(context)) {
        g_codecs.push_back(CodecRecord{context, context->hw_device_ctx != nullptr, g_poolCall});
    }
    return err;
}

// Only a context this records as open is marked, and only once: an address ffmpeg reuses for a later
// context belongs to that context's record.
extern "C" void avcodec_free_context(AVCodecContext** context) {
    static const auto real = realSymbol<void (*)(AVCodecContext**)>("avcodec_free_context");
    if (g_trackCodecs && context != nullptr && *context != nullptr) {
        for (CodecRecord& codec : g_codecs) {
            if (codec.context != *context || codec.freed) continue;
            codec.freed = true;
            codec.freedDuring = g_poolCall;
        }
    }
    real(context);
}

extern "C" int av_seek_frame(AVFormatContext* format, int stream, int64_t timestamp, int flags) {
    static const auto real = realSymbol<int (*)(AVFormatContext*, int, int64_t, int)>("av_seek_frame");
    const bool byte = (flags & AVSEEK_FLAG_BYTE) != 0;
    if (g_tallying) {
        const bool first = g_tally.seeks.empty();
        int64_t aimed = timestamp;
        if (g_loopSeek == LoopSeek::LateFirst && first && !byte && stream >= 0) {
            aimed += av_rescale_q(kLateSeconds, AVRational{1, 1}, format->streams[stream]->time_base);
        }
        const bool refused = g_loopSeek == LoopSeek::FailAll || (g_loopSeek == LoopSeek::FailFirst && first) ||
                             (g_loopSeek == LoopSeek::FailTimestamps && !byte);
        const int result = refused ? AVERROR(EPERM) : real(format, stream, aimed, flags);
        g_tally.seeks.push_back(SeekCall{timestamp, result, byte});
        return result;
    }
    if (!g_inFailingCall) return real(format, stream, timestamp, flags);
    int64_t start = 0;
    AVRational timeBase{1, AV_TIME_BASE};
    if (stream >= 0) {
        timeBase = format->streams[stream]->time_base;
        if (format->streams[stream]->start_time != AV_NOPTS_VALUE) start = format->streams[stream]->start_time;
    }
    int64_t aimed = timestamp;
    if (g_seekShiftSeconds != 0 && g_failingSeeks.empty()) {
        aimed += av_rescale_q(g_seekShiftSeconds, AVRational{1, 1}, timeBase);
    }
    const bool refused = g_seekFailure == SeekFailure::All ||
                         (g_seekFailure == SeekFailure::AwayFromStart && timestamp > start);
    const int result = refused ? AVERROR(EPERM) : real(format, stream, aimed, flags);
    g_failingSeeks.push_back(SeekCall{timestamp, result, byte});
    return result;
}

extern "C" int avformat_open_input(AVFormatContext** context, const char* url, const AVInputFormat* format,
                                   AVDictionary** options) {
    static const auto real =
        realSymbol<int (*)(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**)>("avformat_open_input");
    if (g_inFailingCall) ++g_containerOpens;
    if (g_tallying) ++g_tally.containerOpens;
    return real(context, url, format, options);
}

namespace {

// One validated Device for `body`, and the exit code its checks earn: the whole process for most
// modes, the whole child for a forked case.
int runGraded(const std::string& label, bool hideVideoExtensions, const std::function<void(Device&)>& body) {
    const bool graded = wallpaperx::testing::gradedRun(label, [&](wallpaperx::testing::ValidationGrade& grade) {
        wallpaperx::testing::DecoderValidationOracle oracle;
        {
            std::optional<Device> device;
            try {
                g_hideVideoExtensions = hideVideoExtensions;
                try {
                    device.emplace(/*enableValidation=*/true);
                } catch (...) {
                    g_hideVideoExtensions = false;
                    throw;
                }
                g_hideVideoExtensions = false;
                oracle.attach(*device, grade);
                std::printf("%s: on %s\n", label.c_str(), device->info().gpuName.c_str());
                body(*device);
                device->waitForSubmissions();
            } catch (const std::exception& e) {
                // Reported, not rethrown: an escaping exception would skip the reconciliation and
                // gradedRun's own verdict.
                fail(label + " threw: " + e.what());
            }
            clearFailingCallConditions();
            clearLoopConditions();
            g_clearPositions = false;
            g_countVulkan = false;
            g_heldFences.clear();
            oracle.detach();
        }
        oracle.reconcile(label.c_str());
        return wallpaperx::testing::failureCount() == 0;
    });

    const int failures = wallpaperx::testing::reportFailures("video_pipeline_probe");
    if (!graded || failures > 0) return 1;
    if (g_notVerified) {
        std::printf("%s: NOT VERIFIED here -- reported to ctest as a skip, not a pass\n", label.c_str());
        return kNotVerifiedExitCode;
    }
    std::printf("%s: all checks passed, no unexpected validation messages\n", label.c_str());
    return 0;
}

constexpr int kChildSeconds = 300;

// GOAL: S13.80 -- decoding a stream that changes size on the hardware path does not kill the process.
// WHY-THIS-SHAPE: the case runs in a forked child that builds its own Device after the fork, so no
//       Vulkan state crosses it, and this parent grades how the child ended. Run in-process, the
//       crash this exists for would end the probe with no verdict for that stream or any after it. A
//       child still running after kChildSeconds is killed and failed, never waited out. Under a
//       sanitizer the crash becomes the sanitizer's report and exit 1, graded as a failed child.
// CONTROL: renderer/video/video_decoder.cpp, the decoder before the fix, in two sites: attachHardwareFrames()
//       without its four lines holding the pool, and allocatePicture() skipping holds() when nothing is held.
//       Reverted. shape-device: "the child died by signal 11 (Segmentation fault)" for all four raw H.264
//       patterns, the transport stream, Matroska and HEVC 320x240-160x120-320x240, each named and the next
//       still run; loop-device and release-device died too, both reported. The first site alone no longer
//       crashes: holds() finds no pool, so every hardware picture is declined and decodes in software.
//       Harness, each reverted: the child made to fail its arming -> "could not arm itself against its
//       parent's death" and "not a verdict on the decoder"; kChildSeconds set to 0 -> "still running
//       after 0 seconds, and killed", no child left behind.
void superviseChild(const std::string& what, const std::function<int()>& child) {
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "FAIL: %s: fork() failed: %s\n", what.c_str(), std::strerror(errno));
        fail("the harness could not fork a child for a case, which is not a verdict on the decoder");
        return;
    }
    if (pid == 0) {
        if (!wallpaperx::testing::armAgainstParentDeath(parent)) _exit(wallpaperx::testing::kChildArmFailedExit);
        wallpaperx::testing::suppressCoreDump();
        wallpaperx::testing::g_failures = 0;
        g_notVerified = false;
        const int code = child();
        std::fflush(stdout);
        std::fflush(stderr);
        _exit(code);
    }

    int status = 0;
    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0 && errno != EINTR) {
            std::fprintf(stderr, "FAIL: %s: waitpid() failed: %s\n", what.c_str(), std::strerror(errno));
            kill(pid, SIGKILL);
            fail("the harness lost its child, which is not a verdict on the decoder");
            return;
        }
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec > kChildSeconds) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            std::fprintf(stderr, "FAIL: %s: still running after %d seconds, and killed\n", what.c_str(), kChildSeconds);
            fail("a case did not finish");
            return;
        }
        const timespec pause{0, 20'000'000};
        nanosleep(&pause, nullptr);
    }
    if (WIFSIGNALED(status)) {
        std::fprintf(stderr, "FAIL: %s: the child died by signal %d (%s) before its checks finished\n", what.c_str(),
                     WTERMSIG(status), strsignal(WTERMSIG(status)));
        fail("decoding the stream killed the process");
        return;
    }
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 0) return;
    if (code == kNotVerifiedExitCode) {
        g_notVerified = true;
        return;
    }
    if (const char* fault = wallpaperx::testing::forkPreambleFault(code)) {
        std::fprintf(stderr, "FAIL: %s: %s\n", what.c_str(), fault);
        fail("the harness could not run a case in its child, which is not a verdict on the decoder");
        return;
    }
    std::fprintf(stderr, "FAIL: %s: the child exited with %d; its reasons are printed above\n", what.c_str(), code);
    fail("a case failed in its child");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const std::vector<std::string> modes = {
        "sequence-device",   "sequence-host", "resume-timestamps", "resume-no-timestamps", "resume-late-seek",
        "resume-end",        "resume-seek-failure", "resume-drop-count", "shape", "shape-device",
        "steady-device",     "steady-host",   "loop-device",       "loop-host",  "loop-cost-device",
        "loop-cost-host",    "loop-resume",   "release-device",    "retire-device",
        "halves-device",     "halves-host",   "timing-device",     "timing-host"};
    if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
        std::fprintf(stderr, "usage: %s <mode>, one of:", argv[0]);
        for (const std::string& m : modes) std::fprintf(stderr, " %s", m.c_str());
        std::fprintf(stderr, "\n");
        return 1;
    }
    const std::string label = "video_pipeline_probe (" + mode + ")";

    if (mode == "shape-device" || mode == "loop-device" || mode == "release-device" || mode == "retire-device") {
        std::vector<std::pair<std::string, std::function<void(Device&)>>> cases;
        if (mode == "shape-device") {
            check(kSizeChangeFixtures.size() == kSizeChangeFixtureCount,
                  "the size-change fixture list no longer has the eleven entries this entry is written for");
            for (const SizeChangeFixture& fixture : kSizeChangeFixtures) {
                cases.emplace_back(fixture.file,
                                   [&fixture, label](Device& device) { runShapeDeviceFixture(device, label, fixture); });
            }
            cases.emplace_back("a picture larger than its image",
                               [label](Device& device) { runOversizeChecks(device, label); });
        } else if (mode == "retire-device") {
            for (const SizeChangeFixture& fixture : kSizeChangeFixtures) {
                const std::string file = fixture.file;
                if (file != "counter_shrink_grow.hevc" && file != "counter_tiny.obu" && file != "counter_grow.obu") {
                    continue;
                }
                cases.emplace_back(file + ", fallback waits",
                                   [&fixture, label](Device& device) { runRetireWaitChecks(device, label, fixture); });
                cases.emplace_back(file + ", end of stream",
                                   [&fixture, label](Device& device) { runRetireEndChecks(device, label, fixture); });
                if (file != "counter_grow.obu") {
                    cases.emplace_back(file + ", stalled teardown", [&fixture, label](Device& device) {
                        runStalledTeardownChecks(device, label, fixture);
                    });
                }
            }
            cases.emplace_back("counter_sg_30.hevc, refused before decoding",
                               [label](Device& device) { runRefusedTeardownChecks(device, label); });
        } else if (mode == "loop-device") {
            cases.emplace_back("every loop fixture", [label](Device& device) {
                runLoopChecks(device, label, /*onDevice=*/true);
                runLoopLandingChecks(device, label, /*onDevice=*/true);
            });
        } else {
            cases.emplace_back("counter_resize.h264", [label](Device& device) { runReleaseChecks(device, label); });
        }
        for (const auto& [name, body] : cases) {
            const std::string caseLabel = label + " [" + name + "]";
            superviseChild(caseLabel, [&caseLabel, &body] { return runGraded(caseLabel, false, body); });
        }
        const int failures = wallpaperx::testing::reportFailures("video_pipeline_probe");
        if (failures > 0) return 1;
        if (g_notVerified) {
            std::printf("%s: NOT VERIFIED here -- reported to ctest as a skip, not a pass\n", label.c_str());
            return kNotVerifiedExitCode;
        }
        std::printf("%s: all %zu case(s) passed in their own children\n", label.c_str(), cases.size());
        return 0;
    }

    return runGraded(label, mode == "sequence-host" || mode == "shape" || mode == "steady-host" || mode == "loop-host" ||
                                mode == "loop-cost-host" || mode == "halves-host" || mode == "timing-host",
                     [&](Device& device) {
                         if (mode == "sequence-device") runSequenceChecks(device, label, /*onDevice=*/true);
                         if (mode == "sequence-host") runSequenceChecks(device, label, /*onDevice=*/false);
                         if (mode == "resume-timestamps") runResumeTimestampChecks(device, label);
                         if (mode == "resume-no-timestamps") runResumeNoTimestampChecks(device, label);
                         if (mode == "resume-late-seek") runLateSeekChecks(device, label);
                         if (mode == "resume-end") runEndChecks(device, label);
                         if (mode == "resume-seek-failure") runSeekFailureChecks(device, label);
                         if (mode == "resume-drop-count") runDropCountChecks(device, label);
                         if (mode == "shape") runShapeChecks(device, label);
                         if (mode == "halves-device") runHalvesChecks(device, label, /*onDevice=*/true);
                         if (mode == "halves-host") runHalvesChecks(device, label, /*onDevice=*/false);
                         if (mode == "timing-device") runTimingChecks(device, label, /*onDevice=*/true);
                         if (mode == "timing-host") runTimingChecks(device, label, /*onDevice=*/false);
                         if (mode == "steady-device") runSteadyChecks(device, label, /*onDevice=*/true);
                         if (mode == "steady-host") runSteadyChecks(device, label, /*onDevice=*/false);
                         if (mode == "loop-host") {
                             runLoopChecks(device, label, /*onDevice=*/false);
                             runLoopLandingChecks(device, label, /*onDevice=*/false);
                             runLoopFileChecks(device, label);
                             runPlayOnceChecks(device, label);
                         }
                         if (mode == "loop-cost-device") runLoopCostChecks(device, label, /*onDevice=*/true);
                         if (mode == "loop-cost-host") runLoopCostChecks(device, label, /*onDevice=*/false);
                         if (mode == "loop-resume") runLoopResumeChecks(device, label);
                     });
}
