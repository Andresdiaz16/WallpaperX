// SPDX-License-Identifier: GPL-2.0-or-later
//
// VideoPipeline, and VideoDecoder's resume after a mid-stream hardware failure: S13.56 onwards.
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
        text += (text.empty() ? "" : ", ") + std::to_string(seek.target) + (seek.result < 0 ? " (failed)" : "");
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
    //       seeks to 163840, 148480, 133120, 102400, 40960, 0", 822 pictures decoded.
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
    //       "Away from the start" fails the resume seek and lets the rewind to the start succeed.
    // CONTROL: renderer/video/video_decoder.cpp, each reverted. reopenInSoftware()'s failed-seek branch
    //       without its rewind(): both cases "21 never shown". rewind() not reopening the container when
    //       its own seek fails: the every-seek case alone red, "2 seeks, some failed; container opened 0
    //       time(s)" and "21 never shown".
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
    //       vkWaitForFences x216" (x312 host, x318 Lavapipe; fewer where nothing was pending). The stage
    //       rebuilt on every call: "vkCreateImage x954, vkCreateImageView x1272, ...".
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
    return err;
}

extern "C" int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    static const auto real = realSymbol<int (*)(AVCodecContext*, const AVPacket*)>("avcodec_send_packet");
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
    return err;
}

extern "C" int av_seek_frame(AVFormatContext* format, int stream, int64_t timestamp, int flags) {
    static const auto real = realSymbol<int (*)(AVFormatContext*, int, int64_t, int)>("av_seek_frame");
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
    g_failingSeeks.push_back(SeekCall{timestamp, result});
    return result;
}

extern "C" int avformat_open_input(AVFormatContext** context, const char* url, const AVInputFormat* format,
                                   AVDictionary** options) {
    static const auto real =
        realSymbol<int (*)(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**)>("avformat_open_input");
    if (g_inFailingCall) ++g_containerOpens;
    return real(context, url, format, options);
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const std::vector<std::string> modes = {"sequence-device",     "sequence-host",   "resume-timestamps",
                                            "resume-no-timestamps", "resume-late-seek", "resume-end",
                                            "resume-seek-failure", "resume-drop-count", "shape",
                                            "steady-device",       "steady-host"};
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
                g_hideVideoExtensions = mode == "sequence-host" || mode == "shape" || mode == "steady-host";
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
                device->waitForSubmissions();
            } catch (const std::exception& e) {
                // Reported, not rethrown: an escaping exception would skip the reconciliation and
                // gradedRun's own verdict.
                fail(label + " threw: " + e.what());
            }
            clearFailingCallConditions();
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
