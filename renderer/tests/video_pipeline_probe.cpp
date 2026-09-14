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
#include "support/gpu_readback.h"
#include "support/picture_counter.h"
#include "support/validation_grade.h"
#include "support/video_probe_support.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <dlfcn.h>

#include <algorithm>
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

using wallpaperx::Device;
using wallpaperx::OffscreenTarget;
using wallpaperx::OffscreenTargetManager;
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
        g_devicePictures = 0;
        g_hostPictures = 0;
        g_recordPictures = true;
        for (size_t call = 0;; ++call) {
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
            try {
                more = pipeline.renderNext(targets, target);
            } catch (...) {
                if (failing) endFailingCall();
                throw;
            }
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
    // CONTROL: each reverted. renderer/video/video_pipeline.cpp renderNext() rendering before converting:
    //       every fixture red on both paths and both drivers, "call 1 showed -1 where 0 was due; 0 shown
    //       more than once, 1 never shown, 1 unreadable". renderer/video/video_decoder.cpp admit()
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
    for (const int after : {1, 5, 9, 17, 26, 40, 58, 77}) {
        const PipelineRun run = runPipeline(device, asset(file), reference->ids.size(), FailurePlan{after, {}});
        const std::string what = failurePoint(file, after);
        if (!failureExercised(label, what, run)) continue;
        checkSoftwareTookOver(label, what, run);
        if (gradeSequence(label, what, run, *reference)) {
            std::printf("%s: %s: every picture once, %d decoded to resume\n", label.c_str(), what.c_str(),
                        run.failing.picturesDecoded);
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
    //       unreadable, so "never black" needs no separate check. Software only: ffmpeg's Vulkan H.264
    //       decoder crashes at a size change, stock command line included.
    // CONTROL: renderer/video/video_pipeline.cpp renderNext(), each reverted, both drivers. The stage
    //       built only for the first picture: "the pipeline threw after 30 pictures: FrameConverter: the
    //       frame's picture differs". Rendering before converting: "2 never shown, 2 unreadable" -- black
    //       on the first call and again at the size change.
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

// ------------------------------------------------------------------------- //
// What a steady picture costs
// ------------------------------------------------------------------------- //

void runSteadyChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_long.mp4";
    if (!haveAsset(file, label)) return;

    // GOAL: S13.67 -- once its stage exists, a call creates no Vulkan object and waits on nothing.
    // WHY-THIS-SHAPE: counted at this executable's Vulkan symbols during renderNext() alone. The
    //       queue is drained before each counted call, so a wait inside it is the pipeline's and not
    //       one a slow GPU forced: FrameConverter's host path waits when two uploads are still in
    //       flight, and counting across an undrained queue would grade the scheduler. The first calls
    //       are left out because the Device's fence and command buffer pool grows to its most batches.
    // CONTROL: renderer/video/video_pipeline.cpp renderNext(), each reverted, on the device path, the
    //       host path and Lavapipe. Device::waitForSubmissions() after the render: "318 steady calls made
    //       vkWaitForFences x318" on all three (fewer where nothing is pending). The stage rebuilt on every
    //       call: "vkCreateImage x954, vkCreateImageView x1272, ..." on the device path.
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
        if (plan.playback) {
            pipeline.emplace(device, file, kShaderDir, *plan.playback);
        } else {
            pipeline.emplace(device, file, kShaderDir);
        }
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
            call.returned = pipeline->renderNext(targets, target);
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
    // WHY-THIS-SHAPE: every call read back and compared with picture (call mod N) of an independent
    //       decode; comparing the boundary call alone would miss a pass that starts right and repeats
    //       a picture two calls later. Opens and Vulkan work are counted in the boundary call alone,
    //       with the queue drained before it. The resize fixture is software only (S13.66) and exempt
    //       from the Vulkan count, since its boundary changes size and must rebuild.
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
        if (fixture.changesSize && onDevice) continue;
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
        const LoopRun run = runLoop(device, asset(fixture.file), plan);
        const std::string what = std::string(fixture.file) + " (" + fixture.what + "), three passes";
        if (!loopOnPath(label, what, run, onDevice)) continue;
        bool good = gradeLoop(label, what, run, *reference, plan.calls, plan.calls);
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
    //       packet: the transport stream "call 121 (pass 2, picture 0 of it) showed 24 where 0 was due"
    //       (Matroska green: nothing is read for it to judge). A failed read after a seek taken as landed:
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
    // CONTROL: renderer/video/video_pipeline.cpp renderNext() restarting without a stage, reverted: "first
    //       call decoded 0 picture(s), seeks to -2048".
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
    //       "1 decoder open(s)". restart() seeking once more after 100 pictures: each boundary green, the
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
    //       108 pictures, where playing once cost 18". Keeping m_resume: aborted on an unset std::optional,
    //       exit 134. Reopening a software decoder for the hardware: "2 device pictures from the failure on".
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
WALLPAPERX_COUNTED(vkWaitForFences, (VkDevice d, uint32_t n, const VkFence* f, VkBool32 all, uint64_t t),
                   (d, n, f, all, t))
WALLPAPERX_COUNTED(vkWaitSemaphores, (VkDevice d, const VkSemaphoreWaitInfo* i, uint64_t t), (d, i, t))
WALLPAPERX_COUNTED(vkQueueWaitIdle, (VkQueue q), (q))
WALLPAPERX_COUNTED(vkDeviceWaitIdle, (VkDevice d), (d))
#undef WALLPAPERX_COUNTED

extern "C" int avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    static const auto real = realSymbol<int (*)(AVCodecContext*, AVFrame*)>("avcodec_receive_frame");
    const bool decoders = isDecodersContext(context);
    if (decoders && g_receiveExternalRemaining > 0) {
        --g_receiveExternalRemaining;
        return AVERROR_EXTERNAL;
    }
    const int err = real(context, frame);
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
    return err;
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

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const std::vector<std::string> modes = {"sequence-device",     "sequence-host",   "resume-timestamps",
                                            "resume-no-timestamps", "resume-late-seek", "resume-end",
                                            "resume-seek-failure", "resume-drop-count", "shape",
                                            "steady-device",       "steady-host",     "loop-device",
                                            "loop-host",           "loop-cost-device", "loop-cost-host",
                                            "loop-resume"};
    if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
        std::fprintf(stderr, "usage: %s <mode>, one of:", argv[0]);
        for (const std::string& m : modes) std::fprintf(stderr, " %s", m.c_str());
        std::fprintf(stderr, "\n");
        return 1;
    }
    const std::string label = "video_pipeline_probe (" + mode + ")";

    const bool graded = wallpaperx::testing::gradedRun(label, [&](wallpaperx::testing::ValidationGrade& grade) {
        wallpaperx::testing::DecoderValidationOracle oracle;
        {
            std::optional<Device> device;
            try {
                g_hideVideoExtensions = mode == "sequence-host" || mode == "shape" || mode == "steady-host" ||
                                        mode == "loop-host" || mode == "loop-cost-host";
                try {
                    device.emplace(/*enableValidation=*/true);
                } catch (...) {
                    g_hideVideoExtensions = false;
                    throw;
                }
                g_hideVideoExtensions = false;
                oracle.attach(*device, grade);
                std::printf("%s: on %s\n", label.c_str(), device->info().gpuName.c_str());

                if (mode == "sequence-device") runSequenceChecks(*device, label, /*onDevice=*/true);
                if (mode == "sequence-host") runSequenceChecks(*device, label, /*onDevice=*/false);
                if (mode == "resume-timestamps") runResumeTimestampChecks(*device, label);
                if (mode == "resume-no-timestamps") runResumeNoTimestampChecks(*device, label);
                if (mode == "resume-late-seek") runLateSeekChecks(*device, label);
                if (mode == "resume-end") runEndChecks(*device, label);
                if (mode == "resume-seek-failure") runSeekFailureChecks(*device, label);
                if (mode == "resume-drop-count") runDropCountChecks(*device, label);
                if (mode == "shape") runShapeChecks(*device, label);
                if (mode == "steady-device") runSteadyChecks(*device, label, /*onDevice=*/true);
                if (mode == "steady-host") runSteadyChecks(*device, label, /*onDevice=*/false);
                if (mode == "loop-device") {
                    runLoopChecks(*device, label, /*onDevice=*/true);
                    runLoopLandingChecks(*device, label, /*onDevice=*/true);
                }
                if (mode == "loop-host") {
                    runLoopChecks(*device, label, /*onDevice=*/false);
                    runLoopLandingChecks(*device, label, /*onDevice=*/false);
                    runLoopFileChecks(*device, label);
                    runPlayOnceChecks(*device, label);
                }
                if (mode == "loop-cost-device") runLoopCostChecks(*device, label, /*onDevice=*/true);
                if (mode == "loop-cost-host") runLoopCostChecks(*device, label, /*onDevice=*/false);
                if (mode == "loop-resume") runLoopResumeChecks(*device, label);
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
