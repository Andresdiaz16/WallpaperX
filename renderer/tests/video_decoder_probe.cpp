// SPDX-License-Identifier: GPL-2.0-or-later
//
// The ffmpeg bridge: S13.37-S13.43.
//
// WHY THIS FILE EXISTS. Nothing in the tree constructs a VideoDecoder. Its 701 lines went
// through four review passes and were never executed, and repair rounds are where defects
// concentrate, so this probe is the first thing to run most of it.
//
// WHY THE VALIDATION ORACLE IS NOT THE DEVICE'S COUNTER HERE. ffmpeg's own Vulkan H.264 decoder
// trips VUID-VkVideoBeginCodingInfoKHR-flags-07244 on this driver -- reproduced 2026-09-10 with
// the stock `ffmpeg -hwaccel vulkan` command line and none of this project's code involved -- so
// grading the count would fail every hardware run for a defect in a dependency. Rather than
// waive the oracle, this probe installs a messenger of its own on the Device's instance, excuses
// exactly that one message id by name, and fails on any other. It then reconciles its own total
// against the Device's counter after the Device is gone, so a warning emitted outside its
// window -- instance creation, or teardown, which is where the layer's most valuable complaints
// are -- still fails. The alternative, WALLPAPERX_ALLOW_NO_VALIDATION, would retire the oracle
// for the whole run and for every message, which is the vacuous-green trap this project has hit
// before.
//
// CONTROL for the excuse itself: kExcusedDecoderMessageId, in support/video_probe_support.h, set
// to a string no message carries. Observed
// on the hardware entry: "FAIL: video_decoder_probe (hardware): 8 Vulkan validation warning(s)"
// -- so the eight messages a green run excuses are the eight it names, and the list is not a
// blanket. Reverted.
//
// SIX ENTRIES, ONE BINARY, because each needs a different device or a different injected
// failure, and one verdict for all of them would report a state that was never reached as
// verified.

#include "video/video_decoder.h"

#include "vulkan/device.h"
#include "vulkan/gpu_fault.h"

#include "support/check.h"
#include "support/validation_grade.h"
#include "support/video_probe_support.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using wallpaperx::DecodedFrame;
using wallpaperx::Device;
using wallpaperx::GpuFailure;
using wallpaperx::VideoDecoder;
using wallpaperx::WallpaperContentError;
using wallpaperx::testing::check;
using wallpaperx::testing::fail;
using wallpaperx::testing::ScratchDir;

constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

// ------------------------------------------------------------------------- //
// Injected failures. Each is inert until a mode turns it on.
// ------------------------------------------------------------------------- //

// Strips every VK_KHR_video* extension from what the driver reports, so a GPU that has a decoder
// reports none. Without it, Path::SoftwareNoDecoder is only reachable on the software driver --
// and a state only reachable in continuous integration is not a state the shipping path is
// graded in.
bool g_hideVideoExtensions = false;

// Fails the next av_hwframe_ctx_init(), which is ffmpeg failing AFTER the device said yes. No
// file can produce this: the device either declines during the capability query, in which case
// this call is never reached, or it accepts and ffmpeg succeeds.
bool g_failNextFramesInit = false;

// Makes every vkQueueSubmit the Device itself issues report a lost device, which is what
// VideoDecoder::deviceHealthy() asks. ffmpeg's own submissions are unaffected: it calls through
// pointers from vkGetDeviceProcAddr, which do not pass through this executable's symbol table.
bool g_deviceLost = false;

// Fails the next vkQueueSubmit with a host-memory error, for the one submit that has to be seen
// to fail without leaving the device unusable afterwards.
bool g_failNextSubmitOnce = false;

// How many more calls to each ffmpeg decode entry point answer with an injected error.
//
// These exist because byte damage does not reach the branches that handle them. Measured
// 2026-09-10: flipping 1544 payload bytes of an H.264 elementary stream takes it from 20 decoded
// frames to 18, and neither avcodec_send_packet nor avcodec_receive_frame ever reports
// AVERROR_INVALIDDATA -- ffmpeg conceals the damage and simply emits fewer pictures. Confirmed by
// negative control: making either branch throw leaves that run green. So the branches are graded
// by returning the error from ffmpeg's own entry points, which is the only thing that reaches
// them; a decremented count, not a flag, so the injection stops and the loop cannot spin.
//
// ARMED AFTER THE DECODER IS OPEN, never before. avformat_find_stream_info(), which runs inside
// VideoDecoder's constructor, decodes packets through these same entry points on codec contexts
// of its own -- so a count armed at the top is spent there and never reaches next() at all. That
// went unnoticed for one round precisely because the count still reached zero, which is why the
// checks below assert consumption AND an effect on the frames, not consumption alone.
int g_sendInvalidDataRemaining = 0;
int g_receiveInvalidDataRemaining = 0;
int g_sendEagainRemaining = 0;
// Which ffmpeg decode call the first injection of the current phase landed on, against a count of
// every such call in the process. Compared with the call index at which the phase armed itself,
// so "the injection reached VideoDecoder::next()" is asserted rather than assumed -- the two
// phases whose expected frame count is unchanged have no other way to tell that apart from an
// injection spent inside the constructor.
int g_ffmpegDecodeCalls = 0;
int g_firstInjectionCall = -1;

// What one Device::submit was given, read off the VkSubmitInfo rather than inferred.
struct RecordedSubmit {
    std::vector<VkSemaphore> waitSemaphores;
    std::vector<VkPipelineStageFlags> waitStages;
    std::vector<uint64_t> waitValues;
    std::vector<VkSemaphore> signalSemaphores;
    std::vector<uint64_t> signalValues;
    bool hasTimelineInfo = false;
};
bool g_recordingSubmits = false;
std::vector<RecordedSubmit> g_submits;

// ------------------------------------------------------------------------- //
// Fixtures
// ------------------------------------------------------------------------- //

std::filesystem::path assetDir() { return std::filesystem::path(WALLPAPERX_VIDEO_ASSET_DIR); }

std::string asset(const char* name) { return (assetDir() / name).string(); }

bool haveAssets(const std::vector<const char*>& names) {
    for (const char* name : names) {
        if (!std::filesystem::exists(assetDir() / name)) {
            std::printf("video_decoder_probe: fixture %s was not generated (no ffmpeg command line at configure "
                        "time?) -- nothing here can be answered\n",
                        name);
            return false;
        }
    }
    return true;
}

