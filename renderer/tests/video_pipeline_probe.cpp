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
#include "video/ycbcr_renderer.h"
#include "vulkan/device.h"
#include "vulkan/owned_device.h"
#include "vulkan/gpu_fault.h"
#include "vulkan/offscreen.h"

#include "catch_up_budget.h"
#include "support/check.h"
#include "support/forked_child.h"
#include "support/gpu_readback.h"
#include "support/sanitizer_ignored_caller.h"
#include "support/length_change_flv.h"
#include "support/picture_counter.h"
#include "support/reset_point_reference.h"
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
#include <execinfo.h>
#include <pthread.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
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
#include <utility>
#include <variant>
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

// Set for the whole process by modes whose checks count or inject inside the caller's own call: strips
// VK_KHR_internally_synchronized_queues, so the Device keeps its queues to itself and the pipeline decodes
// on the calling thread, with no thread decoding ahead outside that call.
bool g_hideSynchronizedQueues = false;

// Set by damaged-device-no-inline: strips VK_KHR_video_maintenance2 alone, so the Device decodes on the hardware
// with ffmpeg's cached session parameters, the configuration E20's side-data guard is for.
bool g_hideVideoMaintenance2 = false;

// The decoder's own codec context is the one with `opaque` set; avformat_find_stream_info decodes
// through the same entry points on contexts of its own, which must neither be failed nor counted.
bool isDecodersContext(const AVCodecContext* context) { return context != nullptr && context->opaque != nullptr; }

int g_receiveExternalRemaining = 0;

// Pictures the decoder received, by where they live, while a run records them.
bool g_recordPictures = false;
int g_devicePictures = 0;
// Packets with data sent to the decoder's software context while a run records pictures, shown or not.
std::atomic<int> g_softwarePacketsSent{0};
int g_hostPictures = 0;

// Everything below is counted, or applies, only during the call that meets the failure.
bool g_inFailingCall = false;
int g_failingPictures = 0;
struct SeekCall {
    int64_t target;  // as the decoder asked, before any shift below
    int result;
    bool byte = false;
    // Whether the first packet of the sought stream read after this seek was a keyframe; unset when
    // none was read before the next seek.
    std::optional<bool> landedOnKeyframe;
};
std::vector<SeekCall> g_failingSeeks;
// The g_failingSeeks entry, and its stream, still waiting for its first packet; -1 for none.
int g_seekAwaitingPacket = -1;
int g_seekAwaitingStream = -1;
int g_containerOpens = 0;
// Opens and flushes of the decoder's SOFTWARE context (no hw_device_ctx): a seek retry flushes and reuses
// the decoder, so a software pass starts at an open or a flush, and these two together number the passes.
// A hardware context is never counted, so pass 1 cannot be the failing hardware decoder's.
int g_decoderOpens = 0;
int g_decoderFlushes = 0;
// Added to the first seek's target: a demuxer that lands past what it was asked for.
int64_t g_seekShiftSeconds = 0;
// Second: only the second seek of the call fails -- a start's search, or a software start's retry.
enum class SeekFailure { None, AwayFromStart, All, Second };
SeekFailure g_seekFailure = SeekFailure::None;
// avcodec_send_packet refuses every packet sent to the software context while the software pass under way
// in the failing call is the g_refuseFrom-th to g_refuseThrough-th; 0 is off.
int g_refuseFrom = 0;
int g_refuseThrough = 0;
// Pictures the decoder receives lose their timestamp.
bool g_stripTimestamps = false;
// av_hwframe_ctx_init fails this many more times for Vulkan images: ffmpeg refusing the device's images
// inside the format callback of the first packet sent, on a device that still answers.
int g_failFramesInit = 0;
// Vulkan frames contexts av_hwframe_ctx_init made: the device's yes to a stream's headers.
int g_vulkanFramesMade = 0;
// The next vkCreateVideoSessionKHR fails: ffmpeg's own decoder setup failing after the device's yes. Counted when it
// fires. ffmpeg loads the function through the decoder's vkGetDeviceProcAddr, interposed below.
bool g_failNextSession = false;
int g_sessionsFailed = 0;
// The next device picture the decoder's hardware context allocates asks for a second one first, from
// inside the decoder's own get_buffer2: the request ffmpeg makes for a stand-in reference. Counted when made.
bool g_standInAtNextPicture = false;
int g_standInsForced = 0;
// Whether the last picture the decoder's context returned while g_recordPictures was set was a device one.
bool g_lastReceivedDevice = false;

// Packets sent to the decoder's context, pictures it returned and packets the demuxer read while
// g_logTraffic is set, each with the number of seeks recorded in g_failingSeeks when it happened; a
// read also with the number of packets sent before it.
struct TrafficEntry {
    int64_t timestamp;
    size_t afterSeeks;
    bool device = false;
    size_t sentBefore = 0;
};
bool g_logTraffic = false;
std::vector<TrafficEntry> g_sentPackets;
std::vector<TrafficEntry> g_returnedPictures;
std::vector<TrafficEntry> g_readPackets;

// g_logTraffic for one scope, its logs emptied as it starts and kept after it ends.
struct TrafficLog {
    TrafficLog() {
        g_sentPackets.clear();
        g_returnedPictures.clear();
        g_readPackets.clear();
        g_logTraffic = true;
    }
    ~TrafficLog() { g_logTraffic = false; }
    TrafficLog(const TrafficLog&) = delete;
    TrafficLog& operator=(const TrafficLog&) = delete;
};

void clearFailingCallConditions() {
    g_stripTimestamps = false;
    g_receiveExternalRemaining = 0;
    g_inFailingCall = false;
    g_seekShiftSeconds = 0;
    g_seekAwaitingPacket = -1;
    g_seekFailure = SeekFailure::None;
    g_refuseFrom = 0;
    g_refuseThrough = 0;
    g_failFramesInit = 0;
    g_standInAtNextPicture = false;
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
// Set once a case confirmed a hardware codec leaked at teardown; vkDestroyDevice called after that is the
// device destroyed under the leaked codec's objects.
bool g_watchDeviceDestroy = false;
bool g_destroyedUnderLeak = false;
// What av_seek_frame does during a tallied call. LateFirst moves the call's first timestamp seek
// kLateSeconds past its target.
enum class LoopSeek { None, FailAll, FailFirst, FailTimestamps, LateFirst };
LoopSeek g_loopSeek = LoopSeek::None;
constexpr int kLateSeconds = 2;
// av_read_frame hands out packets without a byte position; with g_clearDts also without a decode time.
bool g_clearPositions = false;
bool g_clearDts = false;
int g_dtsCleared = 0;
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

// Per thread: a call's own Vulkan work runs on the calling thread, since the Device aborts on a GPU call from any
// other; a decode thread's calls are ffmpeg's frame pool growing, and landed in a count whenever its timing fell
// inside the call.
thread_local bool g_countVulkan = false;
std::array<int, kVulkanCallCount> g_vulkanCalls{};
// The images that plane views (a view of one plane of a multi-planar image) were made of while g_countVulkan
// was set: what the renderer makes the first time it shows a decoded pool image.
thread_local std::vector<VkImage> g_planeViewImages;

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
    int freedAfterPictures = -1;  // g_trackedPictures when it was freed
    bool freedOnMainThread = false;
    // A copy of a picture's reference, not a reference of its own: reads the pool's count while
    // `freed` is false, and adds nothing to it.
    AVBufferRef peek{};
};
std::vector<PoolRecord> g_pools;
// Pictures the decoder received while g_trackPools is set, on any thread: when a pool was freed, by
// playback position rather than by call.
std::atomic<int> g_trackedPictures{0};
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
        pool->freedAfterPictures = g_trackedPictures.load();
        pool->freedOnMainThread = gettid() == getpid();
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

// Device codecs of the decoder's, open at once and opened in all, while g_countDeviceCodecs is set. When the second
// opens -- the first opened by a decoder switch -- the next vkAllocateMemory returns g_allocationAtSwitch (unless
// VK_SUCCESS), or with g_framesAtSwitch the next Vulkan frames init fails. Atomic: the decode thread opens codecs.
std::atomic<bool> g_countDeviceCodecs{false};
std::atomic<int> g_deviceCodecsLive{0};
std::atomic<int> g_deviceCodecsMax{0};
std::atomic<int> g_deviceCodecsOpened{0};
std::atomic<VkResult> g_allocationAtSwitch{VK_SUCCESS};
std::atomic<bool> g_allocationArmed{false};
std::atomic<bool> g_allocationInjected{false};
std::atomic<bool> g_framesAtSwitch{false};
std::atomic<bool> g_framesArmed{false};
std::atomic<int> g_deviceCodecsFreed{0};
// With g_pictureAtSwitch the injected allocation is instead the first one a picture of the switch's decoder makes
// (allocatePicture()'s path), never one of its setup.
std::atomic<bool> g_pictureAtSwitch{false};
std::atomic<bool> g_pictureArmed{false};
// With g_bitstreamAtSwitch it is instead the first allocation after that decoder's first picture returned from
// avcodec_default_get_buffer2: the slice's bitstream buffer, which no callback of the decoder's sees.
std::atomic<bool> g_bitstreamAtSwitch{false};
std::atomic<bool> g_bitstreamArmed{false};
// Which device codec, counted in opening order, the injection is armed for: 1 is the constructor's, 2 a switch's.
std::atomic<int> g_injectAtCodec{2};
// Depth of avcodec_default_get_buffer2 calls in progress, and whether the injected allocation was inside one.
std::atomic<int> g_inGetBuffer{0};
std::atomic<bool> g_injectedInPicture{false};
std::atomic<uint64_t> g_injectedSize{0};
// When the decoder's first device codec reports EOF -- the end of its emptying at a switch -- the next fenced
// submission is held and its wait refused: the switch's question goes unanswered.
bool g_unanswerAtSwitch = false;
bool g_unanswerFired = false;

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
    // Every call decodes through decodeWithin() with this, then draws; Nothing is renderNext().
    wallpaperx::PictureSkip skip = wallpaperx::PictureSkip::Nothing;
};

struct FailingCall {
    bool reached = false;
    int injectionsLeft = 0;
    int devicePicturesBefore = 0;
    int picturesDecoded = 0;
    std::vector<SeekCall> seeks;
    int containerOpens = 0;
    int decoderOpens = 0;
    int decoderFlushes = 0;
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
        run.failing.decoderFlushes = g_decoderFlushes;
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
                g_decoderFlushes = 0;
                run.failing.devicePicturesBefore = g_devicePictures;
                hostBeforeFailure = g_hostPictures;
                if (plan.arm) plan.arm();
                g_receiveExternalRemaining = 1;
                g_inFailingCall = true;
            }
            bool more = false;
            g_poolCall = static_cast<int>(call);
            try {
                if (plan.skip == wallpaperx::PictureSkip::Nothing) {
                    more = pipeline.renderNext(targets, target);
                } else {
                    more = pipeline.decodeWithin(UINT32_MAX, plan.skip) == VideoPipeline::Decode::Decoded;
                    if (more) pipeline.draw(targets, target, wallpaperx::Placement{});
                }
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

// Where the first seek landed, for a FAIL line: on a keyframe, on a non-keyframe, or nowhere read.
const char* describeLanding(const std::vector<SeekCall>& seeks) {
    if (seeks.empty() || !seeks[0].landedOnKeyframe) return "no packet read";
    return *seeks[0].landedOnKeyframe ? "a keyframe" : "a non-keyframe";
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
    //       the draw in draw(), so "rendering before converting" is renderNext() calling draw() first.
    //       Re-run there 2026-09-15: all four fixtures red on the host path, "the pipeline threw after 0
    //       pictures: VideoPipeline::draw: no picture has been decoded" -- the split turned what used to
    //       show a stale picture into a throw, which is why this text no longer says "call 1 showed -1".
    //       Re-run 2026-10-26 (G6), control_batch.sh, sequence-host on the RTX: the same line, all fixtures.
    //       renderer/video/video_decoder.cpp admit()
    //       dropping, outside a resume, a picture whose timestamp is not above the last returned:
    //       counter_jump.ts "60 pictures shown against 120", counter_repeat.mkv "30 pictures shown
    //       against 60"; the rising and untimestamped fixtures stayed green.
    // COVERAGE-ASSERT: each irregular fixture's independent decode shows a timestamp not above the one
    //       before; otherwise the drop is never offered.
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
    // CONTROL: renderer/video/video_decoder.cpp, each reverted. seekToResume()'s seek disabled (its `if`
    //       made `if (false && ...)`), so software rewinds and drops through -- the old replay: every
    //       picture still once, the bound red, "decoded 31 pictures" at 30 in and "292" at 291 in, via
    //       control_batch.sh 2026-09-30. admit()'s DropThrough test `<=` made `<`: "1 shown more than once" at
    //       all three points. DropThrough also dropping the first picture past the resume point: "1 never
    //       shown" at all three. seekToResume()'s last arm -- the one with no seek, taken when the target is
    //       at the stream's start -- without its rewind(): the first point alone red, "call 2 showed 24 where
    //       1 was due ... 23 never shown", via control_batch.sh 2026-10-01.
    //       This entry runs with the synchronised queues hidden (g_hideSynchronizedQueues), so it decodes on
    //       the calling thread: with the decode thread on, the resume ran outside the measured call, which
    //       counted 0 pictures and 0 seeks and left the bound 0 against 25. Re-run hidden 2026-10-23 (G3d),
    //       RTX 3080 Ti, control_batch.sh, bytes restored: seek disabled -> "decoded 31 pictures, more than
    //       the 25" and "292" at 291; `<` -> "call 2 showed 0 where 1 was due; 1 shown more than once"; last
    //       arm without rewind() -> "call 2 showed 24 where 1 was due ... 23 never shown". The decode-thread
    //       resume is graded by decode-thread-resume (S15.892) for order only, not this cost bound.
    // COVERAGE-ASSERT: the late point is at least four bounds in -- red with a keyframe every 100
    //       substituted, "321 pictures with a keyframe gap of 100" -- and each run asserts the failure
    //       met device pictures and software took over, and that the failing call decoded at least one
    //       picture: red with this mode taken off the hidden list (test-side, by hand, 2026-10-23).
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
        if (run.failing.picturesDecoded == 0) {
            std::fprintf(stderr, "FAIL: %s: %s: the call that met the failure decoded no picture, so its cost was "
                                 "not measured\n",
                         label.c_str(), what.c_str());
            fail("the resume after a hardware failure ran outside the call that measures it");
        } else if (run.failing.picturesDecoded > bound) {
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
    //       pictures land on non-keyframes whose next keyframe is past the resume point, and were each
    //       followed by one a second earlier. Only its sequence is asserted, since how far the demuxer
    //       overshoots is ffmpeg's.
    // CONTROL: via control_batch.sh, 2026-09-30, reverted: renderer/video/video_decoder.cpp admit() taking
    //       a Seeked picture past the resume point as landed (DropThrough) instead of retrying -> MP4 "22
    //       never shown" and "1 seek(s) made after the late landing"; transport stream after 40, "9 never
    //       shown". After 100 it stayed green: no keyframe follows that landing, so its retry comes from
    //       the end-of-stream branch, and that point grades less.
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
    // GOAL: S15.178 -- the retry after a late landing aims one second further back, not more: the call
    //       decodes at most a second of pictures and a keyframe interval, plus the picture after the last shown.
    // WHY-THIS-SHAPE: counted at the decoder, never timed; the MP4's index lands every retry on the keyframe at
    //       or before its target, so the count follows the retry's distance. The transport stream is not
    //       bounded: its seeks land where its demuxer decides.
    // CONTROL: via control_batch.sh, 2026-10-01, reverted, renderer/video/video_decoder.cpp nextSeekBack()'s
    //       first step made two seconds -> "seeks to 148480, 117760, 77 pictures decoded to resume, more than the
    //       55 of a second and a keyframe interval" (53 unbroken). No other check here sees that step.
    const int retryBound = kFixturePicturesPerSecond + reference->longestKeyframeGap + 1;
    if (run.failing.picturesDecoded > retryBound) {
        std::fprintf(stderr, "FAIL: %s: %s: %s, %d pictures decoded to resume, more than the %d of a second and a "
                             "keyframe interval\n",
                     label.c_str(), what.c_str(), describeSeeks(seeks).c_str(), run.failing.picturesDecoded, retryBound);
        fail("the retry after a late landing aimed further back than one second");
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
    // CONTROL: renderer/video/video_decoder.cpp next(), via control_batch.sh, 2026-09-30, each reverted.
    //       End of stream during a seek never walking back: "counter_seg.ts, hardware failing after 119
    //       picture(s): 119 pictures shown against 120 ... 1 never shown". Always walking back: "pictures
    //       after the seek untimestamped: seeks to 163840, 148480, 133120, 102400, 40960, -1024", 822
    //       pictures decoded -- the last being the rewind's seek to packet 1's decode timestamp.
    //       Both re-run 2026-10-19 (T7b), resume-end, queues hidden: held, the same lines.
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

    // GOAL: S15.160, end of stream -- a resume seek that lands on a non-keyframe, with a keyframe at or
    //       before the last picture shown next, counts as landed: end of stream then ends playback
    //       with that one seek.
    // WHY-THIS-SHAPE: the landing verdict is read only at end of stream while the seek is unsettled,
    //       so timestamps are stripped as above; the seek is moved a second early because a transport
    //       stream's own seek lands on the last packet decoded before its target, with no keyframe
    //       presented at or before it still to come.
    // CONTROL: via control_batch.sh, each reverted, in renderer/video/video_decoder.cpp. Re-run 2026-10-01:
    //       recordLanding() always setting Landing::Unproven -> "seeks to 489000, 399000, 309000, 126000"
    //       (the MP4 half above red too). 2026-09-30: next()'s skip marking the landing Unproven and decoding the
    //       non-keyframe -- the verdict read at the packet landed on -> the same line, the MP4 half green.
    //       Re-run 2026-10-19 (T7), queues hidden: recordLanding() always Unproven held, the same line.
    //       Re-run 2026-10-19 (T7b), queues hidden: the 2026-09-30 break (the landing marked Unproven, the
    //       skip loop disabled) held, the same line.
    // COVERAGE-ASSERT: the first packet read after the seek must not be a keyframe -- red with
    //       counter_long.mp4 substituted (test-side, by hand, 2026-09-30), "the first landing on a keyframe".
    const char* const transport = "counter_seg.ts";
    const std::optional<CounterReference> transportReference = counterReference(label, transport);
    if (!transportReference) return;
    const int transportPictures = static_cast<int>(transportReference->ids.size());
    const PipelineRun early = runPipeline(device, asset(transport), transportReference->ids.size(),
                                          FailurePlan{transportPictures, [] {
                                                          g_stripTimestamps = true;
                                                          g_seekShiftSeconds = -1;
                                                      }});
    const std::string earlyWhat =
        failurePoint(transport, transportPictures) + ", its seek landing a second early, pictures after it untimestamped";
    if (!failureExercised(label, earlyWhat, early)) return;
    gradeSequence(label, earlyWhat, early, *transportReference);
    const std::vector<SeekCall>& seeks = early.failing.seeks;
    const bool skipped = !seeks.empty() && seeks[0].landedOnKeyframe == false;
    if (!skipped || seeks.size() != 1) {
        std::fprintf(stderr, "FAIL: %s: %s: %s, the first landing on %s\n", label.c_str(), earlyWhat.c_str(),
                     describeSeeks(seeks).c_str(), describeLanding(seeks));
        fail(!skipped ? "the early seek did not land on a non-keyframe, so the skip to a keyframe was not exercised"
                      : "end of stream after a seek proven by the keyframe after its landing walked back");
    }
    std::printf("%s: %s: %d pictures decoded, %s\n", label.c_str(), earlyWhat.c_str(), early.failing.picturesDecoded,
                describeSeeks(seeks).c_str());
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
    // CONTROL: renderer/video/video_decoder.cpp, each reverted. seekToResume()'s last arm, which a failed
    //       seek takes, without its rewind(): both cases "21 never shown" (the every-seek case also "1 seeks,
    //       some failed; container opened 0 time(s)"), via control_batch.sh 2026-09-30. rewind() without its
    //       reopen once every
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
    //       or refused too. A pass is numbered by what starts it in the failing call -- the software
    //       decoder's open, then each flush of that decoder -- because a retry flushes and reuses the
    //       decoder rather than opening another. Numbering by decoder opens alone would see one pass; a
    //       hardware context is never counted or refused, so pass 1 cannot be the failing hardware decoder.
    // CONTROL: renderer/video/video_decoder.cpp, via control_batch.sh, re-run 2026-10-01, each reverted.
    //       seekToResume()'s `m_resume = DropCount{m_returned.count};` one picture short: the first case
    //       "91 pictures shown against 90 ... call 41 showed 39 where 40 was due ... 1 shown more than
    //       once". next() returning the end instead of throwing with DropCount pending (its `if` made
    //       `if (false)`): the second case "40 pictures shown, then the stream ended".
    //       Re-run 2026-10-19 (T7), queues hidden: the first break held, the same line.
    // COVERAGE-ASSERT: at least two recovery passes in the failing call -- red with the refusal never
    //       applied (test-side, `g_refuseFrom = 0`, by hand, re-run 2026-10-01), "1 recovery pass(es) (1
    //       decoder open(s), 0 flush(es))" for both cases.
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
        const int passes = run.failing.decoderOpens + run.failing.decoderFlushes;
        if (passes < 2) {
            std::fprintf(stderr, "FAIL: %s: %s: %d recovery pass(es) (%d decoder open(s), %d flush(es))\n", label.c_str(),
                         what.c_str(), passes, run.failing.decoderOpens, run.failing.decoderFlushes);
            fail("the resume never started a second software pass, so counting was not exercised");
            continue;
        }
        if (c.refuseThrough == 1) {
            checkSoftwareTookOver(label, what, run);
            if (gradeSequence(label, what, run, *reference)) {
                std::printf("%s: %s: every picture once after %d recovery passes (%d decoder open(s), %d flush(es))\n",
                            label.c_str(), what.c_str(), passes, run.failing.decoderOpens, run.failing.decoderFlushes);
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

    // GOAL: S15.610 -- a hardware failure on a pass that has skipped pictures, where the resume could only
    //       count pictures, loses the place but not the file: after exactly the pictures already shown, software
    //       plays the stream again from its first picture, rising to its end, with no rejection; the same
    //       failure on a pass that skipped nothing resumes by count and shows every picture once. False if
    //       software counts the pictures returned while skipping (fewer than the packets they came from) and
    //       resumes part way in, or refuses a playable file as bad content.
    // WHY-THIS-SHAPE: the raw H.264 stream carries no timestamps, and its first software pass is refused as
    //       S13.64's first case does, so the resume by packet number ends without meeting its packet and falls
    //       back to counting; its unreferenced B pictures give skipping something to leave out. Each call decodes through decodeWithin() asking for unreferenced pictures skipped, then
    //       draws. The counter read back from every call shows which picture was on screen.
    // CONTROL: control_batch.sh 2026-10-15, reverted, video_pipeline_resume_drop_count (RTX), renderer/video/video_decoder.cpp: seekToResume()
    //       without its start from the first picture after skipping (so it resumes by count) -> "FAIL: ... skipping
    //       unreferenced pictures: 75 pictures shown, from the first again 0, both runs rising 1, then the stream
    //       ended"; the old refusal put back -> "... 24 pictures shown, from the first again 0, both runs rising 0,
    //       then VideoDecoder: ...". Green: 85 shown (24, then the file again), 90 of 90 skipping nothing.
    for (const bool skipping : {true, false}) {
        constexpr int kShown = 24;
        const PipelineRun run =
            runPipeline(device, asset(file), reference->ids.size(),
                        FailurePlan{kShown,
                                    [] {
                                        g_refuseFrom = 1;
                                        g_refuseThrough = 1;
                                    },
                                    skipping ? wallpaperx::PictureSkip::Unreferenced : wallpaperx::PictureSkip::Nothing});
        const std::string what = failurePoint(file, kShown) + (skipping ? ", skipping unreferenced pictures" : ", skipping nothing");
        if (!failureExercised(label, what, run)) continue;
        std::printf("%s: %s: %zu pictures shown, then %s\n", label.c_str(), what.c_str(), run.shown.size(),
                    run.threw.empty() ? (run.ended ? "the stream ended" : "nothing") : run.threw.c_str());
        if (!skipping) {
            checkSoftwareTookOver(label, what, run);
            gradeSequence(label, what, run, *reference);
            continue;
        }
        // Two rising runs: the pictures shown before the failure, then the stream again from its first one.
        bool restartedRising = run.shown.size() > static_cast<size_t>(kShown) + 1;
        for (size_t i = 1; restartedRising && i < run.shown.size(); ++i) {
            if (i != static_cast<size_t>(kShown) && run.shown[i] <= run.shown[i - 1]) restartedRising = false;
        }
        const bool fromFirst = run.shown.size() > static_cast<size_t>(kShown) && run.shown[kShown] == run.shown.front();
        if (run.threwContentError || !run.ended || !fromFirst || !restartedRising) {
            std::fprintf(stderr, "FAIL: %s: %s: %zu pictures shown, from the first again %d, both runs rising %d, then %s\n",
                         label.c_str(), what.c_str(), run.shown.size(), fromFirst ? 1 : 0, restartedRising ? 1 : 0,
                         run.threw.empty() ? (run.ended ? "the stream ended" : "nothing") : run.threw.c_str());
            fail("a resume after skipping that could only count pictures did not play the file again from its first picture");
        }
    }
}

// ------------------------------------------------------------------------- //
// A pipeline opened at a position another pipeline on the same file handed out
// ------------------------------------------------------------------------- //

// A file's video packets in decode order, read with libavformat alone. `nal` is the type of the first
// picture-carrying NAL unit in the packet, -1 for none and for AV1; `reset` is whether the packet resets
// the decoder, as support/reset_point_reference.h reads it.
struct GopLayout {
    AVCodecID codec = AV_CODEC_ID_NONE;
    struct Packet {
        int64_t timestamp;
        bool key;
        int nal;
        bool reset;
    };
    std::vector<Packet> packets;
    std::vector<int64_t> pictures;  // the timestamps sorted: picture i's, in output order

    // A keyframe that does not reset the decoder: an H.264 non-IDR I picture, or an HEVC CRA picture.
    bool openKeyframe(size_t i) const {
        return packets[i].key && (codec == AV_CODEC_ID_H264 ? packets[i].nal == 1 : packets[i].nal == 21);
    }
    // Packets after keyframe `key` in decode order, before the next keyframe, shown before it.
    std::vector<size_t> leading(size_t key) const {
        std::vector<size_t> found;
        for (size_t i = key + 1; i < packets.size() && !packets[i].key; ++i) {
            if (packets[i].timestamp < packets[key].timestamp) found.push_back(i);
        }
        return found;
    }
    std::optional<size_t> pictureAt(int64_t timestamp) const {
        const auto it = std::find(pictures.begin(), pictures.end(), timestamp);
        return it == pictures.end() ? std::nullopt : std::optional<size_t>(static_cast<size_t>(it - pictures.begin()));
    }
    std::optional<size_t> packetAt(int64_t timestamp) const {
        for (size_t i = 0; i < packets.size(); ++i) {
            if (packets[i].timestamp == timestamp) return i;
        }
        return std::nullopt;
    }
    // Where a start at the picture shown at `timestamp` decodes from on the device: the last reset point in
    // decode order shown before it, or packet 0, from which every picture decodes.
    size_t nearestReset(int64_t timestamp) const {
        size_t found = 0;
        for (size_t i = 1; i < packets.size(); ++i) {
            if (packets[i].reset && packets[i].timestamp < timestamp) found = i;
        }
        return found;
    }
    // The keyframe-flagged packet shown last before `timestamp`: where an indexed container's seek lands.
    std::optional<size_t> lastKeyBefore(int64_t timestamp) const {
        std::optional<size_t> found;
        for (size_t i = 0; i < packets.size(); ++i) {
            if (packets[i].key && packets[i].timestamp < timestamp &&
                (!found || packets[i].timestamp > packets[*found].timestamp)) {
                found = i;
            }
        }
        return found;
    }
    // Whether a seek to the picture shown at `timestamp` lands on an open keyframe.
    bool landsOnOpenKeyframe(int64_t timestamp) const {
        const std::optional<size_t> key = lastKeyBefore(timestamp);
        return key && openKeyframe(*key);
    }
    std::vector<size_t> resets() const {
        std::vector<size_t> found;
        for (size_t i = 0; i < packets.size(); ++i) {
            if (packets[i].reset) found.push_back(i);
        }
        return found;
    }
    size_t picture(size_t packet) const { return pictureAt(packets[packet].timestamp).value_or(0); }
    std::string describe(size_t i) const {
        const Packet& p = packets[i];
        const bool hevc = codec == AV_CODEC_ID_HEVC;
        const char* kind = p.reset                                          ? (codec == AV_CODEC_ID_AV1 ? "a shown key frame" : "an IDR")
                           : codec == AV_CODEC_ID_H264 && p.key && p.nal == 1 ? "a non-IDR I picture"
                           : hevc && p.nal == 21                              ? "a CRA"
                           : hevc && p.nal >= 16 && p.nal <= 18               ? "a BLA"
                           : p.key ? "a keyframe-flagged packet that does not reset the decoder"
                                   : "not a keyframe";
        return "packet " + std::to_string(i) + " (picture " + std::to_string(picture(i)) + ", " + kind + ")";
    }
};

// `keyFlagsOnly` accepts any codec and reads only timestamps and the container's keyframe flags: `nal` is -1
// and `reset` false throughout.
std::optional<GopLayout> readGopLayout(const std::string& file, std::string& why, bool keyFlagsOnly = false) {
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
        why = "cannot open " + file;
        return std::nullopt;
    }
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) {
        why = file + " has no video stream";
        return std::nullopt;
    }
    GopLayout layout;
    const AVCodecParameters& parameters = *format->streams[stream]->codecpar;
    layout.codec = parameters.codec_id;
    const bool known = layout.codec == AV_CODEC_ID_H264 || layout.codec == AV_CODEC_ID_HEVC || layout.codec == AV_CODEC_ID_AV1;
    if (!known && !keyFlagsOnly) {
        why = file + " is not H.264, HEVC or AV1";
        return std::nullopt;
    }
    const int lengthSize = known ? wallpaperx::testing::nalLengthSize(parameters) : 0;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream && !known) {
            layout.packets.push_back(GopLayout::Packet{packet->pts, (packet->flags & AV_PKT_FLAG_KEY) != 0, -1, false});
            layout.pictures.push_back(packet->pts);
        } else if (packet->stream_index == stream) {
            const auto size = static_cast<size_t>(packet->size);
            const auto slice = layout.codec == AV_CODEC_ID_AV1
                                   ? std::nullopt
                                   : wallpaperx::testing::firstSlice(packet->data, size, layout.codec, lengthSize);
            layout.packets.push_back(GopLayout::Packet{packet->pts, (packet->flags & AV_PKT_FLAG_KEY) != 0,
                                                       slice ? slice->type : -1,
                                                       wallpaperx::testing::referenceResets(parameters, packet->data, size)});
            layout.pictures.push_back(packet->pts);
        }
        av_packet_unref(packet);
    }
    std::sort(layout.pictures.begin(), layout.pictures.end());
    return layout;
}

// Whether the decode after a start on the device began at packet `expected` of `layout` and went on in
// decode order: the first packet in g_sentPackets is that one, and each one sent after it is the next.
// Prints and fails otherwise.
bool startedAt(const std::string& label, const std::string& what, const GopLayout& layout, size_t expected) {
    if (g_sentPackets.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: no packet was sent to the decoder\n", label.c_str(), what.c_str());
        fail("a start on the device sent the decoder nothing");
        return false;
    }
    const std::optional<size_t> first = layout.packetAt(g_sentPackets.front().timestamp);
    bool ok = true;
    if (!first || !layout.packets[*first].reset) {
        std::fprintf(stderr, "FAIL: %s: %s: the first packet sent to the decoder was %s, which does not reset the decoder\n",
                     label.c_str(), what.c_str(), first ? layout.describe(*first).c_str() : "none of the file's");
        fail("a start on the device decodes from a packet that resets the decoder");
        ok = false;
    } else if (*first != expected) {
        std::fprintf(stderr, "FAIL: %s: %s: the first packet sent to the decoder was %s, where the nearest reset point "
                             "before the picture is %s\n",
                     label.c_str(), what.c_str(), layout.describe(*first).c_str(), layout.describe(expected).c_str());
        fail("a start on the device decodes from the nearest reset point before its picture");
        ok = false;
    }
    for (size_t i = 0; first && i < g_sentPackets.size(); ++i) {
        const size_t want = *first + i;
        if (want < layout.packets.size() && g_sentPackets[i].timestamp == layout.packets[want].timestamp) continue;
        std::fprintf(stderr, "FAIL: %s: %s: packet %zu sent after the start is not packet %zu of the file\n", label.c_str(),
                     what.c_str(), i, want);
        fail("a start on the device decodes every packet after its reset point, in order");
        return false;
    }
    return ok;
}

// What a looping pipeline opened at `startAt` showed over `calls` calls, read back after each. The
// constructor and the first call run as a "failing call" for the interposers, so the seeks made to
// reach the position are recorded and `arm`'s conditions (a late seek, a failing one) apply to them.
// Those conditions are cleared after the first call; given `beforeCall`, they are left to it instead,
// and it is called before each later call with that call's index.
struct StartRun {
    std::vector<int> shown;
    std::vector<bool> startsPass;
    std::string threw;
    std::string readbackFault;
    std::vector<SeekCall> seeks;
    int devicePictures = 0;
    int hostPictures = 0;
};

StartRun runFrom(Device& device, const std::string& file, std::optional<wallpaperx::StreamPosition> startAt,
                 size_t calls, const std::function<void()>& arm = {},
                 const std::function<void(size_t call)>& beforeCall = {}) {
    StartRun run;
    struct Clear {
        ~Clear() {
            clearFailingCallConditions();
            g_recordPictures = false;
        }
    } clear;
    OffscreenTargetManager targets(device);
    const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
    g_devicePictures = 0;
    g_hostPictures = 0;
    g_recordPictures = true;
    g_failingSeeks.clear();
    if (arm) arm();
    g_inFailingCall = true;
    try {
        VideoPipeline pipeline(device, file, kShaderDir, VideoPipeline::Playback::Loop, startAt);
        for (size_t call = 0; call < calls; ++call) {
            if (call > 0 && beforeCall) beforeCall(call);
            const bool more = pipeline.renderNext(targets, target);
            if (call == 0) {
                run.seeks = g_failingSeeks;
                if (!beforeCall) clearFailingCallConditions();
            }
            if (!more) break;
            run.startsPass.push_back(pipeline.timing().startsPass);
            run.shown.push_back(wallpaperx::testing::counterFromBgra(
                wallpaperx::testing::reclaimAndReadback(device, targets, target), target.width, target.height));
        }
    } catch (const std::exception& e) {
        run.threw = e.what();
    }
    run.devicePictures = g_devicePictures;
    run.hostPictures = g_hostPictures;
    return run;
}

// The position of the `index`-th picture (0-based), as a pipeline that showed it hands it out, and the
// counter it showed; nothing, with the reason printed as a harness failure, when there is none.
std::optional<wallpaperx::StreamPosition> positionAt(Device& device, const std::string& label, const char* file,
                                                     size_t index, int expectedId) {
    try {
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
        VideoPipeline pipeline(device, asset(file), kShaderDir, VideoPipeline::Playback::Loop);
        for (size_t call = 0; call <= index; ++call) {
            if (!pipeline.renderNext(targets, target)) break;
        }
        const int shown = wallpaperx::testing::counterFromBgra(
            wallpaperx::testing::reclaimAndReadback(device, targets, target), target.width, target.height);
        const std::optional<wallpaperx::StreamPosition> position = pipeline.shownPosition();
        if (shown == expectedId && position) return position;
        std::fprintf(stderr, "FAIL: %s: %s: after %zu call(s) the pipeline showed %d where %d was due, %s a position\n",
                     label.c_str(), file, index + 1, shown, expectedId, position ? "with" : "without");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s: %s: reaching picture %zu threw: %s\n", label.c_str(), file, index, e.what());
    }
    fail("the harness could not take the position a pipeline hands out for a shown picture");
    return std::nullopt;
}

std::string joinIds(const std::vector<int>& ids) {
    std::string text;
    for (const int id : ids) text += (text.empty() ? "" : " ") + std::to_string(id);
    return text;
}

// Pictures a start is graded on: its own and the four after it.
constexpr size_t kFollowing = 5;

// kCatchUpPictures, read from renderer/video/video_decoder.cpp at build time (tests/support/catch_up_budget.cmake):
// a start decodes at most this many pictures forward to reach its position.
constexpr size_t kCatchUpBudget = WALLPAPERX_CATCH_UP_PICTURES;

// runFrom() over kFollowing calls with the traffic logs recording, which outlive the call for the caller to
// read. A run that threw is reported here and gives nothing.
std::optional<StartRun> runLoggedStart(Device& device, const std::string& label, const std::string& what,
                                       const std::string& file, wallpaperx::StreamPosition position,
                                       const std::function<void()>& arm = {}) {
    StartRun run;
    {
        const TrafficLog log;
        run = runFrom(device, file, position, kFollowing, arm);
    }
    if (run.threw.empty()) return run;
    std::fprintf(stderr, "FAIL: %s: %s: threw after %zu pictures: %s\n", label.c_str(), what.c_str(), run.shown.size(),
                 run.threw.c_str());
    fail("a pipeline opened at a position threw");
    return std::nullopt;
}

// Which starting points a mode grades. The transport stream's are an entry of their own on the device,
// so a validation message there names that case (see S15.145): their seeks land on non-keyframes, and
// only the start's search for a reset point keeps those from the hardware decoder.
enum class StartPoints { All, Mp4, TransportStream };

void runStartAtChecks(Device& device, const std::string& label, bool onDevice, StartPoints points) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;

    // GOAL: S15.141 -- a pipeline opened at a position another pipeline on the same file handed out for a
    //       shown picture shows THAT picture first, then its successors in order, and marks its first
    //       picture as not starting a pass: on an MP4 and a transport stream with B-frames, early, mid and
    //       late in the file, and when the first seek lands past the picture. S15.142 -- on the hardware
    //       path, the search further back after that late landing stays on the hardware decoder. S15.159
    //       (software only) -- after a seek, packets are skipped only up to the first keyframe.
    // WHY-THIS-SHAPE: pictures are identified by the counter drawn into them, read back from the target,
    //       against libavcodec's own order; the position comes from a pipeline that showed the picture,
    //       as the daemon takes it. Several points, because which keyframe a seek lands on and how many
    //       reordered pictures it drops differ with each. The late landing is the S13.61 shift of the
    //       first seek by two seconds. "Stays on hardware" is read from where the decoder's pictures
    //       live, never from path(). A start on the device never enters the Seeked state, so breaks in
    //       startAt()'s software branch, admit()'s landing or beforeLandingKeyframe() reach start-at-host only.
    // CONTROL: via control_batch.sh, each alone, reverted, renderer/video/video_decoder.cpp. Re-run
    //       2026-10-01: startAt()'s `before` made the position itself -> on start-at-device and start-at-host
    //       "FAIL: ... counter_long.mp4 opened at picture 40: showed 41 42 43 44 45 where 40 41 42 43 44 was
    //       due". Host only, 2026-10-01: seekToResume()'s `m_resume = Seeked{back};` deleted, which a software
    //       start now reaches through -> "showed 24 25 26 27 28 where 40 ..."; 2026-09-30:
    //       S15.159, beforeLandingKeyframe() without its `Landing::NoPacket` clause -> "counter_seg.ts opened
    //       at picture 40: showed 48 72 96 0 1 where 40 41 42 43 44 was due". S15.142, device, via
    //       control_batch.sh 2026-10-01: startAtResetPoint() handing a window with no reset point to software
    //       (the resume state set, `m_path = Path::SoftwareRefused`, and a reopen in software required) ->
    //       "counter_long.mp4 opened at picture 290, its first seek landing late: 0 device and 55 host
    //       picture(s)" (7 host on the 2026-10-19 re-run). These four re-run 2026-10-19 (T7b), queues hidden on
    //       start-at-device: all held, the same lines. In
    //       renderer/video/picture_source.cpp, next()'s `startsPass` made `!m_returnedAny` (re-run there via
    //       control_batch.sh 2026-10-19, T7, queues hidden) -> on start-at-device "its first picture is marked as starting a
    //       pass" at every point.
    // COVERAGE-ASSERT: the late-landing run must show a second seek aimed further back, or the late landing
    //       was never exercised -- red with the shift set to 0 (test-side, by hand, 2026-09-30, start-at-device),
    //       "seeks to 148479" and "a start whose seek landed late was not retried further back"; and the
    //       number of points graded.
    struct Point {
        const char* file;
        size_t index;
        int shiftSeconds;  // added to the first seek's target
    };
    int graded = 0;
    int offered = 0;
    for (const Point& point : {Point{"counter_long.mp4", 40, 0}, Point{"counter_long.mp4", 100, 0},
                               Point{"counter_long.mp4", 290, 0}, Point{"counter_long.mp4", 290, 2},
                               Point{"counter_seg.ts", 40, 0}, Point{"counter_seg.ts", 100, 0},
                               Point{"counter_seg.ts", 60, -1}}) {
        const bool late = point.shiftSeconds > 0;
        const bool early = point.shiftSeconds < 0;
        const bool transport = std::string(point.file).ends_with(".ts");
        if ((points == StartPoints::Mp4 && transport) || (points == StartPoints::TransportStream && !transport)) continue;
        ++offered;
        const std::optional<CounterReference> reference = counterReference(label, point.file);
        if (!reference) continue;
        const std::vector<int>& ids = reference->ids;
        if (point.index + kFollowing > ids.size()) {
            fail("a starting point is too near the end of its fixture to grade the pictures after it");
            continue;
        }
        const std::optional<wallpaperx::StreamPosition> position =
            positionAt(device, label, point.file, point.index, ids[point.index]);
        if (!position) continue;
        const std::string what =
            std::string(point.file) + " opened at picture " + std::to_string(point.index) +
            (late ? ", its first seek landing late" : early ? ", its first seek landing a second early" : "");
        const int shift = point.shiftSeconds;
        const std::optional<StartRun> started =
            runLoggedStart(device, label, what, asset(point.file), *position,
                           shift != 0 ? std::function<void()>([shift] { g_seekShiftSeconds = shift; })
                                      : std::function<void()>());
        if (!started) continue;
        const StartRun& run = *started;
        ++graded;
        const std::vector<int> due(ids.begin() + static_cast<long>(point.index),
                                   ids.begin() + static_cast<long>(point.index + kFollowing));
        if (run.shown != due) {
            std::fprintf(stderr, "FAIL: %s: %s: showed %s where %s was due\n", label.c_str(), what.c_str(),
                         joinIds(run.shown).c_str(), joinIds(due).c_str());
            fail("a pipeline opened at a picture shows it first, then its successors");
        }
        if (run.startsPass.empty() || run.startsPass.front()) {
            std::fprintf(stderr, "FAIL: %s: %s: its first picture is marked as starting a pass\n", label.c_str(),
                         what.c_str());
            fail("the first picture of a pipeline opened part way in does not start a pass");
        }
        // GOAL: S15.164 -- on the device, a closed-GOP start decodes from the nearest reset point before
        //       its picture, then every packet after it in decode order.
        // WHY-THIS-SHAPE: read from the packets the decoder was sent, against the file's own packets read
        //       with libavformat, so a start from an earlier reset point that still shows the right picture
        //       is seen; the pictures alone cannot tell the two apart.
        // CONTROL: via control_batch.sh, 2026-09-30, reverted, renderer/video/reset_point.cpp lengthSize()
        //       returning 0 for avcC -> on start-at-device "counter_long.mp4 opened at picture 40: the first
        //       packet sent to the decoder was packet 0 (picture 0, an IDR), where the nearest reset point
        //       before the picture is packet 24 (picture 24, an IDR)", and the same at 100 and 290.
        if (onDevice) {
            std::string why;
            const std::optional<GopLayout> layout = readGopLayout(asset(point.file), why);
            if (!layout) {
                std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(), why.c_str());
                fail("the harness could not read the fixture's packet layout, which is not a verdict on the decoder");
            } else {
                startedAt(label, what, *layout, layout->nearestReset(layout->pictures[point.index]));
            }
        }
        // GOAL: S15.166 -- a start on a closed-GOP MP4 makes exactly one seek, on the device as in software:
        //       the container's index lands it on a keyframe at or before the picture, which resets the decoder.
        // WHY-THIS-SHAPE: a seek count, because a second seek to the same place shows the same pictures. Not
        //       on the transport stream, whose points 40 and 100 search further back by design.
        // CONTROL: via control_batch.sh, 2026-09-30, reverted, renderer/video/video_decoder.cpp
        //       startAtResetPoint() seeking to the reset point in the first window too (`back != 0 &&` deleted)
        //       -> on start-at-device "counter_long.mp4 opened at picture 40: seeks to 20479, 20479; a closed-GOP
        //       MP4 start made more than one seek". Re-run 2026-10-19 (T7b), queues hidden: held, the same line.
        if (!transport && !late && run.seeks.size() != 1) {
            std::fprintf(stderr, "FAIL: %s: %s: %s; a closed-GOP MP4 start made more than one seek\n", label.c_str(),
                         what.c_str(), describeSeeks(run.seeks).c_str());
            fail("a start on a closed-GOP MP4 makes one seek");
        }
        // GOAL: S15.160 -- a seek that lands on a picture that is not a keyframe is judged at the first
        //       keyframe after it: one at or before the picture asked for proves the landing, with no
        //       second seek, and the picture itself is shown first. On the device the judge is the start's
        //       search, which takes the first reset point after the landing.
        // WHY-THIS-SHAPE: the first seek is moved a second early, landing on packet 31 of counter_seg.ts
        //       with keyframe 48 next, for picture 60. No natural transport-stream start can do this: the
        //       seek lands on the last packet decoded at or before the target, so every later keyframe is
        //       also presented after it -- points 40 and 100 always search further back.
        // CONTROL: via control_batch.sh, 2026-09-30, reverted, renderer/video/video_decoder.cpp. Host only:
        //       next()'s skip also discarding the first keyframe it meets -> on start-at-host "seeks to 311999,
        //       221999, the first landing on a non-keyframe", pictures still 60-64. Device: startAtResetPoint()
        //       seeking to the reset point in the first window too (`back != 0 &&` deleted) -> on
        //       start-at-device-ts "seeks to 311999, 311999, the first landing on a non-keyframe".
        //       Both re-run 2026-10-19 (T7b): held, the same lines.
        // COVERAGE-ASSERT: the first packet read after the seek must not be a keyframe, or the skip was never
        //       exercised -- red with counter_long.mp4 substituted (test-side, by hand, 2026-09-30), "seeks to
        //       30719, the first landing on a keyframe".
        if (early) {
            const bool skipped = !run.seeks.empty() && run.seeks[0].landedOnKeyframe == false;
            if (!skipped || run.seeks.size() != 1) {
                std::fprintf(stderr, "FAIL: %s: %s: %s, the first landing on %s\n", label.c_str(), what.c_str(),
                             describeSeeks(run.seeks).c_str(), describeLanding(run.seeks));
                fail(!skipped ? "the early seek did not land on a non-keyframe, so the skip to a keyframe was not exercised"
                              : "a seek proven by the keyframe after its non-keyframe landing was retried");
            }
        }
        if (late) {
            if (run.seeks.size() < 2 || run.seeks[1].target >= run.seeks[0].target) {
                std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(), describeSeeks(run.seeks).c_str());
                fail("a start whose seek landed late was not retried further back, so the retry was not exercised");
            }
            if (onDevice && (run.hostPictures != 0 || run.devicePictures == 0)) {
                std::fprintf(stderr, "FAIL: %s: %s: %d device and %d host picture(s)\n", label.c_str(), what.c_str(),
                             run.devicePictures, run.hostPictures);
                fail("the search further back after a late landing stays on the hardware decoder");
            }
        }
        std::printf("%s: %s: showed %s, %s, %d device and %d host picture(s)\n", label.c_str(), what.c_str(),
                    joinIds(run.shown).c_str(), describeSeeks(run.seeks).c_str(), run.devicePictures, run.hostPictures);
    }
    if (!g_notVerified) {
        check(offered == (points == StartPoints::All ? 7 : points == StartPoints::Mp4 ? 4 : 3) && graded == offered,
              "every starting point this check is written for was graded");
    }
    if (points == StartPoints::TransportStream) return;

    // GOAL: S15.143 -- a position at the file's first picture starts there, as a pass of its own. When every
    //       seek of a start fails, the start reads from the file's first packet if its position is within the
    //       catch-up budget of the first picture -- showing that position first, not marked as a pass -- and
    //       past the budget shows the first picture as a new pass. On the device and in software alike.
    // WHY-THIS-SHAPE: the seek is failed from outside, as S13.63 does, because the fixture's container
    //       seeks; a failure leaves the container where ffmpeg left it, which only a rewind makes known.
    //       Pictures 100 and 290 lie either side of the budget, asserted against the decoder's own constant.
    // CONTROL: via control_batch.sh, 2026-10-01, each alone, reverted, renderer/video/video_decoder.cpp.
    //       startAt()'s `if (before <= streamStart()) return;` deleted -> on start-at-host "counter_long.mp4
    //       opened at picture 0: showed 0 1 2 3 4 (first marked no)". Software arm: seekToResume()'s `else if
    //       (starting && !withinCatchUp(...))` arm deleted -> on start-at-host "opened at picture 290, every
    //       seek failing: showed 290 291 292 293 294 (first marked no) where 0 1 2 3 4 was due". Device arm:
    //       `back == 0 ||` put back in startAtResetPoint()'s failed-seek branch -> on start-at-device "opened
    //       at picture 100, every seek failing: showed 0 1 2 3 4 (first marked yes) where 100 101 102 103 104
    //       was due".
    // COVERAGE-ASSERT: each failing case recorded a failed seek.
    {
        const char* const file = "counter_long.mp4";
        const std::optional<CounterReference> reference = counterReference(label, file);
        if (!reference) return;
        const std::vector<int>& ids = reference->ids;
        constexpr size_t kWithin = 100;
        constexpr size_t kPast = 290;
        if (!(kWithin < kCatchUpBudget && kPast > kCatchUpBudget) || ids.size() < kPast + kFollowing) {
            fail("the failed-seek starts no longer lie either side of the catch-up budget in counter_long.mp4");
            return;
        }
        const std::vector<int> due(ids.begin(), ids.begin() + kFollowing);
        const std::optional<wallpaperx::StreamPosition> first = positionAt(device, label, file, 0, ids[0]);
        if (!first) return;
        const StartRun atFirst = runFrom(device, asset(file), first, kFollowing);
        const bool firstOk = atFirst.threw.empty() && atFirst.shown == due && !atFirst.startsPass.empty() &&
                             atFirst.startsPass.front();
        if (!firstOk) {
            std::fprintf(stderr, "FAIL: %s: %s opened at picture 0: showed %s (first marked %s) where %s was due%s\n",
                         label.c_str(), file, joinIds(atFirst.shown).c_str(),
                         !atFirst.startsPass.empty() && atFirst.startsPass.front() ? "yes" : "no", joinIds(due).c_str(),
                         atFirst.threw.empty() ? "" : ("; threw " + atFirst.threw).c_str());
            fail("a pipeline opened at the first picture starts there, as a pass of its own");
        }
        for (const size_t index : {kWithin, kPast}) {
            const bool within = index < kCatchUpBudget;
            const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, file, index, ids[index]);
            if (!position) continue;
            const StartRun failed = runFrom(device, asset(file), position, kFollowing, [] { g_seekFailure = SeekFailure::All; });
            const bool seekFailed = std::any_of(failed.seeks.begin(), failed.seeks.end(),
                                                [](const SeekCall& seek) { return seek.result < 0; });
            const size_t shows = within ? index : 0;
            const std::vector<int> expected(ids.begin() + static_cast<long>(shows),
                                            ids.begin() + static_cast<long>(shows + kFollowing));
            const bool passMark = !failed.startsPass.empty() && failed.startsPass.front();
            if (!failed.threw.empty() || !seekFailed || failed.shown != expected || passMark == within ||
                failed.startsPass.empty()) {
                std::fprintf(stderr, "FAIL: %s: %s opened at picture %zu, every seek failing: showed %s (first marked %s) "
                                     "where %s was due; %s%s\n",
                             label.c_str(), file, index, joinIds(failed.shown).c_str(), passMark ? "yes" : "no",
                             joinIds(expected).c_str(), describeSeeks(failed.seeks).c_str(),
                             failed.threw.empty() ? "" : ("; threw " + failed.threw).c_str());
                fail(!seekFailed ? "no seek of the start failed, so the failed-seek rule was not exercised"
                     : within    ? "a start whose seeks fail within the budget reads from the first packet to its position"
                                 : "a start whose seeks fail past the budget shows the first picture as a new pass");
            }
            std::printf("%s: %s opened at picture %zu, every seek failing: showed %s, %s\n", label.c_str(), file, index,
                        joinIds(failed.shown).c_str(), describeSeeks(failed.seeks).c_str());
        }
    }

    // GOAL: S15.144 -- a pipeline opened at a file's LAST picture shows it, then loops to the first
    //       picture and plays on: the picture it opened at does not start a pass and the loop's does. The
    //       daemon parks a file whose pass holds one picture -- two successive pass starts -- so a resumed
    //       last picture marked as a pass start would park a playing video for good.
    // CONTROL: renderer/video/picture_source.cpp, next()'s `startsPass` made `!m_returnedAny` (the S15.141 break
    //       above), via control_batch.sh 2026-10-15 and at its new home 2026-10-19 (T7), reverted -> "FAIL: video_pipeline_probe (start-at-host): counter_short.mp4 opened at
    //       its last picture: showed 19 0 1 where 19 0 1 was due; pass starts 110", on start-at-host (Lavapipe)
    //       and start-at-device (RTX).
    {
        const char* const file = "counter_short.mp4";
        const std::optional<CounterReference> reference = counterReference(label, file);
        if (!reference || reference->ids.size() < 3) return;
        const std::vector<int>& ids = reference->ids;
        const size_t last = ids.size() - 1;
        const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, file, last, ids[last]);
        if (!position) return;
        const StartRun run = runFrom(device, asset(file), position, 3);
        const std::vector<int> due{ids[last], ids[0], ids[1]};
        const std::vector<bool> marks{false, true, false};
        if (!run.threw.empty() || run.shown != due || run.startsPass != marks) {
            std::fprintf(stderr, "FAIL: %s: %s opened at its last picture: showed %s where %s was due; pass starts %s%s\n",
                         label.c_str(), file, joinIds(run.shown).c_str(), joinIds(due).c_str(),
                         [&] {
                             std::string text;
                             for (const bool mark : run.startsPass) text += mark ? "1" : "0";
                             return text;
                         }()
                             .c_str(),
                         run.threw.empty() ? "" : ("; threw " + run.threw).c_str());
            fail("a pipeline opened at the last picture loops and plays on, only the loop's picture starting a pass");
        }
    }
}

// What a VideoDecoder returned on its way to one picture and the four after it, called with next(limit) until
// that picture came (limit 0: next() alone). Pictures counted are those ffmpeg delivered during each call.
struct BoundedRun {
    std::vector<std::optional<std::chrono::nanoseconds>> times;
    std::vector<std::optional<wallpaperx::StreamPosition>> positions;
    std::vector<int> droppingCalls;  // pictures delivered in each call that answered Dropping
    int finalCall = 0;               // pictures delivered in the call that returned the picture
    int devicePictures = 0;
    int hostPictures = 0;
    bool failureConsumed = true;
    std::string threw;
};

// With `failAfter`, the decoder opens at the start, returns that many pictures, and the measured calls meet a
// hardware failure first; otherwise it opens at `startAt`.
BoundedRun boundedRun(Device& device, const std::string& file, std::optional<wallpaperx::StreamPosition> startAt,
                      int failAfter, uint32_t limit) {
    BoundedRun run;
    struct Clear {
        ~Clear() {
            g_tallying = false;
            g_receiveExternalRemaining = 0;
        }
    } clear;
    const auto tally = [&] {
        run.devicePictures += g_tally.devicePictures;
        run.hostPictures += g_tally.hostPictures;
        g_tallying = false;
        return g_tally.pictures;
    };
    try {
        VideoDecoder decoder(device, file, startAt);
        for (int i = 0; i < failAfter; ++i) {
            if (!decoder.next()) {
                run.threw = "the stream ended before the failure point";
                return run;
            }
        }
        if (failAfter > 0) g_receiveExternalRemaining = 1;
        std::optional<DecodedFrame> first;
        for (int call = 0; !first; ++call) {
            if (call > 2000) {
                run.threw = "no picture after 2000 bounded calls";
                return run;
            }
            g_tally = CallTally{};
            g_tallying = true;
            if (limit == 0) {
                first = decoder.next();
                run.finalCall = tally();
                if (!first) break;
                continue;
            }
            auto step = decoder.next(limit);
            const int delivered = tally();
            if (std::holds_alternative<VideoDecoder::Dropping>(step)) {
                run.droppingCalls.push_back(delivered);
                continue;
            }
            first = std::move(std::get<std::optional<DecodedFrame>>(step));
            run.finalCall = delivered;
            if (!first) break;
        }
        if (!first) {
            run.threw = "the stream ended before a picture";
            return run;
        }
        run.failureConsumed = g_receiveExternalRemaining == 0;
        run.times.push_back(first->presentationTime());
        run.positions.push_back(first->position());
        for (size_t i = 1; i < kFollowing; ++i) {
            const std::optional<DecodedFrame> next = decoder.next();
            if (!next) break;
            run.times.push_back(next->presentationTime());
            run.positions.push_back(next->position());
        }
    } catch (const std::exception& e) {
        run.threw = e.what();
    }
    return run;
}

void runBoundedDecodeChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_long.mp4";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    // 46 and 70 are 22 pictures past the keyframes at 24 and 48, so each catch-up drops about that many.
    constexpr size_t kStart = 46;
    constexpr int kFailAfter = 70;
    const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, file, kStart, reference->ids[kStart]);
    if (!position) return;

    // GOAL: S15.413 -- VideoDecoder::next(n) delivers at most n pictures per call that answers Dropping (it
    //       drops at least one), and a run of such calls returns exactly the picture and position next()
    //       returns, then the same pictures after it: on a start part way in (the device's reset-point start,
    //       or software's seek), and on the device for software's resume after a hardware failure.
    // WHY-THIS-SHAPE: each call's pictures are counted where ffmpeg delivers them, and the run is compared with
    //       an unbounded run of the same decoder from the same point, by timestamp and position -- the counter
    //       would not see a position handed out wrong. Limits 1 and 4, so both a stop after every picture and
    //       a stop part way through a group are taken. The total delivered must match too: a bounded run that
    //       decoded a picture twice, or skipped one it then had to find again, shows there.
    // CONTROL: via control_run.sh, 2026-10-08, each alone, reverted, in renderer/video/video_decoder.cpp decode().
    //       `++dropped >= *dropLimit` made `>` -> on bounded_decode_device and bounded_decode_host_lavapipe "11
    //       call(s) answered Dropping, 11 of them delivering other than 1 to 1 picture(s)" at every case, the
    //       sequences still equal. The Dropping return also clearing `m_resume` -> "2 picture(s) delivered against
    //       23 unbounded; picture 1 of 5 differs in timestamp or position" at both limits and cases, on both
    //       entries; the limit-1 runs also fail the line above, having answered Dropping once (the coverage half).
    // COVERAGE-ASSERT: the unbounded catch-up delivers at least 10 pictures, and the limit-1 run answered
    //       Dropping at least 9 times; the failure case's injected failure was consumed and software
    //       delivered pictures after it; the start cases delivered only where the entry says.
    struct Case {
        const char* what;
        std::optional<wallpaperx::StreamPosition> startAt;
        int failAfter;
    };
    std::vector<Case> cases{{"opened at picture 46", position, 0}};
    if (onDevice) cases.push_back({"resumed in software after a hardware failure 70 pictures in", std::nullopt, kFailAfter});
    int graded = 0;
    for (const Case& c : cases) {
        const BoundedRun unbounded = boundedRun(device, asset(file), c.startAt, c.failAfter, 0);
        const std::string base = std::string(file) + " " + c.what;
        if (!unbounded.threw.empty() || unbounded.times.size() != kFollowing) {
            std::fprintf(stderr, "FAIL: %s: %s: the unbounded run %s\n", label.c_str(), base.c_str(),
                         unbounded.threw.empty() ? "returned fewer than five pictures" : unbounded.threw.c_str());
            fail("the unbounded decode a bounded run is compared with did not complete");
            continue;
        }
        const bool premise = unbounded.finalCall >= 10 && unbounded.failureConsumed &&
                             (c.failAfter > 0 ? unbounded.hostPictures > 0
                                              : (onDevice ? unbounded.hostPictures == 0 : unbounded.devicePictures == 0));
        if (!premise) {
            std::fprintf(stderr, "FAIL: %s: %s: the unbounded catch-up delivered %d picture(s), %d on the device and %d in "
                                 "software; failure %s\n",
                         label.c_str(), base.c_str(), unbounded.finalCall, unbounded.devicePictures,
                         unbounded.hostPictures, unbounded.failureConsumed ? "consumed" : "never consumed");
            fail("a bounded-decode case did not reach the catch-up it is written for");
            continue;
        }
        for (const uint32_t limit : {1u, 4u}) {
            const BoundedRun bounded = boundedRun(device, asset(file), c.startAt, c.failAfter, limit);
            const std::string what = base + ", next(" + std::to_string(limit) + ")";
            if (!bounded.threw.empty()) {
                std::fprintf(stderr, "FAIL: %s: %s: threw: %s\n", label.c_str(), what.c_str(), bounded.threw.c_str());
                fail("a bounded decode threw where the unbounded one did not");
                continue;
            }
            ++graded;
            int delivered = bounded.finalCall;
            int overLimit = 0;
            for (const int n : bounded.droppingCalls) {
                delivered += n;
                if (n < 1 || n > static_cast<int>(limit)) ++overLimit;
            }
            if (overLimit != 0 || bounded.finalCall > static_cast<int>(limit) ||
                (limit == 1 && bounded.droppingCalls.size() < 9)) {
                std::fprintf(stderr, "FAIL: %s: %s: %zu call(s) answered Dropping, %d of them delivering other than 1 to "
                                     "%u picture(s); the last delivered %d\n",
                             label.c_str(), what.c_str(), bounded.droppingCalls.size(), overLimit, limit,
                             bounded.finalCall);
                fail("next(n) delivers at most n pictures per call, and at least one when it answers Dropping");
            }
            if (bounded.times != unbounded.times || bounded.positions != unbounded.positions ||
                delivered != unbounded.finalCall) {
                size_t first = 0;
                while (first < bounded.times.size() && first < unbounded.times.size() &&
                       bounded.times[first] == unbounded.times[first] && bounded.positions[first] == unbounded.positions[first]) {
                    ++first;
                }
                std::fprintf(stderr, "FAIL: %s: %s: %d picture(s) delivered against %d unbounded; picture %zu of %zu differs "
                                     "in timestamp or position\n",
                             label.c_str(), what.c_str(), delivered, unbounded.finalCall, first + 1,
                             unbounded.times.size());
                fail("bounded calls to next(n) return the picture and position next() returns, and the same pictures after");
            }
            std::printf("%s: %s: %zu call(s) answered Dropping, then one returned the picture; %d delivered (unbounded %d)\n",
                        label.c_str(), what.c_str(), bounded.droppingCalls.size(), delivered, unbounded.finalCall);
        }
    }
    check(graded == static_cast<int>(cases.size()) * 2, "every bounded-decode case this check is written for was graded");
}

// GOAL: S15.172 -- a start part way in on the device, whose pictures then lose their timestamps before the
//       hardware fails, resumes in software at the picture it started at: never at the picture as many in
//       from the file's start as were returned since the start. S15.179 -- the same past the catch-up
//       budget with the resume's seek failing: the resume is still of running playback, so it reads from
//       the first packet to the picture it started at, and no picture after the failure starts a pass.
// WHY-THIS-SHAPE: started at 150 of counter_long.mp4, the 20 pictures after the first are returned with their
//       timestamps stripped, leaving the resume a count and no timestamp; the next call meets a hardware
//       failure. A count measured from the file's first packet shows picture 21 there. At 300, past the
//       budget, a device that decoded after a part-way start reopens from the first packet without seeking, so
//       the seek S15.179 needs is reached another way: a stand-in forced at the start's first picture sends it
//       to software by seek (the device accepted nothing), and the device takes it back at the IDR at 312, from
//       a seek, so packets are unnumbered; 3 pictures are then stripped and the hardware fails with the resume
//       seek failing. A resume taken for a start that has shown nothing gives up there and shows picture 0.
// CONTROL: via control_batch.sh, 2026-10-01, reverted, renderer/video/video_decoder.cpp seekToResume():
//       S15.172: the re-run from the start's position (`if (!m_returned.lastTimestamp && !byPacket &&
//       m_startedAfter)`) made `if (false)` -> "opened at picture 150, ...: after the hardware failure showed
//       21 22 23 (pass marks 000) where 150 151 152 was due". S15.179, rewritten case, control_batch.sh 2026-10-20
//       (G1), RTX 3080 Ti, reverted: that re-run also setting `m_returned.count = 0;` -> "... a stand-in at its first
//       picture, 3 pictures without timestamps, then a hardware failure, its resume seek failing: after the hardware
//       failure showed 0 1 2 (pass marks 000) where 300 301 302 was due"; green restored.
// COVERAGE-ASSERT: the injected failure was consumed and software drew the pictures after it; in the S15.179
//       case the stand-in was forced once, the picture before the strip came from the device, and a seek made
//       after the failure failed.
void runStartedFailureChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_long.mp4";
    constexpr size_t kAfter = 3;
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    const std::vector<int>& ids = reference->ids;
    struct Case {
        size_t start;
        size_t stripFrom;  // the first call whose pictures lose their timestamps
        size_t stripped;
        bool failResumeSeek;
        bool standInAtStart;
    };
    for (const Case& c : {Case{150, 1, 20, false, false}, Case{300, 13, 3, true, true}}) {
        if (c.start + c.stripFrom + c.stripped + kAfter > ids.size() || (c.failResumeSeek && c.start <= kCatchUpBudget)) {
            fail("counter_long.mp4 does not fit the start this check makes");
            continue;
        }
        const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, file, c.start, ids[c.start]);
        if (!position) continue;
        const size_t failing = c.stripFrom + c.stripped;  // the call that meets the hardware failure
        bool consumed = false;
        bool resumeSeekFailed = false;
        bool deviceBeforeStrip = !c.standInAtStart;
        int hostBefore = 0;
        size_t seeksBefore = 0;
        g_standInsForced = 0;
        // g_stripTimestamps and the seek failure apply inside a failing call only, which runFrom() opens.
        const StartRun run = runFrom(
            device, asset(file), position, failing + kAfter,
            [&] { g_standInAtNextPicture = c.standInAtStart; },
            [&](size_t call) {
                if (call == 1) g_standInAtNextPicture = false;
                if (call == c.stripFrom) {
                    if (c.standInAtStart) deviceBeforeStrip = g_lastReceivedDevice;
                    g_stripTimestamps = true;
                }
                if (call == failing) {
                    g_stripTimestamps = false;
                    hostBefore = g_hostPictures;
                    seeksBefore = g_failingSeeks.size();
                    g_receiveExternalRemaining = 1;
                    if (c.failResumeSeek) g_seekFailure = SeekFailure::AwayFromStart;
                }
                if (call == failing + 1) {
                    consumed = g_receiveExternalRemaining == 0;
                    resumeSeekFailed = std::any_of(g_failingSeeks.begin() + static_cast<long>(seeksBefore),
                                                   g_failingSeeks.end(), [](const SeekCall& seek) { return seek.result < 0; });
                    g_seekFailure = SeekFailure::None;
                }
            });
        const int hostAfter = run.hostPictures - hostBefore;
        const std::vector<int> due(ids.begin() + static_cast<long>(c.start), ids.begin() + static_cast<long>(c.start + kAfter));
        const size_t from = std::min(run.shown.size(), failing);
        const std::vector<int> after(run.shown.begin() + static_cast<long>(from), run.shown.end());
        std::string marks;
        for (size_t i = from; i < run.startsPass.size(); ++i) marks += run.startsPass[i] ? "1" : "0";
        const std::string what = std::string(file) + " opened at picture " + std::to_string(c.start) +
                                 (c.standInAtStart ? ", a stand-in at its first picture" : "") + ", " +
                                 std::to_string(c.stripped) + " pictures without timestamps, then a hardware failure" +
                                 (c.failResumeSeek ? ", its resume seek failing" : "");
        const bool standInMet = !c.standInAtStart || g_standInsForced == 1;
        if (!run.threw.empty() || !consumed || hostAfter == 0 || (c.failResumeSeek && !resumeSeekFailed) || !standInMet ||
            !deviceBeforeStrip) {
            std::fprintf(stderr, "FAIL: %s: %s: %s; failure %s, %d host picture(s) after it; %s; %d stand-in(s) forced; "
                         "picture before the strip from the %s\n",
                         label.c_str(), what.c_str(), run.threw.empty() ? "ran" : ("threw " + run.threw).c_str(),
                         consumed ? "consumed" : "never reached", hostAfter,
                         resumeSeekFailed ? "a seek after it failed" : "no seek after it failed", g_standInsForced,
                         deviceBeforeStrip ? "device" : "host");
            fail("the hardware failure after a start's untimestamped pictures was not exercised");
        } else if (after != due || marks.find('1') != std::string::npos) {
            std::fprintf(stderr, "FAIL: %s: %s: after the hardware failure showed %s (pass marks %s) where %s was due\n",
                         label.c_str(), what.c_str(), joinIds(after).c_str(), marks.c_str(), joinIds(due).c_str());
            fail("a start that lost its timestamps resumes after a hardware failure at the picture it started at, "
                 "still mid-stream");
        }
        std::printf("%s: %s: showed %s\n", label.c_str(), what.c_str(), joinIds(run.shown).c_str());
    }
}

// GOAL: S15.920 -- a software reopen after a device failure keeps its position. When the device decoded after
//       a part-way start it reads from the first packet, with no seek, and drops forward to the resume point;
//       when the device refused at the start's first packet it seeks to the nearest keyframe and drops a few.
//       Either way the first picture returned after the reopen is the resume picture.
// WHY-THIS-SHAPE: counter_long.mp4 opened at 300 (past the catch-up budget, so reading from the first packet
//       decodes about 300 pictures and a seek about 12 from the IDR at 288). Software pictures decoded are
//       counted at the decoder: a pictures-shown check alone passes both ways, since both keep the position.
//       The refusal is forced in av_hwframe_ctx_init, inside the first packet's format callback, so it reaches
//       the path on any card with a decoder; the 10-bit file reaches it only where the device refuses 10-bit.
// CONTROL: control_batch.sh 2026-10-20 (G1), RTX 3080 Ti, each alone, reverted, renderer/video/video_decoder.cpp
//       reopenInSoftware(): `fromFirstPacket = false` -> "... 3 pictures on the device, then a hardware failure:
//       decoded 20 picture(s) in software to reach 300, so it seeked where the device had decoded after a part-way
//       start"; `fromFirstPacket = m_parameterSets != InExtradata` -> "... the device refusing its first packet:
//       decoded 305 picture(s) in software to reach 300, so it read from the first packet where a seek was due".
// COVERAGE-ASSERT: the failure or refusal was consumed; the decoded-then-failed case drew device pictures first.
void runReopenPositionChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_long.mp4";
    constexpr size_t kStart = 300;
    constexpr size_t kOnDevice = 3;
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    const std::vector<int>& ids = reference->ids;
    if (ids.size() < kStart + kOnDevice + kFollowing || kStart <= kCatchUpBudget) {
        fail("counter_long.mp4 does not fit the starts this check makes");
        return;
    }
    const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, file, kStart, ids[kStart]);
    if (!position) return;
    for (const bool refuseAtStart : {false, true}) {
        const std::string what = std::string(file) + " opened at picture " + std::to_string(kStart) +
                                 (refuseAtStart ? ", the device refusing its first packet"
                                                : ", " + std::to_string(kOnDevice) + " pictures on the device, then a hardware failure");
        int hostBefore = 0;
        int deviceBefore = 0;
        bool consumed = false;
        const size_t failing = refuseAtStart ? 0 : kOnDevice;
        const size_t calls = failing + kFollowing;
        const StartRun run = runFrom(
            device, asset(file), position, calls, [&] { if (refuseAtStart) g_failFramesInit = 1; },
            [&](size_t call) {
                if (call == 1 && refuseAtStart) consumed = g_failFramesInit == 0;
                if (call == failing && !refuseAtStart) {
                    hostBefore = g_hostPictures;
                    deviceBefore = g_devicePictures;
                    g_receiveExternalRemaining = 1;
                }
                if (call == failing + 1 && !refuseAtStart) consumed = g_receiveExternalRemaining == 0;
            });
        const int softwareDecoded = run.hostPictures - hostBefore;
        const std::vector<int> due(ids.begin() + static_cast<long>(kStart), ids.begin() + static_cast<long>(kStart + calls));
        std::printf("%s: %s: showed %s; %d picture(s) decoded in software for it, %d on the device before\n", label.c_str(),
                    what.c_str(), joinIds(run.shown).c_str(), softwareDecoded, deviceBefore);
        const bool premise = refuseAtStart ? run.devicePictures == 0 : deviceBefore > 0 && run.devicePictures == deviceBefore;
        if (!run.threw.empty() || !consumed || !premise) {
            std::fprintf(stderr, "FAIL: %s: %s: %s; %s; %d device picture(s) before\n", label.c_str(), what.c_str(),
                         run.threw.empty() ? "ran" : ("threw " + run.threw).c_str(),
                         consumed ? "consumed" : "never reached", deviceBefore);
            fail("the device failure or refusal this reopen is graded on was not exercised");
            continue;
        }
        if (run.shown != due) {
            std::fprintf(stderr, "FAIL: %s: %s: showed %s where %s was due\n", label.c_str(), what.c_str(),
                         joinIds(run.shown).c_str(), joinIds(due).c_str());
            fail("a software reopen after a device failure keeps its position");
        }
        const bool fromFirstPacket = softwareDecoded > static_cast<int>(kCatchUpBudget);
        if (fromFirstPacket == refuseAtStart) {
            std::fprintf(stderr, "FAIL: %s: %s: decoded %d picture(s) in software to reach %zu, so it %s\n", label.c_str(),
                         what.c_str(), softwareDecoded, kStart,
                         fromFirstPacket ? "read from the first packet where a seek was due"
                                         : "seeked where the device had decoded after a part-way start");
            fail(refuseAtStart ? "a reopen after a refusal at the start's first packet seeks to the nearest keyframe"
                               : "a reopen after the device decoded past a part-way start reads from the first packet");
        }
    }
}

// What a looping pipeline showed over `calls` calls from the file's start, and whether the last picture the
// decoder received in each call -- the one shown -- came from the device.
struct DamagedRun {
    std::vector<int> shown;
    std::vector<bool> device;
    std::vector<bool> startsPass;
    std::string threw;
    bool exhausted = false;  // what was thrown was a ResourceExhausted
    bool fromSetup = false;  // VideoPipeline::softwareFromSetup() after the last picture: the daemon's "all"
    int softwarePackets = 0;  // sent to a software decoder over the whole run
};

// Video packets in the file, and those before the first that resets the decoder by the reference reader
// (testing::referenceResets, not the production rule under test); -1 for either when unreadable or absent.
struct FixturePackets {
    int count = -1;
    int lead = -1;
};
FixturePackets fixturePackets(const std::string& file) {
    std::string why;
    const std::optional<GopLayout> layout = readGopLayout(file, why);
    if (!layout) {
        std::fprintf(stderr, "  the harness could not read %s: %s\n", file.c_str(), why.c_str());
        return {};
    }
    const std::vector<size_t> resets = layout->resets();
    return {static_cast<int>(layout->packets.size()), resets.empty() ? -1 : static_cast<int>(resets.front())};
}

DamagedRun runDamaged(Device& device, const std::string& file, size_t calls) {
    DamagedRun run;
    struct Clear {
        ~Clear() { g_recordPictures = false; }
    } clear;
    try {
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
        g_softwarePacketsSent.store(0);
        g_recordPictures = true;
        VideoPipeline pipeline(device, file, kShaderDir, VideoPipeline::Playback::Loop);
        for (size_t call = 0; call < calls; ++call) {
            g_lastReceivedDevice = false;
            if (!pipeline.renderNext(targets, target)) break;
            run.device.push_back(g_lastReceivedDevice);
            run.startsPass.push_back(pipeline.timing().startsPass);
            run.shown.push_back(wallpaperx::testing::counterFromBgra(
                wallpaperx::testing::reclaimAndReadback(device, targets, target), target.width, target.height));
        }
        run.fromSetup = pipeline.softwareFromSetup();
    } catch (const wallpaperx::ResourceExhausted& e) {
        run.threw = e.what();
        run.exhausted = true;
    } catch (const std::exception& e) {
        run.threw = e.what();
    }
    run.softwarePackets = g_softwarePacketsSent.load();
    return run;
}

// GOAL: S15.915 -- a damaged H.264 or HEVC file (a reference dropped, or a stream cut inside a group) never makes the
//       hardware decoder misuse Vulkan, and every picture is shown once, by drawn counter, on both passes.
//       S15.916 -- after the stand-in the stream returns to the device at the next IDR, nothing lost or repeated.
//       S15.917 -- a healthy file starting inside a group (counter_cut_nonidr.mp4: non-IDR I at 48, IDR at 150;
//       counter_cut_cra.mp4: HEVC CRA at 48, IDR at 180) is in software up to that IDR only, on every pass.
//       S15.918 -- the third stand-in in a pass keeps the stream in software, later passes included; restart()
//       re-arms the device otherwise. S15.919 -- after parameter sets beyond the extradata (counter_ps_gap.mp4, in
//       band), a stand-in keeps the rest of that pass in software.
// WHY-THIS-SHAPE: a fixture's stand-in must hold the pixels the encoder predicted from, or the counter cannot grade
//       it: H.264 stand-ins copy the last reference (one reference picture, or the dropped one drawing it), HEVC's
//       are flat grey (the dropped picture is grey). B pictures are what make the device misuse Vulkan; G1's files
//       without them gave no message even unguarded. The Vulkan half is this entry's whole-run validation grade, so
//       it cannot name the file; which decoder drew each picture is read at the decoder.
// CONTROL: control_batch.sh, RTX 3080 Ti, each alone, reverted, green restored. renderer/video/video_decoder.cpp:
//       allocatePicture() not refusing an H.264 stand-in -> "66 Vulkan validation warning(s)" (07238, 07265, 07239,
//       07151, 07264, 07266, 01026); not refusing an HEVC one -> "16 Vulkan validation warning(s)" (07265, 07239);
//       advanceHandBack() never handing back -> "counter_cut_nonidr.mp4: pass 1 after picture 48" and "counter_cut_cra.mp4: pass 1 after
//       picture 48"; kStandInsPerPass 1 -> "counter_cut_cra.mp4: pass 1 after picture 48"; restart() clearing the
//       hand-back -> "counter_ps_gap.mp4: pass 2 decoders SSS..." (deleting its re-arm line stays green here: these
//       files end a pass already AwaitingReset; the switch-failure checks at S15.935 catch that one). The Emptying
//       drain skipped -> "counter_gap.mp4: ... first difference at call 105"; kStandInsPerPass 4 -> "the device
//       took the stream back 3 time(s) in pass 1, not 2"; restart() re-arming a stream that gave up -> "... stays
//       in software, later passes included"; advanceHandBack() ignoring m_parameterSets -> "counter_ps_gap.mp4:
//       pass 1 after picture 159". All re-run G3e 2026-10-23, decode thread off (damaged-device hides the queues).
// COVERAGE-ASSERT: each pass shows the reference's full count; every fixture in the table is graded; on the device a
//       file with a gap drew device pictures before it and host pictures after it.
// Whether the driver, as this process lets the Device see it, offers VK_KHR_video_maintenance2 with its feature:
// asked of the driver, not of the Device, so a Device that fails to enable it is graded against what it should do.
bool inlineParametersOffered(const Device& device) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device.physicalDevice(), nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device.physicalDevice(), nullptr, &count, available.data());
    available.resize(count);
    if (std::none_of(available.begin(), available.end(), [](const VkExtensionProperties& e) {
            return std::strcmp(e.extensionName, VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME) == 0;
        })) {
        return false;
    }
    VkPhysicalDeviceVideoMaintenance2FeaturesKHR feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &feature;
    vkGetPhysicalDeviceFeatures2(device.physicalDevice(), &query);
    return feature.videoMaintenance2 == VK_TRUE;
}

void runDamagedChecks(Device& device, const std::string& label, bool onDevice, const char* only = nullptr) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;
    struct Fixture {
        const char* file;
        int firstGap;     // the first dropped picture's number
        int handBackAt;   // the first picture the device takes back in pass 1 after the first gap; -1 for none
        int stayFrom;     // with no hand-back: from this picture on pass 1 stays in software
        bool deviceInPass2;
        int firstSoftware = -1;  // the first picture shown from software; -1: the one after firstGap - 1;
                                 // kNoneShown: none, the stand-ins fall before the first picture shown
        bool samePass2 = false;  // pass 2 must show pass 1's decoder pattern
    };
    constexpr int kNoneShown = -2;
    const Fixture fixtures[] = {{"counter_gap.mp4", 99, 120, -1, true},
                                {"counter_gap_every.mp4", 10, -1, 70, false},
                                {"counter_ps_gap.mp4", 159, -1, 159, true},
                                {"counter_gap_bf.mp4", 96, 120, -1, true, 94, true},
                                {"counter_cut_start.ts", 60, 60, -1, true, kNoneShown, true},
                                {"counter_lead_then_i.mp4", 150, 150, -1, true, kNoneShown, true},
                                {"counter_hevc_gap.mp4", 99, -1, 95, true, 95, true},
                                {"counter_cut_nonidr.mp4", 48, 150, -1, true, 48, true},
                                {"counter_cut_cra.mp4", 48, 180, -1, true, 48, true}};
    // GOAL: S15.924/S15.931 -- counter_ps_gap.flv's parameter sets change at picture 120 through container side
    //       data (NEW_EXTRADATA), and picture 159 is dropped. With or without inline parameters
    //       (VK_KHR_video_maintenance2) the device draws 0-158 on every pass -- without them through a fresh device
    //       decoder at 120 -- then software from the stand-in, every number right, no 07154/07155 (this entry's
    //       validation grade). False if a pass after the first stays in software from its start.
    // WHY-THIS-SHAPE: samePass2 makes pass 2 repeat pass 1's decoders picture by picture; deviceInPass2 alone
    //       would pass a pass 2 that returned to the device only late. One row for both drivers: the switch makes
    //       the weaker no-inline row of the earlier design wrong, not merely weaker.
    // CONTROL: control_batch.sh 2026-10-22, RTX 3080 Ti, each alone, reverted, on damaged_device_no_inline,
    //       renderer/video/video_decoder.cpp decode(): the side-data guard off -> "counter_ps_gap.flv: ran; 598 of
    //       598 shown, first difference at call 121 (114 where 120 was due)" and "20 Vulkan validation warning(s)";
    //       software instead of a fresh device decoder at 120, and (2026-10-23) renderer/video/reset_point.cpp
    //       changesExtradata() returning false -> each "counter_ps_gap.flv: decoders DDD...(120 D)S...";
    //       rewind() not resetting m_parameterSets, and advanceHandBack() waiting for packet 50 -> each
    //       "counter_ps_gap.flv: pass 1 decoders ..., pass 2 decoders ...". restart() not re-arming the hand-back
    //       stayed green (the pass-2 return does not go through it).
    std::vector<Fixture> table(std::begin(fixtures), std::end(fixtures));
    table.push_back(Fixture{"counter_ps_gap.flv", 159, -1, 159, true, -1, true});
    if (only != nullptr) std::erase_if(table, [&](const Fixture& f) { return std::strcmp(f.file, only) != 0; });
    if (onDevice) {
        std::printf("%s: inline parameter sets %s, %s offered\n", label.c_str(), device.videoInlineParameters() ? "on" : "off",
                    inlineParametersOffered(device) ? "VK_KHR_video_maintenance2" : "nothing");
    }
    int graded = 0;
    for (const Fixture& f : table) {
        const std::optional<CounterReference> reference = counterReference(label, f.file);
        if (!reference) continue;
        const std::vector<int>& ids = reference->ids;
        const size_t n = ids.size();
        const DamagedRun run = runDamaged(device, asset(f.file), 2 * n);
        std::vector<int> due = ids;
        due.insert(due.end(), ids.begin(), ids.end());
        std::string kinds;
        for (const bool d : run.device) kinds += d ? 'D' : 'S';
        std::printf("%s: %s: %zu shown over two passes, %d packet(s) sent to software; decoders %s\n", label.c_str(),
                    f.file, run.shown.size(), run.softwarePackets, kinds.c_str());
        if (!run.threw.empty() || run.shown != due || run.startsPass.size() != 2 * n || !run.startsPass[n]) {
            size_t first = 0;
            while (first < run.shown.size() && first < due.size() && run.shown[first] == due[first]) ++first;
            std::fprintf(stderr, "FAIL: %s: %s: %s; %zu of %zu shown, first difference at call %zu (%d where %d was due)\n",
                         label.c_str(), f.file, run.threw.empty() ? "ran" : ("threw " + run.threw).c_str(),
                         run.shown.size(), due.size(), first + 1, first < run.shown.size() ? run.shown[first] : -1,
                         first < due.size() ? due[first] : -1);
            fail("a damaged file shows every picture once, in order, on each pass");
            continue;
        }
        ++graded;
        // GOAL: S15.953 -- a lead-in that ends at a picture FFmpeg may show (counter_lead_then_i.mp4: B and P pictures,
        //       then the non-IDR I 48 with no recovery-point SEI, the IDR at 150) is read past, rewound and sent as
        //       before. In software every packet of the file is sent on every pass. False if a pass drops any.
        // WHY-THIS-SHAPE: FFmpeg shows none of 39-149 here either way (its I-picture guess needs one reference; the
        //       PPS declares three), so the drawn counters graded above cannot see the rewind; only the packets sent
        //       can. On the device the rewind is not observable: the first non-IDR picture sent makes a stand-in, and
        //       reopenInSoftware() rewinds to packet 1 itself. A lower bound: a resent packet only adds.
        // CONTROL: control_batch.sh 2026-10-24 (T18d), restored, renderer/video/video_decoder.cpp skipUnshownLead()'s
        //       rewind() deleted -> damaged_host and damaged_host_lavapipe "counter_lead_then_i.mp4: 515 packet(s) sent
        //       ... at least 520 due"; renderer/video/reset_point.cpp h264UnitMayShow() not counting an I slice ->
        //       damaged_host "... 300 packet(s) sent ...". Re-run 2026-10-24 (T19a) after the count moved to
        //       readGopLayout(): the rewind deletion -> damaged_host "515 packet(s) sent ... at least 520 due".
        if (!onDevice && std::strcmp(f.file, "counter_lead_then_i.mp4") == 0) {
            const int packets = fixturePackets(asset(f.file)).count;
            if (packets <= 0) {
                std::fprintf(stderr, "FAIL: %s: %s: the harness could not count the file's packets\n", label.c_str(), f.file);
                fail("the harness could not count the file's packets, which is not a verdict on the decoder");
            } else if (run.softwarePackets < 2 * packets) {
                std::fprintf(stderr, "FAIL: %s: %s: %d packet(s) sent to software over two passes, at least %d due "
                             "(%d in the file, each pass)\n", label.c_str(), f.file, run.softwarePackets, 2 * packets, packets);
                fail("a lead-in FFmpeg may show is read past, rewound and sent on every pass");
            }
        }
        if (!onDevice) continue;
        // GOAL: S15.953 -- an H.264 stream whose lead-in FFmpeg never shows (counter_cut_start.ts: P and B pictures
        //       up to an IDR) sends none of it to software, on either pass; the shown pictures are graded above.
        // WHY-THIS-SHAPE: the skipped pictures are never shown, so the drawn counters cannot see them; the packets
        //       sent to the decoder's software context are counted at avcodec_send_packet instead.
        // CONTROL: control_batch.sh 2026-10-24 (T18b), reverted, renderer/video/video_decoder.cpp next(): the
        //       skipUnshownLead() call given `&& false` -> "counter_cut_start.ts: 30 packet(s) sent to software over
        //       two passes, 0 due"; re-run 2026-10-24 (T19a) with the lead read by readGopLayout(), same line.
        // COVERAGE-ASSERT: the fixture must still hold packets before its first IDR (the reference reader over the file),
        //       or there is no lead-in to skip and the 0 says nothing.
        if (std::strcmp(f.file, "counter_cut_start.ts") == 0) {
            const int lead = fixturePackets(asset(f.file)).lead;
            const int due = 0;
            if (lead <= 0) {
                std::fprintf(stderr, "FAIL: %s: %s: no reset point found by the harness\n", label.c_str(), f.file);
                fail("the harness could not count the packets before the first IDR, which is not a verdict on the decoder");
            } else if (run.softwarePackets != due) {
                std::fprintf(stderr, "FAIL: %s: %s: %d packet(s) sent to software over two passes, %d due\n",
                             label.c_str(), f.file, run.softwarePackets, due);
                fail("a lead-in FFmpeg never shows is sent to no decoder; one it may show is sent as before");
            }
        }
        // GOAL: S15.953 -- a lead-in that ends at a picture FFmpeg may show (counter_lead_then_i.mp4: B and P pictures,
        //       then the non-IDR I 48 with no recovery-point SEI, the IDR at 150) is not skipped on the device: each
        //       pass makes a stand-in at its first non-IDR picture and sends the lead-in to software. False if both
        //       passes skip the lead-in up to the IDR. Whether every pass sends all of it, and the rewind, are graded
        //       only by the software check above: on the device reopenInSoftware() resends from packet 1 by design.
        // WHY-THIS-SHAPE: the drawn counters cannot tell (FFmpeg shows none of 39-149 either way); only the packets
        //       sent can. A lower bound tied to the file's own pre-IDR count, not equality: a later hand-back (graded
        //       above, all-device) only adds software packets.
        // CONTROL: control_batch.sh 2026-10-24 (T18d), restored, renderer/video/reset_point.cpp h264UnitMayShow() not
        //       counting an I slice -> "counter_lead_then_i.mp4: 0 packet(s) of the lead-in sent ... at least 220 due";
        //       re-run 2026-10-24 (T19a) with the lead read by readGopLayout() (110 before the IDR), same line.
        //       Deleting skipUnshownLead()'s rewind() stays green here (see the software check above).
        // COVERAGE-ASSERT: the lead count must be found (> 0); a fixture whose lead ended at the IDR would send 0
        //       and go red here, not pass.
        if (std::strcmp(f.file, "counter_lead_then_i.mp4") == 0) {
            const int lead = fixturePackets(asset(f.file)).lead;
            if (lead <= 0) {
                std::fprintf(stderr, "FAIL: %s: %s: no reset point found by the harness\n", label.c_str(), f.file);
                fail("the harness could not count the packets before the first IDR, which is not a verdict on the decoder");
            } else if (run.softwarePackets < 2 * lead) {
                std::fprintf(stderr,
                             "FAIL: %s: %s: %d packet(s) of the lead-in sent to software over two passes, at least %d due "
                             "(%d before the IDR, each pass)\n",
                             label.c_str(), f.file, run.softwarePackets, 2 * lead, lead);
                fail("a lead-in FFmpeg may show is read past, rewound and sent on every pass");
            }
        }
        // Index of picture number `id` in a pass.
        const auto at = [&](int id) {
            return static_cast<size_t>(std::find(ids.begin(), ids.end(), id) - ids.begin());
        };
        if (f.firstSoftware == kNoneShown) {
            if (std::find(run.device.begin(), run.device.end(), false) != run.device.end()) {
                std::fprintf(stderr, "FAIL: %s: %s: decoders %s\n", label.c_str(), f.file, kinds.c_str());
                fail("a stream cut inside a group shows every picture from the device, both passes");
            }
            continue;
        }
        const size_t gap = f.firstSoftware >= 0 ? at(f.firstSoftware) : at(f.firstGap - 1) + 1;
        const bool deviceBefore = std::all_of(run.device.begin(), run.device.begin() + static_cast<long>(gap), [](bool d) { return d; });
        const bool hostAfterGap = gap < n && !run.device[gap];
        if (!deviceBefore || !hostAfterGap) {
            std::fprintf(stderr, "FAIL: %s: %s: decoders %s\n", label.c_str(), f.file, kinds.c_str());
            fail("the damaged file did not decode on the device up to its first gap and in software after it, so no "
                 "stand-in was exercised");
            continue;
        }
        // Pass 1 from the gap: software, then the device from handBackAt on; or software to the end.
        bool pass1 = true;
        for (size_t i = gap; i < n; ++i) {
            if (f.handBackAt >= 0 && run.device[i] != (ids[i] >= f.handBackAt)) pass1 = false;
            if (f.handBackAt < 0 && ids[i] >= f.stayFrom && run.device[i]) pass1 = false;
        }
        if (!pass1) {
            const std::string where = f.handBackAt >= 0 ? "software until " + std::to_string(f.handBackAt) + ", the device from it"
                                                        : "software from " + std::to_string(f.stayFrom) + " to the end";
            std::fprintf(stderr, "FAIL: %s: %s: pass 1 after picture %d: decoders %s, where %s was due\n", label.c_str(),
                         f.file, f.firstGap, kinds.substr(gap, n - gap).c_str(), where.c_str());
            fail(f.handBackAt >= 0 ? "a stand-in sends the stream to software until the next IDR, and the device from it"
                                   : "a stream that gave up, or sent sets beyond its extradata, stays in software");
        }
        if (f.samePass2 && !std::equal(run.device.begin(), run.device.begin() + static_cast<long>(n), run.device.begin() + static_cast<long>(n))) {
            std::fprintf(stderr, "FAIL: %s: %s: pass 1 decoders %s, pass 2 decoders %s\n", label.c_str(), f.file,
                         kinds.substr(0, n).c_str(), kinds.substr(n).c_str());
            fail("each pass spends the same pictures in software");
        }
        const bool anyDevice2 = std::any_of(run.device.begin() + static_cast<long>(n), run.device.end(), [](bool d) { return d; });
        if (anyDevice2 != f.deviceInPass2) {
            std::fprintf(stderr, "FAIL: %s: %s: pass 2 decoders %s\n", label.c_str(), f.file, kinds.substr(n).c_str());
            fail(f.deviceInPass2 ? "restart() re-arms the device for the next pass"
                                 : "after the third stand-in in a pass the stream stays in software, later passes included");
        }
        // S15.918's count: on counter_gap_every.mp4 the device takes the stream back exactly twice in pass 1.
        if (std::string(f.file) == "counter_gap_every.mp4") {
            int backs = 0;
            for (size_t i = 1; i < n; ++i) backs += run.device[i] && !run.device[i - 1] ? 1 : 0;
            if (backs != 2) {
                std::fprintf(stderr, "FAIL: %s: %s: the device took the stream back %d time(s) in pass 1, not 2: %s\n",
                             label.c_str(), f.file, backs, kinds.substr(0, n).c_str());
                fail("a stream returns to the device after its first two stand-ins of a pass, and not after the third");
            }
        }
    }
    if (!g_notVerified) check(graded == static_cast<int>(table.size()) && !table.empty(), "every damaged fixture this check is written for was graded");
}

// GOAL: S15.928 -- a file whose NAL length size changes at picture 120 (NEW_EXTRADATA, with new parameter sets)
//       shows every picture, rightly numbered, on passes 2 and 3 as on pass 1: on the device with
//       VK_KHR_video_maintenance2 (damaged-device), without it (damaged-device-no-inline), and in software
//       (damaged-host). On the device every picture is the device's. False if a later pass loses, misnumbers or
//       leaves the device for any picture.
// WHY-THIS-SHAPE: the files are built at run time from counter_ps.h264 (support/length_change_flv.h), both
//       directions, since only a 4-byte reader of 2-byte data misreads; three passes, because the loop back to
//       packet 1 after a pass that adopted the change is what G2b's defect broke, and the second loop is where
//       a per-pass leftover would first repeat. Graded by drawn counter against a plain libavcodec decode.
// CONTROL: control_batch.sh 2026-10-22, RTX 3080 Ti, each alone, reverted, renderer/video/video_decoder.cpp:
//       alignCodecExtradata() returning at once -> "length 4-then-2: pass 2 shows every picture" on damaged_device,
//       damaged_device_no_inline and damaged_host; software instead of a fresh device decoder at a parameter-set
//       change -> damaged_device_no_inline "length 4-then-2: every picture of every pass comes from the device".
// COVERAGE-ASSERT: every pass compares the reference's full 300 pictures, and both files must be graded --
//       red ("both length-change files were graded (0)") with the reference forced empty (test-side, by hand).
void runLengthChangePipelineChecks(Device& device, const std::string& label, bool onDevice) {
    if (!haveAsset("counter_ps.h264", label)) return;
    int graded = 0;
    for (const auto& [firstPrefix, secondPrefix] : {std::pair{4, 2}, std::pair{2, 4}}) {
        const std::string name = std::to_string(firstPrefix) + "-then-" + std::to_string(secondPrefix);
        const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                           ("wpx_pipeline_length_" + name + "_" + std::to_string(getpid()) + ".flv");
        struct Remove {
            std::filesystem::path path;
            ~Remove() { std::filesystem::remove(path); }
        } remove{path};
        const std::string file = wallpaperx::testing::buildLengthChangeFlv(asset("counter_ps.h264"), path, firstPrefix,
                                                                           secondPrefix);
        std::string why;
        const std::optional<CounterReference> reference =
            file.empty() ? std::nullopt : wallpaperx::testing::decodeCounterReference(file, why);
        if (!reference || reference->ids.size() != 300 ||
            std::find(reference->ids.begin(), reference->ids.end(), -1) != reference->ids.end()) {
            std::fprintf(stderr, "FAIL: %s: length %s: the harness could not build or read the fixture (%s)\n",
                         label.c_str(), name.c_str(), why.c_str());
            fail("the length-change fixture could not be built, which is not a verdict on the pipeline");
            continue;
        }
        const std::vector<int>& ids = reference->ids;
        const size_t n = ids.size();
        const DamagedRun run = runDamaged(device, file, 3 * n);
        std::string kinds;
        for (const bool d : run.device) kinds += d ? 'D' : 'S';
        std::printf("%s: length %s: %zu shown over three passes; decoders %s\n", label.c_str(), name.c_str(),
                    run.shown.size(), kinds.c_str());
        if (!run.threw.empty()) {
            std::fprintf(stderr, "FAIL: %s: length %s: threw %s\n", label.c_str(), name.c_str(), run.threw.c_str());
            fail("a length-change file plays three passes without raising");
            continue;
        }
        for (size_t pass = 0; pass < 3; ++pass) {
            const size_t from = std::min(pass * n, run.shown.size());
            const size_t to = std::min(from + n, run.shown.size());
            const std::vector<int> got(run.shown.begin() + static_cast<long>(from), run.shown.begin() + static_cast<long>(to));
            size_t first = 0;
            while (first < got.size() && got[first] == ids[first]) ++first;
            check(got == ids, label + ": length " + name + ": pass " + std::to_string(pass + 1) +
                                  " shows every picture, rightly numbered: " + std::to_string(got.size()) + " of 300, " +
                                  "first difference at " + std::to_string(first) + " (" +
                                  (first < got.size() ? std::to_string(got[first]) : std::string("none")) + " where " +
                                  (first < n ? std::to_string(ids[first]) : std::string("none")) + " was due)");
        }
        if (onDevice) {
            check(run.device.size() == 3 * n && std::all_of(run.device.begin(), run.device.end(), [](bool d) { return d; }),
                  label + ": length " + name + ": every picture of every pass comes from the device: " + kinds);
        }
        ++graded;
    }
    check(graded == 2, label + ": both length-change files were graded (" + std::to_string(graded) + ")");
}

struct CountedRun {
    DamagedRun run;
    std::string kinds;
    int opened;
    int maxLive;
    bool injected;
};

// runDamaged over `calls` pictures of `file`, counting the decoder's device codecs; `allocation` (unless VK_SUCCESS)
// or `framesFail` is injected into the first device decoder a switch opens.
// `atBitstream` moves it to the slice buffer allocated after the decoder's first picture; `atCodec` 1 moves it to the
// constructor's decoder.
CountedRun runCountingCodecs(Device& device, const std::string& file, size_t calls, VkResult allocation = VK_SUCCESS,
                             bool framesFail = false, bool atPicture = false, bool atBitstream = false, int atCodec = 2) {
    g_pictureAtSwitch = atPicture;
    g_pictureArmed = false;
    g_bitstreamAtSwitch = atBitstream;
    g_bitstreamArmed = false;
    g_injectAtCodec = atCodec;
    g_injectedInPicture = false;
    g_injectedSize = 0;
    g_deviceCodecsLive = 0;
    g_deviceCodecsMax = 0;
    g_deviceCodecsOpened = 0;
    g_allocationAtSwitch = allocation;
    g_allocationInjected = false;
    g_framesAtSwitch = framesFail;
    g_framesArmed = false;
    g_countDeviceCodecs = true;
    struct Clear {
        ~Clear() {
            g_countDeviceCodecs = false;
            g_allocationAtSwitch = VK_SUCCESS;
            g_allocationArmed = false;
            g_pictureAtSwitch = false;
            g_pictureArmed = false;
            g_bitstreamAtSwitch = false;
            g_bitstreamArmed = false;
            g_injectAtCodec = 2;
            g_framesAtSwitch = false;
            g_failFramesInit = 0;
        }
    } clear;
    CountedRun counted{runDamaged(device, file, calls), {}, 0, 0, false};
    for (const bool d : counted.run.device) counted.kinds += d ? 'D' : 'S';
    counted.opened = g_deviceCodecsOpened;
    counted.maxLive = g_deviceCodecsMax;
    counted.injected = framesFail ? g_framesArmed && g_failFramesInit == 0 : g_allocationInjected.load();
    return counted;
}

// The first pass index (0-2) whose pictures differ from `ids`, or -1 when all three passes show every one.
int firstWrongPass(const DamagedRun& run, const std::vector<int>& ids) {
    for (size_t pass = 0; pass < 3; ++pass) {
        const size_t from = pass * ids.size();
        if (run.shown.size() < from + ids.size() ||
            !std::equal(ids.begin(), ids.end(), run.shown.begin() + static_cast<long>(from))) {
            return static_cast<int>(pass);
        }
    }
    return -1;
}

// GOAL: S15.933 -- a device whose driver offers VK_KHR_video_maintenance2 enables it, and a side-data
//       parameter-set change at picture 120 then stays on that device decoder: one device codec over three
//       passes, every picture the device's. S15.934 -- without the extension the change opens a fresh device
//       decoder only after the old one is freed (never two open at once), pictures stay on the device, and a
//       length-size-only change opens no new decoder. False otherwise.
// WHY-THIS-SHAPE: codecs are counted at avcodec_open2/avcodec_free_context by interposer, the only place the
//       order of free and open is visible from outside; the pictures alone would be right with two sessions
//       open. The offered half asks the driver, not the Device, so a Device leaving it off is graded.
// CONTROL: control_batch.sh 2026-10-22, RTX 3080 Ti, each alone, reverted: renderer/vulkan/device.cpp
//       `inlineParameters = false` -> damaged_device "a driver offering VK_KHR_video_maintenance2 has it enabled";
//       renderer/video/video_decoder.cpp the two `!m_device.videoInlineParameters()` guards dropped -> damaged_device
//       "switch 4-then-2 stays on one device decoder ... (6 opened)"; switchDecoder() freeing the old codec after
//       openDecoder() -> damaged_device_no_inline "never two device decoders open at once ... (6 opened, at most 2
//       open)"; renderer/video/reset_point.cpp differInParameterSets() comparing the length byte unmasked ->
//       "a length-size-only change opens no new decoder (6 device codecs opened)".
void runSwitchChecks(Device& device, const std::string& label, bool inlineExpected) {
    if (!haveAsset("counter_ps.h264", label)) return;
    if (inlineExpected) {
        if (!inlineParametersOffered(device)) {
            std::printf("SKIP: %s: the driver does not offer VK_KHR_video_maintenance2; S15.933 is not graded here\n",
                        label.c_str());
            return;
        }
        check(device.videoInlineParameters(), label + ": a driver offering VK_KHR_video_maintenance2 has it enabled");
    }
    int graded = 0;
    for (const bool same : {false, true}) {
        if (inlineExpected && same) continue;
        const std::string name = same ? "4-then-2, same parameter sets" : "4-then-2";
        const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                           ("wpx_switch_" + std::to_string(same) + "_" + std::to_string(getpid()) + ".flv");
        struct Remove {
            std::filesystem::path path;
            ~Remove() { std::filesystem::remove(path); }
        } remove{path};
        const std::string file = wallpaperx::testing::buildLengthChangeFlv(asset("counter_ps.h264"), path, 4, 2, same);
        std::string why;
        const std::optional<CounterReference> reference =
            file.empty() ? std::nullopt : wallpaperx::testing::decodeCounterReference(file, why);
        if (!reference || reference->ids.size() != 300) {
            std::fprintf(stderr, "FAIL: %s: switch %s: the harness could not build or read the fixture (%s)\n",
                         label.c_str(), name.c_str(), why.c_str());
            fail("the switch fixture could not be built, which is not a verdict on the decoder");
            continue;
        }
        const std::vector<int>& ids = reference->ids;
        const CountedRun c = runCountingCodecs(device, file, 900);
        std::printf("%s: switch %s: %d device codecs opened, at most %d open at once; decoders %s\n", label.c_str(),
                    name.c_str(), c.opened, c.maxLive, c.kinds.c_str());
        check(c.run.threw.empty() && firstWrongPass(c.run, ids) < 0 && c.kinds == std::string(900, 'D'),
              label + ": switch " + name + ": three passes shown right, all on the device (threw '" + c.run.threw +
                  "', first wrong pass " + std::to_string(firstWrongPass(c.run, ids) + 1) + ")");
        if (inlineExpected) {
            check(c.opened == 1, label + ": switch 4-then-2 stays on one device decoder with inline parameter sets (" +
                                     std::to_string(c.opened) + " opened)");
        } else if (same) {
            check(c.opened == 1, label + ": a length-size-only change opens no new decoder (" + std::to_string(c.opened) +
                                     " device codecs opened)");
        } else {
            // COVERAGE-ASSERT: at least one switch happened, or "never two at once" holds trivially.
            check(c.opened >= 2 && c.maxLive == 1,
                  label + ": never two device decoders open at once across a switch (" + std::to_string(c.opened) +
                      " opened, at most " + std::to_string(c.maxLive) + " open)");
        }
        ++graded;
    }
    check(graded == (inlineExpected ? 1 : 2), label + ": every switch fixture was graded");
}

// GOAL: S15.935 (b), S15.936 -- at a switch on the GPU thread, a device that does not answer the question asked
//       before the old device decoder is freed raises GpuFailure, and that decoder is not freed (ffmpeg's free
//       would wait on its work without bound) and no new one is opened. False otherwise.
// WHY-THIS-SHAPE: armed at the old codec's EOF that ends its emptying: switchDecoder() submits nothing between
//       that and its question, so the held, refused batch is the question's. The pipeline outlives the catch, so
//       frees are counted at the throw, not after its destructor -- which may free once the device answers again.
// CONTROL: control_batch.sh 2026-10-22, RTX 3080 Ti, each alone, reverted, damaged_device_no_inline,
//       renderer/video/video_decoder.cpp switchDecoder(): the question dropped -> "the old device decoder is not
//       freed and no new one opened (1 freed, 2 opened)" and "exactly the switch's question was refused (0
//       refused)"; its GpuFailure thrown as std::runtime_error -> "... at a switch raises GpuFailure".
// COVERAGE-ASSERT: the arming fired, and exactly the switch's question was refused (one stalled wait).
void runUnansweredSwitchChecks(Device& device, const std::string& label, const std::string& file) {
    const std::string what = label + ": a device that no longer answers at a switch";
    bool gpuFailure = false;
    std::string threw;
    int shown = 0;
    int freedAtThrow = -1;
    int openedAtThrow = -1;
    int stalled = 0;
    g_unanswerFired = false;
    g_deviceCodecsLive = 0;
    g_deviceCodecsOpened = 0;
    g_deviceCodecsFreed = 0;
    {
        struct Clear {
            ~Clear() {
                g_unanswerAtSwitch = false;
                g_stallHeld = false;
                g_holdNextSubmission = false;
                g_countDeviceCodecs = false;
            }
        } clear;
        std::optional<OffscreenTargetManager> targets;
        std::optional<VideoPipeline> pipeline;
        try {
            targets.emplace(device);
            const OffscreenTarget& target = targets->create(1, kTargetWidth, kTargetHeight);
            g_countDeviceCodecs = true;
            pipeline.emplace(device, file, kShaderDir, VideoPipeline::Playback::Loop);
            const int stalledBefore = g_stalledWaits;
            g_unanswerAtSwitch = true;
            try {
                for (int call = 0; call < 300 && pipeline->renderNext(*targets, target); ++call) ++shown;
            } catch (const wallpaperx::GpuFailure& e) {
                gpuFailure = true;
                threw = e.what();
            } catch (const std::exception& e) {
                threw = e.what();
            }
            freedAtThrow = g_deviceCodecsFreed;
            openedAtThrow = g_deviceCodecsOpened;
            stalled = g_stalledWaits - stalledBefore;
        } catch (const std::exception& e) {
            threw = std::string("setup: ") + e.what();
        }
        g_unanswerAtSwitch = false;
        g_stallHeld = false;
        g_holdNextSubmission = false;
        g_heldFences.clear();
        const auto retireBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!device.hasCompleted(device.lastSubmission()) && std::chrono::steady_clock::now() < retireBy) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        try {
            pipeline.reset();
            targets.reset();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: %s: tearing down after the run threw: %s\n", what.c_str(), e.what());
            fail("teardown after the unanswered switch threw");
        }
    }
    std::printf("%s: fired %s, %d shown, threw '%s' (GpuFailure %s), device codecs at the throw: %d opened, %d freed; "
                "%d wait(s) refused\n",
                what.c_str(), g_unanswerFired ? "yes" : "no", shown, threw.c_str(), gpuFailure ? "yes" : "no",
                openedAtThrow, freedAtThrow, stalled);
    if (!device.hasCompleted(device.lastSubmission())) {
        std::fprintf(stderr, "FAIL: %s: the held batch had not completed 5 s after the hold was let go\n", what.c_str());
        fail("the harness could not retire the batch it held, so nothing after it in this child can be trusted");
        return;
    }
    if (!g_unanswerFired) {
        std::fprintf(stderr, "FAIL: %s: the old decoder's emptying never ended, so nothing was made to go unanswered\n",
                     what.c_str());
        fail("the unanswered-switch arming was not reached, which is not a verdict on the decoder");
        return;
    }
    check(gpuFailure, what + " raises GpuFailure (threw '" + threw + "')");
    check(freedAtThrow == 0 && openedAtThrow == 1,
          what + ": the old device decoder is not freed and no new one opened (" + std::to_string(freedAtThrow) +
              " freed, " + std::to_string(openedAtThrow) + " opened)");
    check(stalled == 1, what + ": exactly the switch's question was refused (" + std::to_string(stalled) + " refused)");
}

// GOAL: S15.940 -- memory running out at a device decoder's picture or slice-buffer allocation is never swallowed:
//       host memory on any decoder, and GPU memory on the constructor's, raise ResourceExhausted once ffmpeg returns,
//       with every picture shown before it right; GPU memory on a switch's decoder sends only the rest of that pass
//       to software. No Vulkan validation message follows (the entry's own oracle). False otherwise.
// WHY-THIS-SHAPE: the slice buffer is armed as the first allocation after the decoder's first picture left
//       avcodec_default_get_buffer2, and each case asserts where its injection landed (inside a picture or not), so
//       a "bitstream" case cannot silently test a picture. Arming "the Nth vkAllocateMemory" instead would land
//       wherever setup happened to end. ffmpeg's own host mallocs are not reachable from here (G2k).
// CONTROL: control_batch.sh 2026-10-23, RTX 3080 Ti, each alone, restored, damaged_device_no_inline,
//       renderer/video/video_decoder.cpp: failDecode() sending GPU memory out on any decoder to software -> "device
//       memory out at the constructor's first picture raises ..." (and its slice buffer); outOfDeviceMemory() true
//       for any failed allocation -> "host memory out at the switch's first picture raises ..."; both post-call
//       `failedAllocation()` checks deleted -> "host memory out at the constructor's slice buffer raises ..." and
//       "24 Vulkan validation warning(s)"; those deleted and allocatePicture() returning every error unclassified ->
//       "host memory out at the constructor's first picture raises ...".
// COVERAGE-ASSERT: each case must report its injection consumed at the site it names, or fails as the harness's.
void runShortageAtDecodeChecks(Device& device, const std::string& label, const std::string& file,
                               const std::vector<int>& ids) {
    struct Case {
        const char* name;
        VkResult allocation;
        int codec;
        bool bitstream;
        bool raises;
    };
    const std::string toSoftware = std::string(120, 'D') + std::string(180, 'S') + std::string(600, 'D');
    int graded = 0;
    for (const Case& k : {Case{"host memory out at the constructor's first picture", VK_ERROR_OUT_OF_HOST_MEMORY, 1,
                               false, true},
                          Case{"device memory out at the constructor's first picture", VK_ERROR_OUT_OF_DEVICE_MEMORY, 1,
                               false, true},
                          Case{"host memory out at the switch's first picture", VK_ERROR_OUT_OF_HOST_MEMORY, 2, false,
                               true},
                          Case{"host memory out at the constructor's slice buffer", VK_ERROR_OUT_OF_HOST_MEMORY, 1, true,
                               true},
                          Case{"device memory out at the constructor's slice buffer", VK_ERROR_OUT_OF_DEVICE_MEMORY, 1,
                               true, true},
                          Case{"host memory out at the switch's slice buffer", VK_ERROR_OUT_OF_HOST_MEMORY, 2, true, true},
                          Case{"device memory out at the switch's slice buffer", VK_ERROR_OUT_OF_DEVICE_MEMORY, 2, true,
                               false}}) {
        const CountedRun c = runCountingCodecs(device, file, 900, k.allocation, false, !k.bitstream, k.bitstream, k.codec);
        const bool inPicture = g_injectedInPicture.load();
        std::printf("%s: %s: injected %s (%s a picture, %llu bytes), raised ResourceExhausted %s, threw '%s', %zu shown; "
                    "decoders %s\n",
                    label.c_str(), k.name, c.injected ? "yes" : "no", inPicture ? "inside" : "outside",
                    static_cast<unsigned long long>(g_injectedSize.load()), c.run.exhausted ? "yes" : "no",
                    c.run.threw.c_str(), c.run.shown.size(), c.kinds.c_str());
        if (!c.injected || inPicture == k.bitstream) {
            std::fprintf(stderr, "FAIL: %s: %s: the injection %s\n", label.c_str(), k.name,
                         c.injected ? "landed at the wrong site" : "was never reached");
            fail("a decode-time shortage injection did not land where its case says, which is not a verdict on the decoder");
            continue;
        }
        ++graded;
        if (k.raises) {
            const bool shownRight = c.run.shown.size() <= ids.size() &&
                                    std::equal(c.run.shown.begin(), c.run.shown.end(), ids.begin());
            check(c.run.exhausted && shownRight,
                  label + ": " + k.name + " raises ResourceExhausted, every picture before it right (threw '" +
                      c.run.threw + "', " + std::to_string(c.run.shown.size()) + " shown, " +
                      (shownRight ? "right" : "WRONG") + ")");
        } else {
            check(c.run.threw.empty() && firstWrongPass(c.run, ids) < 0 && c.kinds == toSoftware,
                  label + ": " + k.name + ": software for the rest of pass 1 only, every picture right (threw '" +
                      c.run.threw + "', first wrong pass " + std::to_string(firstWrongPass(c.run, ids) + 1) +
                      "): " + c.kinds);
        }
    }
    check(graded == 7, label + ": every decode-time shortage case was graded (" + std::to_string(graded) + " of 7)");
}

// GOAL: S15.930 -- a rewind proven to land on packet 1 leaves the stream's own extradata in effect, whatever the
//       pass before it adopted: on the 4-then-2 length-change FLV, passes 2 and 3 show every picture right both
//       when packets carry neither byte position nor decode time (rewind()'s seek branch, which the extradata map
//       cannot place) and when every seek fails (its reopen branch). False otherwise.
// WHY-THIS-SHAPE: on an unaltered file the landing itself already restores the stream's extradata, so the reset is
//       invisible there (T13a's NUT attempt went green with it deleted); these are the two conditions under which
//       G2k showed it is the one that counts. Positions and decode times are cleared in av_read_frame, and seeks
//       refused in av_seek_frame, for the whole run.
// CONTROL: control_batch.sh 2026-10-23, RTX 3080 Ti, each alone, restored, damaged_device_no_inline,
//       renderer/video/video_decoder.cpp rewind(): the seek branch's `m_extradata.reset()` deleted -> "rewind with
//       packets carrying no position or decode time: ... first wrong pass 2"; the reopen branch's deleted -> "rewind
//       with every seek refused: ... first wrong pass 2".
// COVERAGE-ASSERT: the stripped run must have cleared every pass's packets and reopened nothing (so the seek branch
//       ran); the refused run must have refused seeks and reopened the container once per rewind.
void runRewindResetChecks(Device& device, const std::string& label) {
    if (!haveAsset("counter_ps.h264", label)) return;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("wpx_rewind_reset_" + std::to_string(getpid()) + ".flv");
    struct Remove {
        std::filesystem::path path;
        ~Remove() { std::filesystem::remove(path); }
    } remove{path};
    const std::string file = wallpaperx::testing::buildLengthChangeFlv(asset("counter_ps.h264"), path, 4, 2);
    std::string why;
    const std::optional<CounterReference> reference =
        file.empty() ? std::nullopt : wallpaperx::testing::decodeCounterReference(file, why);
    if (!reference || reference->ids.size() != 300) {
        std::fprintf(stderr, "FAIL: %s: rewind reset: the harness could not build or read the fixture (%s)\n",
                     label.c_str(), why.c_str());
        fail("the rewind fixture could not be built, which is not a verdict on the decoder");
        return;
    }
    const std::vector<int>& ids = reference->ids;
    int graded = 0;
    for (const bool stripTimes : {true, false}) {
        const std::string what = label + ": rewind " +
                                 (stripTimes ? "with packets carrying no position or decode time" : "with every seek refused");
        struct Clear {
            ~Clear() {
                g_tallying = false;
                g_loopSeek = LoopSeek::None;
                g_clearPositions = false;
                g_clearDts = false;
            }
        } clear;
        g_tally = CallTally{};
        g_dtsCleared = 0;
        g_loopSeek = stripTimes ? LoopSeek::None : LoopSeek::FailAll;
        g_clearPositions = stripTimes;
        g_clearDts = stripTimes;
        g_tallying = true;
        const DamagedRun run = runDamaged(device, file, 900);
        g_tallying = false;
        int refused = 0;
        for (const SeekCall& seek : g_tally.seeks) refused += seek.result < 0 ? 1 : 0;
        const int wrong = firstWrongPass(run, ids);
        std::printf("%s: %zu shown, threw '%s', first wrong pass %d; %zu seek(s), %d refused, %d container open(s), "
                    "%d position(s) and %d decode time(s) cleared\n",
                    what.c_str(), run.shown.size(), run.threw.c_str(), wrong + 1, g_tally.seeks.size(), refused,
                    g_tally.containerOpens, g_tally.positionsCleared, g_dtsCleared);
        const bool reached = stripTimes ? g_tally.positionsCleared >= 600 && g_dtsCleared >= 600 &&
                                              g_tally.containerOpens == 1 && !g_tally.seeks.empty()
                                        : refused >= 2 && g_tally.containerOpens >= 3;
        if (!reached) {
            std::fprintf(stderr, "FAIL: %s: the condition was not set up as the case needs (see the counts above)\n",
                         what.c_str());
            fail("a rewind case did not reach the rewind branch it is written for, which is not a verdict on the decoder");
            continue;
        }
        ++graded;
        check(run.threw.empty() && run.shown.size() == 900 && wrong < 0,
              what + ": every picture of passes 1 to 3 right (threw '" + run.threw + "', " +
                  std::to_string(run.shown.size()) + " shown, first wrong pass " + std::to_string(wrong + 1) + ")");
    }
    check(graded == 2, label + ": both rewind cases were graded (" + std::to_string(graded) + ")");
}

// GOAL: S15.935 -- a fresh device decoder that fails to open at a switch (device memory out at its first
//       allocation, or its images' context failing with AVERROR_EXTERNAL) sends only the rest of that pass to
//       software: pass 1 is the device's up to 120 and software after, passes 2 and 3 all the device's, every
//       picture right. False otherwise.
// WHY-THIS-SHAPE: injected in the probe's vkAllocateMemory / av_hwframe_ctx_init interposers, armed when the
//       second device codec opens -- the switch's -- so the constructor's decoder, whose failures keep their old
//       meaning, is never hit. A per-picture D/S pattern, not a count, so a pass 2 late to the device is red.
// CONTROL: control_batch.sh 2026-10-22, RTX 3080 Ti, reverted, damaged_device_no_inline,
//       renderer/video/video_decoder.cpp refuseAfterFailure() not setting NextPass -> "device memory out at the
//       switch: software for the rest of pass 1 only ...: D x120 then S x780", and the same for "the switch's
//       images context failing". Both injections land in ffmpeg's session/frames setup (choose()'s path); the
//       picture case below is the one that reaches outOfDeviceMemory() through allocatePicture(). Also red
//       (2026-10-23), on both lines and the picture and slice-buffer cases: restart() not re-arming the hand-back.
// COVERAGE-ASSERT: each run must report its injection consumed, or it fails as the harness's.
void runSwitchFailureChecks(Device& device, const std::string& label) {
    if (!haveAsset("counter_ps.h264", label)) return;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("wpx_switch_fail_" + std::to_string(getpid()) + ".flv");
    struct Remove {
        std::filesystem::path path;
        ~Remove() { std::filesystem::remove(path); }
    } remove{path};
    const std::string file = wallpaperx::testing::buildLengthChangeFlv(asset("counter_ps.h264"), path, 4, 2);
    std::string why;
    const std::optional<CounterReference> reference =
        file.empty() ? std::nullopt : wallpaperx::testing::decodeCounterReference(file, why);
    if (!reference || reference->ids.size() != 300) {
        std::fprintf(stderr, "FAIL: %s: switch failure: the harness could not build or read the fixture (%s)\n",
                     label.c_str(), why.c_str());
        fail("the switch-failure fixture could not be built, which is not a verdict on the decoder");
        return;
    }
    const std::vector<int>& ids = reference->ids;
    const std::string expected = std::string(120, 'D') + std::string(180, 'S') + std::string(600, 'D');
    struct Case {
        const char* name;
        VkResult allocation;
        bool frames;
        bool picture;
    };
    // GOAL: S15.935 (c) -- device memory running out at an ordinary picture allocation of the switch's decoder, after
    //       its setup succeeded, is classified as the device's (failDecode() through outOfDeviceMemory()): the
    //       rest of pass 1 in software, passes 2 and 3 on the device, nothing raised. False if it is read as host
    //       memory or as a decode error.
    // WHY-THIS-SHAPE: the first two cases fail in ffmpeg's setup, choose()'s path, which never consults the
    //       classification; failing "the Nth allocation" by count would land wherever setup happens to end. So the
    //       injection is armed in avcodec_default_get_buffer2 -- allocatePicture()'s own call -- and re-armed when a
    //       pooled picture allocated nothing, so it can only fire inside a picture allocation.
    // CONTROL: control_batch.sh 2026-10-23, RTX 3080 Ti, each alone, restored, damaged_device_no_inline,
    //       renderer/video/video_decoder.cpp: allocatePicture() handing failDecode() EINVAL instead of its error, and
    //       allocatePicture() returning every error unclassified with decode()'s two failedAllocation() tests off ->
    //       both "device memory out at a picture of the switch's decoder: software for the rest of pass 1 only ...".
    //       The unclassified return alone stays green: decode()'s failedAllocation() test after the call catches it.
    for (const Case& k : {Case{"device memory out at the switch", VK_ERROR_OUT_OF_DEVICE_MEMORY, false, false},
                          Case{"the switch's images context failing", VK_SUCCESS, true, false},
                          Case{"device memory out at a picture of the switch's decoder", VK_ERROR_OUT_OF_DEVICE_MEMORY,
                               false, true}}) {
        const CountedRun c = runCountingCodecs(device, file, 900, k.allocation, k.frames, k.picture);
        std::printf("%s: %s: injected %s; decoders %s\n", label.c_str(), k.name, c.injected ? "yes" : "no",
                    c.kinds.c_str());
        if (!c.injected) {
            std::fprintf(stderr, "FAIL: %s: %s: the injection was never reached\n", label.c_str(), k.name);
            fail("the switch-failure injection was not reached, which is not a verdict on the decoder");
            continue;
        }
        check(c.run.threw.empty() && firstWrongPass(c.run, ids) < 0,
              label + ": " + k.name + ": three passes shown right without raising (threw '" + c.run.threw +
                  "', first wrong pass " + std::to_string(firstWrongPass(c.run, ids) + 1) + ")");
        check(c.kinds == expected, label + ": " + k.name + ": software for the rest of pass 1 only, passes 2 and 3 "
                                           "on the device: " + c.kinds);
    }
    // GOAL: S15.935 (a) -- host memory running out in the switch's decoder setup is raised as ResourceExhausted, not
    //       sent to software for the pass. False otherwise.
    // WHY-THIS-SHAPE: the same injection site as the device-memory case, so the two differ only in the VkResult
    //       and the check grades the classification, not where the failure lands.
    // CONTROL: control_batch.sh 2026-10-22, RTX 3080 Ti, each alone, reverted, damaged_device_no_inline,
    //       renderer/video/video_decoder.cpp: choose()'s `raisesShortage(failedAllocation())` line deleted, and
    //       outOfDeviceMemory() true for any failed allocation -> both "host memory out at the switch raises
    //       ResourceExhausted (threw '' ...)".
    const CountedRun host = runCountingCodecs(device, file, 900, VK_ERROR_OUT_OF_HOST_MEMORY);
    std::printf("%s: host memory out at the switch: injected %s, raised ResourceExhausted %s, threw '%s'; decoders %s\n",
                label.c_str(), host.injected ? "yes" : "no", host.run.exhausted ? "yes" : "no", host.run.threw.c_str(),
                host.kinds.c_str());
    if (!host.injected) {
        std::fprintf(stderr, "FAIL: %s: host memory out at the switch: the injection was never reached\n", label.c_str());
        fail("the host-memory injection was not reached, which is not a verdict on the decoder");
    } else {
        check(host.run.exhausted, label + ": host memory out at the switch raises ResourceExhausted (threw '" +
                                      host.run.threw + "'; decoders " + host.kinds + ")");
    }
    runShortageAtDecodeChecks(device, label, file, ids);
    runUnansweredSwitchChecks(device, label, file);
}

// GOAL: S15.955 -- the opening is reported as refused (softwareFromSetup(), the daemon's "all") exactly when no picture
//       of the stream reaches the device and nothing hands it back: the card refusing it (10-bit H.264, VP9), the first
//       decoder's hardware setup failing (av_hwframe_ctx_init, or ffmpeg's own setup after the device's yes), or a
//       refused second sequence header before the first picture (sps_late.ts). A failure on a switched decoder costs
//       that pass only and is not reported so. False if any row below reads the other way.
// WHY-THIS-SHAPE: graded on VideoPipeline::softwareFromSetup(), the value the daemon's report is read from, rather than
//       over the bus: the setup failures can only be injected from a test binary's own interposers, and the daemon
//       binary has none. Each injected or "after the yes" row first proves its condition was reached (injection
//       consumed, a Vulkan frames context made, the first picture where the row needs it), else it fails as the
//       harness's; without that a row whose failure never fires would pass as "not refused" or as a plain refusal.
// CONTROL: control_batch.sh 2026-10-24 (T19b), RTX 3080 Ti, each alone, restored, video_pipeline_opening_report, in
//       renderer/video/video_decoder.cpp: refuseAfterFailure() setting m_path instead of calling refuse() -> "...
//       av_hwframe_ctx_init failing on the first decoder: reported as no picture reaching the card" and the same for
//       "ffmpeg's setup failing after the device's yes"; choose() settling Accepted at the device's yes -> the
//       sps_late.ts row and the "ffmpeg's setup failing" row. In renderer/video/video_decoder.h: refuse() never
//       settling Refused -> all five rows.
// COVERAGE-ASSERT: the rows graded are counted against the rows listed.
void runOpeningReportChecks(Device& device, const std::string& label) {
    if (device.info().videoDecodeOps == 0) {
        std::printf("%s: %s decodes no video, so no opening can be refused -- not answerable here\n", label.c_str(),
                    device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }
    if (!haveAsset("h264_10bit.mp4", label) || !haveAsset("sps_late.ts", label) || !haveAsset("h264_crop.mp4", label) ||
        !haveAsset("counter_ps.h264", label)) {
        return;
    }
    struct Row {
        const char* name;
        const char* file;
        bool framesFail;   // av_hwframe_ctx_init fails in the constructor's decoder
        bool sessionFail;  // ffmpeg's setup fails after the device's yes, in the constructor's decoder
        bool yesFirst;     // the device must have accepted the stream's first headers
    };
    std::vector<Row> rows = {
        {"10-bit H.264, refused by the card", "h264_10bit.mp4", false, false, false},
        {"a refused second sequence header before the first picture", "sps_late.ts", false, false, true},
        {"av_hwframe_ctx_init failing on the first decoder", "h264_crop.mp4", true, false, false},
        {"ffmpeg's setup failing after the device's yes, on the first decoder", "h264_crop.mp4", false, true, true},
    };
#if WALLPAPERX_HAVE_VP9_FIXTURE
    if (haveAsset("counter_vp9.webm", label)) rows.push_back({"VP9, refused by the card", "counter_vp9.webm", false, false, false});
#else
    std::printf("%s: VP9 not graded: this build's ffmpeg has no libvpx-vp9 encoder, so counter_vp9.webm is not "
                "generated\n", label.c_str());
#endif
    int graded = 0;
    for (const Row& row : rows) {
        g_vulkanFramesMade = 0;
        g_sessionsFailed = 0;
        g_failNextSession = row.sessionFail;
        const CountedRun c = runCountingCodecs(device, asset(row.file), 20, VK_SUCCESS, row.framesFail, false, false, 1);
        g_failNextSession = false;
        std::printf("%s: %s: frames contexts made %d, sessions failed %d, frames init injected %s; pictures %s; "
                    "refused opening %s; threw '%s'\n",
                    label.c_str(), row.name, g_vulkanFramesMade, g_sessionsFailed,
                    row.framesFail ? (c.injected ? "yes" : "NO") : "-", c.kinds.c_str(), c.run.fromSetup ? "yes" : "no",
                    c.run.threw.c_str());
        const bool reached = c.run.threw.empty() && !c.kinds.empty() && c.kinds[0] == 'S' &&
                             (!row.framesFail || c.injected) && (!row.sessionFail || g_sessionsFailed == 1) &&
                             (!row.yesFirst || g_vulkanFramesMade >= 1);
        if (!reached) {
            std::fprintf(stderr, "FAIL: %s: %s: the case was not set up as it needs (see the counts above)\n",
                         label.c_str(), row.name);
            fail("an opening-report case did not reach its condition, which is not a verdict on the decoder");
            continue;
        }
        ++graded;
        check(c.run.fromSetup, label + ": " + row.name + ": reported as no picture reaching the card (\"all\")");
    }
    // COVERAGE-ASSERT: every row above was graded, not skipped as unreached.
    check(graded == static_cast<int>(rows.size()), label + ": every opening row was graded (" +
                                                      std::to_string(graded) + " of " + std::to_string(rows.size()) + ")");

    // GOAL: S15.955, the other half -- the switch's fresh device decoder failing its setup after pictures came from
    //       the device sends that pass to software and is NOT reported as "all".
    // CONTROL: control_batch.sh 2026-10-24 (T19b), restored: refuse() (renderer/video/video_decoder.h) setting
    //       m_opening = Refused unconditionally -> "...: a failure on a switched decoder is not reported as "all"".
    //       Deleting only refuse()'s `!m_switched` stays green: here the first picture has already settled the
    //       opening, and no fixture switches decoders before the first picture, so that guard alone is ungraded.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("wpx_opening_" + std::to_string(getpid()) + ".flv");
    struct Remove {
        std::filesystem::path path;
        ~Remove() { std::filesystem::remove(path); }
    } remove{path};
    const std::string file = wallpaperx::testing::buildLengthChangeFlv(asset("counter_ps.h264"), path, 4, 2);
    if (file.empty()) {
        fail("the switch fixture could not be built, which is not a verdict on the decoder");
        return;
    }
    const CountedRun s = runCountingCodecs(device, file, 300, VK_SUCCESS, true);
    std::printf("%s: switch's images context failing: injected %s; first picture %c; refused opening %s\n",
                label.c_str(), s.injected ? "yes" : "no", s.kinds.empty() ? '-' : s.kinds[0],
                s.run.fromSetup ? "yes" : "no");
    if (!s.injected || s.kinds.empty() || s.kinds[0] != 'D' || s.kinds.find('S') == std::string::npos) {
        fail("the switch-failure case did not reach a failing switched decoder, which is not a verdict on the decoder");
        return;
    }
    check(!s.run.fromSetup, label + ": a failure on a switched decoder is not reported as \"all\"");
}

// GOAL: S15.173 -- a position past the end of the file -- the old position of a file since replaced in place
//       by a shorter one -- starts at the file's first picture, as a new pass, on the device and in software.
// WHY-THIS-SHAPE: picture 300's position in counter_long.mp4 applied to its first 100 pictures, encoded alike,
//       so every packet lies before the position and the start drops all of them.
// CONTROL: via control_batch.sh, re-run 2026-10-01, reverted: in renderer/video/video_decoder.cpp next(), the
//       `if (m_startedAfter && m_returned.count == 0) { restart(); continue; }` clause deleted -> on
//       start-at-device and start-at-host "counter_long_cut.mp4 opened at picture 300 of counter_long.mp4:
//       showed  (first marked no) where 0 1 2 3 4 was due".
//       Re-run 2026-10-19 by T5b, queues hidden: held.
// COVERAGE-ASSERT: the cut is shorter than the position, both counted from the independent decodes.
void runPastEndChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;
    constexpr size_t kAt = 300;
    const std::optional<CounterReference> full = counterReference(label, "counter_long.mp4");
    const std::optional<CounterReference> cut = counterReference(label, "counter_long_cut.mp4");
    if (!full || !cut) return;
    if (full->ids.size() <= kAt || cut->ids.size() >= kAt || cut->ids.size() < kFollowing) {
        std::fprintf(stderr, "FAIL: %s: counter_long.mp4 has %zu pictures and its cut %zu\n", label.c_str(),
                     full->ids.size(), cut->ids.size());
        fail("the cut fixture is not shorter than the position this check applies to it");
        return;
    }
    const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, "counter_long.mp4", kAt, full->ids[kAt]);
    if (!position) return;
    const std::string what = "counter_long_cut.mp4 opened at picture " + std::to_string(kAt) + " of counter_long.mp4";
    const std::optional<StartRun> run = runLoggedStart(device, label, what, asset("counter_long_cut.mp4"), *position);
    if (!run) return;
    const std::vector<int> due(cut->ids.begin(), cut->ids.begin() + static_cast<long>(kFollowing));
    const bool passMark = !run->startsPass.empty() && run->startsPass.front();
    if (run->shown != due || !passMark) {
        std::fprintf(stderr, "FAIL: %s: %s: showed %s (first marked %s) where %s was due\n", label.c_str(), what.c_str(),
                     joinIds(run->shown).c_str(), passMark ? "yes" : "no", joinIds(due).c_str());
        fail("a position past the end of the file starts at its first picture, as a new pass");
    }
    std::printf("%s: %s: showed %s, %s\n", label.c_str(), what.c_str(), joinIds(run->shown).c_str(),
                describeSeeks(run->seeks).c_str());
}

// GOAL: S15.174 -- a software start whose seeks land past its picture gives up once a retry would aim further
//       back than the catch-up budget, and shows the file's first picture as a new pass having decoded a few
//       pictures, not every one up to the position; a retry seek that fails with the position past the budget
//       does the same. Within the budget the start shows its own picture.
// WHY-THIS-SHAPE: MPEG-2, which no device here decodes, in a transport stream with keyframes at 0, 600 and
//       1200: its seeks land on the next keyframe after their target, so a start at 1150 lands on 1200 until
//       a retry aims below 600 -- 32 s back, past the budget -- and would then decode 600 to 1150. At 550 a
//       landing at or before the picture is never found by seeking, so its case fails the retry seek instead,
//       which reaches the other arm. Pictures decoded are counted at the decoder over the run.
// CONTROL: via control_batch.sh, re-run 2026-10-01, each alone, reverted, renderer/video/video_decoder.cpp
//       seekToResume(): `if (starting && back != 0 && !withinCatchUp(back))`'s arm deleted -> "opened at
//       picture 1150: showed 600 601 602 603 604 (first marked no) where 0 1 2 3 4 was due; seeks to 3578999,
//       ..., 698999, 11 picture(s) decoded" -- the retries go on until one lands at or before the picture,
//       on keyframe 600, which recordLanding() then shows as past the budget; the `else if (starting &&
//       !withinCatchUp(...))` arm deleted -> "opened at picture 550, its retry seek failing: showed 550 551
//       552 553 554 (first marked no) ... 556 picture(s) decoded". The first arm alone deleted leaves a start
//       at 550 green, measured again: its retry target falls before the stream's start and the second arm
//       takes it.
// COVERAGE-ASSERT: the first seek of each past-budget start landed past the picture (a retry followed it),
//       and the failing case's second seek failed.
void runSoftwareBudgetChecks(Device& device, const std::string& label) {
    if (!videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_onekey_m2.ts";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    const std::vector<int>& ids = reference->ids;
    struct Case {
        size_t index;
        bool failRetry;
        bool exact;
    };
    int graded = 0;
    for (const Case& c : {Case{200, false, true}, Case{1150, false, false}, Case{550, true, false}}) {
        if (ids.size() < c.index + kFollowing) {
            fail("the MPEG-2 fixture is too short for the starts this check makes");
            return;
        }
        const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, file, c.index, ids[c.index]);
        if (!position) continue;
        const std::string what = std::string(file) + " opened at picture " + std::to_string(c.index) +
                                 (c.failRetry ? ", its retry seek failing" : "");
        const std::optional<StartRun> run =
            runLoggedStart(device, label, what, asset(file), *position,
                           c.failRetry ? std::function<void()>([] { g_seekFailure = SeekFailure::Second; })
                                       : std::function<void()>());
        if (!run) continue;
        ++graded;
        const size_t first = c.exact ? c.index : 0;
        const std::vector<int> due(ids.begin() + static_cast<long>(first),
                                   ids.begin() + static_cast<long>(first + kFollowing));
        const bool passMark = !run->startsPass.empty() && run->startsPass.front();
        const int decoded = run->hostPictures + run->devicePictures;
        std::printf("%s: %s: showed %s, %s, %d picture(s) decoded\n", label.c_str(), what.c_str(),
                    joinIds(run->shown).c_str(), describeSeeks(run->seeks).c_str(), decoded);
        if (run->shown != due || passMark == c.exact || (!c.exact && decoded >= static_cast<int>(kCatchUpBudget))) {
            std::fprintf(stderr, "FAIL: %s: %s: showed %s (first marked %s) where %s was due; %s, %d picture(s) decoded\n",
                         label.c_str(), what.c_str(), joinIds(run->shown).c_str(), passMark ? "yes" : "no",
                         joinIds(due).c_str(), describeSeeks(run->seeks).c_str(), decoded);
            fail(c.exact ? "a software start within the budget shows its own picture"
                         : "a software start whose retries pass the budget shows the first picture, having decoded a few");
        }
        const bool exercised = c.exact || (c.failRetry ? run->seeks.size() >= 2 && run->seeks[1].result < 0
                                                       : run->seeks.size() >= 2);
        if (!exercised) {
            std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(), describeSeeks(run->seeks).c_str());
            fail("the start's first seek did not land past its picture, so no retry was at stake");
        }
    }
    if (!g_notVerified) check(graded == 3, "every software start past the budget this check is written for was graded");
}

// GOAL: S15.181 -- a software start whose first seek lands on a keyframe at or before its picture but past
//       the catch-up budget from it shows that keyframe's picture first, not marked as a pass, having decoded
//       a few pictures rather than every one up to the position; a keyframe at the file's start makes it the
//       first picture as a new pass instead; within the budget the start shows its own picture.
// WHY-THIS-SHAPE: MPEG-2 (decoded in software on every device) in Matroska, whose index lands each seek on
//       the keyframe at or before its target, with keyframes at 0, 600, 1200 and 1800: 1190 and 1790 land 590
//       past keyframes 600 and 1200, 590 past keyframe 0, and 650 lands 50 past keyframe 600. Pictures decoded
//       are counted at the decoder over the run.
// CONTROL: via control_batch.sh, 2026-10-01, each alone, reverted, renderer/video/video_decoder.cpp
//       recordLanding(): its new arm's `!withinCatchUp(...)` made `false` -> "counter_sparse_m2.mkv opened at
//       picture 1190: showed 1190 1191 1192 1193 1194 (first marked no) where 600 601 602 603 604 was due; ...
//       595 picture(s) decoded", and 1790 and 590 alike; that arm's `at <= streamStart()` made `false` -> "opened at picture 590: showed
//       0 1 2 3 4 (first marked no) where 0 1 2 3 4 was due".
// COVERAGE-ASSERT: the keyframes are exactly pictures 0, 600, 1200 and 1800, read from the packets, each
//       start lies on the side of the budget its case needs, and each made one seek.
void runSoftwareLandingChecks(Device& device, const std::string& label) {
    if (!videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_sparse_m2.mkv";
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    const std::vector<int>& ids = reference->ids;
    std::string why;
    const std::optional<GopLayout> layout = readGopLayout(asset(file), why, /*keyFlagsOnly=*/true);
    if (!layout) {
        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), file, why.c_str());
        fail("the harness could not read the fixture's packet layout, which is not a verdict on the decoder");
        return;
    }
    std::vector<size_t> keys;
    for (const GopLayout::Packet& packet : layout->packets) {
        if (!packet.key) continue;
        if (const std::optional<size_t> index = layout->pictureAt(packet.timestamp)) keys.push_back(*index);
    }
    const bool shape = keys == std::vector<size_t>{0, 600, 1200, 1800} && ids.size() == layout->pictures.size() &&
                       ids.size() >= 1790 + kFollowing && 590 > kCatchUpBudget && 650 - 600 <= kCatchUpBudget;
    if (!shape) {
        std::string list;
        for (const size_t k : keys) list += (list.empty() ? "" : ", ") + std::to_string(k);
        std::fprintf(stderr, "FAIL: %s: %s: %zu pictures decoded, %zu packets; keyframes at pictures %s; budget %zu\n",
                     label.c_str(), file, ids.size(), layout->pictures.size(), list.c_str(), kCatchUpBudget);
        fail("the sparse-keyframe MPEG-2 fixture lost the structure the software landing cases need");
        return;
    }
    struct Case {
        size_t index;
        size_t shows;
        bool passMark;
    };
    int graded = 0;
    for (const Case& c : {Case{650, 650, false}, Case{1190, 600, false}, Case{1790, 1200, false}, Case{590, 0, true}}) {
        const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, file, c.index, ids[c.index]);
        if (!position) continue;
        const std::string what = std::string(file) + " opened at picture " + std::to_string(c.index);
        const std::optional<StartRun> run = runLoggedStart(device, label, what, asset(file), *position);
        if (!run) continue;
        ++graded;
        const std::vector<int> due(ids.begin() + static_cast<long>(c.shows),
                                   ids.begin() + static_cast<long>(c.shows + kFollowing));
        const bool passMark = !run->startsPass.empty() && run->startsPass.front();
        const int decoded = run->hostPictures + run->devicePictures;
        const bool exact = c.shows == c.index;
        std::printf("%s: %s: showed %s, %s, %d picture(s) decoded\n", label.c_str(), what.c_str(),
                    joinIds(run->shown).c_str(), describeSeeks(run->seeks).c_str(), decoded);
        if (run->shown != due || run->startsPass.empty() || passMark != c.passMark ||
            (!exact && decoded >= static_cast<int>(kCatchUpBudget))) {
            std::fprintf(stderr, "FAIL: %s: %s: showed %s (first marked %s) where %s was due; %s, %d picture(s) decoded\n",
                         label.c_str(), what.c_str(), joinIds(run->shown).c_str(), passMark ? "yes" : "no",
                         joinIds(due).c_str(), describeSeeks(run->seeks).c_str(), decoded);
            fail(exact        ? "a software start within the budget of the keyframe it lands on shows its own picture"
                 : c.passMark ? "a software start landing on the file's first keyframe past the budget shows the first "
                                "picture as a new pass"
                              : "a software start landing past the budget from a keyframe part way in shows that keyframe "
                                "first, having decoded a few pictures");
        }
        if (run->seeks.size() != 1) {
            std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), what.c_str(), describeSeeks(run->seeks).c_str());
            fail("a software start in Matroska made other than one seek, so the landing was not what this case grades");
        }
    }
    if (!g_notVerified) check(graded == 4, "every software landing start this check is written for was graded");
}

// ------------------------------------------------------------------------- //
// Opening an open-GOP stream part way in
// ------------------------------------------------------------------------- //

// The luma samples of the picture timestamped `timestamp`, from a software decode fed the file's video
// packets from decode-order index `from` on and told to output pictures whose references are missing;
// nothing if that picture never came out.
std::optional<std::vector<uint8_t>> softwareLuma(const std::string& file, size_t from, int64_t timestamp,
                                                 std::string& why) {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    struct Free {
        AVFormatContext*& format;
        AVCodecContext*& codec;
        AVPacket*& packet;
        AVFrame*& frame;
        ~Free() {
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&codec);
            avformat_close_input(&format);
        }
    } guard{format, codec, packet, frame};
    if (packet == nullptr || frame == nullptr || avformat_open_input(&format, file.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(format, nullptr) < 0) {
        why = "cannot open " + file;
        return std::nullopt;
    }
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVCodec* decoder = stream >= 0 ? avcodec_find_decoder(format->streams[stream]->codecpar->codec_id) : nullptr;
    codec = decoder != nullptr ? avcodec_alloc_context3(decoder) : nullptr;
    if (codec == nullptr || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0) {
        why = "cannot make a decoder for " + file;
        return std::nullopt;
    }
    codec->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    codec->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    if (avcodec_open2(codec, decoder, nullptr) < 0) {
        why = "cannot open a decoder for " + file;
        return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> luma;
    auto take = [&] {
        while (avcodec_receive_frame(codec, frame) == 0) {
            if (frame->best_effort_timestamp == timestamp && frame->format == AV_PIX_FMT_YUV420P) {
                std::vector<uint8_t> samples;
                for (int y = 0; y < frame->height; ++y) {
                    const uint8_t* row = frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0];
                    samples.insert(samples.end(), row, row + frame->width);
                }
                luma = std::move(samples);
            }
            av_frame_unref(frame);
        }
    };
    size_t index = 0;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream && index++ >= from) {
            avcodec_send_packet(codec, packet);
            take();
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(codec, nullptr);
    take();
    return luma;
}

// A seek after which decoding started at an open keyframe: the first packet sent after it is one, and
// every one of its leading pictures is sent after it, before any later seek. With `afterSkip`, only a
// seek that landed on a non-keyframe counts, so the decoder's skip is what stopped there. The seek's
// index and the keyframe's packet index.
std::optional<std::pair<size_t, size_t>> leadingPicturesSent(const GopLayout& layout, const std::vector<SeekCall>& seeks,
                                                             bool afterSkip) {
    for (size_t s = 0; s < seeks.size(); ++s) {
        if (afterSkip && seeks[s].landedOnKeyframe != false) continue;
        std::vector<int64_t> sent;
        for (const TrafficEntry& entry : g_sentPackets) {
            if (entry.afterSeeks == s + 1) sent.push_back(entry.timestamp);
        }
        if (sent.empty()) continue;
        for (size_t k = 0; k < layout.packets.size(); ++k) {
            if (layout.packets[k].timestamp != sent.front() || !layout.openKeyframe(k)) continue;
            const std::vector<size_t> leading = layout.leading(k);
            const bool all = !leading.empty() && std::all_of(leading.begin(), leading.end(), [&](size_t l) {
                return std::find(sent.begin(), sent.end(), layout.packets[l].timestamp) != sent.end();
            });
            if (all) return std::pair<size_t, size_t>{s, k};
        }
    }
    return std::nullopt;
}

void runOpenGopStartChecks(Device& device, const std::string& label, bool onDevice, const char* file) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return;
    std::string why;
    const std::optional<GopLayout> layout = readGopLayout(asset(file), why);
    if (!layout || layout->pictures.size() != reference->ids.size()) {
        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), file,
                     layout ? "its packets and its decoded pictures differ in number" : why.c_str());
        fail("the harness could not read the fixture's packet layout, which is not a verdict on the decoder");
        return;
    }
    const bool hevc = layout->codec == AV_CODEC_ID_HEVC;
    const VkVideoCodecOperationFlagsKHR op =
        hevc ? VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR : VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    if (onDevice && (device.info().videoDecodeOps & op) == 0) {
        std::printf("%s: %s cannot decode %s -- not answerable\n", label.c_str(), device.info().gpuName.c_str(),
                    hevc ? "HEVC" : "H.264");
        g_notVerified = true;
        return;
    }
    const std::vector<int>& ids = reference->ids;
    constexpr size_t kLeadingTarget = 94;

    // GOAL: S15.161 -- the fixture has the structure this check exists for: a keyframe-flagged packet that
    //       does not reset the decoder (H.264 non-IDR I, HEVC CRA), followed in decode order by pictures
    //       shown before it that reference pictures from before it; pictures 93 and 94 are two of them.
    // WHY-THIS-SHAPE: read from the packets and a decode, never trusted from the encoder options: libx264
    //       and libx265 decide the layout, and a fixture that lost it would leave every check below green
    //       for want of the case. For H.264 "references across" is shown by decoding from the keyframe on:
    //       picture 94 must come out different from the full decode. HEVC's decoder never outputs such a
    //       picture, so there the RASL NAL type -- the one the standard lets reference across -- is the proof.
    // CONTROL: test-side, by hand, 2026-09-30, on start-at-host-open-gop: both recipes in
    //       renderer/CMakeLists.txt rebuilt with open-gop=0 -> "pictures 93 and 94 are not leading pictures
    //       of a non-IDR of one keyframe", and "... RASL pictures of a CRA ..." for HEVC; the comparison made
    //       on picture 97, which follows the keyframe -> "picture 94 decoded from the keyframe on matches the
    //       full decode, so it references nothing before the keyframe" (the message names the constant).
    std::optional<size_t> key;
    for (size_t k = 0; k < layout->packets.size() && !key; ++k) {
        if (!layout->openKeyframe(k)) continue;
        const std::vector<size_t> leading = layout->leading(k);
        auto has = [&](size_t picture) {
            return std::any_of(leading.begin(), leading.end(), [&](size_t l) {
                return layout->packets[l].timestamp == layout->pictures[picture];
            });
        };
        const bool rasl = std::all_of(leading.begin(), leading.end(), [&](size_t l) {
            return layout->packets[l].nal == 8 || layout->packets[l].nal == 9;
        });
        if (has(kLeadingTarget - 1) && has(kLeadingTarget) && (!hevc || rasl)) key = k;
    }
    if (!key) {
        std::fprintf(stderr, "FAIL: %s: %s: pictures %zu and %zu are not leading %s of one keyframe\n", label.c_str(),
                     file, kLeadingTarget - 1, kLeadingTarget,
                     hevc ? "RASL pictures of a CRA" : "pictures of a non-IDR");
        fail("the open-GOP fixture lost its structure, so no leading picture is ever at stake");
        return;
    }
    const std::optional<size_t> keyPicture = layout->pictureAt(layout->packets[*key].timestamp);
    std::string across = hevc ? "its leading pictures are RASL" : "";
    if (!hevc) {
        const int64_t timestamp = layout->pictures[kLeadingTarget];
        const std::optional<std::vector<uint8_t>> full = softwareLuma(asset(file), 0, timestamp, why);
        const std::optional<std::vector<uint8_t>> cut = softwareLuma(asset(file), *key, timestamp, why);
        size_t differing = 0;
        if (full && cut && full->size() == cut->size()) {
            for (size_t i = 0; i < full->size(); ++i) differing += (*full)[i] != (*cut)[i] ? 1 : 0;
        }
        if (!full || !cut || full->size() != cut->size() || differing == 0) {
            std::fprintf(stderr, "FAIL: %s: %s: picture %zu decoded from the keyframe on %s\n", label.c_str(), file,
                         kLeadingTarget,
                         !full || !cut ? ("did not come out" + (why.empty() ? "" : ": " + why)).c_str()
                                       : "matches the full decode, so it references nothing before the keyframe");
            fail("the open-GOP fixture's leading pictures do not reference across the keyframe");
            return;
        }
        across = "picture " + std::to_string(kLeadingTarget) + " decoded from the keyframe on differs in " +
                 std::to_string(differing) + " of " + std::to_string(full->size()) + " luma samples";
    }
    std::printf("%s: %s: keyframe at picture %zu is %s, its leading pictures %zu and %zu shown before it; %s\n",
                label.c_str(), file, keyPicture.value_or(0), hevc ? "a CRA" : "a non-IDR I picture", kLeadingTarget - 1,
                kLeadingTarget, across.c_str());

    struct Point {
        size_t index;
        int shiftSeconds;
        const char* what;
    };
    int graded = 0;
    const Point points[] = {
        {kLeadingTarget, -1, "its first seek moved a second early"}, {110, 0, "its first seek unmoved"}};
    for (const Point& point : points) {
        if (point.index + kFollowing > ids.size()) {
            fail("a starting point is too near the end of its fixture to grade the pictures after it");
            continue;
        }
        const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, file, point.index, ids[point.index]);
        if (!position) continue;
        const std::string what = std::string(file) + " opened at picture " + std::to_string(point.index) + ", " + point.what;
        const int shift = point.shiftSeconds;
        const std::optional<StartRun> started =
            runLoggedStart(device, label, what, asset(file), *position,
                           shift != 0 ? std::function<void()>([shift] { g_seekShiftSeconds = shift; })
                                      : std::function<void()>());
        if (!started) continue;
        const StartRun& run = *started;
        ++graded;
        // GOAL: S15.162 -- opened at a picture of an open-GOP stream, the pipeline shows that picture first,
        //       then its successors: in software, when decoding after the seek starts at a keyframe whose
        //       leading pictures reference pictures that were never decoded; on the device, when it starts
        //       at the reset point further back.
        // WHY-THIS-SHAPE: picture 94 is the second leading picture of keyframe 96, so if ffmpeg output the
        //       leading pictures, 93 would settle a software landing and 94 would be shown decoded without its
        //       reference; at 93 itself the landing would read as late and be retried. ffmpeg 9.0.2 outputs
        //       none of them, so the retry from keyframe 48 shows 94 -- the grade holds either way, read from
        //       the pixels.
        // CONTROL: via control_batch.sh, 2026-09-30, reverted, renderer/video/video_decoder.cpp. startAt()'s
        //       `before` made the position itself -> on every device and host entry "showed 95 96 97 98 99
        //       where 94 ..." and "showed 111 ... where 110 ...". Host only: admit() taking a first picture
        //       past the resume point as landed -> both transport streams on start-at-host-open-gop, "opened
        //       at picture 94 ...: showed 97 98 99 100 101 where 94 95 96 97 98 was due". The device-pictures
        //       check, 2026-10-01: S15.142's break (a window with no reset point handed to software) -> on
        //       start-at-device-open-gop "0 device and 52 host picture(s)" at 94 and "0 device and 19" at 110.
        const std::vector<int> due(ids.begin() + static_cast<long>(point.index),
                                   ids.begin() + static_cast<long>(point.index + kFollowing));
        if (run.shown != due) {
            std::fprintf(stderr, "FAIL: %s: %s: showed %s where %s was due\n", label.c_str(), what.c_str(),
                         joinIds(run.shown).c_str(), joinIds(due).c_str());
            fail("a pipeline opened at a picture of an open-GOP stream shows it first, then its successors");
        }
        if (onDevice && (run.hostPictures != 0 || run.devicePictures == 0)) {
            std::fprintf(stderr, "FAIL: %s: %s: %d device and %d host picture(s)\n", label.c_str(), what.c_str(),
                         run.devicePictures, run.hostPictures);
            fail("the open-GOP start was not decoded on the hardware decoder, so its validation grade says nothing");
        }
        if (onDevice) {
            // GOAL: S15.161 (device) -- an open-GOP start on the device decodes from the nearest reset
            //       point before its picture, here the IDR at picture 0, never from the open keyframe a seek
            //       lands on, and then every packet after it in decode order.
            // WHY-THIS-SHAPE: read from the packets the decoder was sent; the pictures are right either way,
            //       and a start at the open keyframe shows itself only as validation messages, which this
            //       entry's grade counts but cannot attribute.
            // CONTROL: via control_batch.sh, 2026-10-23 (G3d), RTX 3080 Ti, reverted, renderer/video/video_decoder.cpp
            //       scanWindow() taking the container's keyframe flag for resetsDecoder() -> on all three
            //       device entries "the first packet sent to the decoder was packet 45 (picture 48, a non-IDR
            //       I picture), which does not reset the decoder" (a CRA for HEVC), beside "0 device and 53 host
            //       picture(s)": allocatePicture()'s missing-reference test now sends such a start to software,
            //       so the validation grade stays quiet. renderer/video/reset_point.cpp hevcUnit() also accepting
            //       a CRA -> the same line on start-at-device-open-gop-hevc.
            // COVERAGE-ASSERT: the keyframe a seek to the picture lands on must be an open keyframe, so a
            //       start from it would have been the open-GOP case; red with the point moved to picture 30
            //       (test-side, by hand, 2026-09-30): "the keyframe shown last before the picture is packet 0
            //       (picture 0, an IDR), not an open keyframe".
            const int64_t at = layout->pictures[point.index];
            const size_t from = layout->nearestReset(at);
            startedAt(label, what, *layout, from);
            const std::optional<size_t> landing = layout->lastKeyBefore(at);
            if (!layout->landsOnOpenKeyframe(at)) {
                std::fprintf(stderr, "FAIL: %s: %s: the keyframe shown last before the picture is %s, not an open "
                                     "keyframe\n",
                             label.c_str(), what.c_str(), landing ? layout->describe(*landing).c_str() : "none");
                fail("no open keyframe lies between the reset point and the picture, so the open-GOP case was not at stake");
            }
            std::printf("%s: %s: showed %s, %s; decoded from %s, past %s; %d device and %d host picture(s)\n",
                        label.c_str(), what.c_str(), joinIds(run.shown).c_str(), describeSeeks(run.seeks).c_str(),
                        layout->describe(from).c_str(), landing ? layout->describe(*landing).c_str() : "no keyframe",
                        run.devicePictures, run.hostPictures);
            continue;
        }
        // COVERAGE-ASSERT: in software, S15.161's case must have been reached: the first packet sent after a
        //       seek was an open keyframe, followed by every one of its leading pictures; on a transport stream
        //       that seek must have landed on a non-keyframe, so the decoder's skip is what stopped there.
        //       CONTROL: test-side, by hand, 2026-09-30: avcodec_send_packet's interposer dropping every
        //       packet shown before the last keyframe sent -> on start-at-host-open-gop, all four files,
        //       both points: "no seek was followed by an open keyframe and all its leading pictures".
        const bool transport = std::string(file).ends_with(".ts");
        const std::optional<std::pair<size_t, size_t>> reached = leadingPicturesSent(*layout, run.seeks, transport);
        std::string returned;
        size_t listed = 0;
        for (const TrafficEntry& picture : g_returnedPictures) {
            if (!reached || picture.afterSeeks != reached->first + 1 || listed++ >= 6) continue;
            const std::optional<size_t> index = layout->pictureAt(picture.timestamp);
            returned += (returned.empty() ? "" : " ") + (index ? std::to_string(*index) : std::string("?"));
        }
        if (!reached) {
            std::fprintf(stderr, "FAIL: %s: %s: %s, the first landing on %s; no seek was followed by an open "
                                 "keyframe and all its leading pictures\n",
                         label.c_str(), what.c_str(), describeSeeks(run.seeks).c_str(), describeLanding(run.seeks));
            fail("no leading picture of an open keyframe reached the decoder, so the case was not exercised");
        }
        std::printf("%s: %s: showed %s, %s; after seek %zu the decoder was sent the keyframe at picture %zu and "
                    "its leading pictures, and returned pictures %s; %d device and %d host picture(s)\n",
                    label.c_str(), what.c_str(), joinIds(run.shown).c_str(), describeSeeks(run.seeks).c_str(),
                    reached ? reached->first + 1 : 0,
                    reached ? layout->pictureAt(layout->packets[reached->second].timestamp).value_or(0) : 0,
                    returned.c_str(), run.devicePictures, run.hostPictures);
    }
    if (!g_notVerified) check(graded == 2, "both open-GOP starting points were graded");
}

// ------------------------------------------------------------------------- //
// A start on the device whose reset point lies far back
// ------------------------------------------------------------------------- //

// Packets the demuxer read before the first was sent to the decoder, in the last logged run: the search
// for a reset point, and nothing of the decode after it.
size_t searchReads() {
    return static_cast<size_t>(std::count_if(g_readPackets.begin(), g_readPackets.end(),
                                             [](const TrafficEntry& read) { return read.sentBefore == 0; }));
}

// What a start at `index` must show first: its own picture, the reset point's picture, or the file's first
// picture as a new pass.
enum class Shows { Picture, ResetPoint, FileStart };
struct ResetStart {
    size_t index;
    Shows shows;
};

std::optional<GopLayout> deviceLayout(Device& device, const std::string& label, const char* file,
                                      VkVideoCodecOperationFlagsKHR op, const char* codecName) {
    if (!hardwareDecoderPresent(device, label) || !haveAsset(file, label)) return std::nullopt;
    if ((device.info().videoDecodeOps & op) == 0) {
        std::printf("%s: %s cannot decode %s -- not answerable\n", label.c_str(), device.info().gpuName.c_str(), codecName);
        g_notVerified = true;
        return std::nullopt;
    }
    std::string why;
    std::optional<GopLayout> layout = readGopLayout(asset(file), why);
    if (!layout) {
        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), file, why.c_str());
        fail("the harness could not read the fixture's packet layout, which is not a verdict on the decoder");
    }
    return layout;
}

// Opens `file` on the device at each start and grades the pictures shown, the first one's pass mark, where
// the decode began and that it stayed on the device; `afterRun` sees each run's traffic logs. The number
// of starts graded.
int gradeResetStarts(Device& device, const std::string& label, const char* file, const GopLayout& layout,
                     std::initializer_list<ResetStart> starts,
                     const std::function<void(const std::string& what, size_t index)>& afterRun = {}) {
    const std::optional<CounterReference> reference = counterReference(label, file);
    if (!reference) return 0;
    const std::vector<int>& ids = reference->ids;
    if (ids.size() != layout.pictures.size()) {
        std::fprintf(stderr, "FAIL: %s: %s: %zu pictures decoded, %zu packets read\n", label.c_str(), file, ids.size(),
                     layout.pictures.size());
        fail("the harness could not match the fixture's packets to its pictures, which is not a verdict on the decoder");
        return 0;
    }
    int graded = 0;
    for (const ResetStart& start : starts) {
        const size_t from = layout.nearestReset(layout.pictures[start.index]);
        const size_t first = start.shows == Shows::Picture ? start.index : start.shows == Shows::ResetPoint ? layout.picture(from) : 0;
        if (first + kFollowing > ids.size()) {
            fail("a starting point is too near the end of its fixture to grade the pictures after it");
            continue;
        }
        const std::optional<wallpaperx::StreamPosition> position = positionAt(device, label, file, start.index, ids[start.index]);
        if (!position) continue;
        const std::string what = std::string(file) + " opened at picture " + std::to_string(start.index);
        const std::optional<StartRun> started = runLoggedStart(device, label, what, asset(file), *position);
        if (!started) continue;
        const StartRun& run = *started;
        ++graded;
        const std::vector<int> due(ids.begin() + static_cast<long>(first), ids.begin() + static_cast<long>(first + kFollowing));
        if (run.shown != due) {
            std::fprintf(stderr, "FAIL: %s: %s: showed %s where %s was due\n", label.c_str(), what.c_str(),
                         joinIds(run.shown).c_str(), joinIds(due).c_str());
            fail(start.shows == Shows::Picture      ? "a start on the device within the catch-up budget shows its own picture"
                 : start.shows == Shows::ResetPoint ? "a start past the catch-up budget from a reset point part way in "
                                                      "shows that reset point's picture first"
                                                    : "a start past the catch-up budget from the file's first picture shows "
                                                      "that picture first");
        }
        const bool passMark = !run.startsPass.empty() && run.startsPass.front();
        if (run.startsPass.empty() || passMark != (start.shows == Shows::FileStart)) {
            std::fprintf(stderr, "FAIL: %s: %s: its first picture is %smarked as starting a pass\n", label.c_str(),
                         what.c_str(), passMark ? "" : "not ");
            fail(start.shows == Shows::FileStart ? "a start that falls back to the file's first picture shows it as a new pass"
                                                 : "a start part way in does not mark its first picture as a pass");
        }
        startedAt(label, what, layout, from);
        if (run.hostPictures != 0 || run.devicePictures == 0) {
            std::fprintf(stderr, "FAIL: %s: %s: %d device and %d host picture(s)\n", label.c_str(), what.c_str(),
                         run.devicePictures, run.hostPictures);
            fail("a start on the device was not decoded on the hardware decoder");
        }
        if (afterRun) afterRun(what, start.index);
        std::printf("%s: %s: showed %s (first %smarked as a pass), %s; decoded from %s, %zu pictures before it; %d "
                    "device and %d host picture(s)\n",
                    label.c_str(), what.c_str(), joinIds(run.shown).c_str(), passMark ? "" : "not ",
                    describeSeeks(run.seeks).c_str(), layout.describe(from).c_str(), start.index - layout.picture(from),
                    run.devicePictures, run.hostPictures);
    }
    return graded;
}

void runBudgetChecks(Device& device, const std::string& label) {
    const VkVideoCodecOperationFlagsKHR h264 = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    int graded = 0;
    int offered = 0;

    // GOAL: S15.167 -- a start on the device whose only reset point before its picture is the file's first
    //       picture decodes forward from it when the picture is within the catch-up budget of 250 pictures,
    //       and past the budget shows the file's first picture, marked as starting a new pass.
    // WHY-THIS-SHAPE: an open-GOP file whose only IDR is picture 0, opened at 230 and at 280, either side of
    //       the budget; the pictures, the pass mark and the packets sent each tell a different wrong answer
    //       apart, and the budget is counted in pictures, so neither point is near the edge.
    // CONTROL: via control_batch.sh, 2026-10-01, each alone, reverted, renderer/video/video_decoder.cpp
    //       startAtResetPoint(): `withinBudget` made `true` -> "counter_opengop_long.mp4 opened at picture 280:
    //       showed 280 281 282 283 284 where 0 1 2 3 4 was due"; the past-budget branch's `rewind(); return;`
    //       deleted -> "opened at picture 280: its first picture is not marked as starting a pass".
    // COVERAGE-ASSERT: the file's only reset point is packet 0, the keyframe a seek to each picture lands on
    //       is an open keyframe, and 230 and 280 lie either side of the budget, their distances from that
    //       reset point read from the packets; red with the recipe's open-gop=1 removed (test-side, by hand,
    //       2026-09-30): "counter_opengop_long.mp4: 7 reset point(s) ...; the keyframes before pictures 230 and
    //       280 are not both open". The budget is the decoder's own: kCatchUpPictures set to 100000, via
    //       control_batch.sh 2026-10-01 -> this structure line red, "lost the structure the catch-up budget's
    //       file-start case needs".
    if (const std::optional<GopLayout> layout = deviceLayout(device, label, "counter_opengop_long.mp4", h264, "H.264")) {
        offered += 2;
        const std::vector<size_t> resets = layout->resets();
        const auto openLanding = [&](size_t index) { return layout->landsOnOpenKeyframe(layout->pictures[index]); };
        const size_t from = resets.empty() ? 0 : layout->picture(resets[0]);
        if (resets != std::vector<size_t>{0} || !openLanding(230) || !openLanding(280) || 230 - from >= kCatchUpBudget ||
            280 - from <= kCatchUpBudget) {
            std::fprintf(stderr, "FAIL: %s: counter_opengop_long.mp4: %zu reset point(s), the first %s; the keyframes "
                                 "before pictures 230 and 280 %s open\n",
                         label.c_str(), resets.size(), resets.empty() ? "none" : layout->describe(resets[0]).c_str(),
                         openLanding(230) && openLanding(280) ? "are" : "are not both");
            fail("the long open-GOP fixture lost the structure the catch-up budget's file-start case needs");
        } else {
            graded += gradeResetStarts(device, label, "counter_opengop_long.mp4", *layout,
                                       {ResetStart{230, Shows::Picture}, ResetStart{280, Shows::FileStart}});
        }
    }

    // GOAL: S15.168 -- past the budget from a reset point part way in, a start on the device shows that
    //       reset point's picture first, not marked as a new pass. S15.169 -- when one search window holds two
    //       reset points, the start decodes from the nearer one.
    // WHY-THIS-SHAPE: IDRs at pictures 120 and 132 with no keyframe between them, then open GOP: picture 402
    //       is 270 past IDR 132, and picture 245 lands its seeks on open keyframes 228 and 180 until the
    //       window aimed four seconds back, which lands on IDR 120 and reads on past IDR 132. The pictures
    //       at 245 are right from either IDR, so only the packets sent show which was used.
    // CONTROL: via control_batch.sh, 2026-09-30, each alone, reverted, renderer/video/video_decoder.cpp:
    //       S15.168, `av_sat_sub64(reset.time, 1)` made `resume` -> "counter_two_idr.mp4 opened at picture
    //       402: showed 402 403 404 405 406 where 132 133 134 135 136 was due"; S15.169, scanWindow() called
    //       with `stopAtFirst` true in every window -> "opened at picture 245: the first packet sent to the
    //       decoder was packet 120 (picture 120, an IDR), where the nearest reset point before the picture
    //       is packet 132 (picture 132, an IDR)", and the run-time assert below red with it. S15.177: that
    //       break with seekToPacket()'s first `av_packet_unref(m_packet);` also deleted, so the held reset
    //       point is sent twice -> the same lines and "packet 1 sent after the start is not packet 121 of the
    //       file". Re-run 2026-09-30.
    // COVERAGE-ASSERT: the layout -- three reset points, the last two with no keyframe between them, open
    //       keyframes before both pictures, 245 within and 402 past the budget from IDR 132 -- red with the
    //       recipe's second forced IDR removed (test-side, by hand, 2026-09-30): "reset points packet 0 (picture
    //       0, an IDR), packet 120 (picture 120, an IDR)". At run time, one stretch of reads between two seeks
    //       must read both IDRs before anything is decoded: the search saw both, so nearest against first was
    //       at stake. It reads the search's own traffic, so the stopAtFirst break turns it red too.
    if (const std::optional<GopLayout> layout = deviceLayout(device, label, "counter_two_idr.mp4", h264, "H.264")) {
        offered += 2;
        const std::vector<size_t> resets = layout->resets();
        const bool shape = resets.size() == 3 && resets[0] == 0 && [&] {
            for (size_t i = resets[1] + 1; i < resets[2]; ++i) {
                if (layout->packets[i].key) return false;
            }
            return true;
        }();
        const auto openLanding = [&](size_t index) { return layout->landsOnOpenKeyframe(layout->pictures[index]); };
        const size_t near = shape ? layout->picture(resets[2]) : 0;
        if (!shape || !openLanding(245) || !openLanding(402) || 245 - near > kCatchUpBudget || 402 - near <= kCatchUpBudget) {
            std::string list;
            for (const size_t r : resets) list += (list.empty() ? "" : ", ") + layout->describe(r);
            std::fprintf(stderr, "FAIL: %s: counter_two_idr.mp4: reset points %s; the keyframes before pictures 245 and "
                                 "402 %s open\n",
                         label.c_str(), list.c_str(), openLanding(245) && openLanding(402) ? "are" : "are not both");
            fail("the two-IDR fixture lost the structure the nearest-reset-point and part-way budget cases need");
        } else {
            const int64_t a = layout->packets[resets[1]].timestamp;
            const int64_t b = layout->packets[resets[2]].timestamp;
            graded += gradeResetStarts(
                device, label, "counter_two_idr.mp4", *layout,
                {ResetStart{245, Shows::Picture}, ResetStart{402, Shows::ResetPoint}},
                [&](const std::string& what, size_t index) {
                    if (index != 245) return;
                    bool both = false;
                    for (const TrafficEntry& read : g_readPackets) {
                        if (read.timestamp != a || read.sentBefore != 0) continue;
                        both = both || std::any_of(g_readPackets.begin(), g_readPackets.end(), [&](const TrafficEntry& other) {
                                   return other.timestamp == b && other.sentBefore == 0 && other.afterSeeks == read.afterSeeks;
                               });
                    }
                    if (!both) {
                        std::fprintf(stderr, "FAIL: %s: %s: no stretch of reads between two seeks read both IDRs before "
                                             "decoding began\n",
                                     label.c_str(), what.c_str());
                        fail("no search window held both reset points, so nearest against first was not at stake");
                    }
                });
        }
    }

    // GOAL: S15.171 -- the search for a reset point is bounded by the budget: a start on the device past the
    //       budget, on an open-GOP file whose only IDR is its first picture, reads about as many packets far into
    //       the file as nearer in, and never back to the file's start; when the search's second seek fails, it
    //       reads no further before starting at the first picture.
    // WHY-THIS-SHAPE: the packets the demuxer read before the first was sent, at pictures 700 and 1150 of 1200.
    //       The two are compared, not held to a number: how much one window reads is the container's, while
    //       growing with the position is the defect. Each must also stay below its own position, which a
    //       search read back to the start cannot. The pictures are graded as a start at the first picture.
    // CONTROL: via control_batch.sh, each alone, reverted, renderer/video/video_decoder.cpp
    //       startAtResetPoint(). 2026-09-30: `|| !withinCatchUp(back)` deleted from the no-reset-point branch
    //       -> "searching read 711 packets at picture 700 and 1162 at 1150" (520 and 490 unbroken). 2026-10-01:
    //       the failed-seek branch's `if (!withinCatchUp(resume - streamStart()))` made `if (false)` -> "its
    //       second seek failing: seeks to 588799, 573439 (failed), -1024, -1024; ... 1156 packets read
    //       searching" (5 unbroken).
    // COVERAGE-ASSERT: both positions lie more than twice the budget past the file's only reset point, read
    //       from the packets, so a bounded search and one read back to the start cannot read alike; and the
    //       failure case must show its second seek failed.
    if (const std::optional<GopLayout> layout = deviceLayout(device, label, "counter_opengop_xlong.mp4", h264, "H.264")) {
        const char* const file = "counter_opengop_xlong.mp4";
        constexpr size_t kNear = 700;
        constexpr size_t kFar = 1150;
        offered += 3;
        const std::vector<size_t> resets = layout->resets();
        if (resets != std::vector<size_t>{0} || layout->pictures.size() < kFar + kFollowing ||
            kNear - layout->picture(0) <= 2 * kCatchUpBudget) {
            std::fprintf(stderr, "FAIL: %s: %s: %zu pictures, %zu reset point(s), the first %s\n", label.c_str(), file,
                         layout->pictures.size(), resets.size(), resets.empty() ? "none" : layout->describe(resets[0]).c_str());
            fail("the extra-long open-GOP fixture lost the structure the bounded search needs");
        } else {
            std::vector<size_t> reads;
            graded += gradeResetStarts(device, label, file, *layout,
                                       {ResetStart{kNear, Shows::FileStart}, ResetStart{kFar, Shows::FileStart}},
                                       [&](const std::string&, size_t) { reads.push_back(searchReads()); });
            if (reads.size() == 2) {
                std::printf("%s: %s: searching read %zu packets at picture %zu and %zu at %zu\n", label.c_str(), file,
                            reads[0], kNear, reads[1], kFar);
                if (reads[0] >= kNear || reads[1] >= kFar || reads[1] >= reads[0] + (kFar - kNear) / 2) {
                    std::fprintf(stderr, "FAIL: %s: %s: searching read %zu packets at picture %zu and %zu at %zu\n",
                                 label.c_str(), file, reads[0], kNear, reads[1], kFar);
                    fail("a start past the budget searched further back the further into the file it lay");
                }
            }
            const std::optional<CounterReference> reference = counterReference(label, file);
            // CONTROL: test-side, by hand, 2026-10-01: the reference cut to 1000 pictures here -> "FAIL: ...
            //       counter_opengop_xlong.mp4: 1000 pictures decoded, 1200 packets read" and the harness line
            //       below, where the unguarded read past the end had been undefined behaviour. Restored.
            const bool matched = reference && reference->ids.size() == layout->pictures.size();
            if (reference && !matched) {
                std::fprintf(stderr, "FAIL: %s: %s: %zu pictures decoded, %zu packets read\n", label.c_str(), file,
                             reference->ids.size(), layout->pictures.size());
                fail("the harness could not match the fixture's packets to its pictures, which is not a verdict on the "
                     "decoder");
            }
            const std::optional<wallpaperx::StreamPosition> position =
                matched ? positionAt(device, label, file, kFar, reference->ids[kFar]) : std::nullopt;
            const std::string what = std::string(file) + " opened at picture " + std::to_string(kFar) +
                                     ", its second seek failing";
            const std::optional<StartRun> run =
                position ? runLoggedStart(device, label, what, asset(file), *position,
                                          [] { g_seekFailure = SeekFailure::Second; })
                         : std::nullopt;
            if (run) {
                ++graded;
                const size_t searched = searchReads();
                const bool secondFailed = run->seeks.size() >= 2 && run->seeks[1].result < 0;
                const bool fileStart = !run->shown.empty() && run->shown.front() == reference->ids[0] &&
                                       !run->startsPass.empty() && run->startsPass.front();
                std::printf("%s: %s: %s; showed %s; %zu packets read searching\n", label.c_str(), what.c_str(),
                            describeSeeks(run->seeks).c_str(), joinIds(run->shown).c_str(), searched);
                if (!secondFailed || !fileStart || searched >= kCatchUpBudget) {
                    std::fprintf(stderr, "FAIL: %s: %s: %s; showed %s (first %smarked as a pass); %zu packets read "
                                         "searching\n",
                                 label.c_str(), what.c_str(), describeSeeks(run->seeks).c_str(), joinIds(run->shown).c_str(),
                                 fileStart ? "" : "maybe not ", searched);
                    fail(!secondFailed ? "the second seek of the search did not fail, so the case was not exercised"
                                       : "a search whose seek failed past the budget read on instead of starting at the "
                                         "first picture");
                }
            }
        }
    }
    // GOAL: S15.180 -- on the device the budget is measured from the reset point the start decodes from,
    //       whichever search window found it: past the budget from a reset point part way in, the start shows
    //       that reset point's picture, not marked as a pass; past it from the file's first picture, the first
    //       picture as a new pass; within it, its own picture.
    // WHY-THIS-SHAPE: IDRs 900 pictures apart, so the first window's reset point lies far back from 890 and
    //       1780; 1000 and 1150 (the budget exactly, which is inclusive) are within it of IDR 900. The pictures
    //       shown, the pass mark and the first packet sent each tell a different wrong answer apart.
    // CONTROL: via control_batch.sh, 2026-10-01, reverted, renderer/video/video_decoder.cpp startAtResetPoint():
    //       `withinBudget` made `back == 0 || withinCatchUp(...)` -> "counter_sparse_h264.mp4 opened at picture
    //       1780: showed 1780 1781 1782 1783 1784 where 900 901 902 903 904 was due", and at 890 "showed 890 ...
    //       where 0 1 2 3 4 was due" and "its first picture is not marked as starting a pass".
    // COVERAGE-ASSERT: the fixture's reset points are exactly pictures 0, 900 and 1800, no other packet is a
    //       keyframe, and each start lies on the side of the budget its case needs, read from the packets.
    if (const std::optional<GopLayout> layout = deviceLayout(device, label, "counter_sparse_h264.mp4", h264, "H.264")) {
        const char* const file = "counter_sparse_h264.mp4";
        offered += 4;
        const std::vector<size_t> resets = layout->resets();
        std::vector<size_t> at;
        for (const size_t r : resets) at.push_back(layout->picture(r));
        const size_t keys = static_cast<size_t>(std::count_if(layout->packets.begin(), layout->packets.end(),
                                                              [](const GopLayout::Packet& p) { return p.key; }));
        const bool shape = at == std::vector<size_t>{0, 900, 1800} && keys == 3 && layout->pictures.size() >= 1780 + kFollowing &&
                           890 > kCatchUpBudget && 1150 - 900 <= kCatchUpBudget && 1780 - 900 > kCatchUpBudget;
        if (!shape) {
            std::string list;
            for (const size_t p : at) list += (list.empty() ? "" : ", ") + std::to_string(p);
            std::fprintf(stderr, "FAIL: %s: %s: %zu pictures, reset points at pictures %s, %zu keyframe(s); budget %zu\n",
                         label.c_str(), file, layout->pictures.size(), list.c_str(), keys, kCatchUpBudget);
            fail("the sparse-keyframe H.264 fixture lost the structure the reset-point budget cases need");
        } else {
            graded += gradeResetStarts(device, label, file, *layout,
                                       {ResetStart{890, Shows::FileStart}, ResetStart{1000, Shows::Picture},
                                        ResetStart{1150, Shows::Picture}, ResetStart{1780, Shows::ResetPoint}});
        }
    }
    if (!g_notVerified) {
        check(offered == 11 && graded == offered, "every start this check is written for was graded");
    }
}

void runAv1StartChecks(Device& device, const std::string& label) {
    // GOAL: S15.170 -- an AV1 file opened part way in on the device decodes from the shown key frame
    //       nearest before its picture, and shows that picture first, decoded on the device.
    // WHY-THIS-SHAPE: picture 100 with key frames every 24 pictures: the nearest is picture 96, so a start
    //       that misreads AV1's key frames falls back to the file's first packet and still shows 100 --
    //       within the budget -- and only the packets sent tell the two apart.
    // CONTROL: via control_batch.sh, 2026-09-30, reverted, renderer/video/reset_point.cpp av1TemporalUnit()
    //       returning false -> "counter_av1.mp4 opened at picture 100: the first packet sent to the decoder
    //       was packet 0 (picture 0, a shown key frame), where the nearest reset point before the picture is
    //       packet 96 (picture 96, a shown key frame)".
    // COVERAGE-ASSERT: the nearest reset point before picture 100 must be part way in, read from the packets;
    //       red with the point moved to 20 (test-side, by hand, 2026-09-30), "no shown key frame part way in
    //       before picture 20".
    const std::optional<GopLayout> layout =
        deviceLayout(device, label, "counter_av1.mp4", VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "AV1");
    if (!layout) return;
    constexpr size_t kPoint = 100;
    if (layout->pictures.size() <= kPoint + 5 || layout->nearestReset(layout->pictures[kPoint]) == 0) {
        std::fprintf(stderr, "FAIL: %s: counter_av1.mp4: %zu pictures, and no shown key frame part way in before "
                             "picture %zu\n",
                     label.c_str(), layout->pictures.size(), kPoint);
        fail("the AV1 fixture lost the key frame part way in that this check starts at");
        return;
    }
    const int graded = gradeResetStarts(device, label, "counter_av1.mp4", *layout, {ResetStart{kPoint, Shows::Picture}});
    if (!g_notVerified) check(graded == 1, "the AV1 start was graded");
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
    // CONTROL: renderer/video/video_pipeline.cpp, each reverted. convert() building a renderer only for the
    //       first picture (`if (!m_renderer)`), via control_batch.sh 2026-10-26 (G6) on the Lavapipe entry:
    //       "counter_resize.h264: the pipeline threw after 30 pictures: YcbcrRenderer: the frame's shape
    //       differs from the one this renderer was built for". The rendering-before-converting mutation is now
    //       renderNext() drawing before it decodes, which throws on the FIRST call rather than showing black; that was
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
    //       The first break re-run 2026-10-19 by T5b on shape-device, queues hidden: held.
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
                     !pool.freed                        ? "never freed"
                     : !pool.freedWhileOpen             ? "freed only when the pipeline was destroyed"
                     : pool.freedDuring == kOutsideCalls ? "freed between calls, outside any call this mode grades"
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

// Every picture of `file` decoded directly and drawn by a YcbcrRenderer of the probe's own, so each
// extent() and visible() read is paired with the counter drawn in that same picture.
bool gradeReportedSizes(Device& device, const std::string& label, const std::string& what,
                        const SizeChangeFixture& fixture, bool shrinksOnDevice, const CounterReference& reference) {
    struct Shown {
        int id;
        VkExtent2D extent;
        VkRect2D visible;
        bool onDevice;
    };
    std::vector<Shown> shown;
    std::string threw;
    try {
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
        VideoDecoder decoder(device, asset(fixture.file));
        std::unique_ptr<wallpaperx::YcbcrRenderer> stage;
        while (std::optional<DecodedFrame> frame = decoder.next()) {
            if (shown.size() > reference.ids.size() + 8) {
                threw = "the stream did not end";
                break;
            }
            Shown entry{-1, frame->extent(), frame->visible(), frame->storage() == DecodedFrame::Storage::DeviceImage};
            if (!stage || !(wallpaperx::YcbcrRenderer::shapeOf(*frame) == stage->shape())) {
                stage.reset();
                stage = std::make_unique<wallpaperx::YcbcrRenderer>(device, std::move(*frame), kShaderDir);
            } else {
                stage->show(std::move(*frame));
            }
            targets.render(*stage, target);
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
    //       with a coded extent of 640x480" (re-run 2026-10-26, G6, through YcbcrRenderer: "... on the device"). visible() two pixels
    //       narrower on device frames, drawn through YcbcrRenderer: "picture 0 is 320x240, reported visible
    //       318x240", restored green.
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
    //       allocatePicture(), stayed green. Both re-run 2026-10-19 (T7b), shape-device, queues hidden: held,
    //       the same lines.
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
    //       the taller case "next() returned a picture reporting 320x241". Both re-run 2026-10-19 (T7b),
    //       queues hidden: held, the same lines.
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
    // CONTROL: renderer/video/video_decoder.cpp, each reverted, via control_batch.sh 2026-10-23 (G3d), RTX 3080
    //       Ti, bytes restored. reopenInSoftware() calling closeDecoder() instead of retireDecoder(): "the
    //       fallback call 29 made 2 wait(s), 1 on the held batch", end-of-stream case red beside it.
    //       releaseRetiredCodec() without its lastReader hasCompleted() test (the probe test kept), and without
    //       both: "the hardware codec was freed in call 29, while its last reader was still running".
    //       decode() without its top-of-call releaseRetiredCodec() stays GREEN: the one before it returns a
    //       picture frees the codec in the same call. Without both: "never freed by call 33, the first after its
    //       last reader completed" (35 for counter_tiny.obu). releaseRetiredCodec() waiting on the last reader
    //       when it has not completed: "the fallback call 29 made 1 wait(s), 1 on the held batch".
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

// GOAL: S15.936, the retired decoder -- a hardware codec retired at a fallback while the device does not answer
//       is never freed during playback, no call hangs or waits on the playback path, and closing with the device
//       still silent frees nothing and records the leak. False if it is freed unanswered, waited for, or freed at
//       close. Chosen: the device stays silent from the fallback to the close, so the leak, not a later free.
// WHY-THIS-SHAPE: the silence starts at the fallback call itself: the decoder sends its probe while retiring the
//       codec, so silencing the device after the fallback (the old shape) only met a probe already answered.
//       The next fenced submission is held before every call; the decoder-only harness submits nothing else, so
//       the fallback's probe is the one held, and any wait on it is told VK_TIMEOUT at once (a stopped GPU).
// CONTROL: control_batch.sh 2026-10-23, RTX 3080 Ti, each restored (renderer/video/video_decoder.cpp):
//       releaseRetiredCodec() without `|| !m_device.hasCompleted(*sent)` -> "... is never freed during playback
//       and makes no wait (0 wait(s), 3 of 3 later calls returned, freed in call 29)", the close check red too;
//       closeDecoder()'s leak branch freeing the retired codec -> "closing with the device still silent frees
//       nothing (codec freed at teardown)".
// COVERAGE-ASSERT: no call before the fallback made a fenced submission, the fallback's one was held and still
//       running after the playback calls, and device pictures came before the fallback. Not controlled.
void runUnansweredRetiredChecks(Device& device, const std::string& label, const SizeChangeFixture& fixture) {
    if (!declinedHere(device, label, fixture)) return;
    const std::string what = std::string(fixture.file) + ", a retired hardware codec with the device not answering";
    constexpr int kPlaybackCalls = 3;
    std::string stopped;
    std::string threw;
    std::optional<size_t> fallback;
    int devicePictures = 0;
    bool probeHeld = false;
    bool stillHeld = false;
    int playbackWaits = 0;
    int playbackRefused = 0;
    int returnedAfter = 0;
    try {
        RetireHarness harness(device, asset(fixture.file));
        g_stallHeld = true;
        for (size_t n = 0; n < 400 && !fallback; ++n) {
            g_holdNextSubmission = true;
            const DirectCall call = harness.call();
            g_holdNextSubmission = false;
            if (call.openedSoftware) {
                fallback = harness.calls.size() - 1;
                probeHeld = g_heldFences.size() == 1;
            } else if (!g_heldFences.empty()) {
                stopped = "call " + std::to_string(n + 1) + " made a fenced submission before any fallback";
                break;
            } else if (!call.returned) {
                stopped = "the stream ended on the device after " + std::to_string(n) + " pictures";
                break;
            }
        }
        if (!fallback && stopped.empty()) stopped = "no fallback in 400 calls";
        if (fallback) {
            devicePictures = devicePicturesBefore(harness, *fallback);
            playbackWaits = harness.calls.back().waits;
            const int before = g_stalledWaits;
            for (int i = 0; i < kPlaybackCalls; ++i) {
                const DirectCall call = harness.call();
                playbackWaits += call.waits;
                returnedAfter += call.returned ? 1 : 0;
            }
            playbackRefused = g_stalledWaits - before;
            stillHeld = !g_heldFences.empty() && !device.hasCompleted(device.lastSubmission());
            g_holdNextSubmission = true;
            static_cast<void>(harness.destroy());
            g_holdNextSubmission = false;
        }
        g_stallHeld = false;
    } catch (const std::exception& e) {
        threw = e.what();
    }
    g_holdNextSubmission = false;
    g_stallHeld = false;
    g_heldFences.clear();
    const auto retireBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!device.hasCompleted(device.lastSubmission()) && std::chrono::steady_clock::now() < retireBy) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!device.hasCompleted(device.lastSubmission())) {
        std::fprintf(stderr, "FAIL: %s: %s: the held batch had not completed 5 s after the hold was let go\n",
                     label.c_str(), what.c_str());
        fail("the harness could not retire the batch it held, so nothing after it in this child can be trusted");
        return;
    }
    const CodecRecord* hardware = RetireHarness::hardwareCodec();
    std::printf("%s: %s: fallback at call %s, its probe %s; %d call(s) after it returned %d picture(s), %d wait(s), "
                "%d refused; hardware codec %s\n",
                label.c_str(), what.c_str(), fallback ? std::to_string(*fallback + 1).c_str() : stopped.c_str(),
                probeHeld ? "held" : "not held", kPlaybackCalls, returnedAfter, playbackWaits, playbackRefused,
                codecFate(hardware).c_str());
    if (!threw.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: threw: %s\n", label.c_str(), what.c_str(), threw.c_str());
        fail("the unanswered retired-codec run threw");
        return;
    }
    if (!fallback || !probeHeld || !stillHeld || hardware == nullptr) {
        std::fprintf(stderr, "FAIL: %s: %s: the retirement probe was not held through playback (%s; probe %s, %s "
                             "after the playback calls)\n",
                     label.c_str(), what.c_str(), fallback ? "fallback reached" : stopped.c_str(),
                     probeHeld ? "held" : "not held", stillHeld ? "still running" : "complete");
        fail("the silent device was not in place at the retirement, which is not a verdict on the decoder");
        return;
    }
    if (devicePictures == 0) {
        std::printf("%s: %s: nothing was decoded on the device before the fallback -- not answerable\n",
                    label.c_str(), what.c_str());
        g_notVerified = true;
        return;
    }
    const bool freedInPlayback = hardware->freed && hardware->freedDuring != kDestroying;
    check(!freedInPlayback && playbackWaits == 0 && returnedAfter == kPlaybackCalls,
          label + ": " + what + ": the retired codec is never freed during playback and makes no wait (" +
              std::to_string(playbackWaits) + " wait(s), " + std::to_string(returnedAfter) + " of " +
              std::to_string(kPlaybackCalls) + " later calls returned, " +
              (freedInPlayback ? "freed in call " + std::to_string(hardware->freedDuring + 1) : "not freed") + ")");
    check(!hardware->freed,
          label + ": " + what + ": closing with the device still silent frees nothing (codec " + codecFate(hardware) +
              ")");
    if (!hardware->freed) g_watchDeviceDestroy = true;
}

void runUnansweredProbeTeardownChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label)) return;
    const char* const file = "counter_resize.h264";
    if (!haveAsset(file, label)) return;
    const std::string what = std::string(file) + ", torn down on the hardware with the device not answering";

    // GOAL: S13.121 -- tearing down a hardware decoder that holds decode images, with every renderer batch
    //       finished, first asks the device a bounded question, and when that goes unanswered leaks the codec
    //       and its image pools rather than entering ffmpeg's unbounded wait on its own decode work.
    // WHY-THIS-SHAPE: every batch is drained before teardown, so safeToFree() says yes and cannot short-circuit
    //       to the leak on its own -- the stalled-teardown check (S13.91) reaches the leak that way, which is why
    //       it never grades this branch. Only the probe's own submission is held and its wait told VK_TIMEOUT,
    //       the answer a stopped GPU gives, so the branch runs without waiting out the Device's bound.
    // CONTROL: renderer/video/video_decoder.cpp closeDecoder() without `(decodeWorkHeld && !decodeWorkCanDrain())`,
    //       reverted: "0 unanswered probe(s) at teardown, not 1". The probe made and its answer ignored, reverted:
    //       "the hardware codec was freed at teardown" and "pool 1 of 1 was freed at teardown". NVIDIA only.
    //       Re-run 2026-10-19 (T7), decodeWorkCanDrain() now asking deviceAnswers(): the first break held.
    //       The second re-run 2026-10-19 (T7b): held, "the hardware codec was freed at teardown".
    // COVERAGE-ASSERT: every call before teardown returned a device picture, the codec and a pool were alive when
    //       teardown began, and exactly one wait was refused -- the probe's, since nothing else was held.
    std::string threw;
    bool devicePictures = true;
    bool aliveAtTeardown = false;
    bool drained = false;
    try {
        RetireHarness harness(device, asset(file));
        for (int i = 0; i < 3; ++i) {
            const DirectCall call = harness.call();
            devicePictures = devicePictures && call.returned && call.onDevice;
        }
        device.waitForSubmissions();
        drained = device.hasCompleted(device.lastSubmission());
        const CodecRecord* hardware = RetireHarness::hardwareCodec();
        aliveAtTeardown = hardware != nullptr && !hardware->freed && !g_pools.empty() && !g_pools.front().freed;
        g_holdNextSubmission = true;
        g_stallHeld = true;
        static_cast<void>(harness.destroy());
        g_holdNextSubmission = false;
        g_stallHeld = false;
    } catch (const std::exception& e) {
        g_stallHeld = false;
        g_holdNextSubmission = false;
        threw = e.what();
    }
    // Lets the Device retire the probe's batch, so the entry's closing wait does not report it as a stall. It
    // was submitted a moment ago, so it is polled to completion first: that wait refuses a stamped batch.
    g_heldFences.clear();
    const auto retireBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!device.hasCompleted(device.lastSubmission()) && std::chrono::steady_clock::now() < retireBy) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!device.hasCompleted(device.lastSubmission())) {
        std::fprintf(stderr, "FAIL: %s: %s: the probe's empty batch had not completed 5 s after the hold was let go\n",
                     label.c_str(), what.c_str());
        fail("the harness could not retire the batch it held, so nothing after it in this child can be trusted");
        return;
    }
    if (!threw.empty()) {
        std::fprintf(stderr, "FAIL: %s: %s: threw: %s\n", label.c_str(), what.c_str(), threw.c_str());
        fail("the unanswered-probe teardown run threw");
        return;
    }
    if (!devicePictures) {
        std::printf("%s: %s: this device did not decode every picture of the run on the hardware -- not answerable\n",
                    label.c_str(), what.c_str());
        g_notVerified = true;
        return;
    }
    const CodecRecord* hardware = RetireHarness::hardwareCodec();
    std::printf("%s: %s: %d unanswered probe(s); hardware codec %s; %zu pool(s)\n", label.c_str(), what.c_str(),
                g_stalledWaits, codecFate(hardware).c_str(), g_pools.size());
    if (!drained || !aliveAtTeardown) {
        std::fprintf(stderr, "FAIL: %s: %s: renderer batches %s before teardown; codec and pool %s alive at it\n",
                     label.c_str(), what.c_str(), drained ? "finished" : "still running",
                     aliveAtTeardown ? "were" : "were not");
        fail("teardown was not reached with a live hardware codec and every renderer batch finished");
        return;
    }
    if (g_stalledWaits != 1) {
        std::fprintf(stderr, "FAIL: %s: %s: %d unanswered probe(s) at teardown, not 1\n", label.c_str(), what.c_str(),
                     g_stalledWaits);
        fail("a teardown holding decode images did not ask the device a bounded question before freeing them");
        return;
    }
    if (hardware->freed) {
        std::fprintf(stderr, "FAIL: %s: %s: the hardware codec was %s\n", label.c_str(), what.c_str(),
                     codecFate(hardware).c_str());
        fail("a hardware codec was freed after the device did not answer, entering ffmpeg's unbounded wait");
    } else {
        // Graded by runGraded() once the Device is gone.
        g_watchDeviceDestroy = true;
    }
    for (size_t i = 0; i < g_pools.size(); ++i) {
        if (!g_pools[i].freed || g_pools[i].freedDuring != kDestroying) continue;
        std::fprintf(stderr, "FAIL: %s: %s: pool %zu of %zu was freed at teardown\n", label.c_str(), what.c_str(), i + 1,
                     g_pools.size());
        fail("an image pool was freed after the device did not answer");
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
    //       held, reverted: "teardown made 1 wait(s), 1 on a held batch". closeDecoder() dropping the retired
    //       codec without avcodec_free_context(), via control_batch.sh 2026-10-15, reverted (RTX) -> "FAIL: the hardware codec refused
    //       before decoding was not freed at teardown".
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

// Whether a wait made through this executable's vkWaitForFences is counted -- the proof that "no
// waits" is a measurement. Made by the probe itself, on a fence created already signalled, because
// the pipeline's own first call cannot prove it: the Device polls a fence before waiting on it, so
// it calls vkWaitForFences only if the GPU has not finished yet, and on Lavapipe under load it
// sometimes has -- a first call making no wait is legitimate, and requiring one failed the loop
// entry intermittently under a parallel ctest run.
// CONTROL: the `++g_vulkanCalls[k_vkWaitForFences]` line deleted from this file's vkWaitForFences ->
//       "FAIL: a wait made through this probe's vkWaitForFences is counted ..." on both the loop and
//       the steady entry, where the first-call wait test it replaces would also have gone red, but
//       only by the scheduler's leave. Restored.
bool waitCounterIsWired(Device& device) {
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device.device(), &info, nullptr, &fence) != VK_SUCCESS) return false;
    g_vulkanCalls.fill(0);
    g_countVulkan = true;
    const VkResult waited = vkWaitForFences(device.device(), 1, &fence, VK_TRUE, 0);
    g_countVulkan = false;
    vkDestroyFence(device.device(), fence, nullptr);
    return waited == VK_SUCCESS && g_vulkanCalls[k_vkWaitForFences] == 1;
}

void runSteadyChecks(Device& device, const std::string& label, bool onDevice) {
    if (onDevice ? !hardwareDecoderPresent(device, label) : !videoExtensionsHidden(device, label)) return;
    const char* const file = "counter_long.mp4";
    if (!haveAsset(file, label)) return;

    // GOAL: S13.67 -- once its stage exists, a call creates no Vulkan object and waits on nothing, except that the
    //       first showing of each decoded pool image may make that image's plane views, descriptor pool and set;
    //       once every pool image has been shown once, nothing is created. False for a renderer that remakes a
    //       source for an image it has shown before, or that meets an unbounded stream of new images.
    // WHY-THIS-SHAPE: a first showing is recognised by the plane views the call made (g_planeViewImages): all of one
    //       image never viewed before. Excusing "the first N calls" instead would grade the scheduler -- when ffmpeg
    //       first hands out each pool image depends on decode-ahead timing -- and excusing any call that made plane
    //       views would let a renderer that remakes them for an image already shown pass. Only that showing's own
    //       footprint (the views, one pool, one set) is excused; anything else it made still counts.
    // CONTROL: control_batch.sh 2026-10-25 (T20a), RTX 3080 Ti, device path, each reverted: YcbcrRenderer::sourceFor()
    //       (renderer/video/ycbcr_renderer.cpp) never finding its cached source -> "318 steady calls made vkCreateImageView
    //       x939, vkCreateDescriptorPool x313, vkAllocateDescriptorSets x313"; every hardware frame kept alive by
    //       VideoDecoder (renderer/video/video_decoder.cpp), so the pool makes an image per picture -> "321 distinct
    //       decoded images were shown for the first time"; Device::waitForSubmissions() after the render in
    //       VideoPipeline::draw() (renderer/video/video_pipeline.cpp) -> "318 steady calls made vkWaitForFences x318".
    //       Green: 7 pool images shown for the first time, 318 steady calls made none.
    // The rest of the shape: counted at this executable's Vulkan symbols during renderNext() alone -- which since
    //       the split is decodeNext() and draw() together, so both halves are inside the count. The
    //       queue is drained before each counted call, so a wait inside it is the pipeline's and not
    //       one a slow GPU forced: YcbcrRenderer's host path waits when two uploads are still in
    //       flight, and counting across an undrained queue would grade the scheduler. The first calls
    //       are left out because the Device's fence and command buffer pool grows to its most batches.
    //       Counted on the calling thread only (g_countVulkan is thread_local): the decode thread's calls are
    //       ffmpeg's frame pool growing, now seen through the decoder's vkAllocateMemory wrapper, and a process-wide
    //       flag counted one whenever it fell inside a call -- "318 steady calls made vkAllocateMemory x1".
    // CONTROL: renderer/video/video_pipeline.cpp, each reverted. Both mutations moved with the split and
    //       were re-run at their new homes 2026-09-15, on the host path; the device and Lavapipe runs were
    //       not repeated this round. Device::waitForSubmissions() after the render, now in draw(): "318
    //       steady calls made vkWaitForFences x318". The stage rebuilt on every call, now in decodeNext():
    //       "318 steady calls made vkCreateImage x1272, vkCreateImageView x1272, vkCreateBuffer x636,
    //       vkCreateSampler x318, ... vkWaitForFences x317". Re-run 2026-10-23 (T14), RTX 3080 Ti, device path,
    //       control_batch.sh, restored: waitForSubmissions() after the render in draw() -> "318 steady calls made
    //       vkWaitForFences x249"; a fence created and destroyed in draw() -> "vkCreateFence x318". Test-side, the
    //       flag made process-wide again: 5 of 6 runs "318 steady calls made vkAllocateMemory x1"; per thread, 8 of 8 green.
    //       Re-run 2026-10-26 (G6), steady-device, control_batch.sh, restored: convert() building a new YcbcrRenderer
    //       for every picture -> "318 steady calls made vkCreateImageView x939, vkCreateSampler x318, ..."; the
    //       waitForSubmissions() in draw() -> "318 steady calls made vkWaitForFences x317".
    // COVERAGE-ASSERT: the first call, which builds the renderer, must be seen creating -- red with
    //       counting never switched on, "the first call was seen making none" -- and the wait counter
    //       proven wired by waitCounterIsWired(); and 300 calls counted, red on the 20-picture fixture,
    //       "only 17 calls counted". The first call is NOT required to wait: it waits only if the GPU
    //       is still busy when asked, and requiring it failed the Lavapipe loop entry intermittently
    //       under a parallel ctest run.
    constexpr int kUncountedCalls = 3;
    constexpr int kMinCountedCalls = 300;
    // ffmpeg's decode pool holds the decoder's reference pictures and a few in flight; H.264 and HEVC allow at most
    // 16 references, so more distinct images than this is images made per picture, not a pool.
    constexpr size_t kMaxPoolImages = 32;
    VulkanTally first;
    VulkanTally steady;
    std::vector<VkImage> shownImages;
    int firstShowings = 0;
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
            g_planeViewImages.clear();
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
            const std::vector<VkImage>& viewed = g_planeViewImages;
            const bool oneImage = !viewed.empty() && std::all_of(viewed.begin(), viewed.end(),
                                                                 [&](VkImage i) { return i == viewed.front(); });
            if (oneImage && std::find(shownImages.begin(), shownImages.end(), viewed.front()) == shownImages.end()) {
                shownImages.push_back(viewed.front());
                ++firstShowings;
                const auto excuse = [&](VulkanCall call, int n) { g_vulkanCalls[call] -= std::min(g_vulkanCalls[call], n); };
                excuse(k_vkCreateImageView, static_cast<int>(viewed.size()));
                excuse(k_vkCreateDescriptorPool, 1);
                excuse(k_vkAllocateDescriptorSets, 1);
            }
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
    check(waitCounterIsWired(device), "a wait made through this probe's vkWaitForFences is counted, so a steady "
                                      "count of no waits is a measurement");
    if (first.creations() == 0) {
        std::fprintf(stderr, "FAIL: %s: the first call was seen making %s\n", label.c_str(), first.describe().c_str());
        fail("the Vulkan interposers did not see the stage being built, so a count of zero would mean nothing");
        return;
    }
    if (counted < kMinCountedCalls) {
        std::fprintf(stderr, "FAIL: %s: only %d calls counted\n", label.c_str(), counted);
        fail("too few steady calls were counted to grade");
        return;
    }
    // COVERAGE-ASSERT: on the device path the plane views must have been recognised, or the allowance above was
    //       never exercised and a count of zero would not show that the shown images were the pool's.
    if (onDevice && shownImages.empty()) {
        std::fprintf(stderr, "FAIL: %s: no plane view of a decoded image was seen\n", label.c_str());
        fail("the first showing of a pool image was never recognised");
        return;
    }
    if (shownImages.size() > kMaxPoolImages) {
        std::fprintf(stderr, "FAIL: %s: %zu distinct decoded images were shown for the first time\n", label.c_str(),
                     shownImages.size());
        fail("the renderer met more decoded images than a pool holds");
    }
    if (steady.creations() != 0 || steady.waits() != 0) {
        std::fprintf(stderr, "FAIL: %s: %d steady calls made %s\n", label.c_str(), counted, steady.describe().c_str());
        fail("a steady picture created a Vulkan object or waited");
    }
    std::printf("%s: first call made %s; %d steady calls made %s; %d pool images shown for the first time\n",
                label.c_str(), first.describe().c_str(), counted, steady.describe().c_str(), firstShowings);
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
    //       open(s)". renderer/video/video_pipeline.cpp hold() resetting the stage at a loop's first picture: "boundary 1
    //       made vkCreateImage x3, ..."; waiting for submissions after that call's render: "vkWaitForFences
    //       x1". The wait placed before the render stayed green: nothing is pending, no Vulkan call is made.
    //       Re-run 2026-10-19 (T7), loop-device, queues hidden: restart() without its flush and the stage reset held,
    //       the same lines. Re-run 2026-10-19 (T7b), loop-device, queues hidden, each held: rewind() dropping
    //       packet 1 ("call 91 (pass 2, picture 0 of it) showed 24", raw H.264 and the transport stream);
    //       seekToFirstPacket() without the presentation-time seek (the same counter_bf2.mkv line); the wait,
    //       now in video_pipeline.cpp draw() after the render of a pass's first picture ("vkWaitForFences x1").
    // COVERAGE-ASSERT: the calls made and read back match the plan -- red with every seventh skipped, "60
    //       calls made and 51 read back" -- and the first call is seen building the stage, red with
    //       counting off, "the first call was seen making none". That the WAIT counter is wired is
    //       proven by waitCounterIsWired(), not by the first call waiting: whether it waits depends on
    //       whether the GPU finished first, which is the scheduler's to decide. counter_bf2.mkv's missing
    //       decode timestamp is asserted, red with the MP4 claimed instead.
    check(waitCounterIsWired(device), "a wait made through this probe's vkWaitForFences is counted, so a boundary "
                                      "count of no waits is a measurement");
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
        if (!run.calls.empty() && run.calls[0].vulkan.creations() == 0) {
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
    // CONTROL: renderer/video/video_decoder.cpp, each reverted, via control_batch.sh 2026-09-30.
    //       seekLandsOnFirstPacket() taking any packet as packet 1 (its PacketMark::matches() call made
    //       `true`): the transport stream red on both paths, "no rewind landing was rejected at a loop
    //       boundary", and on loop-device "call 121 (pass 2, picture 0 of it) showed 24 where 0 was due";
    //       Matroska green, nothing being read for it to judge. A failed read after a seek taken as landed:
    //       on loop-host, Matroska "call 61 returned false". rewind() without its reopen leaves this check
    //       green, measured via control_batch.sh 2026-10-01: the Matroska case reads on from where its failed
    //       seeks left it, so the reopen half of S13.72 is not graded here. runLoopFileChecks() grades it:
    //       "counter_short.mp4 deleted after 10 pictures, every seek failing from then: call 21 returned false".
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
    // CONTROL: renderer/video/video_decoder.cpp PacketMark::matches(), each reverted, via control_batch.sh
    //       2026-09-30 on loop-host. The match without a position always false: MP4, transport stream and
    //       Matroska "1 container open(s)" at the boundary. That match on "no position" alone: "call 322 (pass
    //       2, picture 0 of it) showed 48 where 0 was due" on the late landing; the other runs stayed green.
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
    // CONTROL: renderer/video/video_decoder.cpp openContainer()'s `if (m_io == nullptr)` made `if (true)`
    //       with a closeFile() before the openFile(), so the PATH is opened again at every container
    //       open rather than the descriptor being reused, reverted: deletion "threw during call 21:
    //       VideoDecoder: cannot open .../deleted.mp4: No such file or directory"; rename "call 21
    //       (pass 2, picture 0 of it) showed 1000 where 0 was due". Re-measured 2026-09-20, after the
    //       decoder stopped calling avio_open2 and started opening the descriptor itself.
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
    // CONTROL: renderer/video/picture_source.cpp, next()'s `startsPass = true;` after the
    //       restart deleted (re-run there 2026-10-19, T7, timing-host on Lavapipe: held) -> "FAIL: the first picture after a loop is marked as starting a pass",
    //       printing "1 mark(s), where 2 were due", and the spacing check with it ("-1900000000 ns at
    //       20" -- the whole file backwards, which is what the mark exists to keep a caller from
    //       using). renderer/video/video_decoder.cpp, next()'s `frame.m_presentation` given
    //       AVRational{1, 1} as its base -> "FAIL: each picture is due one file interval after the one
    //       before it", printing "38 of 38 steps are not 100000000 ns; first: 1024000000000 ns at 1".
    //       Reverted. The second observed 2026-09-18; the first via control_batch.sh 2026-09-30 on timing-host,
    //       which also printed "39 pair(s) compared, where 38 were due". Via control_batch.sh 2026-10-15, reverted, timing-host on
    //       Lavapipe: timing() returning an empty PictureTiming instead of throwing -> "FAIL: timing() before
    //       any picture has been decoded throws std::logic_error"; PictureSource::next() recording no presentation time (re-run 2026-10-19, T7: held) ->
    //       "FAIL: every picture of a file that carries timestamps reports a presentation time and a duration";
    //       renderer/video/video_decoder.cpp, decode()'s duration from twice the frame's -> "FAIL: and the
    //       duration each reports is the file's own picture interval". The timing(), AVRational{1, 1} and
    //       duration breaks re-run 2026-10-19 (T7b), timing-host on Lavapipe: each held, its own line.
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
// Looping: a pass end with packets past an edit list's end
// ------------------------------------------------------------------------- //

uint64_t readBigEndian(const std::vector<uint8_t>& bytes, size_t at, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value = (value << 8) | bytes[at + i];
    return value;
}

void writeBigEndian(std::vector<uint8_t>& bytes, size_t at, size_t width, uint64_t value) {
    for (size_t i = width; i-- > 0;) {
        bytes[at + i] = static_cast<uint8_t>(value & 0xff);
        value >>= 8;
    }
}

struct Mp4Box {
    std::string type;
    size_t body = 0;  // first byte after the header
    size_t end = 0;
};

// The boxes directly inside [from, to); empty when one is malformed.
std::vector<Mp4Box> mp4Boxes(const std::vector<uint8_t>& bytes, size_t from, size_t to) {
    std::vector<Mp4Box> boxes;
    for (size_t at = from; at + 8 <= to;) {
        uint64_t size = readBigEndian(bytes, at, 4);
        size_t header = 8;
        if (size == 1) {
            if (at + 16 > to) return {};
            size = readBigEndian(bytes, at + 8, 8);
            header = 16;
        } else if (size == 0) {
            size = to - at;
        }
        if (size < header || size > to - at) return {};
        boxes.push_back(Mp4Box{std::string(reinterpret_cast<const char*>(&bytes[at + 4]), 4), at + header, at + size});
        at += size;
    }
    return boxes;
}

// Shortens the first segment of every track's edit list by `seconds`, in the copy held in `bytes`.
// Returns how many edit lists were changed: 0 when the file has none, or a layout this does not read.
size_t trimEditLists(std::vector<uint8_t>& bytes, double seconds) {
    size_t changed = 0;
    for (const Mp4Box& moov : mp4Boxes(bytes, 0, bytes.size())) {
        if (moov.type != "moov") continue;
        std::optional<uint64_t> timescale;
        const std::vector<Mp4Box> inMoov = mp4Boxes(bytes, moov.body, moov.end);
        for (const Mp4Box& box : inMoov) {
            if (box.type != "mvhd" || box.body + 32 > box.end) continue;
            const bool wide = bytes[box.body] == 1;
            timescale = readBigEndian(bytes, box.body + (wide ? 20 : 12), 4);
        }
        if (!timescale || *timescale == 0) return 0;
        for (const Mp4Box& trak : inMoov) {
            if (trak.type != "trak") continue;
            for (const Mp4Box& edts : mp4Boxes(bytes, trak.body, trak.end)) {
                if (edts.type != "edts") continue;
                for (const Mp4Box& elst : mp4Boxes(bytes, edts.body, edts.end)) {
                    if (elst.type != "elst" || elst.body + 8 > elst.end) continue;
                    const size_t width = bytes[elst.body] == 1 ? 8 : 4;
                    const size_t entries = readBigEndian(bytes, elst.body + 4, 4);
                    const size_t duration = elst.body + 8;
                    if (entries == 0 || duration + width > elst.end) continue;
                    const uint64_t trim = static_cast<uint64_t>(seconds * static_cast<double>(*timescale));
                    const uint64_t length = readBigEndian(bytes, duration, width);
                    if (length <= trim) continue;
                    writeBigEndian(bytes, duration, width, length - trim);
                    ++changed;
                }
            }
        }
    }
    return changed;
}

// Video packets the demuxer flags AV_PKT_FLAG_DISCARD that are presented after every packet it does not:
// the ones an edit list's end leaves past the pass. -1 when the file cannot be read.
int trailingDiscardPackets(const std::string& file) {
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, file.c_str(), nullptr, nullptr) < 0) return -1;
    struct Close {
        AVFormatContext*& format;
        ~Close() { avformat_close_input(&format); }
    } close{format};
    if (avformat_find_stream_info(format, nullptr) < 0) return -1;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) return -1;
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) return -1;
    std::optional<int64_t> lastKept;
    std::vector<int64_t> discarded;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream && packet->pts != AV_NOPTS_VALUE) {
            if ((packet->flags & AV_PKT_FLAG_DISCARD) != 0) {
                discarded.push_back(packet->pts);
            } else if (!lastKept || packet->pts > *lastKept) {
                lastKept = packet->pts;
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (!lastKept) return 0;
    return static_cast<int>(std::count_if(discarded.begin(), discarded.end(), [&](int64_t pts) { return pts > *lastKept; }));
}

void runTrimmedEndChecks(Device& device, const std::string& label) {
    const char* const original = "counter_loop_short.mp4";
    if (!haveAsset(original, label)) return;
    namespace fs = std::filesystem;
    std::string pattern = (fs::temp_directory_path() / "wallpaperx-trim-XXXXXX").string();
    if (mkdtemp(pattern.data()) == nullptr) {
        std::fprintf(stderr, "FAIL: %s: cannot create a scratch directory from %s\n", label.c_str(), pattern.c_str());
        fail("the harness could not create a scratch directory for the trimmed copy");
        return;
    }
    struct Scratch {
        fs::path path;
        ~Scratch() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } scratch{pattern};
    const std::string trimmed = (scratch.path / "trimmed.mp4").string();
    {
        std::vector<uint8_t> bytes;
        if (FILE* in = std::fopen(asset(original).c_str(), "rb")) {
            uint8_t buffer[65536];
            for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, in)) > 0;) bytes.insert(bytes.end(), buffer, buffer + n);
            std::fclose(in);
        }
        // Five pictures of the fixture's thirty a second.
        const size_t changed = bytes.empty() ? 0 : trimEditLists(bytes, 5.0 / 30.0);
        FILE* out = changed == 0 ? nullptr : std::fopen(trimmed.c_str(), "wb");
        const bool written = out != nullptr && std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
        if (out != nullptr && std::fclose(out) != 0) out = nullptr;
        if (changed == 0 || !written) {
            std::fprintf(stderr, "FAIL: %s: %s: %zu edit list(s) found to trim, %s\n", label.c_str(), original, changed,
                         written ? "written" : "copy not written");
            fail("the harness could not make a copy of the fixture whose edit list ends before its packets");
            return;
        }
    }
    const int trailing = trailingDiscardPackets(trimmed);
    if (trailing <= 0) {
        std::printf("%s: the trimmed copy of %s has %d trailing packet(s) flagged discard by the demuxer, so a pass "
                    "end lengthened by them cannot be told -- NOT VERIFIED\n",
                    label.c_str(), original, trailing);
        g_notVerified = true;
        return;
    }

    // GOAL: S15.625 -- a loop's pass end ignores the packets past an MP4 edit list's end, which the
    //       demuxer flags discard: the passEnded the first picture of pass 2 reports equals the last shown
    //       picture's presentation time plus its duration. False if those packets lengthen the pass.
    // WHY-THIS-SHAPE: the fixture is made here, from a copy whose `elst` duration is lowered five pictures,
    //       since FFmpeg's muxer writes no edit list that trims the end. The demuxer's discard flags on the
    //       trailing packets are counted first; with none the run is NOT VERIFIED, as the goal is unreached.
    // CONTROL: control_batch.sh 2026-10-15, reverted, timing_device (RTX) and timing_host_lavapipe:
    //       renderer/video/video_decoder.cpp readVideoPacket() updating m_readThrough for a discard packet ->
    //       "pass end 666666666 ns; last shown picture 500000000 ns lasting 33333333 ns", both entries.
    std::vector<VideoPipeline::PictureTiming> shown;
    std::string threw;
    constexpr size_t kCap = 200;
    std::optional<size_t> boundary;
    try {
        VideoPipeline pipeline(device, trimmed, kShaderDir, VideoPipeline::Playback::Loop);
        while (shown.size() < kCap && !boundary && pipeline.decodeNext()) {
            shown.push_back(pipeline.timing());
            if (shown.size() > 1 && shown.back().startsPass) boundary = shown.size() - 1;
        }
    } catch (const std::exception& e) {
        threw = e.what();
    }
    if (!threw.empty() || !boundary) {
        std::fprintf(stderr, "FAIL: %s: trimmed %s: %zu picture(s) decoded, %s%s\n", label.c_str(), original,
                     shown.size(), threw.empty() ? "no loop boundary reached" : "threw: ", threw.c_str());
        fail("the trimmed copy did not loop, so its pass end was never reported");
        return;
    }
    const VideoPipeline::PictureTiming& last = shown[*boundary - 1];
    const VideoPipeline::PictureTiming& first = shown[*boundary];
    if (!last.presentation || !last.duration || !first.passEnded ||
        *first.passEnded != *last.presentation + *last.duration) {
        const auto ns = [](const std::optional<std::chrono::nanoseconds>& t) {
            return t ? std::to_string(t->count()) : std::string("none");
        };
        std::fprintf(stderr, "FAIL: %s: trimmed %s: pass end %s ns; last shown picture %s ns lasting %s ns\n",
                     label.c_str(), original, ns(first.passEnded).c_str(), ns(last.presentation).c_str(),
                     ns(last.duration).c_str());
        fail("the pass end reported at the loop is not the last shown picture's end");
        return;
    }
    std::printf("%s: trimmed %s: %d trailing discard packet(s); %zu picture(s) a pass, ending at %lld ns as its last "
                "picture does\n",
                label.c_str(), original, trailing, *boundary, static_cast<long long>(first.passEnded->count()));
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
    //       decoded throws std::logic_error rather than drawing whatever the target held.
    // WHY-THIS-SHAPE: what is watched across the lone decode is whether the target is PUBLISHED, not
    //       what its pixels hold. Reading the pixels is the obvious version and it cannot be written:
    //       the readback reclaims the published frame, and a target may be reclaimed once per render,
    //       so a second read with no draw between throws rather than answering. Publication is the
    //       property anyway -- only render() publishes, so a decodeNext() that drew anywhere would leave
    //       the target published, and one that did not cannot.
    // CONTROL: control_batch.sh 2026-10-15, re-run 2026-10-26 (G6, all three held), reverted, halves-host on Lavapipe,
    //       renderer/video/video_pipeline.cpp: draw()'s no-renderer guard returning instead of throwing -> "FAIL: draw() before any picture has been decoded
    //       throws std::logic_error"; draw() rendering only while nothing has been shown -> "FAIL: and the draw
    //       that follows it does publish one"; convert() converting only the first picture -> "FAIL: and the
    //       draw that follows shows the picture that decode had decoded". The untouched-target check has no
    //       production mutation that expresses it: decodeNext() takes no target and so cannot be made to draw
    //       into one. Its control is in this harness: renderNext() substituted for that one decodeNext() call,
    //       which publishes and reddens it.
    // COVERAGE-ASSERT: the picture drawn after the lone decode is asserted to be the NEXT one, so a
    //       readback that had stopped distinguishing pictures fails here rather than letting the
    //       untouched-target check pass. renderNext() matching the two halves is graded by
    //       runSequenceChecks, which drives renderNext() through runPipeline.
    std::string threw;
    bool drawBeforeDecodeThrew = false;
    std::string otherThrow;
    bool decodedFirst = false;
    bool decodedSecond = false;
    bool publishedAfterLoneDecode = true;
    bool publishedAfterDraw = false;
    int afterFirstDraw = -2;
    int afterSecondDraw = -2;
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
                pipeline.draw(targets, target, wallpaperx::Placement{});
            } catch (const std::logic_error&) {
                drawBeforeDecodeThrew = true;
            } catch (const std::exception& e) {
                otherThrow = e.what();
            }

            decodedFirst = pipeline.decodeNext();
            if (decodedFirst) {
                pipeline.draw(targets, target, wallpaperx::Placement{});
                afterFirstDraw = shown();

                // Decoded and converted, deliberately not drawn: what the daemon does for a pass it may
                // still abandon. shown() above reclaimed the frame the draw published, so the target is
                // unpublished here and only a draw can publish it again.
                decodedSecond = pipeline.decodeNext();
                if (decodedSecond) {
                    publishedAfterLoneDecode = target.published();
                    pipeline.draw(targets, target, wallpaperx::Placement{});
                    publishedAfterDraw = target.published();
                    // Read back only once published: a reclaim of an unpublished target throws, which would
                    // report this run as having thrown instead of naming the check below.
                    if (publishedAfterDraw) afterSecondDraw = shown();
                }
            }
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
    if (thenShown) {
        std::printf("%s: %s: the target held picture %d across a decode, then showed picture %d\n", label.c_str(), file,
                    afterFirstDraw, afterSecondDraw);
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
    // CONTROL: renderer/video/picture_source.cpp next(), its restart's `&& m_returnedAny` dropped, re-observed
    //       on the Lavapipe loop-host entry via control_batch.sh 2026-10-19 (T7): "a looping pipeline that has
    //       shown nothing started its stream again instead of reporting the end". Reverted.
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
    //       All three re-run 2026-10-19 (T7b), loop-cost-device, queues hidden: held, the same lines (the
    //       decoder reopen clearing the path first).
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
    //       Re-run 2026-10-19 (T7), queues hidden: keeping m_returned held, "decoded 31 pictures, where playing once cost 7".
    //       The other three re-run 2026-10-19 (T7b), queues hidden: each held, the same lines.
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

// ------------------------------------------------------------------------- //
// The decode thread (S15.882-S15.888), seen through the probe's own DecodeHooks and interposers
// ------------------------------------------------------------------------- //

std::atomic<pid_t> g_gpuTid{0};     // the thread that calls the pipeline
std::atomic<pid_t> g_decodeTid{0};  // learned in the around hook, which only a decode thread calls
std::atomic<int> g_aroundCalls{0};
std::atomic<bool> g_inAround{false};
std::atomic<bool> g_countThreaded{false};
// Pictures the decoder's context returned while g_countThreaded is set, and how many the caller has
// taken: incremented BEFORE a call that is known to return one, so a receive never sees it late.
std::atomic<int> g_threadReceived{0};
std::atomic<int> g_threadHostReceived{0};
std::atomic<int> g_receivedOffGpuThread{0};
std::atomic<int> g_taken{0};
std::atomic<int> g_maxUntaken{0};
// avcodec_receive_frame on the decode thread answers AVERROR_EXTERNAL this many times.
std::atomic<int> g_threadExternal{0};
std::atomic<bool> g_watchParks{false};
// operator new on the pipeline's caller fails once inside DecodeAhead::start(): 1 the first allocation
// there outside the hook's installation, 2 the hook's.
std::atomic<int> g_failNew{0};
std::atomic<int> g_newFailures{0};
// Pictures received on the decode thread while g_countOnThread is set, counted on the interposer's
// ordinary path: g_countThreaded's returns before g_stripTimestamps is applied.
std::atomic<bool> g_countOnThread{false};
std::atomic<int> g_onThreadReceived{0};
std::atomic<int> g_onThreadHost{0};

// Below under g_threadMutex, every change signalled on g_threadChanged.
std::mutex g_threadMutex;
std::condition_variable g_threadChanged;
int g_parks = 0;  // the decode thread waited on a condition outside a decode: its queue was full
int g_readyCalls = 0;
int g_questions = 0;  // ready hook calls from inside a decode: the device-health question
bool g_gateClosed = false;
bool g_atGate = false;

// Holding a decode thread inside ffmpeg past DecodeAhead's stop bound (S15.889): set, the decoder's next
// avcodec_receive_frame on the decode thread waits until it is cleared.
std::atomic<bool> g_holdInFfmpeg{false};
std::atomic<bool> g_heldInFfmpeg{false};
std::atomic<bool> g_decodeThreadGone{false};
// operator delete reports a free of this address (only where the probe replaces it).
std::atomic<const void*> g_watchedDevice{nullptr};
std::atomic<bool> g_watchedDeviceFreed{false};

// Armed on a decode thread by the around hook; its destructor runs as that thread exits.
struct ThreadExitMark {
    bool armed = false;
    ~ThreadExitMark() {
        if (!armed) return;
        {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            g_decodeThreadGone = true;
        }
        g_threadChanged.notify_all();
    }
};
thread_local ThreadExitMark t_exitMark;

void resetThreadWatch() {
    g_gpuTid = gettid();
    g_decodeTid = 0;
    g_aroundCalls = 0;
    g_inAround = false;
    g_countThreaded = false;
    g_threadReceived = 0;
    g_threadHostReceived = 0;
    g_receivedOffGpuThread = 0;
    g_taken = 0;
    g_maxUntaken = 0;
    g_threadExternal = 0;
    g_watchParks = false;
    g_failNew = 0;
    g_newFailures = 0;
    std::lock_guard<std::mutex> lock(g_threadMutex);
    g_parks = 0;
    g_readyCalls = 0;
    g_questions = 0;
    g_gateClosed = false;
    g_atGate = false;
}

// Opens the gate as it goes, so a pipeline destroyed after it never waits out its stop bound on a
// decode thread the probe still holds. Declare after the pipeline.
struct ThreadWatchEnd {
    ~ThreadWatchEnd() {
        {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            g_gateClosed = false;
        }
        g_threadChanged.notify_all();
        g_countThreaded = false;
        g_watchParks = false;
        g_threadExternal = 0;
        g_failNew = 0;
    }
};

void setGate(bool closed) {
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        g_gateClosed = closed;
    }
    g_threadChanged.notify_all();
}

VideoPipeline::DecodeHooks watchedHooks() {
    VideoPipeline::DecodeHooks hooks;
    hooks.around = [](const std::function<void()>& step) {
        g_decodeTid = gettid();
        t_exitMark.armed = true;
        ++g_aroundCalls;
        // Set before the gate, so the gate's own wait is not taken for the queue-full wait.
        g_inAround = true;
        struct Leave {
            ~Leave() { g_inAround = false; }
        } leave;
        {
            std::unique_lock<std::mutex> lock(g_threadMutex);
            if (g_gateClosed) {
                g_atGate = true;
                g_threadChanged.notify_all();
                g_threadChanged.wait(lock, [] { return !g_gateClosed; });
                g_atGate = false;
            }
        }
        step();
    };
    hooks.ready = [] {
        {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            ++g_readyCalls;
            if (gettid() == g_decodeTid.load() && g_inAround.load()) ++g_questions;
        }
        g_threadChanged.notify_all();
    };
    return hooks;
}

// Waits for `pred` (called under g_threadMutex); past the bound the wait is a failure named `what`.
template <class Pred>
bool waitThread(const std::string& label, const char* what, Pred pred, std::chrono::seconds bound = std::chrono::seconds(10)) {
    std::unique_lock<std::mutex> lock(g_threadMutex);
    if (g_threadChanged.wait_for(lock, bound, pred)) return true;
    std::fprintf(stderr, "FAIL: %s: %s (waited %lld s)\n", label.c_str(), what, static_cast<long long>(bound.count()));
    fail(what);
    return false;
}

int readyCalls() {
    std::lock_guard<std::mutex> lock(g_threadMutex);
    return g_readyCalls;
}

// The next picture through takeReady() alone, called again only after the ready hook was: the way the
// daemon's clock round uses it. Empty, with the reason failed, when the hook never comes.
std::optional<VideoPipeline::Decode> takeWhenReady(VideoPipeline& pipeline, wallpaperx::PictureSkip skip,
                                                   const std::string& label) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const int before = readyCalls();
        const VideoPipeline::Decode answer = pipeline.takeReady(skip);
        if (answer != VideoPipeline::Decode::Pending) return answer;
        if (!waitThread(label, "takeReady() answered Pending and the ready hook never followed",
                        [before] { return g_readyCalls > before; })) {
            return std::nullopt;
        }
    }
    std::fprintf(stderr, "FAIL: %s: 16 ready-hook rounds in a row brought no picture\n", label.c_str());
    fail("the ready hook kept bringing rounds that found no picture");
    return std::nullopt;
}

bool queuesShared(const Device& device, const std::string& label) {
    if (device.queuesInternallySynchronized()) return true;
    std::printf("%s: %s does not share its queues (no VK_KHR_internally_synchronized_queues), so no decode thread "
                "runs here -- not answerable\n",
                label.c_str(), device.info().gpuName.c_str());
    g_notVerified = true;
    return false;
}

constexpr const char* kThreadFixture = "counter_long.mp4";

// A pipeline on kThreadFixture whose first picture is decoded and whose decode thread has filled its
// queue and stopped. Null, with the reason reported, otherwise.
std::unique_ptr<VideoPipeline> filledPipeline(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label) || !queuesShared(device, label) || !haveAsset(kThreadFixture, label)) {
        return nullptr;
    }
    auto pipeline = std::make_unique<VideoPipeline>(device, asset(kThreadFixture), kShaderDir,
                                                    VideoPipeline::Playback::Once, std::nullopt, watchedHooks());
    g_watchParks = true;
    g_countThreaded = true;
    g_taken = 1;
    if (!pipeline->decodeNext()) {
        std::fprintf(stderr, "FAIL: %s: %s showed no first picture\n", label.c_str(), kThreadFixture);
        fail("the fixture's first picture was not decoded");
        return nullptr;
    }
    if (g_threadHostReceived != 0) {
        std::printf("%s: %s decoded in software here -- not answerable\n", label.c_str(), kThreadFixture);
        g_notVerified = true;
        return nullptr;
    }
    if (!waitThread(label, "the decode thread never stopped with its queue full", [] { return g_parks > 0; })) {
        return nullptr;
    }
    return pipeline;
}

// Empties the queue of a filledPipeline() and holds its decode thread at the gate, before the decode
// call that would refill it.
bool emptyAndHold(VideoPipeline& pipeline, const std::string& label) {
    setGate(true);
    for (int i = 0; i < 2; ++i) {
        ++g_taken;
        if (pipeline.takeReady(wallpaperx::PictureSkip::Nothing) != VideoPipeline::Decode::Decoded) {
            std::fprintf(stderr, "FAIL: %s: a full queue had no picture %d to take\n", label.c_str(), i + 1);
            fail("the harness could not empty a full queue");
            return false;
        }
    }
    return waitThread(label, "the decode thread never came back for its next decode", [] { return g_atGate; });
}

// Takes whatever is queued, however much that is, until the decode thread waits at the gate with nothing
// left to take: once it is there with the gate closed, nothing new can be queued.
bool drainAndHold(VideoPipeline& pipeline, const std::string& label) {
    setGate(true);
    for (int round = 0; round < 8; ++round) {
        ++g_taken;
        const VideoPipeline::Decode answer = pipeline.takeReady(wallpaperx::PictureSkip::Nothing);
        if (answer == VideoPipeline::Decode::Decoded) continue;
        --g_taken;
        if (answer != VideoPipeline::Decode::Pending) {
            std::fprintf(stderr, "FAIL: %s: the stream ended while the harness drained its queue\n", label.c_str());
            fail("the harness could not drain the decode thread's queue");
            return false;
        }
        bool held = false;
        {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            held = g_atGate;
        }
        if (held) {
            // Held before this Pending was answered or after: either way nothing was queued in between,
            // since the thread is held before its next decode -- take once more to be sure.
            ++g_taken;
            const VideoPipeline::Decode again = pipeline.takeReady(wallpaperx::PictureSkip::Nothing);
            if (again == VideoPipeline::Decode::Decoded) continue;
            --g_taken;
            return true;
        }
        if (!waitThread(label, "the decode thread never came back to the gate while its queue was drained",
                        [] { return g_atGate; })) {
            return false;
        }
    }
    std::fprintf(stderr, "FAIL: %s: 8 rounds of draining never found the queue empty with the thread held\n",
                 label.c_str());
    fail("the harness could not drain the decode thread's queue");
    return false;
}

void runDecodeDepthChecks(Device& device, const std::string& label) {
    resetThreadWatch();
    std::unique_ptr<VideoPipeline> pipeline = filledPipeline(device, label);
    const ThreadWatchEnd end;
    if (!pipeline) return;
    // GOAL: S15.882 -- the decode thread never holds more than 2 decoded pictures the caller has not taken.
    // WHY-THIS-SHAPE: counted in the pipeline, at every picture ffmpeg hands the decoder, against the pictures
    //       taken -- and taken only while the thread is parked on its queue-full wait (pthread_cond_wait from
    //       the decode thread outside a decode), so no receive can race a take. Inferring the depth from how
    //       long a take waits would be an assertion on the scheduler.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored: kDepth 2 -> 3 in renderer/video/decode_ahead.cpp
    //       -> "3 decoded pictures were untaken at once, above the 2 the decode thread may hold".
    // COVERAGE-ASSERT: 40 rounds each parked and took, and the most untaken seen must reach 2, or the
    //       queue never filled and the bound was never offered.
    constexpr int kRounds = 40;
    int seen = 0;
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        seen = g_parks;
    }
    int rounds = 0;
    for (; rounds < kRounds; ++rounds) {
        ++g_taken;
        if (pipeline->takeReady(wallpaperx::PictureSkip::Nothing) != VideoPipeline::Decode::Decoded) {
            std::fprintf(stderr, "FAIL: %s: round %d: the parked decode thread had no picture ready\n", label.c_str(),
                         rounds);
            fail("a decode thread waiting with a full queue had nothing to take");
            return;
        }
        if (!waitThread(label, "the decode thread did not stop again after a picture was taken",
                        [&seen] { return g_parks > seen; })) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_threadMutex);
        seen = g_parks;
    }
    const int most = g_maxUntaken.load();
    if (most > 2) {
        std::fprintf(stderr, "FAIL: %s: %d decoded pictures were untaken at once, above the 2 the decode thread "
                             "may hold\n",
                     label.c_str(), most);
        fail("the decode thread held more than 2 decoded, untaken pictures");
    } else if (most < 2) {
        std::fprintf(stderr, "FAIL: %s: at most %d untaken picture(s) seen in %d rounds\n", label.c_str(), most, rounds);
        fail("the decode thread's queue never filled, so its bound was not exercised");
    } else {
        std::printf("%s: %d rounds, at most %d decoded pictures untaken\n", label.c_str(), rounds, most);
    }
}

void runDecodePendingChecks(Device& device, const std::string& label) {
    resetThreadWatch();
    std::unique_ptr<VideoPipeline> pipeline = filledPipeline(device, label);
    const ThreadWatchEnd end;
    if (!pipeline || !emptyAndHold(*pipeline, label)) return;
    // GOAL: S15.883, first half -- with nothing decoded, takeReady() answers Pending without waiting for the
    //       decode thread.
    // WHY-THIS-SHAPE: the thread is held at a test-owned gate, so it cannot produce a picture until the probe
    //       opens it; a takeReady() that returns while the gate is still shut provably did not wait. A
    //       watchdog opens the gate after 5 s so a blocking takeReady() is reported, never hung on.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored: DecodeAhead::take() in renderer/video/decode_ahead.cpp
    //       waiting for an item instead of answering nothing -> "takeReady() with no picture decoded returned
    //       only after the watchdog released the decode thread".
    bool returned = false;
    bool watchdogOpened = false;
    std::optional<VideoPipeline::Decode> answer;
    {
        std::jthread watchdog([&returned, &watchdogOpened] {
            std::unique_lock<std::mutex> lock(g_threadMutex);
            if (g_threadChanged.wait_for(lock, std::chrono::seconds(5), [&returned] { return returned; })) return;
            watchdogOpened = true;
            g_gateClosed = false;
            g_threadChanged.notify_all();
        });
        struct Returned {
            bool& flag;
            ~Returned() {
                {
                    std::lock_guard<std::mutex> lock(g_threadMutex);
                    flag = true;
                }
                g_threadChanged.notify_all();
            }
        } mark{returned};
        answer = pipeline->takeReady(wallpaperx::PictureSkip::Nothing);
    }
    if (watchdogOpened) {
        std::fprintf(stderr, "FAIL: %s: takeReady() with no picture decoded returned only after the watchdog "
                             "released the decode thread\n",
                     label.c_str());
        fail("takeReady() waited for the decode thread");
        return;
    }
    if (answer != VideoPipeline::Decode::Pending) {
        std::fprintf(stderr, "FAIL: %s: takeReady() answered %d with the decode thread held\n", label.c_str(),
                     static_cast<int>(answer.value_or(VideoPipeline::Decode::Pending)));
        fail("takeReady() did not answer Pending with no picture decoded");
        return;
    }
    // GOAL: S15.883, second half -- the next decoded picture brings the round through the ready hook: no
    //       timer, and no takeReady() before the hook is called.
    // WHY-THIS-SHAPE: after Pending the probe calls nothing until the hook fires, so a picture that is
    //       decoded but never announced leaves the wait to fail by name; polling takeReady() here would pass
    //       with the hook broken.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored: `if (wake) notifyReady(shared);` deleted from
    //       DecodeAhead::run() (renderer/video/decode_ahead.cpp) -> "a decoded picture after Pending never called
    //       the ready hook (waited 10 s)".
    const int before = readyCalls();
    setGate(false);
    if (!waitThread(label, "a decoded picture after Pending never called the ready hook",
                    [before] { return g_readyCalls > before; })) {
        return;
    }
    ++g_taken;
    if (pipeline->takeReady(wallpaperx::PictureSkip::Nothing) != VideoPipeline::Decode::Decoded) {
        std::fprintf(stderr, "FAIL: %s: the round the ready hook brought found no picture\n", label.c_str());
        fail("the ready hook was called with no picture ready");
        return;
    }
    std::printf("%s: Pending while the thread was held; the ready hook brought the next picture\n", label.c_str());
}

void runDecodeFailureChecks(Device& device, const std::string& label) {
    resetThreadWatch();
    std::unique_ptr<VideoPipeline> pipeline = filledPipeline(device, label);
    const ThreadWatchEnd end;
    if (!pipeline || !emptyAndHold(*pipeline, label)) return;
    constexpr auto kAnswerDelay = std::chrono::milliseconds(1500);
    int questionsBefore = 0;
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        questionsBefore = g_questions;
    }
    g_threadExternal = 1;
    setGate(false);
    // GOAL: S15.884 -- a hardware failure on the decode thread puts its device-health question to the render
    //       thread (the ready hook, from inside the decode call) and the stream then goes on in software.
    // WHY-THIS-SHAPE: the failure is injected only on the decode thread, with the queue empty and the
    //       thread released from the gate, so the decode call that meets it is known. Asking the Device
    //       directly from that thread would abort on the Device's thread pin, so the hook is the only legal
    //       route; a question never announced leaves the thread waiting, which the bounded wait reports.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored: notifyReady() deleted from DecodeAhead::ask()
    //       (renderer/video/decode_ahead.cpp) -> "the decode thread's device-health question never reached the
    //       render thread (waited 10 s)".
    if (!waitThread(label, "the decode thread's device-health question never reached the render thread",
                    [questionsBefore] { return g_questions > questionsBefore; })) {
        return;
    }
    std::this_thread::sleep_for(kAnswerDelay);
    if (g_threadExternal != 0 || !g_inAround) {
        std::fprintf(stderr, "FAIL: %s: the question was %s\n", label.c_str(),
                     g_threadExternal != 0 ? "asked without the injected failure" : "abandoned before it was answered");
        fail("the delayed answer was not spent inside the failing decode call");
        return;
    }
    ++g_taken;
    const std::optional<VideoPipeline::Decode> first = takeWhenReady(*pipeline, wallpaperx::PictureSkip::Nothing, label);
    if (first != VideoPipeline::Decode::Decoded) {
        std::fprintf(stderr, "FAIL: %s: after the answered question the stream %s\n", label.c_str(),
                     first ? "ended" : "brought no picture");
        fail("playback stopped after a hardware failure on the decode thread");
        return;
    }
    const auto decodeTime = pipeline->timing().decodeTime;
    // GOAL: S15.887 -- the time the decode thread waits for the render thread's answer is not decode time.
    // WHY-THIS-SHAPE: the answer is held back a known 1.5 s (the takeReady() that answers comes only then,
    //       checked above to be while the failing call still waits), and the queue was empty, so the next
    //       picture is the one that call returned; a decodeTime of 1.5 s or more contains the wait. A
    //       software restart of this 320x240 file takes milliseconds.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored: `- m_asking` dropped from PictureSource::next()'s
    //       spend (renderer/video/picture_source.cpp) -> "carries 1511.9 ms of decodeTime, which contains the
    //       1500 ms the render thread took to answer"; restored it reads 6.7 ms.
    if (decodeTime >= kAnswerDelay) {
        std::fprintf(stderr, "FAIL: %s: the picture decoded across the question carries %.1f ms of decodeTime, "
                             "which contains the %lld ms the render thread took to answer\n",
                     label.c_str(), std::chrono::duration<double, std::milli>(decodeTime).count(),
                     static_cast<long long>(kAnswerDelay.count()));
        fail("time spent waiting for the device-health answer was counted as decode time");
    } else {
        std::printf("%s: decodeTime %.1f ms across a %lld ms answer\n", label.c_str(),
                    std::chrono::duration<double, std::milli>(decodeTime).count(),
                    static_cast<long long>(kAnswerDelay.count()));
    }
    // GOAL: S15.884's "goes on in software" -- every one of the next 30 pictures is a new software decode.
    // WHY-THIS-SHAPE: the starting count is taken with the queue drained and the thread held at the gate,
    //       so no picture counted before it can be one of the 30 taken after. Sampled straight after the
    //       first take, up to 2 already-queued pictures (the queue depth) fell on the wrong side, and the
    //       check came up short by exactly 2 in some runs.
    // CONTROL: test-side, 2026-10-19, NVIDIA, ThreadSanitizer build (where the slow caller exposes it): the
    //       drainAndHold() call deleted -> red in 5 of 8 runs, "5 software pictures at the first after the
    //       failure, 34 after 30 more"; restored, 8 of 8 green. The normal build stayed green either way (12 runs).
    if (!drainAndHold(*pipeline, label)) return;
    const int hostBefore = g_threadHostReceived.load();
    setGate(false);
    constexpr int kAfter = 30;
    for (int i = 0; i < kAfter; ++i) {
        ++g_taken;
        if (takeWhenReady(*pipeline, wallpaperx::PictureSkip::Nothing, label) != VideoPipeline::Decode::Decoded) {
            std::fprintf(stderr, "FAIL: %s: picture %d after the failure never came\n", label.c_str(), i + 2);
            fail("playback stopped after a hardware failure on the decode thread");
            return;
        }
    }
    if (hostBefore == 0 || g_threadHostReceived.load() - hostBefore < kAfter) {
        std::fprintf(stderr, "FAIL: %s: %d software pictures at the first after the failure, %d after %d more\n",
                     label.c_str(), hostBefore, g_threadHostReceived.load(), kAfter);
        fail("the stream did not go on in software after a hardware failure on the decode thread");
        return;
    }
    std::printf("%s: question answered on the render thread; %d more pictures in software\n", label.c_str(), kAfter);
}

void runDecodeCatchUpChecks(Device& device, const std::string& label) {
    resetThreadWatch();
    std::unique_ptr<VideoPipeline> pipeline = filledPipeline(device, label);
    const ThreadWatchEnd end;
    if (!pipeline) return;
    constexpr auto kLevel = wallpaperx::PictureSkip::Unreferenced;
    int inARow = 0;
    for (int takes = 0; inARow < 4; ++takes) {
        if (takes == 60) {
            std::fprintf(stderr, "FAIL: %s: 60 pictures taken at Unreferenced, none 4 in a row carrying it\n",
                         label.c_str());
            fail("the skip level asked of the decode thread never reached its pictures");
            return;
        }
        ++g_taken;
        if (takeWhenReady(*pipeline, kLevel, label) != VideoPipeline::Decode::Decoded) {
            fail("the fixture ended or stalled before the skip level settled");
            return;
        }
        inARow = pipeline->timing().skip == kLevel ? inARow + 1 : 0;
    }
    // GOAL: S15.886 -- a catch-up decodeNext(), which names no level, leaves the decode thread skipping at the
    //       level the clock last set.
    // WHY-THIS-SHAPE: 8 pictures through decodeNext(): the 2 queued and the one in flight were decoded at
    //       the level already, so a thread reset to Nothing shows from the fourth on. Graded on each
    //       picture's own recorded level, not on how many pictures were dropped.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored: DecodeAhead::wait() writing the level
    //       unconditionally, `skip.value_or(PictureSkip::Nothing)` (renderer/video/decode_ahead.cpp) -> "picture 4
    //       after a catch-up decodeNext() was decoded at skip level 0".
    for (int i = 0; i < 8; ++i) {
        ++g_taken;
        if (!pipeline->decodeNext()) {
            fail("the fixture ended during the catch-up");
            return;
        }
        if (pipeline->timing().skip != kLevel) {
            std::fprintf(stderr, "FAIL: %s: picture %d after a catch-up decodeNext() was decoded at skip level %d, "
                                 "not the Unreferenced the clock set\n",
                         label.c_str(), i + 1, static_cast<int>(pipeline->timing().skip));
            fail("a catch-up decodeNext() reset the decode thread's skip level");
            return;
        }
    }
    std::printf("%s: 8 catch-up pictures all decoded at the clock's level\n", label.c_str());
}

// `file` decoded through decodeNext() alone; no decode may run off the calling thread.
bool noDecodeThreadFor(Device& device, const std::string& label, const char* file, bool software) {
    resetThreadWatch();
    if (!haveAsset(file, label)) return false;
    VideoPipeline pipeline(device, asset(file), kShaderDir, VideoPipeline::Playback::Once, std::nullopt,
                           watchedHooks());
    const ThreadWatchEnd end;
    g_countThreaded = true;
    for (int i = 0; i < 30; ++i) {
        if (!pipeline.decodeNext()) {
            std::fprintf(stderr, "FAIL: %s: %s ended after %d pictures\n", label.c_str(), file, i);
            fail("the fixture is too short to grade");
            return false;
        }
    }
    const int received = g_threadReceived.load();
    const int host = g_threadHostReceived.load();
    if (software ? host != received : host != 0) {
        std::printf("%s: %s decoded %d of %d pictures in software, not the path this case needs -- not answerable\n",
                    label.c_str(), file, host, received);
        g_notVerified = true;
        return false;
    }
    if (g_aroundCalls != 0 || g_receivedOffGpuThread != 0) {
        std::fprintf(stderr, "FAIL: %s: %s: a decode thread ran (%d decodes through it, %d pictures received off the "
                             "calling thread)\n",
                     label.c_str(), file, g_aroundCalls.load(), g_receivedOffGpuThread.load());
        fail(software ? "a decode thread started for a stream decoded in software"
                      : "a decode thread started on a Device without internally synchronised queues");
        return false;
    }
    std::printf("%s: %s: %d pictures, all on the calling thread\n", label.c_str(), file, received);
    return true;
}

void runNoDecodeThreadChecks(Device& device, const std::string& label, bool software) {
    if (!hardwareDecoderPresent(device, label)) return;
    // GOAL: S15.885 -- no decode thread starts when the Device lacks VK_KHR_internally_synchronized_queues
    //       (hidden by the probe's interposer, asserted in runGraded), nor for a stream decoded in software.
    // WHY-THIS-SHAPE: a thread is seen two ways that need no production seam: the around hook, which only
    //       the decode thread calls, and the thread each picture was received on. The software case runs on
    //       a Device that DOES share its queues (an MPEG-2 file, which has no Vulkan decoder), then plays
    //       an H.264 file on the same Device and requires a thread there, so the instrument is shown able
    //       to see one.
    // CONTROL: via control_batch.sh, 2026-10-23, RTX 3080 Ti, bytes restored: decodeAheadIfSafe()'s `eligible` made
    //       true (renderer/video/video_pipeline.cpp) -> software entry "counter_onekey_m2.ts: a decode
    //       thread ran (32 decodes through it, 32 pictures received off the calling thread)". Without its
    //       queue-sharing test instead -> hidden entry "the child died by signal 6 (Aborted)": the decoder's own
    //       assertMayDecodeHere() pin aborts the first off-thread decode, so this check's message is never
    //       reached there; the forked child is what turns that into a verdict.
    if (!software) {
        noDecodeThreadFor(device, label, kThreadFixture, false);
        return;
    }
    if (!queuesShared(device, label)) return;
    if (!noDecodeThreadFor(device, label, "counter_onekey_m2.ts", true)) return;
    resetThreadWatch();
    VideoPipeline pipeline(device, asset(kThreadFixture), kShaderDir, VideoPipeline::Playback::Once, std::nullopt,
                           watchedHooks());
    const ThreadWatchEnd end;
    for (int i = 0; i < 6; ++i) {
        if (!pipeline.decodeNext()) break;
    }
    if (g_aroundCalls == 0) {
        std::fprintf(stderr, "FAIL: %s: no decode thread seen for %s on the same Device\n", label.c_str(),
                     kThreadFixture);
        fail("the probe cannot see a decode thread, so its absence above proves nothing");
    }
}

void releaseFfmpegHold() {
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        g_holdInFfmpeg = false;
    }
    g_threadChanged.notify_all();
}

// The case's own Device, held as the daemon holds it (OwnedDevice), so the deleter's choice is what is
// graded. The child it runs in ends by _exit, as the daemon does once a thread is left running.
int runLeftRunningCase(const std::string& label) {
    resetThreadWatch();
    g_holdInFfmpeg = false;
    g_heldInFfmpeg = false;
    g_decodeThreadGone = false;
    g_watchedDeviceFreed = false;
    wallpaperx::OwnedDevice device(new Device(/*enableValidation=*/true));
    std::printf("%s: on %s; not graded by the validation layer, since this case ends with the Device leaked "
                "on purpose\n",
                label.c_str(), device->info().gpuName.c_str());
    g_watchedDevice = device.get();
    const auto verdict = [] {
        if (wallpaperx::testing::failureCount() > 0) return 1;
        return g_notVerified ? kNotVerifiedExitCode : 0;
    };
    std::unique_ptr<VideoPipeline> pipeline = filledPipeline(*device, label);
    if (!pipeline) return verdict();
    // A destruction that waits with no bound would otherwise hang the child until it is killed.
    std::atomic<bool> watchdogDone{false};
    std::jthread watchdog([&watchdogDone] {
        std::unique_lock<std::mutex> lock(g_threadMutex);
        if (!g_threadChanged.wait_for(lock, std::chrono::seconds(30), [&] { return watchdogDone.load(); })) {
            g_holdInFfmpeg = false;
            g_threadChanged.notify_all();
        }
    });
    const auto stopWatchdog = [&watchdogDone] {
        {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            watchdogDone = true;
        }
        g_threadChanged.notify_all();
    };
    g_holdInFfmpeg = true;
    ++g_taken;
    if (pipeline->takeReady(wallpaperx::PictureSkip::Nothing) != VideoPipeline::Decode::Decoded) {
        std::fprintf(stderr, "FAIL: %s: a full queue had no picture to take\n", label.c_str());
        fail("the harness could not take a picture from a full queue");
        releaseFfmpegHold();
        stopWatchdog();
        return verdict();
    }
    if (!waitThread(label, "the decode thread never entered ffmpeg again after a picture was taken",
                    [] { return g_heldInFfmpeg.load(); })) {
        releaseFfmpegHold();
        stopWatchdog();
        return verdict();
    }
    const auto began = std::chrono::steady_clock::now();
    pipeline.reset();
    const double stopSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const bool stillHeld = g_holdInFfmpeg.load();
    const bool flagged = Device::threadLeftRunning();
    device.reset();
    const bool freed = g_watchedDeviceFreed.load();
    releaseFfmpegHold();
    stopWatchdog();
    // GOAL: S15.889, renderer half -- a decode thread still inside ffmpeg past DecodeAhead's 10 s stop bound
    //       is left running without the Device being freed under it: the stop returns after the bound,
    //       Device::threadLeftRunning() is set, the OwnedDevice's deleter does not free the Device, and the
    //       thread, let go afterwards, runs to its end.
    // WHY-THIS-SHAPE: held inside avcodec_receive_frame on the decode thread itself (the probe's interposer),
    //       not at the around gate, which is outside ffmpeg. The stop's duration is graded first (9-20 s), so
    //       a thread that was not really held, or a stop with no bound (the watchdog lets go at 30 s), fails
    //       by name instead of passing on a quick join. A freed Device is seen at operator delete; reading
    //       memory freed under the thread could pass unnoticed without a sanitizer.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored, each red alone: DeviceDeleter's
    //       threadLeftRunning() test removed (renderer/vulkan/device.cpp) -> "the Device was freed while its
    //       decode thread was still inside ffmpeg"; ~DecodeAhead calling recordLeak() instead of
    //       recordThreadLeftRunning() (renderer/video/decode_ahead.cpp) -> "Device::threadLeftRunning() is
    //       false".
    if (stopSeconds < 9.0 || stopSeconds > 20.0 || !stillHeld) {
        std::fprintf(stderr, "FAIL: %s: the pipeline's destruction took %.1f s with its decode thread %s inside "
                             "ffmpeg; the stop bound is 10 s\n",
                     label.c_str(), stopSeconds, stillHeld ? "held" : "no longer held");
        fail("the stop did not wait out its bound for a decode thread held inside ffmpeg");
    }
    if (!flagged) {
        std::fprintf(stderr, "FAIL: %s: Device::threadLeftRunning() is false after a decode thread was left inside "
                             "ffmpeg\n",
                     label.c_str());
        fail("a decode thread left running was not reported to the Device");
    }
// The condition that replaces operator delete below; its macro is defined only further down.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    if (freed) {
        std::fprintf(stderr, "FAIL: %s: the Device was freed while its decode thread was still inside ffmpeg\n",
                     label.c_str());
        fail("OwnedDevice freed a Device a thread left running still uses");
    }
#else
    static_cast<void>(freed);
    std::printf("%s: a sanitizer owns operator delete here, so the Device's free is not watched; the sanitizer "
                "reports a read of freed memory instead\n",
                label.c_str());
#endif
    if (waitThread(label, "the decode thread left running never finished once let go out of ffmpeg",
                   [] { return g_decodeThreadGone.load(); })) {
        std::printf("%s: stop took %.1f s; the Device was kept; the thread left running finished once let go\n",
                    label.c_str(), stopSeconds);
    }
    return verdict();
}

void runCompletedOffThreadChecks(Device& device, const std::string& label) {
    const auto offThread = [&device](Device::SubmitTicket ticket) {
        bool answer = false;
        std::thread([&] { answer = device.hasCompleted(ticket); }).join();
        return answer;
    };
    g_heldFences.clear();
    g_holdNextSubmission = true;
    const Device::SubmitTicket held = device.submit([](VkCommandBuffer) {});
    if (g_heldFences.empty()) {
        g_holdNextSubmission = false;
        std::fprintf(stderr, "FAIL: %s: the harness could not hold the batch's fence\n", label.c_str());
        fail("the harness could not hold a batch unfinished");
        return;
    }
    const Device::SubmitTicket later = device.submit([](VkCommandBuffer) {});
    device.wait(later);
    // The GPU thread looks: `later` retires, `held` stays pending, and the watermark moves.
    const bool laterDone = device.hasCompleted(later);
    // GOAL: S15.890 -- Device::hasCompleted() off the GPU thread never answers true for a batch the GPU has
    //       not finished, even after the GPU thread has retired a later one.
    // WHY-THIS-SHAPE: "not finished" is the driver's answer: the probe's vkGetFenceStatus reports the held
    //       batch's fence unsignalled, as for a GPU still running it. A batch submitted after it finishes and
    //       is retired on the GPU thread first, which is what moves the watermark an off-thread answer reads;
    //       asking with nothing retired would pass with a watermark that ignores what is still pending.
    // CONTROL: via control_batch.sh, 2026-10-23, RTX 3080 Ti, bytes restored, each red alone: hasCompleted()'s
    //       off-thread branch answering `true` before its marker test, and retireCompleted() setting the watermark to the
    //       newest serial whatever is pending (renderer/vulkan/device.cpp) -> "answered true off the GPU thread
    //       for a batch the GPU has not finished".
    // COVERAGE-ASSERT: once the batch is let go and the GPU thread has seen it finish, the off-thread
    //       answer must turn true, so the check is not met by an answer that is always false.
    if (!laterDone) {
        std::fprintf(stderr, "FAIL: %s: a batch waited for on the GPU thread still read unfinished there\n",
                     label.c_str());
        fail("the harness did not reach a retired later batch behind a held one");
        g_heldFences.clear();
        return;
    }
    if (offThread(held)) {
        std::fprintf(stderr, "FAIL: %s: hasCompleted() answered true off the GPU thread for a batch the GPU has not "
                             "finished\n",
                     label.c_str());
        fail("hasCompleted() off the GPU thread answered true early");
    }
    // After the off-thread question, so an early answer here cannot hide that one.
    if (device.hasCompleted(held)) {
        std::fprintf(stderr, "FAIL: %s: hasCompleted() answered true on the GPU thread for a batch the GPU has not "
                             "finished\n",
                     label.c_str());
        fail("hasCompleted() on the GPU thread answered true early");
    }
    g_heldFences.clear();
    device.wait(held);
    const bool heldDone = device.hasCompleted(held);
    if (!heldDone || !offThread(held)) {
        std::fprintf(stderr, "FAIL: %s: once finished and seen on the GPU thread (%s), the batch still read "
                             "unfinished off it\n",
                     label.c_str(), heldDone ? "finished" : "unfinished");
        fail("hasCompleted() off the GPU thread never answers true, so its false proves nothing");
        return;
    }
    std::printf("%s: off the GPU thread an unfinished batch read unfinished, and finished once retired\n",
                label.c_str());
}

// What vkCreateDevice and vkGetDeviceQueue2 were given, recorded while g_recordQueues is set (S15.891).
bool g_recordQueues = false;
bool g_createdSynchronized = false;
int g_queueCreates = 0;
int g_queueFetches = 0;
int g_unflaggedQueues = 0;  // printed and counted as each is seen
int g_plainFetches = 0;     // vkGetDeviceQueue, which carries no flags at all

void runQueueFlagChecks(Device& device, const std::string& label) {
    g_recordQueues = false;
    if (!hardwareDecoderPresent(device, label) || !queuesShared(device, label)) return;
    const int expected = 1 + static_cast<int>(device.videoDecodeFamilies().size());
    // GOAL: S15.891 -- every queue the Device creates and fetches carries
    //       VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR when it enables the extension.
    // WHY-THIS-SHAPE: seen at vkCreateDevice and vkGetDeviceQueue2 themselves (interposed), each queue
    //       graded as it passes, so a fetch that then fails Device construction still prints its own FAIL;
    //       queuesInternallySynchronized() alone reports the feature, not the flags on each queue.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored, each red alone: the create
    //       info's `flags = queueFlags` made 0 -> "created without the internally-synchronized flag"; the
    //       fetch's `info.flags = queueFlags` made 0 -> "fetched without the internally-synchronized flag"
    //       (both renderer/vulkan/device.cpp).
    // COVERAGE-ASSERT: the device must have been created with the extension, with at least one queue, and
    //       fetched at least 1 + its decode families' queues, so an interposer that saw nothing fails.
    if (!g_createdSynchronized || g_queueCreates < 1 || g_queueFetches < expected || g_plainFetches != 0) {
        std::fprintf(stderr, "FAIL: %s: saw %s, %d queue(s) created, %d fetched through vkGetDeviceQueue2 (%d "
                             "expected) and %d through vkGetDeviceQueue\n",
                     label.c_str(), g_createdSynchronized ? "the extension enabled" : "no extension enabled",
                     g_queueCreates, g_queueFetches, expected, g_plainFetches);
        fail("the queues the Device made were not all seen with their flags");
    }
    if (g_unflaggedQueues == 0) {
        std::printf("%s: %d queue(s) created and %d fetched, every one internally synchronised\n", label.c_str(),
                    g_queueCreates, g_queueFetches);
    }
}

// S15.892: with the decode thread on, the image pools a size change replaces are freed during playback,
// and none outlives the pipeline -- attributed by playback position, not by call.
void runThreadedPoolChecks(Device& device, const std::string& label, const SizeChangeFixture& fixture) {
    if (!hardwareDecoderPresent(device, label) || !queuesShared(device, label) || !haveAsset(fixture.file, label)) {
        return;
    }
    resetThreadWatch();
    resetPools();
    g_trackedPictures = 0;
    g_trackPools = true;
    g_decoderOpen = true;
    struct Untrack {
        ~Untrack() {
            g_trackPools = false;
            g_decoderOpen = false;
        }
    } untrack;
    auto pipeline = std::make_unique<VideoPipeline>(device, asset(fixture.file), kShaderDir,
                                                    VideoPipeline::Playback::Once, std::nullopt, watchedHooks());
    const ThreadWatchEnd end;
    if (!pipeline->decodeNext()) {
        std::fprintf(stderr, "FAIL: %s: %s showed no first picture\n", label.c_str(), fixture.file);
        fail("the fixture's first picture was not decoded");
        return;
    }
    int shown = 1;
    bool ended = false;
    for (int i = 0; i < 4000 && !ended; ++i) {
        const std::optional<VideoPipeline::Decode> answer =
            takeWhenReady(*pipeline, wallpaperx::PictureSkip::Nothing, label);
        if (!answer) return;
        if (*answer == VideoPipeline::Decode::Decoded) {
            // As the daemon's round does: each picture taken is converted, a submission whose retiring is
            // what tells an off-thread release that the GPU is done with a pool.
            pipeline->convert();
            ++shown;
        }
        if (*answer == VideoPipeline::Decode::Exhausted) ended = true;
    }
    if (!ended || g_aroundCalls == 0) {
        std::fprintf(stderr, "FAIL: %s: %s: %s after %d pictures, %d decode(s) on a thread\n", label.c_str(),
                     fixture.file, ended ? "ended" : "never ended", shown, g_aroundCalls.load());
        fail("the threaded case did not play the fixture to its end on a decode thread");
        return;
    }
    // The thread has handed on the end, after its last decode; nothing below races it.
    const int received = g_trackedPictures.load();
    g_decoderOpen = false;
    pipeline.reset();
    // GOAL: S15.892 (image pools) -- with the decode thread on, each pool a size change replaces is freed
    //       while the pipeline still plays, before its last picture is decoded, and no pool outlives the
    //       pipeline.
    // WHY-THIS-SHAPE: attributed by playback position (pictures the decoder had received when the pool was
    //       freed) and by whether the pipeline still existed, never by call: with the thread on, frees happen
    //       between calls. Read only after the stream's end was taken, which the thread hands on after its
    //       last decode, so no free races the read. Each taken picture is converted, as in the daemon: without
    //       a GPU-thread submission nothing moves the completion watermark an off-thread release reads, and
    //       every pool then read as freed only at destruction.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored: both releaseIdleFramesContexts()
    //       calls removed from VideoDecoder::decode() (renderer/video/video_decoder.cpp) -> "was not freed during
    //       playback".
    // COVERAGE-ASSERT: the fixture must make exactly its listed number of pools, or the replacement was not
    //       exercised.
    if (g_pools.size() != fixture.pools) {
        std::fprintf(stderr, "FAIL: %s: %s made %zu image pool(s), %zu expected\n", label.c_str(), fixture.file,
                     g_pools.size(), fixture.pools);
        fail("the size change did not make the pools it should");
        return;
    }
    int alive = 0;
    for (size_t i = 0; i < g_pools.size(); ++i) {
        const PoolRecord& pool = g_pools[i];
        if (!pool.freed) ++alive;
        if (i + 1 == g_pools.size()) continue;
        if (!pool.freed || !pool.freedWhileOpen || pool.freedAfterPictures >= received) {
            std::fprintf(stderr, "FAIL: %s: %s: pool %zu of %zu (%dx%d, %d pictures in it, freed after %d of %d "
                                 "received, on the %s thread) was not freed during playback: %s\n",
                         label.c_str(), fixture.file, i + 1, g_pools.size(), pool.width, pool.height, pool.pictures,
                         pool.freedAfterPictures, received, pool.freedOnMainThread ? "main" : "decode",
                         !pool.freed            ? "never freed"
                         : !pool.freedWhileOpen ? "freed only when the pipeline was destroyed"
                                                : "freed only after the last picture was decoded");
            fail("a replaced image pool was not freed during playback with the decode thread on");
        }
    }
    if (alive != 0) {
        std::fprintf(stderr, "FAIL: %s: %s: %d image pool(s) alive after the pipeline was destroyed\n",
                     label.c_str(), fixture.file, alive);
        fail("an image pool outlived the pipeline");
    }
    if (alive == 0) {
        std::printf("%s: %s: %d pictures, %zu pools, replaced ones freed during playback\n", label.c_str(),
                    fixture.file, shown, g_pools.size());
    }
}

// A threaded playback read back picture by picture, and what the decode thread held when it failed.
struct ThreadedPlay {
    std::vector<int> shown;
    bool firstStartsPass = false;
    int queued = -1;     // decoded on the thread and not yet taken when the failure was armed
    bool consumed = false;
    int hostAfter = 0;   // software pictures the thread received after the failure
    int threadDecodes = 0;
};

// Plays `file` (from `startAt`) with the decode thread on, taking each picture as the daemon's round does
// and reading it back. With `failAt` >= 0, before taking picture `failAt` it waits until the thread holds
// 2 untaken pictures -- it can decode nothing more until a take -- and arms one hardware failure on it, so
// the failure meets the thread's next decode with the queue known. `strip` removes the timestamps of every
// picture the thread decodes before the failure; the thread is held at the gate until that is set.
std::optional<ThreadedPlay> playThreaded(Device& device, const std::string& label, const std::string& what,
                                         const std::string& file, std::optional<wallpaperx::StreamPosition> startAt,
                                         int failAt, bool strip, int total) {
    resetThreadWatch();
    g_onThreadReceived = 0;
    g_onThreadHost = 0;
    g_countOnThread = true;
    struct Clear {
        ~Clear() {
            g_countOnThread = false;
            clearFailingCallConditions();
        }
    } clear;
    ThreadedPlay run;
    std::string failure;
    try {
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
        VideoPipeline pipeline(device, file, kShaderDir, VideoPipeline::Playback::Loop, startAt, watchedHooks());
        const ThreadWatchEnd end;
        g_watchParks = true;
        const auto readBack = [&] {
            pipeline.draw(targets, target, wallpaperx::Placement{});
            run.shown.push_back(wallpaperx::testing::counterFromBgra(
                wallpaperx::testing::reclaimAndReadback(device, targets, target), target.width, target.height));
        };
        setGate(true);
        if (!pipeline.decodeNext()) {
            failure = "showed no first picture";
        } else {
            run.firstStartsPass = pipeline.timing().startsPass;
            if (strip) {
                g_inFailingCall = true;
                g_stripTimestamps = true;
            }
            setGate(false);
            readBack();
            int taken = 0;  // pictures taken from the thread
            int hostBefore = 0;
            for (int i = 1; i < total && failure.empty(); ++i) {
                if (i == failAt) {
                    if (!waitThread(label, "the decode thread never filled its queue before the failure was armed",
                                    [&taken] { return g_onThreadReceived.load() - taken >= 2; })) {
                        return std::nullopt;
                    }
                    run.queued = g_onThreadReceived.load() - taken;
                    g_stripTimestamps = false;
                    hostBefore = g_onThreadHost.load();
                    g_threadExternal = 1;
                }
                const std::optional<VideoPipeline::Decode> answer =
                    takeWhenReady(pipeline, wallpaperx::PictureSkip::Nothing, label);
                if (!answer) return std::nullopt;
                if (*answer != VideoPipeline::Decode::Decoded) {
                    failure = "the stream ended after " + std::to_string(run.shown.size()) + " pictures";
                    break;
                }
                ++taken;
                readBack();
            }
            run.consumed = failAt >= 0 && g_threadExternal.load() == 0;
            run.hostAfter = g_onThreadHost.load() - hostBefore;
            run.threadDecodes = g_aroundCalls.load();
        }
    } catch (const std::exception& e) {
        failure = std::string("threw: ") + e.what();
    }
    if (failure.empty()) return run;
    std::fprintf(stderr, "FAIL: %s: %s: %s (showed %s)\n", label.c_str(), what.c_str(), failure.c_str(),
                 joinIds(run.shown).c_str());
    fail("a threaded playback did not play the pictures it was asked for");
    return std::nullopt;
}

// ------------------------------------------------------------------------- //
// The read before an FLV start, on the decode thread
// ------------------------------------------------------------------------- //

// While g_countStartReads is set, av_read_frame calls are counted by the thread making them; the decode
// thread's reads may be slowed, and its g_startReadFailAt-th read may return g_startReadFailErr instead.
std::atomic<bool> g_countStartReads{false};
std::atomic<int> g_startReadsGpu{0};
std::atomic<int> g_startReadsThread{0};
std::atomic<int> g_startReadDelayMs{0};
std::atomic<int> g_startReadFailAt{0};
std::atomic<int> g_startReadFailErr{0};
std::atomic<bool> g_startReadFailed{false};

void resetStartReads() {
    g_startReadsGpu = 0;
    g_startReadsThread = 0;
    g_startReadDelayMs = 0;
    g_startReadFailAt = 0;
    g_startReadFailErr = 0;
    g_startReadFailed = false;
}

struct StartReadsEnd {
    ~StartReadsEnd() {
        g_countStartReads = false;
        resetStartReads();
    }
};

constexpr const char* kStartReadFixture = "counter_ps_gap.flv";

// The pictures shown by takeReady() rounds from a pipeline opened at `start`; throws what the pipeline threw.
std::vector<int> takeShown(Device& device, VideoPipeline& pipeline, const std::string& label, size_t count) {
    OffscreenTargetManager targets(device);
    const OffscreenTarget& target = targets.create(1, kTargetWidth, kTargetHeight);
    std::vector<int> shown;
    for (size_t i = 0; i < count; ++i) {
        const std::optional<VideoPipeline::Decode> answer = takeWhenReady(pipeline, wallpaperx::PictureSkip::Nothing, label);
        if (answer != VideoPipeline::Decode::Decoded) break;
        pipeline.convert();
        pipeline.draw(targets, target, wallpaperx::Placement{});
        shown.push_back(wallpaperx::testing::counterFromBgra(
            wallpaperx::testing::reclaimAndReadback(device, targets, target), target.width, target.height));
    }
    return shown;
}

// Whether `shown` is `count` pictures of the stream, consecutive and in order, from any point.
bool consecutiveInStream(const std::vector<int>& shown, const std::vector<int>& ids, size_t count) {
    if (shown.size() != count) return false;
    const auto at = std::find(ids.begin(), ids.end(), shown.front());
    return at != ids.end() && static_cast<size_t>(ids.end() - at) >= count && std::equal(shown.begin(), shown.end(), at);
}

void runStartReadChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label) || !queuesShared(device, label)) return;
    const std::optional<CounterReference> reference = counterReference(label, kStartReadFixture);
    if (!reference) return;
    const std::vector<int>& ids = reference->ids;
    // Far enough in that the read before it is long, and past the parameter-set change at 120.
    const size_t start = ids.size() >= 40 ? ids.size() - 20 : 0;
    if (start < 200) {
        std::fprintf(stderr, "FAIL: %s: %s has %zu pictures, too few for a long read before a start\n", label.c_str(),
                     kStartReadFixture, ids.size());
        fail("the start-read fixture is too short");
        return;
    }
    const std::optional<wallpaperx::StreamPosition> position =
        positionAt(device, label, kStartReadFixture, start, ids[start]);
    if (!position) return;
    const std::vector<int> due(ids.begin() + static_cast<std::ptrdiff_t>(start),
                               ids.begin() + static_cast<std::ptrdiff_t>(start + kFollowing));
    const std::string where = std::string(kStartReadFixture) + " opened at picture " + std::to_string(ids[start]);

    // GOAL: S15.937 -- an FLV start part way in reads the file before the start on the decode thread: the
    //       calling thread's takeReady() makes no read and does not wait for it, and the pictures from the start
    //       are the start's own, in order. False if the read runs on the calling thread while a thread exists.
    // WHY-THIS-SHAPE: the decode thread is held at a test-owned gate from before its first step, so a
    //       takeReady() that answers Pending with the gate shut provably did not wait; reads are counted by the
    //       thread that makes them, in av_read_frame, so "not on the calling thread" is counted, not timed. A
    //       watchdog opens the gate after 5 s so a blocking takeReady() is reported, never hung on.
    // CONTROL: via control_batch.sh, 2026-10-22, RTX 3080 Ti, bytes restored: renderer/video/video_pipeline.cpp
    //       decodeAheadIfSafe() without `|| decoder.startPending()` -> "the start's read ran on the calling thread:
    //       281 packet(s) read inside takeReady(), 0 on the decode thread".
    // COVERAGE-ASSERT: the decode thread itself made at least half as many reads as the start's position.
    {
        resetThreadWatch();
        resetStartReads();
        const StartReadsEnd reads;
        std::vector<int> shown;
        std::string threw;
        bool watchdogOpened = false;
        std::optional<VideoPipeline::Decode> first;
        try {
            setGate(true);
            auto pipeline = std::make_unique<VideoPipeline>(device, asset(kStartReadFixture), kShaderDir,
                                                            VideoPipeline::Playback::Loop, position, watchedHooks());
            const ThreadWatchEnd end;
            g_countStartReads = true;
            bool returned = false;
            {
                std::jthread watchdog([&returned, &watchdogOpened] {
                    std::unique_lock<std::mutex> lock(g_threadMutex);
                    if (g_threadChanged.wait_for(lock, std::chrono::seconds(5), [&returned] { return returned; })) {
                        return;
                    }
                    watchdogOpened = true;
                    g_gateClosed = false;
                    g_threadChanged.notify_all();
                });
                struct Returned {
                    bool& flag;
                    ~Returned() {
                        {
                            std::lock_guard<std::mutex> lock(g_threadMutex);
                            flag = true;
                        }
                        g_threadChanged.notify_all();
                    }
                } mark{returned};
                first = pipeline->takeReady(wallpaperx::PictureSkip::Nothing);
            }
            setGate(false);
            if (first == VideoPipeline::Decode::Pending) shown = takeShown(device, *pipeline, label, kFollowing);
        } catch (const std::exception& e) {
            threw = e.what();
        }
        if (g_startReadsGpu.load() != 0) {
            std::fprintf(stderr, "FAIL: %s: %s: the start's read ran on the calling thread: %d packet(s) read inside "
                                 "takeReady(), %d on the decode thread\n",
                         label.c_str(), where.c_str(), g_startReadsGpu.load(), g_startReadsThread.load());
            fail("the read before an FLV start ran on the calling thread while a decode thread could exist");
        } else if (watchdogOpened || first != VideoPipeline::Decode::Pending) {
            std::fprintf(stderr, "FAIL: %s: %s: the first takeReady() with the decode thread held answered %d%s\n",
                         label.c_str(), where.c_str(), static_cast<int>(first.value_or(VideoPipeline::Decode::Pending)),
                         watchdogOpened ? ", only after the watchdog released the thread" : "");
            fail("takeReady() waited for the read before an FLV start");
        } else if (!threw.empty() || shown != due) {
            std::fprintf(stderr, "FAIL: %s: %s: showed %s where %s was due%s%s\n", label.c_str(), where.c_str(),
                         joinIds(shown).c_str(), joinIds(due).c_str(), threw.empty() ? "" : "; threw: ", threw.c_str());
            fail("the pictures from an FLV start read on the decode thread were not the start's");
        } else if (g_startReadsThread.load() < static_cast<int>(start / 2)) {
            std::fprintf(stderr, "FAIL: %s: %s: the decode thread read only %d packet(s)\n", label.c_str(),
                         where.c_str(), g_startReadsThread.load());
            fail("the decode thread did not make the read before the start, so nothing here graded it");
        } else {
            std::printf("%s: %s: takeReady() answered Pending with the thread held; 0 reads on the calling thread, %d "
                        "on the decode thread; showed %s\n",
                        label.c_str(), where.c_str(), g_startReadsThread.load(), joinIds(shown).c_str());
        }
    }

    // GOAL: S15.939 -- a read error before the start position does not refuse the wallpaper: playback goes on,
    //       showing the stream's own pictures in order. Out of memory in the same read is still raised as
    //       ResourceExhausted.
    // WHY-THIS-SHAPE: the error is injected at the decode thread's 150th read, inside the read before the start
    //       (position 280) and after the parameter-set change at 120, so a start that proceeds can still be
    //       graded picture by picture; injecting before 120 would make a right start depend on what the map
    //       missed, which is not this goal. One-shot, so only the read before the start meets it.
    // CONTROL: via control_batch.sh, 2026-10-22, RTX 3080 Ti, bytes restored, renderer/video/video_decoder.cpp
    //       mapExtradataUpTo(): any error thrown (throwFileFailure) instead of ending the map -> "a read error before
    //       the start refused the wallpaper ... threw 'VideoDecoder: reading before the start (Input/output error)";
    //       its ENOMEM line deleted -> "out of memory in the read before the start was not raised (showed 0 1 2 3 4".
    // COVERAGE-ASSERT: the injection was consumed in each case.
    // GOAL: S15.941 -- after the I/O error, whose damage (read 150) comes after the file's only extradata change
    //       (picture 120), the start lands at the position: every picture from it to the end of the file, and the
    //       whole next pass, right. The completed read's start is the block above. False if it begins at picture 0.
    // WHY-THIS-SHAPE: graded through the next pass, not only kFollowing pictures, because the map left short by the
    //       error must not mislead the following pass either; "consecutive from anywhere" (S15.939's own grade)
    //       passed the start-over at picture 0.
    // CONTROL: control_batch.sh 2026-10-23, RTX 3080 Ti, restored, video_pipeline_decode_thread_start_read,
    //       renderer/video/video_decoder.cpp mapExtradataUpTo() never setting `assumedThrough` -> "an I/O error at
    //       read 150: did not land at the position: showed 0 1 2 3 4, ... first difference at 0".
    const std::vector<int> throughNextPass = [&] {
        std::vector<int> v(ids.begin() + static_cast<std::ptrdiff_t>(start), ids.end());
        v.insert(v.end(), ids.begin(), ids.end());
        return v;
    }();
    for (const int err : {AVERROR(EIO), AVERROR(ENOMEM)}) {
        const bool memory = err == AVERROR(ENOMEM);
        const std::string what = where + (memory ? ", out of memory" : ", an I/O error") + " at read 150";
        resetThreadWatch();
        resetStartReads();
        const StartReadsEnd reads;
        g_startReadFailAt = 150;
        g_startReadFailErr = err;
        g_countStartReads = true;
        std::vector<int> shown;
        std::string threw;
        bool exhausted = false;
        try {
            VideoPipeline pipeline(device, asset(kStartReadFixture), kShaderDir, VideoPipeline::Playback::Loop,
                                   position, watchedHooks());
            const ThreadWatchEnd end;
            shown = takeShown(device, pipeline, label, memory ? kFollowing : throughNextPass.size());
        } catch (const wallpaperx::ResourceExhausted& e) {
            threw = e.what();
            exhausted = true;
        } catch (const std::exception& e) {
            threw = e.what();
        }
        if (!g_startReadFailed.load()) {
            std::fprintf(stderr, "FAIL: %s: %s: the injection was never reached (%d reads on the decode thread)\n",
                         label.c_str(), what.c_str(), g_startReadsThread.load());
            fail("the injected read failure never reached the read before the start");
        } else if (memory && !exhausted) {
            std::fprintf(stderr, "FAIL: %s: %s: out of memory in the read before the start was not raised (showed %s; "
                                 "threw '%s')\n",
                         label.c_str(), what.c_str(), joinIds(shown).c_str(), threw.c_str());
            fail("out of memory in the read before an FLV start was not raised");
        } else if (!memory && (!threw.empty() || shown.size() < kFollowing ||
                               !consecutiveInStream(std::vector<int>(shown.begin(), shown.begin() + kFollowing), ids,
                                                    kFollowing))) {
            std::fprintf(stderr, "FAIL: %s: %s: a read error before the start refused the wallpaper or broke the "
                                 "start: showed %s where %s was due; threw '%s'\n",
                         label.c_str(), what.c_str(), joinIds(shown).c_str(), joinIds(due).c_str(), threw.c_str());
            fail("a read error before an FLV start refused the wallpaper");
        } else if (memory) {
            std::printf("%s: %s: raised ResourceExhausted: %s\n", label.c_str(), what.c_str(), threw.c_str());
        } else if (shown != throughNextPass) {
            size_t first = 0;
            while (first < shown.size() && shown[first] == throughNextPass[first]) ++first;
            std::fprintf(stderr, "FAIL: %s: %s: did not land at the position: showed %s, %zu of %zu, first difference "
                                 "at %zu, where %s was due\n",
                         label.c_str(), what.c_str(),
                         joinIds(std::vector<int>(shown.begin(), shown.begin() + std::min<size_t>(shown.size(), 5))).c_str(),
                         shown.size(), throughNextPass.size(), first, joinIds(due).c_str());
            fail("a start whose read failed after the last extradata change did not land at its position");
        } else {
            std::printf("%s: %s: not refused; landed at the position, %zu pictures right through the next pass\n",
                        label.c_str(), what.c_str(), shown.size());
        }
    }

    // GOAL: S15.938 -- destroying the pipeline during that read stops it promptly, well under the 10 s stop
    //       bound, leaves no decode thread running and no Device marked as having one.
    // WHY-THIS-SHAPE: the decode thread's reads are slowed to 10 ms each, in the probe's av_read_frame, so the
    //       read before the start takes about 2.8 s and destruction provably lands inside it (20 reads made).
    //       Asserting "no read after the destructor began" would be a scheduler assertion, so the check is that
    //       fewer than half of the reads still due were made, and destruction took under 1 s: a read that
    //       ignores the stop makes all of them and takes over 2 s. Last in the entry, because a control that
    //       abandons the thread leaves it reading.
    // CONTROL: via control_batch.sh, 2026-10-22, RTX 3080 Ti, bytes restored: renderer/video/video_decoder.cpp
    //       mapExtradataUpTo()'s stop check made `false &&` -> "made 261 more read(s) of the 259 still due and took
    //       2644.3 ms"; renderer/video/decode_ahead.cpp kStopBound made 1 ms -> "left the decode thread running
    //       (thread still running, Device marked: yes)".
    // COVERAGE-ASSERT: at least 100 reads were still due when destruction began.
    {
        resetThreadWatch();
        resetStartReads();
        const StartReadsEnd reads;
        {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            g_decodeThreadGone = false;
        }
        if (wallpaperx::Device::threadLeftRunning()) {
            std::printf("%s: a thread was already left running in this process -- not answerable\n", label.c_str());
            g_notVerified = true;
            return;
        }
        g_startReadDelayMs = 10;
        g_countStartReads = true;
        std::unique_ptr<VideoPipeline> pipeline;
        try {
            pipeline = std::make_unique<VideoPipeline>(device, asset(kStartReadFixture), kShaderDir,
                                                       VideoPipeline::Playback::Loop, position, watchedHooks());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: %s: %s: opening the pipeline threw: %s\n", label.c_str(), where.c_str(), e.what());
            fail("a pipeline opened at an FLV position threw");
            return;
        }
        if (!waitThread(label, "the decode thread never made 20 reads of the read before the start",
                        [] { return g_startReadsThread.load() >= 20; })) {
            return;
        }
        const int before = g_startReadsThread.load();
        const auto began = std::chrono::steady_clock::now();
        pipeline.reset();
        const auto took = std::chrono::steady_clock::now() - began;
        const int after = g_startReadsThread.load() - before;
        const int due = static_cast<int>(start) - before;
        bool gone = false;
        {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            gone = g_decodeThreadGone;
        }
        const double ms = std::chrono::duration<double, std::milli>(took).count();
        if (due < 100) {
            std::fprintf(stderr, "FAIL: %s: %s: only %d reads were still due when destruction began\n", label.c_str(),
                         where.c_str(), due);
            fail("destruction did not land inside a long read, so the stop was not exercised");
        } else if (!gone || wallpaperx::Device::threadLeftRunning()) {
            std::fprintf(stderr, "FAIL: %s: %s: destruction during the start's read left the decode thread running "
                                 "(thread %s, Device marked: %s)\n",
                         label.c_str(), where.c_str(), gone ? "exited" : "still running",
                         wallpaperx::Device::threadLeftRunning() ? "yes" : "no");
            fail("destroying a pipeline during the read before an FLV start left its thread running");
        } else if (after * 2 >= due || took >= std::chrono::seconds(1)) {
            std::fprintf(stderr, "FAIL: %s: %s: destruction during the start's read made %d more read(s) of the %d "
                                 "still due and took %.1f ms\n",
                         label.c_str(), where.c_str(), after, due, ms);
            fail("destroying a pipeline did not stop the read before an FLV start");
        } else {
            std::printf("%s: %s: destroyed after %d reads: %d more read(s) of %d due, %.2f ms, thread exited\n",
                        label.c_str(), where.c_str(), before, after, due, ms);
        }
    }
}

void runThreadedResumeChecks(Device& device, const std::string& label) {
    if (!hardwareDecoderPresent(device, label) || !queuesShared(device, label)) return;
    const std::optional<CounterReference> full = counterReference(label, "counter_long.mp4");
    const std::optional<CounterReference> cut = counterReference(label, "counter_long_cut.mp4");
    if (!full || !cut) return;
    const std::vector<int>& ids = full->ids;
    constexpr size_t kAfter = 5;
    int graded = 0;

    // GOAL: S15.892 (hardware failure, decode thread on) -- a failure injected on the decode thread reaches the
    //       decoder, and the pictures shown across it are exactly the 2 the thread had decoded and queued at
    //       the failure, then the resume: for a stream with timestamps, the picture after the last queued, each
    //       once, in order; for a start at 150 whose pictures lost their timestamps (S15.172), the start's own
    //       picture. False if a queued picture is lost or repeated, or anything else precedes the resume.
    // WHY-THIS-SHAPE: the failure is armed only once the thread holds 2 untaken pictures, counted at the
    //       decoder on that thread, so which decode meets it is known rather than raced, and the expected order
    //       is built from that measured count. Grading only "the sequence is the stream's" (gradeSequence)
    //       would call the stripped case's 171 172 150 a defect, and a check of the resume alone would miss a
    //       lost queued picture.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored, each alone: DecodeAhead::ask()
    //       discarding the queued pictures (renderer/video/decode_ahead.cpp) -> "failing after 10 pictures:
    //       showed 0 1 ... 10 12 13 ... where 0 1 ... 10 11 12 ... was due (2 queued at the failure)";
    //       VideoDecoder::admit()'s DropThrough test `<=` made `<` (renderer/video/video_decoder.cpp) -> "showed
    //       ... 10 11 11 12 ..."; seekToResume()'s re-run from the start's position made `if (false)` (same
    //       file) -> "... 171 172 23 24 25 ... where ... 171 172 150 151 ...".
    // COVERAGE-ASSERT: the failure was consumed, software decoded the pictures after it on the thread, the
    //       queue held 2 when it was armed, and both cases were graded.
    struct Case {
        const char* what;
        size_t start;
        bool strip;
        int failAt;
    };
    for (const Case& c : {Case{"counter_long.mp4, failing after 10 pictures", 0, false, 10},
                          Case{"counter_long.mp4 opened at picture 150, its pictures losing their timestamps, failing "
                               "after 21",
                               150, true, 21}}) {
        const size_t failAt = static_cast<size_t>(c.failAt);
        if (c.start + failAt + 2 + kAfter > ids.size()) {
            fail("counter_long.mp4 is too short for the threaded failure case");
            continue;
        }
        std::optional<wallpaperx::StreamPosition> position;
        if (c.start != 0) {
            position = positionAt(device, label, "counter_long.mp4", c.start, ids[c.start]);
            if (!position) continue;
        }
        const int total = c.failAt + 2 + static_cast<int>(kAfter);
        const std::optional<ThreadedPlay> run =
            playThreaded(device, label, c.what, asset("counter_long.mp4"), position, c.failAt, c.strip, total);
        if (!run) continue;
        if (!run->consumed || run->hostAfter == 0 || run->queued != 2) {
            std::fprintf(stderr, "FAIL: %s: %s: failure %s, %d software picture(s) after it on the thread, %d queued "
                                 "when it was armed\n",
                         label.c_str(), c.what, run->consumed ? "consumed" : "never reached", run->hostAfter,
                         run->queued);
            fail("the hardware failure on the decode thread was not exercised with a full queue");
            continue;
        }
        ++graded;
        const size_t queued = static_cast<size_t>(run->queued);
        std::vector<int> due(ids.begin() + static_cast<long>(c.start),
                             ids.begin() + static_cast<long>(c.start + failAt + queued));
        const size_t resume = c.strip ? c.start : c.start + failAt + queued;
        due.insert(due.end(), ids.begin() + static_cast<long>(resume),
                   ids.begin() + static_cast<long>(resume + kAfter));
        if (run->shown != due) {
            std::fprintf(stderr, "FAIL: %s: %s: showed %s where %s was due (%zu queued at the failure)\n",
                         label.c_str(), c.what, joinIds(run->shown).c_str(), joinIds(due).c_str(), queued);
            fail("the pictures shown across a hardware failure on the decode thread are not the queued ones and then "
                 "the resume");
        } else {
            std::printf("%s: %s: showed %s; %zu queued at the failure, %d software pictures after it\n",
                        label.c_str(), c.what, joinIds(run->shown).c_str(), queued, run->hostAfter);
        }
    }

    // GOAL: S15.892 / S15.173 with the decode thread on -- a position past the end of the file (picture 300 of
    //       counter_long.mp4 applied to its 100-picture cut) starts at the file's first picture, as a new pass,
    //       and the pictures the thread decodes after it follow in order.
    // WHY-THIS-SHAPE: 30 pictures, not start-at-device's 5, so most are the thread's; the thread having
    //       decoded is required, or the case is the inline one start-at-device already grades.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored: in renderer/video/video_decoder.cpp
    //       next(), `if (m_startedAfter && m_returned.count == 0) { restart(); continue; }` deleted -> "counter_long_cut.mp4 opened at
    //       picture 300 of counter_long.mp4: showed no first picture".
    constexpr int kPastEnd = 30;
    const std::optional<wallpaperx::StreamPosition> past = positionAt(device, label, "counter_long.mp4", 300, ids[300]);
    if (past && cut->ids.size() >= static_cast<size_t>(kPastEnd) && cut->ids.size() < 300) {
        const std::string what = "counter_long_cut.mp4 opened at picture 300 of counter_long.mp4";
        const std::optional<ThreadedPlay> run =
            playThreaded(device, label, what, asset("counter_long_cut.mp4"), past, -1, false, kPastEnd);
        if (run) {
            const std::vector<int> due(cut->ids.begin(), cut->ids.begin() + kPastEnd);
            if (run->threadDecodes == 0) {
                std::fprintf(stderr, "FAIL: %s: %s: no picture was decoded on a decode thread\n", label.c_str(),
                             what.c_str());
                fail("the past-the-end start was not played on a decode thread");
            } else if (run->shown != due || !run->firstStartsPass) {
                std::fprintf(stderr, "FAIL: %s: %s: showed %s (first marked %s) where %s was due\n", label.c_str(),
                             what.c_str(), joinIds(run->shown).c_str(), run->firstStartsPass ? "yes" : "no",
                             joinIds(due).c_str());
                fail("a position past the end of the file starts at its first picture, as a new pass, with the "
                     "decode thread on");
            } else {
                ++graded;
                std::printf("%s: %s: %d pictures in order from the first, %d decodes on the thread\n", label.c_str(),
                            what.c_str(), kPastEnd, run->threadDecodes);
            }
        }
    } else if (past) {
        fail("counter_long_cut.mp4 does not fit the past-the-end case");
    }
    check(graded == 3, "every threaded resume case was graded");
}

void runDecodeStartShortageChecks(Device& device, const std::string& label, int failure);

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
    if ((g_hideSynchronizedQueues || g_hideVideoMaintenance2) && layerName == nullptr) {
        uint32_t all = 0;
        if (const VkResult r = real(physicalDevice, nullptr, &all, nullptr); r != VK_SUCCESS) return r;
        std::vector<VkExtensionProperties> kept(all);
        if (const VkResult r = real(physicalDevice, nullptr, &all, kept.data()); r < 0) return r;
        kept.resize(all);
        std::erase_if(kept, [](const VkExtensionProperties& e) {
            return (g_hideSynchronizedQueues &&
                    std::strcmp(e.extensionName, VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME) == 0) ||
                   (g_hideVideoMaintenance2 && std::strcmp(e.extensionName, VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME) == 0) ||
                   (g_hideVideoExtensions && std::strncmp(e.extensionName, "VK_KHR_video", 12) == 0);
        });
        if (properties == nullptr) {
            *count = static_cast<uint32_t>(kept.size());
            return VK_SUCCESS;
        }
        const uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(kept.size()));
        std::copy_n(kept.begin(), n, properties);
        *count = n;
        return n < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
    }
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
extern "C" VkResult vkCreateImageView(VkDevice d, const VkImageViewCreateInfo* i, const VkAllocationCallbacks* a,
                                      VkImageView* o) {
    static const auto real =
        realSymbol<VkResult (*)(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*)>(
            "vkCreateImageView");
    if (g_countVulkan) {
        ++g_vulkanCalls[k_vkCreateImageView];
        constexpr VkImageAspectFlags kPlanes =
            VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT;
        if (i != nullptr && (i->subresourceRange.aspectMask & kPlanes) != 0) g_planeViewImages.push_back(i->image);
    }
    return real(d, i, a, o);
}
WALLPAPERX_COUNTED(vkCreateBuffer, (VkDevice d, const VkBufferCreateInfo* i, const VkAllocationCallbacks* a, VkBuffer* o),
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

extern "C" VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo* i, const VkAllocationCallbacks* a,
                                     VkDeviceMemory* o) {
    static const auto real =
        realSymbol<VkResult (*)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*)>(
            "vkAllocateMemory");
    if (g_countVulkan) ++g_vulkanCalls[k_vkAllocateMemory];
    if (g_allocationArmed.exchange(false)) {
        g_injectedInPicture = g_inGetBuffer.load() > 0;
        g_injectedSize = i->allocationSize;
        g_allocationInjected = true;
        return g_allocationAtSwitch.load();
    }
    return real(d, i, a, o);
}

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

extern "C" void vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    static const auto real = realSymbol<void (*)(VkDevice, const VkAllocationCallbacks*)>("vkDestroyDevice");
    if (g_watchDeviceDestroy) g_destroyedUnderLeak = true;
    real(device, allocator);
}

extern "C" VkResult vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                               const VkAllocationCallbacks* allocator, VkDevice* device) {
    static const auto real =
        realSymbol<VkResult (*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*)>(
            "vkCreateDevice");
    if (g_recordQueues) {
        g_createdSynchronized = false;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
            if (std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME) == 0) {
                g_createdSynchronized = true;
            }
        }
        for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
            ++g_queueCreates;
            const VkDeviceQueueCreateInfo& queue = info->pQueueCreateInfos[i];
            if (g_createdSynchronized && (queue.flags & VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR) == 0) {
                ++g_unflaggedQueues;
                std::fprintf(stderr, "FAIL: queue family %u was created without the internally-synchronized flag\n",
                             queue.queueFamilyIndex);
                fail("a queue was created without the internally-synchronized flag");
            }
        }
    }
    return real(physical, info, allocator, device);
}

extern "C" void vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    static const auto real =
        realSymbol<void (*)(VkDevice, const VkDeviceQueueInfo2*, VkQueue*)>("vkGetDeviceQueue2");
    if (g_recordQueues) {
        ++g_queueFetches;
        if (g_createdSynchronized && (info->flags & VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR) == 0) {
            ++g_unflaggedQueues;
            std::fprintf(stderr, "FAIL: queue family %u was fetched without the internally-synchronized flag\n",
                         info->queueFamilyIndex);
            fail("a queue was fetched without the internally-synchronized flag");
        }
    }
    real(device, info, queue);
}

extern "C" void vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    static const auto real = realSymbol<void (*)(VkDevice, uint32_t, uint32_t, VkQueue*)>("vkGetDeviceQueue");
    if (g_recordQueues) ++g_plainFetches;
    real(device, family, index, queue);
}

namespace {
PFN_vkCreateVideoSessionKHR g_realCreateSession = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL failingCreateSession(VkDevice device, const VkVideoSessionCreateInfoKHR* info,
                                                    const VkAllocationCallbacks* allocator, VkVideoSessionKHR* session) {
    if (std::exchange(g_failNextSession, false)) {
        ++g_sessionsFailed;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return g_realCreateSession(device, info, allocator, session);
}
}  // namespace

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    static const auto real = realSymbol<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    const PFN_vkVoidFunction function = real(device, name);
    if (function == nullptr || std::strcmp(name, "vkCreateVideoSessionKHR") != 0) return function;
    g_realCreateSession = reinterpret_cast<PFN_vkCreateVideoSessionKHR>(function);
    return reinterpret_cast<PFN_vkVoidFunction>(&failingCreateSession);
}

extern "C" int av_hwframe_ctx_init(AVBufferRef* ref) {
    static const auto real = realSymbol<int (*)(AVBufferRef*)>("av_hwframe_ctx_init");
    auto* frames = reinterpret_cast<AVHWFramesContext*>(ref->data);
    if (g_failFramesInit > 0 && frames->format == AV_PIX_FMT_VULKAN) {
        --g_failFramesInit;
        return AVERROR_EXTERNAL;
    }
    if (g_trackPools && frames->format == AV_PIX_FMT_VULKAN && frames->free != &onPoolFreed) {
        PoolRecord pool;
        pool.width = frames->width;
        pool.height = frames->height;
        pool.createdDuring = g_poolCall;
        g_pools.push_back(pool);
        g_hookedPools.push_back(HookedPool{frames, frames->free, g_pools.size() - 1, g_poolGeneration});
        frames->free = &onPoolFreed;
    }
    const int err = real(ref);
    if (err >= 0 && frames->format == AV_PIX_FMT_VULKAN) ++g_vulkanFramesMade;
    return err;
}

extern "C" int avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    static const auto real = realSymbol<int (*)(AVCodecContext*, AVFrame*)>("avcodec_receive_frame");
    const bool decoders = isDecodersContext(context);
    if (decoders && g_holdInFfmpeg.load() && gettid() == g_decodeTid.load()) {
        std::unique_lock<std::mutex> lock(g_threadMutex);
        g_heldInFfmpeg = true;
        g_threadChanged.notify_all();
        g_threadChanged.wait(lock, [] { return !g_holdInFfmpeg.load(); });
    }
    if (decoders && g_threadExternal.load() > 0 && gettid() == g_decodeTid.load()) {
        int left = g_threadExternal.load();
        while (left > 0 && !g_threadExternal.compare_exchange_weak(left, left - 1)) {
        }
        if (left > 0) return AVERROR_EXTERNAL;
    }
    if (decoders && g_countThreaded.load()) {
        const int err = real(context, frame);
        if (err != 0) return err;
        if (gettid() != g_gpuTid.load()) ++g_receivedOffGpuThread;
        if (frame->format != AV_PIX_FMT_VULKAN) ++g_threadHostReceived;
        const int untaken = ++g_threadReceived - g_taken.load();
        int most = g_maxUntaken.load();
        while (untaken > most && !g_maxUntaken.compare_exchange_weak(most, untaken)) {
        }
        return err;
    }
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
    if (err == AVERROR_EOF && decoders && g_unanswerAtSwitch && context->hw_device_ctx != nullptr &&
        g_deviceCodecsOpened.load() == 1) {
        g_unanswerAtSwitch = false;
        g_unanswerFired = true;
        g_holdNextSubmission = true;
        g_stallHeld = true;
    }
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
    if (err == 0 && decoders && g_trackPools) ++g_trackedPictures;
    if (err == 0 && decoders && g_trackPools && frame->format == AV_PIX_FMT_VULKAN && frame->hw_frames_ctx != nullptr) {
        if (PoolRecord* pool = poolRecord(reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data))) {
            ++pool->pictures;
            pool->peek = *frame->hw_frames_ctx;
        }
    }
    if (err == 0 && decoders && g_logTraffic) {
        g_returnedPictures.push_back(
            TrafficEntry{frame->best_effort_timestamp, g_failingSeeks.size(), frame->format == AV_PIX_FMT_VULKAN});
    }
    if (err == 0 && decoders && g_inFailingCall && g_stripTimestamps) frame->best_effort_timestamp = AV_NOPTS_VALUE;
    if (err == 0 && decoders && g_countOnThread.load() && gettid() == g_decodeTid.load()) {
        ++g_onThreadReceived;
        if (frame->format != AV_PIX_FMT_VULKAN) ++g_onThreadHost;
    }
    if (err == 0 && decoders && g_recordPictures) {
        g_lastReceivedDevice = frame->format == AV_PIX_FMT_VULKAN;
        ++(frame->format == AV_PIX_FMT_VULKAN ? g_devicePictures : g_hostPictures);
        if (g_inFailingCall) ++g_failingPictures;
    }
    if (err == 0 && decoders && g_tallying) {
        ++g_tally.pictures;
        ++(frame->format == AV_PIX_FMT_VULKAN ? g_tally.devicePictures : g_tally.hostPictures);
    }
    return err;
}

// Reached from the decoder's get_buffer2 for the picture it allocates first; the nested call through the
// same callback is the second request of one packet, which is how ffmpeg asks for a stand-in.
extern "C" int avcodec_default_get_buffer2(AVCodecContext* context, AVFrame* frame, int flags) {
    static const auto real = realSymbol<int (*)(AVCodecContext*, AVFrame*, int)>("avcodec_default_get_buffer2");
    if (g_standInAtNextPicture && isDecodersContext(context) && context->hw_frames_ctx != nullptr &&
        context->get_buffer2 != nullptr) {
        g_standInAtNextPicture = false;
        AVFrame* standIn = av_frame_alloc();
        if (standIn != nullptr) {
            standIn->width = frame->width;
            standIn->height = frame->height;
            standIn->format = frame->format;
            if (context->get_buffer2(context, standIn, flags) >= 0) av_frame_unref(standIn);
            av_frame_free(&standIn);
            ++g_standInsForced;
        }
    }
    const bool inject = isDecodersContext(context) && context->hw_frames_ctx != nullptr && g_pictureArmed.exchange(false);
    const bool afterPicture =
        isDecodersContext(context) && context->hw_frames_ctx != nullptr && g_bitstreamArmed.exchange(false);
    if (inject) g_allocationArmed = true;
    ++g_inGetBuffer;
    const int err = real(context, frame, flags);
    --g_inGetBuffer;
    // A picture served from the pool allocated nothing: the next picture carries the injection instead.
    if (inject && g_allocationArmed.exchange(false)) g_pictureArmed = true;
    if (afterPicture) g_allocationArmed = true;
    return err;
}

extern "C" int av_read_frame(AVFormatContext* format, AVPacket* packet) {
    static const auto real = realSymbol<int (*)(AVFormatContext*, AVPacket*)>("av_read_frame");
    if (g_countStartReads.load()) {
        const pid_t tid = gettid();
        if (tid == g_gpuTid.load()) {
            ++g_startReadsGpu;
        } else if (tid == g_decodeTid.load()) {
            const int n = ++g_startReadsThread;
            {
                std::lock_guard<std::mutex> lock(g_threadMutex);
            }
            g_threadChanged.notify_all();
            if (n == g_startReadFailAt.load()) {
                g_startReadFailed = true;
                return g_startReadFailErr.load();
            }
            if (const int delay = g_startReadDelayMs.load(); delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
    }
    const int err = real(format, packet);
    if (err >= 0 && g_inFailingCall && g_seekAwaitingPacket >= 0 && packet->stream_index == g_seekAwaitingStream &&
        static_cast<size_t>(g_seekAwaitingPacket) < g_failingSeeks.size()) {
        g_failingSeeks[static_cast<size_t>(g_seekAwaitingPacket)].landedOnKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        g_seekAwaitingPacket = -1;
    }
    if (err >= 0 && g_logTraffic) {
        g_readPackets.push_back(TrafficEntry{packet->pts, g_failingSeeks.size(), false, g_sentPackets.size()});
    }
    if (err >= 0 && g_clearPositions && packet->pos >= 0) {
        packet->pos = -1;
        if (g_tallying) ++g_tally.positionsCleared;
    }
    if (err >= 0 && g_clearDts && packet->dts != AV_NOPTS_VALUE) {
        packet->dts = AV_NOPTS_VALUE;
        ++g_dtsCleared;
    }
    return err;
}

extern "C" int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    static const auto real = realSymbol<int (*)(AVCodecContext*, const AVPacket*)>("avcodec_send_packet");
    if (g_refuseAllPackets && packet != nullptr && isDecodersContext(context)) {
        ++g_packetsRefused;
        return AVERROR_INVALIDDATA;
    }
    const int pass = g_decoderOpens + g_decoderFlushes;
    if (g_inFailingCall && packet != nullptr && isDecodersContext(context) && context->hw_device_ctx == nullptr &&
        g_refuseFrom > 0 && pass >= g_refuseFrom && pass <= g_refuseThrough) {
        return AVERROR_INVALIDDATA;
    }
    if (g_recordPictures && packet != nullptr && packet->data != nullptr && isDecodersContext(context) &&
        context->hw_device_ctx == nullptr) {
        g_softwarePacketsSent.fetch_add(1, std::memory_order_relaxed);
    }
    const int err = real(context, packet);
    if (g_logTraffic && packet != nullptr && err != AVERROR(EAGAIN) && isDecodersContext(context)) {
        g_sentPackets.push_back(TrafficEntry{packet->pts, g_failingSeeks.size()});
    }
    return err;
}

extern "C" void avcodec_flush_buffers(AVCodecContext* context) {
    static const auto real = realSymbol<void (*)(AVCodecContext*)>("avcodec_flush_buffers");
    if (g_inFailingCall && isDecodersContext(context) && context->hw_device_ctx == nullptr) ++g_decoderFlushes;
    real(context);
}

extern "C" int avcodec_open2(AVCodecContext* context, const AVCodec* codec, AVDictionary** options) {
    static const auto real = realSymbol<int (*)(AVCodecContext*, const AVCodec*, AVDictionary**)>("avcodec_open2");
    const int err = real(context, codec, options);
    if (err >= 0 && g_inFailingCall && isDecodersContext(context) && context->hw_device_ctx == nullptr) ++g_decoderOpens;
    if (err >= 0 && g_tallying && isDecodersContext(context)) ++g_tally.decoderOpens;
    if (err >= 0 && g_trackCodecs && isDecodersContext(context)) {
        g_codecs.push_back(CodecRecord{context, context->hw_device_ctx != nullptr, g_poolCall});
    }
    if (err >= 0 && g_countDeviceCodecs && isDecodersContext(context) && context->hw_device_ctx != nullptr) {
        const int live = ++g_deviceCodecsLive;
        int seen = g_deviceCodecsMax.load();
        while (live > seen && !g_deviceCodecsMax.compare_exchange_weak(seen, live)) {
        }
        if (++g_deviceCodecsOpened == g_injectAtCodec.load()) {
            if (g_allocationAtSwitch.load() != VK_SUCCESS) {
                (g_bitstreamAtSwitch ? g_bitstreamArmed : g_pictureAtSwitch ? g_pictureArmed : g_allocationArmed) = true;
            }
            if (g_framesAtSwitch) {
                g_failFramesInit = 1;
                g_framesArmed = true;
            }
        }
    }
    return err;
}

// Only a context this records as open is marked, and only once: an address ffmpeg reuses for a later
// context belongs to that context's record.
extern "C" void avcodec_free_context(AVCodecContext** context) {
    static const auto real = realSymbol<void (*)(AVCodecContext**)>("avcodec_free_context");
    if (g_countDeviceCodecs && context != nullptr && isDecodersContext(*context) && (*context)->hw_device_ctx != nullptr) {
        --g_deviceCodecsLive;
        ++g_deviceCodecsFreed;
    }
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
                         (g_seekFailure == SeekFailure::AwayFromStart && timestamp > start) ||
                         (g_seekFailure == SeekFailure::Second && g_failingSeeks.size() == 1);
    const int result = refused ? AVERROR(EPERM) : real(format, stream, aimed, flags);
    g_failingSeeks.push_back(SeekCall{timestamp, result, byte});
    g_seekAwaitingPacket = result >= 0 && !byte && stream >= 0 ? static_cast<int>(g_failingSeeks.size()) - 1 : -1;
    g_seekAwaitingStream = stream;
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

// A wait on a condition from the decode thread outside a decode call is its queue-full wait: run() has no
// other, and ask() runs inside one.
extern "C" int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    using Fn = int (*)(pthread_cond_t*, pthread_mutex_t*);
    static std::atomic<Fn> real{nullptr};
    Fn fn = real.load();
    if (fn == nullptr) {
        fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pthread_cond_wait"));
        if (fn == nullptr) std::abort();
        real = fn;
    }
#if defined(__SANITIZE_THREAD__)
    // A wait called straight from a library tsan.supp names `called_from_lib` (ffmpeg, the Vulkan loader,
    // Lavapipe) goes to libc, as the sanitizer itself treats it: forwarded to its interceptor, the wait's
    // relock is seen while the library's own locking is ignored, which reads as lock-order cycles
    // between two of that library's mutexes.
    if (wallpaperx::testing::ignored_caller::fromIgnoredLibrary(__builtin_return_address(0))) {
        static const Fn libcWait = [] {
            const Fn found = wallpaperx::testing::ignored_caller::libcSymbol<Fn>("pthread_cond_wait");
            if (found == nullptr) std::abort();
            return found;
        }();
        fn = libcWait;
    }
#endif
    if (g_watchParks.load() && gettid() == g_decodeTid.load() && !g_inAround.load()) {
        {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            ++g_parks;
        }
        g_threadChanged.notify_all();
    }
    return fn(cond, mutex);
}

// operator new replaced so one allocation inside DecodeAhead::start() can fail (S15.888), recognised by
// the functions on its stack -- the target exports its symbols for dladdr(). Not under AddressSanitizer
// or ThreadSanitizer, which own operator new; that mode then reports itself not answerable.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#define WALLPAPERX_PROBE_REPLACES_NEW 1
namespace {
thread_local bool t_checkingStack = false;

bool allocationInStart(bool hook) {
    void* frames[48];
    const int depth = backtrace(frames, 48);
    bool start = false;
    bool installing = false;
    for (int i = 0; i < depth; ++i) {
        Dl_info info{};
        if (dladdr(frames[i], &info) == 0 || info.dli_sname == nullptr) continue;
        if (std::strstr(info.dli_sname, "11DecodeAhead5start") != nullptr) start = true;
        if (std::strstr(info.dli_sname, "13PictureSource16askDeviceThrough") != nullptr) installing = true;
    }
    return start && installing == hook;
}

void* allocate(std::size_t size) {
    const int failure = g_failNew.load();
    if (failure != 0 && !t_checkingStack && gettid() == g_gpuTid.load()) {
        t_checkingStack = true;
        const bool hit = allocationInStart(failure == 2);
        t_checkingStack = false;
        if (hit && g_failNew.exchange(0) == failure) {
            ++g_newFailures;
            throw std::bad_alloc();
        }
    }
    for (;;) {
        if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}
}  // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
namespace {
void release(void* p) noexcept {
    if (p != nullptr && p == g_watchedDevice.load()) g_watchedDeviceFreed = true;
    std::free(p);
}
}  // namespace
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
// libstdc++'s aligned operator new takes its memory from aligned_alloc, which free() releases.
void operator delete(void* p, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { release(p); }
#endif

namespace {

void runDecodeStartShortageChecks(Device& device, const std::string& label, int failure) {
#ifndef WALLPAPERX_PROBE_REPLACES_NEW
    std::printf("%s: a sanitizer owns operator new in this build, so no allocation can be failed -- not answerable\n",
                label.c_str());
    g_notVerified = true;
    static_cast<void>(device);
    static_cast<void>(failure);
#else
    if (!hardwareDecoderPresent(device, label) || !queuesShared(device, label) || !haveAsset(kThreadFixture, label)) {
        return;
    }
    resetThreadWatch();
    VideoPipeline pipeline(device, asset(kThreadFixture), kShaderDir, VideoPipeline::Playback::Once, std::nullopt,
                           watchedHooks());
    const ThreadWatchEnd end;
    g_countThreaded = true;
    // GOAL: S15.888 -- running out of memory inside DecodeAhead::start() (the hook's installation, or its
    //       first allocation) leaves the decoded picture returned, the pipeline's source intact and the thread
    //       started on a later picture.
    // WHY-THIS-SHAPE: the failure is placed by the functions on the allocating stack, not by an allocation
    //       count, which validation layers and the driver make vary. A lost source is a null dereference on
    //       the next decode, so the case runs in a forked child and a crash is graded, not suffered.
    // CONTROL: via control_batch.sh, 2026-10-19, RTX 3080 Ti, bytes restored: each in renderer/video: the hook installed before
    //       the `try` in DecodeAhead::start() (decode_ahead.cpp, the pre-E8 code) -> hook case "the child died by
    //       signal 11 (Segmentation fault)"; decodeAheadIfSafe()'s bad_alloc catch removed (video_pipeline.cpp)
    //       -> "was reported failed: std::bad_alloc"; decodeAheadIfSafe() called after the first picture only
    //       (video_pipeline.cpp) -> "8 pictures after the shortage, 0 decoded on a thread", both cases.
    // COVERAGE-ASSERT: the failure must fire exactly once and no decode thread may run after that call,
    //       or start() did not meet it.
    g_failNew = failure;
    bool first = false;
    try {
        first = pipeline.decodeNext();
    } catch (const std::exception& e) {
        g_failNew = 0;
        std::fprintf(stderr, "FAIL: %s: the picture decoded while the thread's start ran out of memory was reported "
                             "failed: %s\n",
                     label.c_str(), e.what());
        fail("a memory shortage starting the decode thread failed a decoded picture");
        return;
    }
    g_failNew = 0;
    if (g_newFailures != 1 || g_aroundCalls != 0) {
        std::fprintf(stderr, "FAIL: %s: the allocation failure fired %d time(s) in DecodeAhead::start(), and %d "
                             "decode(s) ran on a thread after it\n",
                     label.c_str(), g_newFailures.load(), g_aroundCalls.load());
        fail("the harness did not fail start() as planned");
        return;
    }
    if (!first) {
        std::fprintf(stderr, "FAIL: %s: decodeNext() answered no picture\n", label.c_str());
        fail("a memory shortage starting the decode thread lost the decoded picture");
        return;
    }
    for (int i = 0; i < 8; ++i) {
        if (!pipeline.decodeNext()) {
            std::fprintf(stderr, "FAIL: %s: picture %d after the shortage never came\n", label.c_str(), i + 2);
            fail("the pipeline stopped decoding after a memory shortage starting the decode thread");
            return;
        }
    }
    if (g_aroundCalls == 0 || g_receivedOffGpuThread == 0) {
        std::fprintf(stderr, "FAIL: %s: 8 pictures after the shortage, %d decoded on a thread\n", label.c_str(),
                     g_receivedOffGpuThread.load());
        fail("no decode thread started on a later picture after a memory shortage");
        return;
    }
    std::printf("%s: picture kept, %d later pictures decoded on the thread started afterwards\n", label.c_str(),
                g_receivedOffGpuThread.load());
#endif
}

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
                // GOAL: the modes that count or inject inside the caller's call really decode on that call's
                //       thread; with a decode thread, counts and injections land between calls instead.
                // WHY-THIS-SHAPE: asserted on the Device, not inferred from the checks passing, so an
                //       extension renamed or enabled another way fails here by name instead of as nine
                //       entries' unrelated counting failures.
                // CONTROL: test-side, 2026-10-19, RTX 3080 Ti: the strip skipped (interposer branch
                //       disabled) -> "FAIL: ... the Device still shares its queues with a decode thread".
                if (g_hideSynchronizedQueues && device->queuesInternallySynchronized()) {
                    std::fprintf(stderr, "FAIL: %s: the Device still shares its queues with a decode thread, so "
                                         "this mode's per-call counts would land outside the call\n",
                                 label.c_str());
                    fail("internally synchronised queues were not hidden from the Device");
                    throw std::runtime_error("cannot grade per-call counts with a decode thread running");
                }
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
            g_clearDts = false;
            g_countVulkan = false;
            g_heldFences.clear();
            oracle.detach();
        }
        // GOAL: S13.121's second half -- a decoder leaked because the device did not answer is reported to the
        //       Device, so a GPU that answers again later does not have its Vulkan device destroyed under the
        //       leaked codec's objects (VUID-vkDestroyDevice-device-05137).
        // WHY-THIS-SHAPE: the probe's batch is let go before the Device is destroyed, so the GPU has caught up
        //       and only the Device's own record of the leak keeps it from destroying itself. Graded at the call,
        //       watched from when the case confirmed the codec leaked, rather than only through the validation
        //       layer's teardown message, which reconcile() below counts without naming.
        // CONTROL: via control_run.sh, 2026-10-08, NVIDIA, reverted: closeDecoder()'s `m_device.recordLeak(...)`
        //       call removed (renderer/video/video_decoder.cpp) -> on video_pipeline_retire_device "FAIL: ...
        //       [counter_resize.h264, device not answering at teardown]: vkDestroyDevice was called while the
        //       leaked hardware codec lived (VUID-vkDestroyDevice-device-05137)", and the layer's own log naming
        //       VUID-vkDestroyDevice-device-05137, counted by reconcile() as one message outside its window.
        if (std::exchange(g_watchDeviceDestroy, false) && g_destroyedUnderLeak) {
            std::fprintf(stderr, "FAIL: %s: vkDestroyDevice was called while the leaked hardware codec lived "
                                 "(VUID-vkDestroyDevice-device-05137)\n",
                         label.c_str());
            fail("a decoder leaked at teardown was not reported to the Device, which then destroyed itself under it");
        }
        g_destroyedUnderLeak = false;
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
        "loop-cost-host",    "loop-resume",   "release-device",    "retire-device", "retire-unanswered",
        "halves-device",     "halves-host",   "timing-device",     "timing-host",       "start-at-device",
        "start-at-device-ts", "start-at-host", "start-at-device-open-gop", "start-at-device-open-gop-hevc",
        "start-at-device-open-gop-mp4", "start-at-host-open-gop", "start-at-device-budget", "start-at-device-av1",
        "bounded-decode-device", "bounded-decode-host", "decode-thread-depth", "decode-thread-pending",
        "decode-thread-failure", "decode-thread-catch-up", "decode-thread-hidden", "decode-thread-software",
        "decode-thread-alloc", "decode-thread-left-running", "decode-thread-pools", "decode-thread-resume", "decode-thread-start-read", "device-completed-off-thread",
        "device-queue-flags", "damaged-device", "damaged-device-no-inline", "damaged-host", "opening-report"};
    if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
        std::fprintf(stderr, "usage: %s <mode>, one of:", argv[0]);
        for (const std::string& m : modes) std::fprintf(stderr, " %s", m.c_str());
        std::fprintf(stderr, "\n");
        return 1;
    }
    const std::string label = "video_pipeline_probe (" + mode + ")";
    // backtrace() loads its unwinder on first use, which allocates: never first inside operator new.
    {
        void* frames[2];
        static_cast<void>(backtrace(frames, 2));
    }
    // GOAL: S13.83/S13.86 and the resume, loop and start-at goals these modes hold are about the decoding
    //       PictureSource does, which is the same code on either thread; these modes attribute every count,
    //       injected failure and pool free to the caller's call, which only holds when decoding is inline.
    // WHY-THIS-SHAPE: hiding the extension needs no production seam. Leaving the thread on made every
    //       injection and free land between calls (13x "the injected hardware failure never reached the
    //       decoder", 7x "a replaced image pool was not freed during playback"), testing the probe's
    //       timing, not the decoder. The same properties with the thread on are not covered here.
    // CONTROL: test-side, 2026-10-19, RTX 3080 Ti: run without this assignment -> all nine entries red as
    //       listed above; with it, all nine green. Production controls re-run under it: both
    //       releaseIdleFramesContexts() calls removed (renderer/video/video_decoder.cpp) -> shape_device "pool 1
    //       of 2 (320x240) was freed only when the pipeline was destroyed"; held via control_batch.sh.
    // resume-no-timestamps, with the thread on: 3 runs in 80 "hardware failing after 5 picture(s): the injected
    //       hardware failure was never consumed", HEAD and working tree alike; hidden: 0 in 200. Its S13.60
    //       control re-run hidden, renderer/video/video_decoder.cpp admit() `==` made `>=` -> "after 17 ... 1 shown
    //       more than once" and the same after 40, via control_batch.sh 2026-10-22.
    // resume-timestamps, with the thread on: its failing call counted 0 pictures and 0 seeks, so its cost bound
    //       could not fail; off this list by hand (test-side, 2026-10-23) -> "the call that met the failure
    //       decoded no picture, so its cost was not measured" at all three points.
    g_hideSynchronizedQueues = mode == "resume-timestamps" || mode == "resume-no-timestamps" ||
                               mode == "resume-late-seek" || mode == "resume-end" ||
                               mode == "resume-seek-failure" ||
                               mode == "resume-drop-count" || mode == "shape-device" || mode == "loop-device" ||
                               mode == "loop-cost-device" || mode == "loop-resume" || mode == "start-at-device" ||
                               mode == "decode-thread-hidden" || mode == "damaged-device" ||
                               mode == "damaged-device-no-inline" || mode == "opening-report";
    g_hideVideoMaintenance2 = mode == "damaged-device-no-inline" || mode == "opening-report";

    g_recordQueues = mode == "device-queue-flags";
    if (mode == "decode-thread-left-running") {
        superviseChild(label, [&label] { return runLeftRunningCase(label); });
        const int failures = wallpaperx::testing::reportFailures("video_pipeline_probe");
        if (failures > 0) return 1;
        if (g_notVerified) {
            std::printf("%s: NOT VERIFIED here -- reported to ctest as a skip, not a pass\n", label.c_str());
            return kNotVerifiedExitCode;
        }
        std::printf("%s: passed in its own child\n", label.c_str());
        return 0;
    }
    if (mode == "shape-device" || mode == "loop-device" || mode == "release-device" || mode == "retire-device" ||
        mode == "retire-unanswered" ||
        mode == "decode-thread-hidden" || mode == "decode-thread-software" || mode == "decode-thread-alloc" ||
        mode == "decode-thread-pools") {
        std::vector<std::pair<std::string, std::function<void(Device&)>>> cases;
        if (mode == "shape-device") {
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
            cases.emplace_back("counter_resize.h264, device not answering at teardown",
                               [label](Device& device) { runUnansweredProbeTeardownChecks(device, label); });
        } else if (mode == "retire-unanswered") {
            for (const SizeChangeFixture& fixture : kSizeChangeFixtures) {
                if (std::string(fixture.file) != "counter_shrink_grow.hevc") continue;
                cases.emplace_back(std::string(fixture.file) + ", retired codec not answered",
                                   [&fixture, label](Device& device) { runUnansweredRetiredChecks(device, label, fixture); });
            }
        } else if (mode == "decode-thread-hidden" || mode == "decode-thread-software") {
            const bool software = mode == "decode-thread-software";
            cases.emplace_back(software ? "a stream decoded in software" : "queues not shared",
                               [label, software](Device& device) { runNoDecodeThreadChecks(device, label, software); });
        } else if (mode == "decode-thread-pools") {
            for (const SizeChangeFixture& fixture : kSizeChangeFixtures) {
                const std::string file = fixture.file;
                if (file != "counter_resize.h264" && file != "counter_shrink_grow.h264") continue;
                cases.emplace_back(file, [&fixture, label](Device& device) {
                    runThreadedPoolChecks(device, label, fixture);
                });
            }
        } else if (mode == "decode-thread-alloc") {
            cases.emplace_back("first allocation in DecodeAhead::start",
                               [label](Device& device) { runDecodeStartShortageChecks(device, label, 1); });
            cases.emplace_back("the device-question hook's allocation",
                               [label](Device& device) { runDecodeStartShortageChecks(device, label, 2); });
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
                                mode == "loop-cost-host" || mode == "halves-host" || mode == "timing-host" ||
                                mode == "start-at-host" || mode == "start-at-host-open-gop" ||
                                mode == "bounded-decode-host" || mode == "damaged-host",
                     [&](Device& device) {
                         if (mode == "decode-thread-depth") runDecodeDepthChecks(device, label);
                         if (mode == "decode-thread-pending") runDecodePendingChecks(device, label);
                         if (mode == "decode-thread-failure") runDecodeFailureChecks(device, label);
                         if (mode == "decode-thread-catch-up") runDecodeCatchUpChecks(device, label);
                         if (mode == "decode-thread-resume") runThreadedResumeChecks(device, label);
                         if (mode == "decode-thread-start-read") runStartReadChecks(device, label);
                         if (mode == "device-completed-off-thread") runCompletedOffThreadChecks(device, label);
                         if (mode == "device-queue-flags") runQueueFlagChecks(device, label);
                         if (mode == "bounded-decode-device") runBoundedDecodeChecks(device, label, /*onDevice=*/true);
                         if (mode == "bounded-decode-host") runBoundedDecodeChecks(device, label, /*onDevice=*/false);
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
                         if (mode == "timing-device") {
                             runTimingChecks(device, label, /*onDevice=*/true);
                             if (hardwareDecoderPresent(device, label)) runTrimmedEndChecks(device, label);
                         }
                         if (mode == "timing-host") {
                             runTimingChecks(device, label, /*onDevice=*/false);
                             if (videoExtensionsHidden(device, label)) runTrimmedEndChecks(device, label);
                         }
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
                         if (mode == "start-at-device") {
                             runStartAtChecks(device, label, true, StartPoints::Mp4);
                             runStartedFailureChecks(device, label);
                             runPastEndChecks(device, label, /*onDevice=*/true);
                         }
                         // GOAL: S15.145 -- a transport stream opened part way in on the hardware decoder
                         //       plays its pictures without breaking the Vulkan specification: this
                         //       entry's validation grade, over the two transport-stream starting points.
                         // WHY-THIS-SHAPE: an entry of its own so a failure here names the case: a
                         //       transport stream's seeks land on non-keyframes, an MP4's never do.
                         // CONTROL: via control_batch.sh, 2026-10-23 (G3d), RTX 3080 Ti, reverted, in
                         //       renderer/video/video_decoder.cpp: scanWindow() taking every packet as a reset
                         //       point alone -> "the first packet sent to the decoder was packet 39 (picture 38,
                         //       not a keyframe)" but no warning, as allocatePicture()'s missing-reference test
                         //       sends the start to software ("0 device and 22 host"); with that test also
                         //       made `&& false` -> "58 Vulkan validation warning(s)". A device start depends
                         //       only on the reset-point test, never on next()'s skip to a keyframe.
                         if (mode == "start-at-device-ts") {
                             runStartAtChecks(device, label, true, StartPoints::TransportStream);
                         }
                         if (mode == "start-at-host") {
                             runStartAtChecks(device, label, false, StartPoints::All);
                             runPastEndChecks(device, label, /*onDevice=*/false);
                             runSoftwareBudgetChecks(device, label);
                             runSoftwareLandingChecks(device, label);
                         }
                         // GOAL: S15.163 -- an open-GOP stream opened part way in on the hardware decoder,
                         //       where a seek lands on a keyframe that does not reset the decoder, breaks no
                         //       Vulkan rule: each entry's validation grade.
                         // WHY-THIS-SHAPE: one entry per codec for the transport streams and one for MP4,
                         //       whose index seeks land on the open keyframe directly; S15.161's device check
                         //       makes each run prove an open keyframe stood where the seek lands.
                         // CONTROL: via control_batch.sh, 2026-10-23 (G3d), RTX 3080 Ti, reverted, in
                         //       renderer/video/video_decoder.cpp: scanWindow() taking the container's keyframe
                         //       flag for resetsDecoder(), with allocatePicture()'s missing-reference test made
                         //       `&& false` -> "52 Vulkan validation warning(s)" on the H.264 transport stream, 8
                         //       on HEVC's, 52 on the MP4 entry. The scanWindow() break alone leaves no warning:
                         //       that test sends the start to software (S15.161's check goes red instead).
                         if (mode == "start-at-device-open-gop") runOpenGopStartChecks(device, label, true, "counter_opengop.ts");
                         if (mode == "start-at-device-open-gop-hevc") {
                             runOpenGopStartChecks(device, label, true, "counter_opengop_hevc.ts");
                         }
                         if (mode == "start-at-device-open-gop-mp4") {
                             runOpenGopStartChecks(device, label, true, "counter_opengop.mp4");
                             runOpenGopStartChecks(device, label, true, "counter_opengop_hevc.mp4");
                         }
                         if (mode == "start-at-device-budget") runBudgetChecks(device, label);
                         if (mode == "damaged-device") {
                             runDamagedChecks(device, label, /*onDevice=*/true);
                             runLengthChangePipelineChecks(device, label, /*onDevice=*/true);
                             runSwitchChecks(device, label, /*inlineExpected=*/true);
                             runReopenPositionChecks(device, label);
                         }
                         if (mode == "damaged-host") {
                             runDamagedChecks(device, label, /*onDevice=*/false);
                             runLengthChangePipelineChecks(device, label, /*onDevice=*/false);
                         }
                         if (mode == "damaged-device-no-inline") {
                             // COVERAGE-ASSERT: the hide reached the Device, or the row below grades the inline path.
                             check(!device.videoInlineParameters() && !inlineParametersOffered(device),
                                   "VK_KHR_video_maintenance2 was hidden from the Device");
                             runDamagedChecks(device, label, /*onDevice=*/true, "counter_ps_gap.flv");
                             runLengthChangePipelineChecks(device, label, /*onDevice=*/true);
                             runSwitchChecks(device, label, /*inlineExpected=*/false);
                             runSwitchFailureChecks(device, label);
                             runRewindResetChecks(device, label);
                         }
                         if (mode == "opening-report") runOpeningReportChecks(device, label);
                         if (mode == "start-at-device-av1") runAv1StartChecks(device, label);
                         if (mode == "start-at-host-open-gop") {
                             for (const char* file : {"counter_opengop.ts", "counter_opengop_hevc.ts", "counter_opengop.mp4",
                                                      "counter_opengop_hevc.mp4"}) {
                                 runOpenGopStartChecks(device, label, false, file);
                             }
                         }
                     });
}