// Copies an H.264 elementary stream with payload bytes flipped, skipping the first two kilobytes
// so the sequence and picture parameter sets survive: a stream whose headers are destroyed is
// refused at open, which is a different property from a stream whose pictures are damaged.
// Returns how many bytes were changed.
int writeDamagedCopy(const std::string& source, const std::string& destination) {
    std::ifstream in(source, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 4096) return 0;
    int flipped = 0;
    for (size_t i = 2000; i < bytes.size(); i += 37) {
        // Never inside a start code: introducing or destroying one changes where packets begin,
        // which makes the demuxer's framing the thing under test instead of the decoder's.
        const bool nearStartCode =
            (i >= 3 && std::memcmp(&bytes[i - 3], "\0\0\0\1", 4) == 0) ||
            (i >= 2 && std::memcmp(&bytes[i - 2], "\0\0\1", 3) == 0);
        if (nearStartCode) continue;
        bytes[i] = static_cast<char>(bytes[i] ^ 0xFF);
        ++flipped;
    }
    std::ofstream out(destination, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out ? flipped : 0;
}

// ------------------------------------------------------------------------- //
// Decoding helpers
// ------------------------------------------------------------------------- //

struct DecodeResult {
    int frames = 0;
    int deviceFrames = 0;
    int hostFrames = 0;
    VideoDecoder::Path path = VideoDecoder::Path::Undecided;
};

const char* pathName(VideoDecoder::Path path) {
    switch (path) {
        case VideoDecoder::Path::Undecided: return "Undecided";
        case VideoDecoder::Path::Hardware: return "Hardware";
        case VideoDecoder::Path::SoftwareNoDecoder: return "SoftwareNoDecoder";
        case VideoDecoder::Path::SoftwareCodec: return "SoftwareCodec";
        case VideoDecoder::Path::SoftwareRefused: return "SoftwareRefused";
    }
    return "?";
}

// Drains a file to the end, counting frames by where they live. Throws on to the caller: which
// exception a failure raises is itself graded, so it must not be swallowed here.
//
// `armAfterOpen` runs once the decoder is open and before the first next(). That is where an
// injected ffmpeg answer has to be switched on: the constructor's own probing decode goes
// through the same entry points, so anything armed earlier is spent there.
DecodeResult decodeAll(Device& device, const std::string& file,
                       const std::function<void()>& armAfterOpen = {}) {
    DecodeResult result;
    VideoDecoder decoder(device, file);
    if (armAfterOpen) armAfterOpen();
    while (std::optional<DecodedFrame> frame = decoder.next()) {
        ++result.frames;
        if (frame->storage() == DecodedFrame::Storage::DeviceImage) {
            ++result.deviceFrames;
        } else {
            ++result.hostFrames;
        }
    }
    result.path = decoder.path();
    return result;
}

// Waits for a batch and reports rather than propagating. Every caller below has already read
// what it needs off the recorded VkSubmitInfo, so a wedged batch must not skip the check that
// says why it wedged: the ten-second bound would otherwise surface as "the probe threw" with no
// line naming the property that was violated.
void waitReporting(Device& device, Device::SubmitTicket ticket, const char* what) {
    try {
        device.wait(ticket);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: waiting for %s did not complete: %s\n", what, e.what());
        fail("a batch this probe submitted never finished");
    }
}

// ------------------------------------------------------------------------- //
// S13.38 / S13.40 / S13.41 -- the hardware path
// ------------------------------------------------------------------------- //

void runHardwareChecks(Device& device) {
    if (!haveAssets({"h264_crop.mp4"})) {
        g_notVerified = true;
        return;
    }
    if (device.info().videoDecodeOps == 0) {
        std::printf("video_decoder_probe (hardware): %s has no hardware video decoder, so the hardware path is not "
                    "answerable here\n",
                    device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }

    VideoDecoder decoder(device, asset("h264_crop.mp4"));
    std::optional<DecodedFrame> first = decoder.next();
    if (!first) {
        fail("the hardware fixture decoded to no frames at all");
        return;
    }

    // GOAL: S13.38 -- a stream this device takes reports Path::Hardware, and the frame it hands
    //       back lives on the device.
    check(decoder.path() == VideoDecoder::Path::Hardware,
          "a stream this device accepts did not report Path::Hardware");
    check(first->storage() == DecodedFrame::Storage::DeviceImage,
          "the hardware path produced a frame held in host memory");

    // GOAL: S13.40 -- the frame carries what a converter needs, read off the frame rather than
    //       assumed. The fixture is 320x238 tagged BT.709 limited range, which H.264 codes as
    //       320x240 with two rows cropped.
    // WHY-THIS-SHAPE: asserting only "the extent is non-zero" would pass for an implementation
    //       that returned the coded size as the visible rectangle, or that defaulted the matrix
    //       to BT.601 full range -- the usual wrong default, and one whose only symptom is a
    //       visibly wrong picture that nothing reports. The fixture is generated with a height
    //       that is NOT a multiple of the macroblock size precisely so coded and visible differ;
    //       at 320x240 the two would be equal and the check could not tell them apart.
    // CONTROL: renderer/video/video_decoder.cpp, two, both reverted. (1) visible() changed to
    //       return the uncropped frame at the origin: "FAIL: the visible rectangle is the coded
    //       extent, so the crop the stream declares was not read", and on the no-decoder entry "FAIL:
    //       a software frame does not report the visible rectangle". (2) the DecodedFrame constructor's `m_colour = FrameColour{...}`
    //       replaced by a default-constructed FrameColour, which is the assume-a-default defect:
    //       "FAIL: the frame's colour matrix is not the BT.709 the fixture is tagged with" and
    //       "... colour range is not the limited range ...".
    check(first->extent().width == 320 && first->extent().height == 240,
          "the coded extent is not the 320x240 the fixture's macroblocks round up to");
    check(first->visible().offset.x == 0 && first->visible().offset.y == 0,
          "the visible rectangle does not start at the fixture's origin");
    check(first->visible().extent.width == 320 && first->visible().extent.height == 238,
          "the visible rectangle is the coded extent, so the crop the stream declares was not read");
    check(first->bitDepth() == 8, "an 8-bit stream did not report a bit depth of 8");
    check(first->pixelFormat() != AV_PIX_FMT_NONE, "the frame reports no pixel format");
    check(first->colour().matrix == AVCOL_SPC_BT709,
          "the frame's colour matrix is not the BT.709 the fixture is tagged with");
    check(first->colour().range == AVCOL_RANGE_MPEG,
          "the frame's colour range is not the limited range the fixture is tagged with");

    // A device frame has no host planes and says so, rather than handing back a null pointer the
    // caller would read.
    //
    // CONTROL: renderer/video/video_decoder.cpp, the storage guard deleted from hostPlanes().
    //       Observed: "FAIL: hostPlanes() on a device frame did not throw std::logic_error". The
    //       mirror -- the guard deleted from deviceImage() -- kills the no-decoder entry with a
    //       SIGSEGV inside ffmpeg's frames context, which is the crash the guard prevents and is
    //       red in the only way that failure mode allows. Both reverted.
    bool refusedHostPlanes = false;
    try {
        (void)first->hostPlanes();
    } catch (const std::logic_error&) {
        refusedHostPlanes = true;
    }
    check(refusedHostPlanes, "hostPlanes() on a device frame did not throw std::logic_error");

    // GOAL: S13.41 -- a batch that reads the frame hands it back exactly as found, waits at the
    //       frame's current value, signals exactly one higher on the same semaphore, and stores
    //       the new value only once the submit succeeded.
    // WHY-THIS-SHAPE: the semaphore handle and its value live inside ffmpeg's AVVkFrame, which
    //       no accessor exposes -- so the protocol is read off the VkSubmitInfo the Device
    //       handed the driver, through an interposed vkQueueSubmit. The simpler alternative,
    //       asserting only that submit() returns without throwing, cannot see a signal that is
    //       not the wait plus one, and cannot see the failure case at all.
    // CONTROL: renderer/video/video_decoder.cpp, two, both reverted. (1)
    //       `scope.timelineSignal.emplace_back(vk->sem[0], current + 1)` changed to `current +
    //       2`: "FAIL: the frame's batch does not signal exactly one above the value it waits
    //       at", plus the two downstream lines and one unexpected validation message. (2)
    //       `vk->layout[0] = VK_IMAGE_LAYOUT_GENERAL;` added after the submit: "FAIL: the frame
    //       does not describe itself the same way after a batch has read it".
    // COVERAGE-ASSERT: each bracket asserts that exactly one submit was recorded inside it, so a
    //       future change that routes the frame's work somewhere else -- or lets ffmpeg's own
    //       submissions fall into the window -- fails here rather than grading the wrong batch.
    const DecodedFrame::DeviceImage before = first->deviceImage();

    g_submits.clear();
    g_recordingSubmits = true;
    Device::SubmitTicket ticket = first->submit([](VkCommandBuffer) {}, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g_recordingSubmits = false;
    if (g_submits.size() != 1) {
        std::fprintf(stderr, "FAIL: reading the frame produced %zu submissions, not one\n", g_submits.size());
        fail("the frame's read batch could not be identified");
        return;
    }
    const RecordedSubmit firstSubmit = g_submits.front();
    waitReporting(device, ticket, "the frame's first read batch");

    check(firstSubmit.hasTimelineInfo, "the frame's batch carried no VkTimelineSemaphoreSubmitInfo");
    check(firstSubmit.waitSemaphores.size() == 1 && firstSubmit.signalSemaphores.size() == 1,
          "the frame's batch does not wait on and signal exactly one semaphore");
    if (firstSubmit.waitSemaphores.size() == 1 && firstSubmit.signalSemaphores.size() == 1) {
        check(firstSubmit.waitSemaphores[0] == firstSubmit.signalSemaphores[0],
              "the frame's batch waits on one semaphore and signals another");
        check(firstSubmit.signalValues[0] == firstSubmit.waitValues[0] + 1,
              "the frame's batch does not signal exactly one above the value it waits at");
        check(firstSubmit.waitStages[0] == VK_PIPELINE_STAGE_TRANSFER_BIT,
              "the frame's batch does not wait at the stage the caller named");
    }

    const DecodedFrame::DeviceImage after = first->deviceImage();
    check(before.image == after.image && before.format == after.format && before.tiling == after.tiling &&
              before.usage == after.usage && before.layout == after.layout,
          "the frame does not describe itself the same way after a batch has read it");

    // A second read must wait where the first one signalled: the stored value advanced.
    g_submits.clear();
    g_recordingSubmits = true;
    ticket = first->submit([](VkCommandBuffer) {}, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g_recordingSubmits = false;
    if (g_submits.size() != 1) {
        fail("the frame's second read batch could not be identified");
        return;
    }
    const RecordedSubmit secondSubmit = g_submits.front();
    check(!secondSubmit.waitValues.empty() && !firstSubmit.signalValues.empty() &&
              secondSubmit.waitValues[0] == firstSubmit.signalValues[0],
          "a second read does not wait where the first one signalled, so the stored value did not advance");
    waitReporting(device, ticket, "the frame's second read batch");

    // GOAL: S13.41 -- a submit that threw must NOT have advanced the stored value. A value
    //       recorded that nothing will signal hangs the decoder's teardown forever.
    // WHY-THIS-SHAPE: no file or driver state produces a failing vkQueueSubmit on demand, so it
    //       is injected -- once, and with a host-memory error rather than a lost device, so the
    //       Device is still usable for the third submit that reads the value back out. That
    //       third submit is the whole check: the failure itself proves nothing, only what the
    //       next batch waits at does.
    // CONTROL: renderer/video/video_decoder.cpp, move `vk->sem_value[0] = current + 1;` above
    //       the `m_device->submit(...)` call. Observed: "FAIL: a submit that threw still
    //       advanced the frame's stored semaphore value", then the wait for that batch reporting
    //       the Device's ten-second bound, and the process hanging in teardown until killed at a
    //       300-second timeout -- the teardown hang the header warns about. Reverted.
    bool submitThrew = false;
    g_failNextSubmitOnce = true;
    try {
        (void)first->submit([](VkCommandBuffer) {}, VK_PIPELINE_STAGE_TRANSFER_BIT);
    } catch (const std::exception&) {
        submitThrew = true;
    }
    g_failNextSubmitOnce = false;
    check(submitThrew, "an injected vkQueueSubmit failure did not reach the caller as an exception");

    g_submits.clear();
    g_recordingSubmits = true;
    ticket = first->submit([](VkCommandBuffer) {}, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g_recordingSubmits = false;
    if (g_submits.size() != 1) {
        fail("the read batch after the failed submit could not be identified");
        return;
    }
    const RecordedSubmit afterFailure = g_submits.front();
    // Checked BEFORE the wait, deliberately: the defect this names is a recorded value nothing
    // will signal, and a batch waiting on one never completes -- so waiting first would surface
    // it as the Device's ten-second bound with no line saying which property was violated.
    check(!afterFailure.waitValues.empty() && !secondSubmit.signalValues.empty() &&
              afterFailure.waitValues[0] == secondSubmit.signalValues[0],
          "a submit that threw still advanced the frame's stored semaphore value");
    waitReporting(device, ticket, "the frame's read batch after the injected submit failure");

    // GOAL: S13.42 -- a moved-from frame throws rather than dereferencing null.
    // CONTROL: renderer/video/video_decoder.cpp, DecodedFrame::frame() returning *m_frame
    //       unconditionally. Observed: the probe died with SIGSEGV after every earlier check passed
    //       and printed no verdict -- the crash this check exists to prevent, red in the only way
    //       that failure mode can be. Reverted.
    DecodedFrame moved = std::move(*first);
    bool movedFromThrew = false;
    try {
        (void)first->storage();
    } catch (const std::logic_error&) {
        movedFromThrew = true;
    }
    check(movedFromThrew, "a moved-from DecodedFrame did not throw std::logic_error");

    device.waitForSubmissions();

    // GOAL: S13.38 -- the hardware path decodes the WHOLE stream on the device, not just the
    //       opening keyframe.
    // WHY-THIS-SHAPE: the count is compared against the same file decoded on a device whose
    //       video extensions were stripped, in this same process, rather than against the number
    //       the fixture recipe happens to produce -- a hardcoded number is a claim about the
    //       generator. Draining the stream also matters for a second reason: the first frame
    //       references no other picture, so a run that stopped there would exercise none of
    //       ffmpeg's reference-picture handling and would never reach the one validation message
    //       this probe excuses, leaving the excuse itself unexercised.
    // CONTROL: via control_batch.sh, 2026-10-01, renderer/video/video_decoder.cpp, next()'s end-of-stream
    //       test widened to take an EAGAIN on the hardware path, once a picture has been returned, as the
    //       end -> "FAIL: the hardware path decoded 1 frames where a software decode of the same file decodes
    //       10". Reverted. Widened for every EAGAIN, the earlier check "decoded to no frames at all" fails
    //       first and this one is not reached.
    int hardwareFrames = 1;
    while (std::optional<DecodedFrame> more = decoder.next()) {
        ++hardwareFrames;
        if (more->storage() != DecodedFrame::Storage::DeviceImage) {
            fail("a later frame of a hardware decode was held in host memory");
            break;
        }
    }
    check(decoder.path() == VideoDecoder::Path::Hardware,
          "the path changed away from Hardware part-way through the stream");

    g_hideVideoExtensions = true;
    int softwareFrames = -1;
    {
        Device software(/*enableValidation=*/false);
        softwareFrames = decodeAll(software, asset("h264_crop.mp4")).frames;
    }
    g_hideVideoExtensions = false;
    if (hardwareFrames != softwareFrames) {
        std::fprintf(stderr,
                     "FAIL: the hardware path decoded %d frames where a software decode of the same file decodes "
                     "%d\n",
                     hardwareFrames, softwareFrames);
        fail("the hardware path did not decode the whole stream");
    }

    std::printf("video_decoder_probe (hardware): %s decoded %d frames of 320x238 BT.709 on the device; semaphore "
                "advanced %llu -> %llu -> %llu\n",
                device.info().gpuName.c_str(), hardwareFrames,
                static_cast<unsigned long long>(firstSubmit.waitValues[0]),
                static_cast<unsigned long long>(secondSubmit.waitValues[0]),
                static_cast<unsigned long long>(afterFailure.waitValues[0]));
}

// ------------------------------------------------------------------------- //
// S13.37 / S13.38 / S13.39 -- refusal, fault, and the software reopen
// ------------------------------------------------------------------------- //

void runRefusedChecks(Device& device) {
    if (!haveAssets({"h264_10bit.mp4", "mpeg4.avi"})) {
        g_notVerified = true;
        return;
    }
    if (device.info().videoDecodeOps == 0) {
        std::printf("video_decoder_probe (refused): %s has no hardware decoder, so a refusal by the decoder cannot "
                    "be told from having none -- not answerable here\n",
                    device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }

    // GOAL: S13.38 -- 10-bit H.264, which this decoder refuses entirely, reports
    //       Path::SoftwareRefused and NOT SoftwareCodec: the device decodes H.264, just not this
    //       stream.
    // GOAL: S13.39 -- the reopen decodes the whole stream from its first packet.
    // WHY-THIS-SHAPE: the frame count is compared against the same file decoded on a decoder
    //       that never tried hardware at all, in the same process, rather than against a
    //       hardcoded number. A hardcoded number is a claim about the fixture generator; the
    //       comparison is a claim about the reopen, and it is the only form that can see a
    //       reopen which resumes where the hardware attempt stopped instead of starting again.
    // CONTROL: renderer/video/video_decoder.cpp, seekToResume() -- which reopenInSoftware() calls --
    //       without the rewind() it makes when no picture has a timestamp to resume after, so software
    //       carries on from the packets the hardware attempt consumed. Observed: "FAIL: the software reopen
    //       produced 5 frames where a never-hardware decode of the same file produces 10", via
    //       control_batch.sh 2026-09-30. Reverted.
    const DecodeResult refused = decodeAll(device, asset("h264_10bit.mp4"));
    check(refused.path == VideoDecoder::Path::SoftwareRefused,
          "10-bit H.264 did not report Path::SoftwareRefused");
    check(refused.frames > 0, "the refused stream produced no frames at all");
    check(refused.deviceFrames == 0, "a refused stream produced a frame on the device");

    // GOAL: S13.38 -- a codec no decode queue here advertises reports SoftwareCodec, which is a
    //       different state from SoftwareRefused above and from SoftwareNoDecoder.
    const DecodeResult codec = decodeAll(device, asset("mpeg4.avi"));
    check(codec.path == VideoDecoder::Path::SoftwareCodec, "MPEG-4 Part 2 did not report Path::SoftwareCodec");
    check(codec.frames > 0, "the unsupported-codec stream produced no frames at all");
    check(codec.deviceFrames == 0, "an unsupported-codec stream produced a frame on the device");

    // The same file with the hardware decoder taken away: whatever that yields is what the reopen
    // must yield too, since both end up on ffmpeg's default decoder reading from the first packet.
    g_hideVideoExtensions = true;
    int softwareFrames = -1;
    {
        Device software(/*enableValidation=*/false);
        softwareFrames = decodeAll(software, asset("h264_10bit.mp4")).frames;
    }
    g_hideVideoExtensions = false;

    if (refused.frames != softwareFrames) {
        std::fprintf(stderr,
                     "FAIL: the software reopen produced %d frames where a never-hardware decode of the same file "
                     "produces %d\n",
                     refused.frames, softwareFrames);
        fail("the reopen did not decode the stream from its first packet");
    }

    std::printf("video_decoder_probe (refused): 10-bit H.264 -> %s (%d frames, against %d from a never-hardware "
                "decode of the same file); MPEG-4 -> %s (%d frames)\n",
                pathName(refused.path), refused.frames, softwareFrames, pathName(codec.path), codec.frames);
}

void runNoDecoderChecks(Device& device) {
    if (!haveAssets({"h264_crop.mp4"})) {
        g_notVerified = true;
        return;
    }

    // GOAL: S13.38 -- a device with no decode queue at all reports SoftwareNoDecoder, which is a
    //       different state from a codec being unsupported and from a stream being refused.
    // WHY-THIS-SHAPE: the device's video extensions are stripped from what the driver reports,
    //       rather than leaving this state to the software driver alone. A state only ever
    //       reached in continuous integration is not a state the shipping path is graded in --
    //       and this entry runs on the real GPU, where the stripped device otherwise behaves
    //       exactly as it does in production.
    // CONTROL: renderer/video/video_decoder.cpp, openDecoder()'s
    //       `if (m_device.info().videoDecodeOps == 0) m_path = Path::SoftwareNoDecoder;` changed
    //       to Path::SoftwareCodec. Observed: "FAIL: a device with no decode queue did not report
    //       Path::SoftwareNoDecoder". Reverted.
    check(device.info().videoDecodeOps == 0,
          "the video extensions were not stripped, so 'no decoder at all' was never exercised");
    check(device.videoDecodeFamilies().empty(), "a decode family survived having the video extensions stripped");

    const DecodeResult result = decodeAll(device, asset("h264_crop.mp4"));
    check(result.path == VideoDecoder::Path::SoftwareNoDecoder,
          "a device with no decode queue did not report Path::SoftwareNoDecoder");
    check(result.frames > 0, "a device with no decode queue decoded nothing");
    check(result.deviceFrames == 0, "a device with no decode queue produced a frame on the device");

    // The whole of S13.40 that a host frame can answer: the same questions, the same answers,
    // read from planes in host memory instead of an image.
    VideoDecoder decoder(device, asset("h264_crop.mp4"));
    std::optional<DecodedFrame> frame = decoder.next();
    if (!frame) {
        fail("the software decoder produced no first frame to inspect");
        return;
    }
    check(frame->storage() == DecodedFrame::Storage::HostPlanes, "the software path produced a device frame");
    check(frame->extent().width == 320 && frame->extent().height == 240,
          "a software frame does not report the coded extent");
    check(frame->visible().extent.width == 320 && frame->visible().extent.height == 238,
          "a software frame does not report the visible rectangle");
    check(frame->colour().matrix == AVCOL_SPC_BT709, "a software frame does not report the stream's colour matrix");
    const DecodedFrame::HostPlanes planes = frame->hostPlanes();
    check(planes.count >= 2, "a planar Y'CbCr software frame reports fewer than two planes");
    bool allPresent = true;
    for (uint32_t i = 0; i < planes.count; ++i) {
        if (planes.data[i] == nullptr || planes.stride[i] == 0) allPresent = false;
    }
    check(allPresent, "a software frame reports a plane with no data or no stride");

    bool refusedDeviceImage = false;
    try {
        (void)frame->deviceImage();
    } catch (const std::logic_error&) {
        refusedDeviceImage = true;
    }
    check(refusedDeviceImage, "deviceImage() on a host frame did not throw std::logic_error");

    std::printf("video_decoder_probe (no-decoder): %s with its video extensions stripped -> %s, %d frames, %u "
                "planes\n",
                device.info().gpuName.c_str(), pathName(result.path), result.frames, planes.count);
}

// The pair that grades the classifier. Both halves inject the SAME ffmpeg failure; only the
// device's answer to a bounded empty submit differs, and that alone must decide the outcome.
void runClassifierChecks(Device& device, bool deviceStops) {
    if (!haveAssets({"h264_crop.mp4"})) {
        g_notVerified = true;
        return;
    }
    if (device.info().videoDecodeOps == 0) {
        std::printf("video_decoder_probe: %s has no hardware decoder, so ffmpeg never reaches the frames-context "
                    "setup this entry fails -- not answerable here\n",
                    device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }

    // GOAL: S13.37 -- a capability refusal selects software; only a fault fails the wallpaper.
    //       The classifier is the device answering a bounded empty submit, not an error code.
    // WHY-THIS-SHAPE: the two halves are the same injected ffmpeg failure -- av_hwframe_ctx_init
    //       returning EIO after the device has already said yes -- differing only in whether the
    //       Device's own submissions still succeed. That is what makes each half the control for
    //       the other: no error code, no message and no file distinguishes them, so an
    //       implementation that classified on the error code would give both the same answer and
    //       exactly one of these two entries would go red. Producing the failure any other way
    //       is not possible: no file reaches this call, because a device that declines the
    //       stream declines it during the capability query instead.
    // CONTROL: renderer/video/video_decoder.cpp, attachHardwareFrames()'s `if (!deviceHealthy())`
    //       changed to `if (false)`. Observed: the fault entry printed "FAIL: a hardware failure
    //       on a device that no longer answers did not raise GpuFailure". The mirrored mutation,
    //       `if (true)`, makes the refusal entry print "FAIL: a hardware failure on a device that
    //       still answers raised GpuFailure". Both reverted.
    g_failNextFramesInit = true;
    g_deviceLost = deviceStops;

    bool sawGpuFailure = false;
    DecodeResult result;
    try {
        result = decodeAll(device, asset("h264_crop.mp4"));
    } catch (const GpuFailure& e) {
        sawGpuFailure = true;
        std::printf("video_decoder_probe: GpuFailure as expected: %s\n", e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: the injected hardware failure raised %s\n", e.what());
        fail("the injected hardware failure raised neither GpuFailure nor a software fallback");
    }

    // Cleared before anything else runs, teardown included: leaving the device reporting lost
    // would turn every later Vulkan call into a second, unrelated failure.
    g_deviceLost = false;
    g_failNextFramesInit = false;

    if (deviceStops) {
        check(sawGpuFailure,
              "a hardware failure on a device that no longer answers did not raise GpuFailure");
    } else {
        check(!sawGpuFailure, "a hardware failure on a device that still answers raised GpuFailure");
        check(result.path == VideoDecoder::Path::SoftwareRefused,
              "a hardware failure on a device that still answers did not select Path::SoftwareRefused");
        check(result.frames > 0, "a hardware failure on a healthy device lost the wallpaper's frames");
        check(result.deviceFrames == 0, "a stream that fell back to software produced a frame on the device");
        std::printf("video_decoder_probe (refused-after-setup): same ffmpeg failure, healthy device -> %s, %d "
                    "frames\n",
                    pathName(result.path), result.frames);
    }
}

// ------------------------------------------------------------------------- //
// S13.42 / S13.43 -- damaged packets, and whose fault a bad file is
// ------------------------------------------------------------------------- //

// Attaches a writer to each of `fifos` if the calling thread is still inside the guarded work after
// a deadline, and again every half second until that work ends -- which is what releases an open
// parked on a pipe nobody writes to. The decoder cannot be built on a thread of its own (a Device
// pins itself to its creating thread and a failed construction unwinds through it), so a watchdog
// that frees the main thread is the only way a regression here reds instead of hanging the probe.
class PipeRescuer {
public:
    explicit PipeRescuer(std::vector<std::string> fifos) : m_fifos(std::move(fifos)) {
        m_thread = std::thread([this] {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_done; })) return;
            m_rescued = true;
            while (!m_done) {
                for (const std::string& fifo : m_fifos) {
                    const int writer = ::open(fifo.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
                    if (writer >= 0) ::close(writer);
                }
                m_changed.wait_for(lock, std::chrono::milliseconds(500), [this] { return m_done; });
            }
        });
    }
    ~PipeRescuer() { finish(); }
    PipeRescuer(const PipeRescuer&) = delete;
    PipeRescuer& operator=(const PipeRescuer&) = delete;

    // True when the guarded work had to be released from a parked open.
    bool finish() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done = true;
        }
        m_changed.notify_all();
        if (m_thread.joinable()) m_thread.join();
        return m_rescued;
    }

private:
    std::vector<std::string> m_fifos;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_done = false;
    bool m_rescued = false;
    std::thread m_thread;
};

// GOAL: S13.177 -- a wallpaper whose CONTENT names further files is refused at once, as the file's
//       fault, and never makes ffmpeg open anything by itself: an ffconcat script and an HLS playlist
//       each listing a named pipe with no writer. Before this fix such a container wedged the
//       decoding thread exactly as a pipe given as the wallpaper itself did, because ffmpeg opened
//       the listed file by path on that thread -- a path the daemon never checked.
// WHY-THIS-SHAPE: three containers, because the refusal has two halves and they cover different
//       ways in. A demuxer that opens a file for its own context goes through the context's io_open
//       hook; one that opens a whole CONTAINER of its own -- the concat demuxer does -- builds a
//       fresh context that inherits the protocol list and nothing else. A third script lists an
//       ordinary video rather than a pipe: it must be refused too (nothing may be opened by path)
//       and it can never park, so it grades the refusal without the watchdog ever mattering.
//       "Refused" is not enough on its own -- a parked open that the watchdog released also ends in
//       an error -- so the rescue flag is what fails the park, and the rescue is what lets that
//       failure be printed instead of hanging the run.
// CONTROL: renderer/video/video_decoder.cpp, three runs, all reverted.
//       (1) `protocol_whitelist` not set, io_open kept -> both ffconcat cases red: the pipe one
//       "parked the decoding thread in an open until a writer appeared", released by the watchdog
//       and printed; the ordinary one "was opened". The HLS case stayed GREEN -- io_open refused
//       its segment on its own.
//       (2) io_open not set, list kept -> all three green: ffmpeg's default open applies the
//       context's protocol list, so the list alone refuses every case here.
//       (3) both removed -> all three red, the HLS one printing ffmpeg's "Opening '.../segment.ts'
//       for reading" before parking, which is what shows its segment open is really reached.
//       So the protocol list is load-bearing on its own; io_open is a working second defence for
//       the HLS path (run 1) but no case this probe can build needs it alone (run 2).
// COVERAGE-ASSERT: the count of cases graded is asserted against a literal.
void runMultiFileChecks(Device& device, const ScratchDir& scratch) {
    const std::string pipe = scratch.file("listed.pipe");
    const std::string segment = scratch.file("segment.ts");
    const std::string ordinary = scratch.file("listed-ordinary.mp4");
    const std::string concatPipe = scratch.file("concat-pipe.mp4");
    const std::string hlsPipe = scratch.file("playlist.m3u8");
    const std::string concatOrdinary = scratch.file("concat-ordinary.mp4");
    std::error_code ec;
    std::filesystem::copy_file(asset("h264_crop.mp4"), ordinary, ec);
    if (ec || ::mkfifo(pipe.c_str(), 0600) != 0 || ::mkfifo(segment.c_str(), 0600) != 0) {
        fail("video_decoder_probe (content): could not make the pipes and copies the multi-file cases list, so "
             "none was exercised");
        return;
    }
    {
        std::ofstream(concatPipe) << "ffconcat version 1.0\nfile listed.pipe\n";
        std::ofstream(hlsPipe) << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXTINF:1.0,\nsegment.ts\n"
                                  "#EXT-X-ENDLIST\n";
        std::ofstream(concatOrdinary) << "ffconcat version 1.0\nfile listed-ordinary.mp4\n";
    }

    const std::pair<const char*, const std::string*> cases[] = {
        {"an ffconcat script listing a named pipe", &concatPipe},
        {"an HLS playlist whose segment is a named pipe", &hlsPipe},
        {"an ffconcat script listing an ordinary video", &concatOrdinary},
    };
    constexpr int kCases = 3;
    check(static_cast<int>(sizeof(cases) / sizeof(cases[0])) == kCases,
          "the multi-file case list no longer has the three entries this check is written for");

    int graded = 0;
    for (const auto& [what, path] : cases) {
        std::string said;
        bool asContent = false;
        bool opened = false;
        bool rescued = false;
        {
            PipeRescuer rescuer({pipe, segment});
            try {
                VideoDecoder decoder(device, *path);
                opened = true;
            } catch (const WallpaperContentError& e) {
                asContent = true;
                said = e.what();
            } catch (const std::exception& e) {
                said = e.what();
            }
            rescued = rescuer.finish();
        }
        std::printf("video_decoder_probe (content): %s -> %s%s\n", what,
                    opened ? "OPENED" : said.c_str(), rescued ? " [released from a parked open by the watchdog]" : "");
        check(!rescued, std::string(what) + " parked the decoding thread in an open until a writer appeared");
        check(!opened, std::string(what) + " was opened, so ffmpeg reached a file by a path nobody checked");
        check(asContent && said.find(*path) != std::string::npos,
              std::string(what) + " was not refused as a content error naming the wallpaper (said: " + said + ")");
        // GOAL: S15.325 -- the refusal says what the wallpaper is in words a user can act on: a list
        //       of other files to play, of which only a single self-contained video can be shown --
        //       not ffmpeg's "Invalid argument" or "Invalid data found when processing input".
        // CONTROL: renderer/video/video_decoder.cpp, throwOpenFailure()'s list of multi-file demuxer
        //       names emptied, via control_run.sh, 2026-10-05 -> "FAIL: an ffconcat script listing a
        //       named pipe is not described as a list of other files (said: VideoDecoder: cannot open
        //       the wallpaper (Invalid argument): ...)", and the same for the other two cases (the HLS
        //       one "Invalid data found when processing input"). Reverted.
        check(said.find("is a list of other files to play") != std::string::npos &&
                  said.find("only a single self-contained video file can be shown") != std::string::npos &&
                  said.find("Invalid argument") == std::string::npos && said.find("Invalid data") == std::string::npos,
              std::string(what) + " is not described as a list of other files (said: " + said + ")");
        ++graded;
    }
    check(graded == kCases, "every multi-file case ran (" + std::to_string(graded) + " of 3)");
}

void runContentChecks(Device& device) {
    if (!haveAssets({"h264_raw.h264", "audio_only.m4a"})) {
        g_notVerified = true;
        return;
    }
    ScratchDir scratch("wallpaperx-decoder");
    if (!scratch.ok()) {
        fail("video_decoder_probe (content): could not make a scratch directory, so nothing here was exercised");
        return;
    }

    // GOAL: S13.43 -- a file the decoder cannot use is the content's fault. WallpaperContentError
    //       and not GpuFailure, which the daemon turns into a permanently unhealthy state over a
    //       file the user could simply replace.
    // CONTROL: renderer/video/video_decoder.cpp, two, because two different sites raise these,
    //       both reverted. (1) throwFileFailure() raising GpuFailure: red for the missing file and
    //       the non-container, and the coverage count with them; the multi-file cases in
    //       runMultiFileChecks go red with it. Observed 2026-09-21. (2) openContainer()'s explicit
    //       `throw WallpaperContentError(... " has no video stream")` changed to GpuFailure: red
    //       for the audio-only file. Neither alone covers all three.
    struct BadFile {
        const char* what;
        std::string path;
    };
    const std::string missing = scratch.file("does-not-exist.mp4");
    const std::string notAVideo = scratch.file("not-a-video.mp4");
    {
        std::ofstream out(notAVideo);
        out << "this is not a container, it is a sentence\n";
    }
    const std::vector<BadFile> bad{
        {"a file that does not exist", missing},
        {"a file that is not a container", notAVideo},
        {"a container with no video stream", asset("audio_only.m4a")},
    };
    int refusedAsContent = 0;
    for (const BadFile& entry : bad) {
        try {
            VideoDecoder decoder(device, entry.path);
            std::fprintf(stderr, "FAIL: %s was opened without complaint\n", entry.what);
            fail("a file the decoder cannot use was accepted");
        } catch (const WallpaperContentError&) {
            ++refusedAsContent;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: %s raised %s instead of WallpaperContentError\n", entry.what, e.what());
            fail("a bad file was blamed on something other than the content");
        }
    }
    // COVERAGE-ASSERT: against a literal three, NOT against bad.size(). Comparing with the list's
    // own length is what a hollowed-out coverage assert looks like: deleting an entry then
    // changes both sides together and the run stays green -- observed, on the first version of
    // this line, by deleting the audio-only entry and watching it pass while reporting two.
    constexpr int kUnusableFiles = 3;
    check(static_cast<int>(bad.size()) == kUnusableFiles,
          "the list of unusable files no longer has the three entries this check is written for");
    check(refusedAsContent == kUnusableFiles, "not every unusable file was refused as a content error");

    // GOAL: S13.182 -- every refusal that names a wallpaper file is worded `VideoDecoder: reason:
    //       path`, reason first and the path last. The daemon keeps only the head of a recorded
    //       error, so a path placed ahead of the reason is all that survives a long one and the
    //       only account of the failure a client gets is lost.
    // WHY-THIS-SHAPE: three failures raised at three different sites -- the open, ffmpeg's own
    //       container error, and the decoder's explicit "no video stream" -- because the property is
    //       that every site goes through one helper, and one site graded says nothing about the
    //       others. That a long path really does lose a path-first reason is graded at the daemon.
    // CONTROL: renderer/video/video_decoder.cpp, the "no video stream" throw put back to the
    //       pre-helper `"VideoDecoder: " + m_file + " has no video stream"` -> "FAIL: a container
    //       with no video stream: the refusal is not worded `VideoDecoder: reason: path`" and the
    //       count "2 of 3". Reverted.
    {
        const std::pair<const char*, std::string> sites[] = {
            {"a file that does not exist", missing},
            {"a file that is not a container", notAVideo},
            {"a container with no video stream", asset("audio_only.m4a")},
        };
        int wellWorded = 0;
        for (const auto& [what, path] : sites) {
            std::string said;
            try {
                VideoDecoder decoder(device, path);
            } catch (const std::exception& e) {
                said = e.what();
            }
            const std::string tail = ": " + path;
            const bool pathLast = said.size() > tail.size() && said.compare(said.size() - tail.size(), tail.size(), tail) == 0;
            const bool componentFirst = said.rfind("VideoDecoder: ", 0) == 0;
            // The reason is what sits between the two, and there must be one.
            const bool hasReason = pathLast && componentFirst && said.size() > tail.size() + 14;
            check(pathLast && componentFirst && hasReason,
                  std::string(what) + ": the refusal is not worded `VideoDecoder: reason: path` (said: " + said + ")");
            if (pathLast && componentFirst && hasReason) ++wellWorded;
        }
        check(wellWorded == 3, "every decoder refusal graded here names its path last (" + std::to_string(wellWorded) +
                                   " of 3)");
    }

    // GOAL: S13.169, the video half -- a wallpaper path that is not a regular file is refused BY
    //       THE VIDEO DECODER, naming the path, rather than handed to ffmpeg. That a path like
    //       this is refused at all belongs to openWallpaperFile and is graded in
    //       wallpaper_file_probe; what is graded here is that VideoDecoder goes through that door
    //       -- it builds its own AVIOContext precisely so ffmpeg is never given the path, and if
    //       that ever regressed to avio_open2 this is what would say so.
    // WHY-THIS-SHAPE: the refusal's WORDING is what is asserted, not merely that
    //       WallpaperContentError came out. Without the fix both of these still raise
    //       WallpaperContentError -- ffmpeg reports "Invalid data found" for the device and EISDIR
    //       for the directory, and both are classed as the file's fault -- so "it threw the right
    //       type" is green either way and grades nothing.
    //       A named pipe is deliberately NOT among the kinds, though it is what the finding was
    //       about: with the refusal removed the open returns and the decoder then blocks in
    //       read(2) on the empty pipe for ever, so a pipe case makes this probe HANG under its own
    //       control instead of reddening -- measured in image_bounds_probe, whose run had to be
    //       killed. The pipe is graded in wallpaper_file_probe, on a thread with a deadline.
    // CONTROL: renderer/wallpaper_file.cpp, `if (!S_ISREG(info.st_mode))` -> `if (false)` -> three
    //       FAILs: "a directory was not refused as not-a-regular-file (said: VideoDecoder: cannot
    //       open ...: Is a directory)", the same for /dev/null saying "Invalid data found when
    //       processing input", and the count "0 of 2". Both messages are WallpaperContentError,
    //       which is why the wording and not the type is what is asserted. Reverted, green.
    // COVERAGE-ASSERT: kIrregularKinds is a literal against the list's own length.
    {
        const std::string videoDirectory = scratch.file("a-directory.mp4");
        std::error_code dirEc;
        std::filesystem::create_directory(videoDirectory, dirEc);
        if (dirEc) {
            fail("video_decoder_probe (content): could not make a directory to offer the decoder, so "
                 "the not-a-regular-file cases were not exercised");
        } else {
            const std::string charDevice = "/dev/null";
            const std::pair<const char*, const std::string*> kinds[] = {
                {"a directory", &videoDirectory},
                {"a character device", &charDevice},
            };
            constexpr int kIrregularKinds = 2;
            check(static_cast<int>(sizeof(kinds) / sizeof(kinds[0])) == kIrregularKinds,
                  "the decoder's irregular-path list no longer has the two entries this check is written for");

            int refusedByKind = 0;
            for (const auto& [what, path] : kinds) {
                std::string said;
                try {
                    VideoDecoder decoder(device, *path);
                    said = "it was opened without complaint";
                } catch (const std::exception& e) {
                    said = e.what();
                }
                std::printf("video_decoder_probe (content): the decoder given %s: %s\n", what, said.c_str());
                const bool named = said.find("is not a regular file") != std::string::npos &&
                                   said.find(*path) != std::string::npos;
                check(named, std::string(what) + " was not refused as not-a-regular-file (said: " + said + ")");
                if (named) ++refusedByKind;
            }
            check(refusedByKind == kIrregularKinds,
                  "every irregular path was refused by the decoder (" + std::to_string(refusedByKind) + " of " +
                      std::to_string(kIrregularKinds) + ")");
        }
    }

    runMultiFileChecks(device, scratch);

    // GOAL: S13.42 -- a real file with damaged payload does not kill the wallpaper. End to end:
    //       it decodes to fewer pictures than the same file undamaged, and to more than none.
    // WHY-THIS-SHAPE: graded on the software path, with the device's video extensions stripped.
    //       Surviving damage is a property of the decode loop and not of where the pixels land,
    //       so the software path answers it in full; and ffmpeg's hardware decoder responds to a
    //       damaged reference picture by emitting further validation complaints of its own,
    //       which would force this probe's excused-message list to grow to cover a dependency's
    //       reaction to deliberately broken input.
    // NO PRODUCTION CONTROL, deliberately, and the reason is the finding: measured 2026-09-10,
    //       damaged bytes reach NO error branch of VideoDecoder at all. ffmpeg conceals the
    //       damage internally and simply emits fewer pictures, so making either damaged-packet
    //       branch throw leaves this phase green. What this phase does grade is the end-to-end
    //       outcome; the branches themselves are graded by injection immediately below. Its own
    //       control is on the harness: exclusive-ORing with 0x00 instead of 0xFF leaves
    //       damagedBytes non-zero and the streams identical, and the comparison below reports
    //       "the damaged fixture is not damaged".
    // COVERAGE-ASSERT: damagedBytes and the frame-count comparison both have to hold, so a
    //       fixture that stopped being damaged fails here instead of grading an intact stream.
    const std::string damaged = scratch.file("damaged.h264");
    const int damagedBytes = writeDamagedCopy(asset("h264_raw.h264"), damaged);
    if (damagedBytes == 0) {
        fail("video_decoder_probe (content): could not write a damaged copy, so packet damage was not exercised");
        return;
    }

    g_hideVideoExtensions = true;
    int cleanFrames = -1;
    int damagedFrames = -1;
    bool damagedThrew = false;
    {
        Device software(/*enableValidation=*/false);
        cleanFrames = decodeAll(software, asset("h264_raw.h264")).frames;
        try {
            damagedFrames = decodeAll(software, damaged).frames;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: a damaged packet was not skipped: %s\n", e.what());
            damagedThrew = true;
        }
    }
    g_hideVideoExtensions = false;

    check(!damagedThrew, "decoding a stream with damaged packets raised instead of skipping them");
    check(cleanFrames > 0, "the undamaged reference stream decoded to nothing");
    check(damagedFrames > 0, "a stream with damaged packets decoded to nothing at all");
    if (damagedFrames >= cleanFrames) {
        std::fprintf(stderr,
                     "FAIL: %d bytes were flipped but the damaged stream still decoded %d frames against the clean "
                     "stream's %d, so nothing was actually damaged\n",
                     damagedBytes, damagedFrames, cleanFrames);
        fail("the damaged fixture is not damaged, so skipping was never exercised");
    }

    std::printf("video_decoder_probe (content): %d files refused as content errors; %d flipped bytes took the "
                "stream from %d frames to %d\n",
                refusedAsContent, damagedBytes, cleanFrames, damagedFrames);

    // GOAL: S13.42 -- each of the three answers ffmpeg can give that this decoder is written to
    //       absorb is actually absorbed: AVERROR_INVALIDDATA from the send, AVERROR_INVALIDDATA
    //       from the receive, and an EAGAIN that means "show me that packet again".
    // WHY-THIS-SHAPE: injected from ffmpeg's own entry points rather than produced by damaging a
    //       file, because damaging a file does not reach any of them -- see the note on the
    //       injection counters above, and the negative control recorded there. Without the
    //       injection these three branches are exactly the kind of code a green suite is blind
    //       to: written defensively, never executed, and free to be wrong.
    //       The frame count is compared against the clean baseline decoded in this same
    //       process, so "the skip lost nothing it should not have" is a measurement.
    // CONTROL: renderer/video/video_decoder.cpp, three mutations, all reverted. (1) `if (err < 0
    //       && !isDamagedPacket(err)) failDecode(...)` narrowed to `if (err < 0)`: "FAIL: a
    //       damaged packet reported by avcodec_send_packet was not skipped: ... Invalid data
    //       found when processing input". (2) the `if (isDamagedPacket(err)) continue;` after
    //       avcodec_receive_frame changed to `failDecode(...)`: the same line naming
    //       avcodec_receive_frame. (3) `const bool resend = m_packet->data != nullptr;` changed
    //       to `= false`: "FAIL: a packet ffmpeg asked to see again was dropped: 15 frames
    //       against the clean stream's 20".
    // COVERAGE-ASSERT: each phase asserts the injection was actually consumed, so a future
    //       change that stops reaching the entry point fails here rather than grading a run in
    //       which nothing was injected.
    g_hideVideoExtensions = true;
    {
        Device software(/*enableValidation=*/false);

        struct Injection {
            const char* what;
            int* counter;
            int amount;
            bool expectSameCount;
        };
        const std::vector<Injection> injections{
            {"a damaged packet reported by avcodec_send_packet", &g_sendInvalidDataRemaining, 3, false},
            {"a damaged packet reported by avcodec_receive_frame", &g_receiveInvalidDataRemaining, 3, true},
            {"a packet ffmpeg asked to see again", &g_sendEagainRemaining, 1, true},
        };
        for (const Injection& injection : injections) {
            int frames = -1;
            bool threw = false;
            int armedAtCall = INT_MAX;
            g_firstInjectionCall = -1;
            try {
                frames = decodeAll(software, asset("h264_raw.h264"), [&] {
                            armedAtCall = g_ffmpegDecodeCalls;
                            *injection.counter = injection.amount;
                        }).frames;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "FAIL: %s was not skipped: %s\n", injection.what, e.what());
                threw = true;
            }
            const int unconsumed = *injection.counter;
            *injection.counter = 0;
            if (threw) {
                fail("an ffmpeg answer this decoder is written to absorb was raised instead");
                continue;
            }
            if (unconsumed != 0) {
                std::fprintf(stderr, "FAIL: %d of %d injections of '%s' were never consumed, so that branch was "
                                     "not reached\n",
                             unconsumed, injection.amount, injection.what);
                fail("an injected ffmpeg answer never reached the decoder");
                continue;
            }
            if (g_firstInjectionCall <= armedAtCall) {
                std::fprintf(stderr,
                             "FAIL: '%s' was injected at ffmpeg decode call %d, which is not after the call the "
                             "decoder finished opening at (%s), so it was spent inside the constructor's own "
                             "probing decode and never reached VideoDecoder::next()\n",
                             injection.what, g_firstInjectionCall,
                             armedAtCall == INT_MAX ? "never armed" : std::to_string(armedAtCall).c_str());
                fail("an injected ffmpeg answer was spent before the decode loop was entered");
                continue;
            }
            check(frames > 0, "an injected ffmpeg answer left the stream decoding nothing at all");
            if (injection.expectSameCount && frames != cleanFrames) {
                std::fprintf(stderr, "FAIL: %s was dropped: %d frames against the clean stream's %d\n",
                             injection.what, frames, cleanFrames);
                fail("absorbing an ffmpeg answer cost frames it should not have");
            }
            if (!injection.expectSameCount && frames >= cleanFrames) {
                std::fprintf(stderr, "FAIL: %d packets were refused but the stream still decoded %d frames against "
                                     "the clean stream's %d\n",
                             injection.amount, frames, cleanFrames);
                fail("a refused packet cost nothing, so it was not actually refused");
            }
            std::printf("video_decoder_probe (content): %d x '%s' -> %d frames (clean: %d)\n", injection.amount,
                        injection.what, frames, cleanFrames);
        }

        // GOAL: S13.42's other half -- a second EAGAIN for the SAME packet is ffmpeg's own
        //       contract broken, and failing is better than spinning on it forever.
        // WHY-THIS-SHAPE: the assertion is on the exception TYPE, because the alternative to
        //       throwing here is an infinite loop, and a check that graded "it did not spin"
        //       could only do so with a timeout -- which X.1 forbids as a success path.
        // CONTROL: renderer/video/video_decoder.cpp, that `throw std::logic_error(...)` changed
        //       to `throw std::runtime_error(...)`. Observed: "FAIL: a packet ffmpeg refused
        //       twice did not raise std::logic_error". Reverted.
        bool refusedTwice = false;
        int armedAtCall = INT_MAX;
        g_firstInjectionCall = -1;
        try {
            (void)decodeAll(software, asset("h264_raw.h264"), [&] {
                armedAtCall = g_ffmpegDecodeCalls;
                g_sendEagainRemaining = 2;
            });
        } catch (const std::logic_error&) {
            refusedTwice = true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "video_decoder_probe (content): a packet refused twice raised %s\n", e.what());
        }
        const int unconsumed = g_sendEagainRemaining;
        g_sendEagainRemaining = 0;
        check(unconsumed == 0, "the second EAGAIN was never consumed, so the same-packet-twice guard was not reached");
        check(g_firstInjectionCall > armedAtCall,
              "the EAGAIN pair was spent before the decode loop was entered, so the guard was not reached");
        check(refusedTwice, "a packet ffmpeg refused twice did not raise std::logic_error");
    }
    g_hideVideoExtensions = false;
}

}  // namespace

// ------------------------------------------------------------------------- //
// Interposers
// ------------------------------------------------------------------------- //

extern "C" VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* layerName,
                                                         uint32_t* count, VkExtensionProperties* properties) {
    using Fn = VkResult (*)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkEnumerateDeviceExtensionProperties"));
    if (real == nullptr) {
        std::fprintf(stderr, "video_decoder_probe: cannot resolve the real vkEnumerateDeviceExtensionProperties\n");
        std::abort();
    }
    if (!g_hideVideoExtensions) return real(physicalDevice, layerName, count, properties);
    return wallpaperx::testing::enumerateWithoutVideoExtensions(real, physicalDevice, layerName, count, properties);
}

extern "C" VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    if (g_recordingSubmits) {
        for (uint32_t i = 0; i < count; ++i) {
            RecordedSubmit record;
            const VkSubmitInfo& info = submits[i];
            for (uint32_t w = 0; w < info.waitSemaphoreCount; ++w) {
                record.waitSemaphores.push_back(info.pWaitSemaphores[w]);
                record.waitStages.push_back(info.pWaitDstStageMask[w]);
            }
            for (uint32_t s = 0; s < info.signalSemaphoreCount; ++s) {
                record.signalSemaphores.push_back(info.pSignalSemaphores[s]);
            }
            for (const auto* node = static_cast<const VkBaseInStructure*>(info.pNext); node != nullptr;
                 node = node->pNext) {
                if (node->sType != VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) continue;
                const auto* timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(node);
                record.hasTimelineInfo = true;
                for (uint32_t w = 0; w < timeline->waitSemaphoreValueCount; ++w) {
                    record.waitValues.push_back(timeline->pWaitSemaphoreValues[w]);
                }
                for (uint32_t s = 0; s < timeline->signalSemaphoreValueCount; ++s) {
                    record.signalValues.push_back(timeline->pSignalSemaphoreValues[s]);
                }
            }
            g_submits.push_back(std::move(record));
        }
    }
    if (g_deviceLost) return VK_ERROR_DEVICE_LOST;
    if (g_failNextSubmitOnce) {
        g_failNextSubmitOnce = false;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    using Fn = VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkQueueSubmit"));
    if (real == nullptr) {
        std::fprintf(stderr, "video_decoder_probe: cannot resolve the real vkQueueSubmit\n");
        std::abort();
    }
    return real(queue, count, submits, fence);
}

// Records the call ordinal of the first injected answer, so a phase can prove its injection
// landed after the decoder was open rather than inside the constructor's own probing decode.
static void noteInjection() {
    if (g_firstInjectionCall < 0) g_firstInjectionCall = g_ffmpegDecodeCalls;
}

extern "C" int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    ++g_ffmpegDecodeCalls;
    if (g_sendEagainRemaining > 0) {
        --g_sendEagainRemaining;
        noteInjection();
        return AVERROR(EAGAIN);
    }
    if (g_sendInvalidDataRemaining > 0) {
        --g_sendInvalidDataRemaining;
        noteInjection();
        return AVERROR_INVALIDDATA;
    }
    using Fn = int (*)(AVCodecContext*, const AVPacket*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avcodec_send_packet"));
    if (real == nullptr) {
        std::fprintf(stderr, "video_decoder_probe: cannot resolve the real avcodec_send_packet\n");
        std::abort();
    }
    return real(context, packet);
}

extern "C" int avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    ++g_ffmpegDecodeCalls;
    if (g_receiveInvalidDataRemaining > 0) {
        --g_receiveInvalidDataRemaining;
        noteInjection();
        return AVERROR_INVALIDDATA;
    }
    using Fn = int (*)(AVCodecContext*, AVFrame*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avcodec_receive_frame"));
    if (real == nullptr) {
        std::fprintf(stderr, "video_decoder_probe: cannot resolve the real avcodec_receive_frame\n");
        std::abort();
    }
    return real(context, frame);
}

extern "C" int av_hwframe_ctx_init(AVBufferRef* ref) {
    if (g_failNextFramesInit) {
        g_failNextFramesInit = false;
        return AVERROR(EIO);
    }
    using Fn = int (*)(AVBufferRef*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "av_hwframe_ctx_init"));
    if (real == nullptr) {
        std::fprintf(stderr, "video_decoder_probe: cannot resolve the real av_hwframe_ctx_init\n");
        std::abort();
    }
    return real(ref);
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode != "hardware" && mode != "refused" && mode != "no-decoder" && mode != "refused-after-setup" &&
        mode != "fault" && mode != "content") {
        std::fprintf(stderr, "usage: %s hardware|refused|no-decoder|refused-after-setup|fault|content\n", argv[0]);
        return 1;
    }
    // The no-decoder entry's whole subject is a Device built without video extensions, so the
    // stripping is on before the Device is constructed rather than switched on mid-run.
    g_hideVideoExtensions = mode == "no-decoder";

    const bool graded = wallpaperx::testing::gradedRun(
        "video_decoder_probe (" + mode + ")", [&](wallpaperx::testing::ValidationGrade& grade) {
            const std::string label = "video_decoder_probe (" + mode + ")";
            wallpaperx::testing::DecoderValidationOracle oracle;
            {
                std::optional<Device> device;
                try {
                    device.emplace(/*enableValidation=*/true);
                    oracle.attach(*device, grade);

                    if (mode == "hardware") {
                        runHardwareChecks(*device);
                    } else if (mode == "refused") {
                        runRefusedChecks(*device);
                    } else if (mode == "no-decoder") {
                        runNoDecoderChecks(*device);
                    } else if (mode == "refused-after-setup") {
                        runClassifierChecks(*device, /*deviceStops=*/false);
                    } else if (mode == "fault") {
                        runClassifierChecks(*device, /*deviceStops=*/true);
                    } else {
                        runContentChecks(*device);
                    }
                } catch (const std::exception& e) {
                    // Reported, not rethrown: an escaping exception would skip both the
                    // reconciliation below and gradedRun's own verdict.
                    fail(std::string("video_decoder_probe threw: ") + e.what());
                }
                // Both switched off unconditionally: a check that returned early with one still
                // set would poison the teardown that follows and report as a second failure with
                // no bearing on what was being graded.
                g_deviceLost = false;
                g_failNextSubmitOnce = false;
                oracle.detach();
            }
            // After the Device is gone: see DecoderValidationOracle::reconcile.
            oracle.reconcile(label.c_str());
            return wallpaperx::testing::failureCount() == 0;
        });

    const int failures = wallpaperx::testing::reportFailures("video_decoder_probe");
    if (!graded || failures > 0) return 1;
    if (g_notVerified) {
        std::printf("video_decoder_probe (%s): NOT VERIFIED on this driver -- reported to ctest as a skip, not a "
                    "pass\n",
                    mode.c_str());
        return kNotVerifiedExitCode;
    }
    std::printf("video_decoder_probe (%s): all checks passed, no unexpected validation messages\n", mode.c_str());
    return 0;
}
