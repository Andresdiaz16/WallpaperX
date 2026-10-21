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
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <map>
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
// AVERROR_INVALIDDATA -- ffmpeg conceals the damage and simply emits fewer pictures, so no damaged file reaches
// either branch, and none is decoded here. So the branches are graded
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

// While set, each packet sent to a decoder: the skip_frame it went with, and whether it was a key packet.
// Set only once the decoder is open, so avformat_find_stream_info()'s own sends are not in it.
bool g_recordSends = false;
std::vector<std::pair<int, bool>> g_sends;
// Beside each send: whether the packet holds an H.264 IDR slice, read from its length-prefixed NAL units.
std::vector<bool> g_sendsIdr;

// The thinning tag checks (runThinningTagChecks). While g_recordThinning is set, each packet's first real send
// records, by its presentation time in ns, the grids' number the test had in force (g_numberInForce) and the
// packet's `opaque` tag as VideoDecoder::decode() set it (renderer/video/video_decoder.cpp tagSent: bits 0-1 the
// level, bit 2 thinned, bits 3-10 the grids' number, the packet's number above bit 10).
bool g_recordThinning = false;
uint8_t g_numberInForce = 0;
std::map<int64_t, uint8_t> g_numberAtSend;
std::vector<uintptr_t> g_thinningTags;
// Armed: the next send whose tag says thinned is refused with EAGAIN, and inside that same call the decoder is
// given g_renumberTo, as a caller's change landing between a send and its resend would be. g_refusedTag is what
// that send carried; g_resendTag what the same packet carried when sent again.
VideoDecoder* g_thinDecoder = nullptr;
std::optional<wallpaperx::NumberedGrids> g_renumberTo;
bool g_eagainOnThinned = false;
std::optional<uintptr_t> g_refusedTag;
std::optional<uintptr_t> g_resendTag;
// Armed: the next send on a hardware codec whose tag says thinned fails with AVERROR_EXTERNAL, the hardware
// decoder failing on a device that still answers, which reopens the stream in software (runThinnedReopenCheck).
bool g_externalOnThinned = false;
bool g_externalFired = false;
constexpr uintptr_t kTagThinnedBit = 4;
constexpr unsigned kTagPacketShift = 11;

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

// While set, every pixel format descriptor this executable's own code looks up reads as a hardware
// format -- a stream that offers a software codec nothing it can hold. Atomics: ffmpeg's frame threads
// run the format callback.
std::atomic<bool> g_everyFormatHardware{false};
std::atomic<int> g_formatsShownAsHardware{0};
std::atomic<int> g_sendsWhileHardwareOnly{0};

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


// ------------------------------------------------------------------------- //
// Decoding helpers
// ------------------------------------------------------------------------- //

struct DecodeResult {
    int frames = 0;
    int deviceFrames = 0;
    int hostFrames = 0;
    VideoDecoder::Path path = VideoDecoder::Path::Undecided;
    // The first picture's readSince(): the packets read since the decoder opened, or since a reopen's rewind.
    uint32_t firstReadSince = 0;
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
        if (++result.frames == 1) result.firstReadSince = frame->readSince();
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

// What a decode of `file` to its end returns when every call names `skip`: how many pictures, whether each
// carried a presentation time and those times rose, whether each says it was decoded at `skip` and is of a
// type that level keeps, whether any packet went to ffmpeg as AVDISCARD_BIDIR, and the path it took.
struct SkipDecode {
    int pictures = 0;
    bool timed = true;
    bool rising = true;
    bool tagged = true;
    bool kept = true;
    bool sentBidirectional = false;
    VideoDecoder::Path path = VideoDecoder::Path::Undecided;
};
SkipDecode decodeSkipping(Device& device, const std::string& file, wallpaperx::PictureSkip skip) {
    SkipDecode out;
    VideoDecoder decoder(device, file);
    g_sends.clear();
    g_recordSends = true;
    struct Stop {
        ~Stop() { g_recordSends = false; }
    } stop;
    std::optional<std::chrono::nanoseconds> last;
    while (std::optional<DecodedFrame> frame = decoder.next(skip)) {
        ++out.pictures;
        if (frame->skip() != skip) out.tagged = false;
        // keptThrough() takes every B picture for an unreferenced one, which the middle B of a pyramid is
        // not: graded from NonIntra up, where no B picture is decoded at all.
        if (skip >= wallpaperx::PictureSkip::NonIntra && frame->keptThrough() < skip) out.kept = false;
        const std::optional<std::chrono::nanoseconds> at = frame->presentationTime();
        if (!at) {
            out.timed = false;
            continue;
        }
        if (last && *at <= *last) out.rising = false;
        last = at;
    }
    out.sentBidirectional =
        std::any_of(g_sends.begin(), g_sends.end(), [](const auto& send) { return send.first == AVDISCARD_BIDIR; });
    out.path = decoder.path();
    return out;
}

void runSkipLevelChecks(Device& device, const char* label, bool hardware, const SkipDecode& all, const SkipDecode& some);
void runThinnedReopenCheck(Device& device);

// GOAL: S15.576 -- VideoDecoder::next(PictureSkip::Unreferenced) leaves out the pictures nothing references:
//       counter_long.mp4 (H.264, three B-frames between references, the middle one referenced) decodes to
//       fewer than three quarters of its 321 pictures, each with a presentation time and those rising;
//       next(PictureSkip::Nothing) returns all 321. On the device here (`hardware`), in software in `content`.
//       False if the request never reaches the codec, reaches it always, or drops pictures out of order.
// WHY-THIS-SHAPE: counts against the same decoder unskipped, so the fixture's own count is not assumed
//       beyond the 321 it is generated with.
// CONTROL: control_batch.sh 2026-10-13, reverted, video_decoder_content_lavapipe, renderer/video/video_decoder.cpp:
//       `skip_frame` always AVDISCARD_DEFAULT -> "FAIL: content, software: asked to skip unreferenced pictures, the
//       decoder returns fewer (321 of 321)"; skipping whatever `skip` asks -> "... asked to skip nothing, the decoder
//       returns every picture (217 of 321)". The first break on video_decoder_hardware (RTX,
//       control_batch.sh 2026-10-13) -> "FAIL: hardware: asked to skip unreferenced pictures, the decoder returns
//       fewer (321 of 321)". Via control_batch.sh 2026-10-15, reverted, video_decoder_content_lavapipe: decode()'s presentation time
//       negated when `skip` is not Nothing -> "FAIL: content, software: the pictures a skipping decode
//       returns carry presentation times, and they rise".
void runPictureSkipChecks(Device& device, const char* label, bool hardware) {
    using wallpaperx::PictureSkip;
    if (!haveAssets({"counter_long.mp4"})) {
        g_notVerified = true;
        return;
    }
    const SkipDecode all = decodeSkipping(device, asset("counter_long.mp4"), PictureSkip::Nothing);
    const SkipDecode some = decodeSkipping(device, asset("counter_long.mp4"), PictureSkip::Unreferenced);
    std::printf("video_decoder_probe (%s): counter_long.mp4 decodes to %d pictures (%s), %d skipping unreferenced ones\n",
                label, all.pictures, pathName(all.path), some.pictures);
    if (hardware && all.path != VideoDecoder::Path::Hardware) {
        std::printf("video_decoder_probe (%s): counter_long.mp4 did not decode on the device, so skipping there is not "
                    "answerable\n",
                    label);
        g_notVerified = true;
        return;
    }
    check(all.pictures == 321, std::string(label) + ": asked to skip nothing, the decoder returns every picture (" +
                                   std::to_string(all.pictures) + " of 321)");
    check(some.pictures < all.pictures * 3 / 4 && some.pictures > 0,
          std::string(label) + ": asked to skip unreferenced pictures, the decoder returns fewer (" +
              std::to_string(some.pictures) + " of " + std::to_string(all.pictures) + ")");
    check(some.timed && some.rising,
          std::string(label) + ": the pictures a skipping decode returns carry presentation times, and they rise");
    runSkipLevelChecks(device, label, hardware, all, some);
}

// Decodes `file` at `from` until `pictures` have been returned, then records every send while it decodes at
// `to` for `after` more pictures. The sends after the switch, in order.
std::vector<std::pair<int, bool>> sendsAfterSwitch(Device& device, const std::string& file, wallpaperx::PictureSkip from,
                                                   int pictures, wallpaperx::PictureSkip to, int after) {
    VideoDecoder decoder(device, file);
    for (int i = 0; i < pictures && decoder.next(from); ++i) {
    }
    g_sends.clear();
    g_sendsIdr.clear();
    g_recordSends = true;
    struct Stop {
        ~Stop() { g_recordSends = false; }
    } stop;
    for (int i = 0; i < after && decoder.next(to); ++i) {
    }
    return g_sends;
}

// The deepest level a decoder of `file` applies, or nothing when the fixture was not generated.
std::optional<wallpaperx::PictureSkip> deepestFor(Device& device, const char* file) {
    if (!std::filesystem::exists(assetDir() / file)) {
        std::printf("video_decoder_probe: fixture %s was not generated, so its deepest level is not answerable\n", file);
        g_notVerified = true;
        return std::nullopt;
    }
    VideoDecoder decoder(device, asset(file));
    return decoder.deepestSkip();
}

// GOAL: S15.608 -- each skip level leaves out more than the one below it, and says so on what it returns:
//       counter_long.mp4 (H.264, key picture every 24, three B pictures between references, the middle one
//       referenced) decodes to 321 pictures skipping nothing, then strictly fewer at Unreferenced and
//       NonIntra, and no more at NonKey than at NonIntra, at least one; every picture returned says the level
//       it was decoded at, and from NonIntra up each is of a type that level keeps; no packet at any level
//       goes to ffmpeg as AVDISCARD_BIDIR, which drops the referenced middle B picture. False if a level maps
//       to the wrong AVDISCARD value, or a picture carries another level's tag.
// GOAL: S15.609 -- a level comes down onto pictures that can reference skipped ones (from NonIntra or NonKey
//       to Unreferenced or Nothing) only at a packet that resets the decoder, an IDR here; every other change
//       takes effect with the very next packet. From NonIntra to every picture: every packet before the next
//       key packet still skips at NonIntra, and that key packet (an IDR in this closed-GOP file) is the
//       first sent skipping nothing; from Unreferenced to Nothing, from NonKey to NonIntra and from NonIntra
//       to NonKey, the very next packet (not a key one) goes at the new level. On counter_opengop_long.mp4
//       (key every 48, only the first an IDR), from NonIntra to every picture, no packet after the switch is
//       sent below NonIntra at a key packet that is not an IDR. False if a lower level is sent mid-group or
//       at an open-GOP key picture, or if a change that skips nothing more waits for a reset point.
// WHY-THIS-SHAPE: the sends are read where VideoDecoder hands each packet to ffmpeg (skip_frame, the key
//       flag, and whether its NAL units hold an IDR slice), so the level each packet went with is seen
//       directly; the pictures returned would not show it, as ffmpeg withholds a picture whose references
//       were skipped. The key flag alone cannot tell an IDR from an open-GOP I picture: both carry it.
// COVERAGE-ASSERT: at least one packet sent after the switch before the key packet (the gate held one back);
//       the first packet after each at-once switch not a key packet; on the open-GOP file at least one key
//       packet that is not an IDR sent after the switch, and an IDR recognised at the file's start.
// CONTROL: control_batch.sh 2026-10-15, reverted, video_decoder_content_lavapipe, renderer/video/video_decoder.cpp: NonIntra sent as
//       AVDISCARD_NONREF -> "FAIL: content, software: each skip level leaves out more than the one below it (321, 217,
//       217, 14)"; NonIntra sent as AVDISCARD_BIDIR -> "... no packet goes to ffmpeg as AVDISCARD_BIDIR (level 2)"; each
//       picture tagged Nothing -> "... every picture says the level it was decoded at"; keptThrough() without its
//       key-picture rule -> "... every picture is of a type its level keeps (level 3)"; lowersOntoSkipped() always
//       false (the reset gate removed) -> "... a lower level is sent only from a key packet"; its `from > Unreferenced` made
//       `from >= Unreferenced` -> "... does not wait for a key packet"; made `to < from` (the old rule) -> "... from
//       key only down to intra only goes through with the very next packet"; made `to != from` -> "... a higher
//       level is sent with the very next packet"; decode()'s resetsDecoder() test replaced by the packet's key flag ->
//       "... on an open-GOP file no lower level is sent at a packet that is not an IDR". Harness, same tool: IDR
//       detection never true -> "... the open-GOP file's first packet was recognised as an IDR"; the open-GOP decode
//       pointed at counter_long.mp4 -> "... at least one open-GOP key packet was sent after the switch ... (0)".
//       Green: 321, 217, 14, 14; open GOP 164 sends after the switch, 3 open-GOP key packets, all at NonIntra. On
//       the device only Nothing and Unreferenced are asked for, so the reset-gate lines run in software only.
// GOAL: S15.619 -- the decoder's deepest level is Unreferenced on the device and NonKey in software for
//       H.264; Nothing for AV1 (whose decoder applies no level), and Unreferenced for MPEG-2, in either; asked
//       for NonKey on the device, every packet it sends goes with AVDISCARD_NONREF, and it returns exactly the
//       pictures Unreferenced returns. False if a level is offered that the codec ignores or cannot come back
//       down from, or the device drops anything beyond unreferenced pictures.
// WHY-THIS-SHAPE: the sends are read at ffmpeg's entry, as above; the count is compared with the same file's
//       Unreferenced decode, so a decoder that skips more (or less) than NONREF on the device shows.
// CONTROL: control_batch.sh 2026-10-14, reverted, video_decoder_hardware (RTX 3080 Ti), renderer/video/video_decoder.cpp:
//       decode()'s std::min(skip, deepestSkip()) made `skip` -> "FAIL: hardware: asked for key pictures only, the
//       device is sent only AVDISCARD_NONREF" and "... drops nothing beyond unreferenced pictures (14 of 217)".
//       Green: 91 sends, all 8 (NONREF); 217 of 217. Via control_batch.sh 2026-10-15, reverted,
//       video_decoder_content_lavapipe, renderer/video/video_decoder.cpp, deepestSkip(): H.264 in software NonIntra ->
//       "FAIL: content, software: the deepest level is key pictures only in software (got 2)"; the default case
//       NonKey -> "... the deepest level for AV1 is Nothing (got 3)"; the MPEG-2 case removed -> "... the deepest
//       level for MPEG-2 is Unreferenced (got 0)"; control_run.sh 2026-10-15, reverted, video_decoder_content_lavapipe:
//       AV_CODEC_ID_VP9 added to the MPEG case -> "... the deepest level for VP9 is Nothing (got 1)".
void runSkipLevelChecks(Device& device, const char* label, bool hardware, const SkipDecode& all, const SkipDecode& some) {
    using wallpaperx::PictureSkip;
    const std::string file = asset("counter_long.mp4");
    const std::string who = std::string(label) + ": ";
    {
        VideoDecoder probe(device, file);
        static_cast<void>(probe.next(PictureSkip::Nothing));
        const PictureSkip deepest = probe.deepestSkip();
        check(deepest == (hardware ? PictureSkip::Unreferenced : PictureSkip::NonKey),
              who + "the deepest level is " + (hardware ? "Unreferenced on the device" : "key pictures only in software") +
                  " (got " + std::to_string(static_cast<int>(deepest)) + ")");
    }
    if (const std::optional<PictureSkip> av1 = deepestFor(device, "counter_av1.mp4")) {
        check(*av1 == PictureSkip::Nothing, who + "the deepest level for AV1 is Nothing (got " +
                                                std::to_string(static_cast<int>(*av1)) + ")");
    }
    if (const std::optional<PictureSkip> mpeg2 = deepestFor(device, "counter_onekey_m2.ts")) {
        check(*mpeg2 == PictureSkip::Unreferenced, who + "the deepest level for MPEG-2 is Unreferenced (got " +
                                                       std::to_string(static_cast<int>(*mpeg2)) + ")");
    }
    if (const std::optional<PictureSkip> vp9 = deepestFor(device, "counter_vp9.webm")) {
        check(*vp9 == PictureSkip::Nothing, who + "the deepest level for VP9 is Nothing (got " +
                                                std::to_string(static_cast<int>(*vp9)) + ")");
    }
    // The device is asked only for the levels VideoDecoder applies there (deepestSkip()).
    const std::vector<PictureSkip> levels =
        hardware ? std::vector{PictureSkip::Nothing, PictureSkip::Unreferenced}
                 : std::vector{PictureSkip::Nothing, PictureSkip::Unreferenced, PictureSkip::NonIntra, PictureSkip::NonKey};
    std::vector<SkipDecode> at{all, some};
    for (size_t i = 2; i < levels.size(); ++i) at.push_back(decodeSkipping(device, file, levels[i]));
    std::string counts;
    for (const SkipDecode& level : at) counts += (counts.empty() ? "" : ", ") + std::to_string(level.pictures);
    std::printf("video_decoder_probe (%s): counter_long.mp4 at each level: %s pictures\n", label, counts.c_str());
    bool fewerEachLevel = at[0].pictures == 321;
    for (size_t i = 1; i < at.size(); ++i) {
        fewerEachLevel = fewerEachLevel && (levels[i] == PictureSkip::NonKey ? at[i].pictures <= at[i - 1].pictures &&
                                                                                    at[i].pictures >= 1
                                                                              : at[i].pictures < at[i - 1].pictures);
    }
    check(fewerEachLevel, who + "each skip level leaves out more than the one below it (" + counts + ")");
    for (size_t i = 0; i < at.size(); ++i) {
        check(at[i].tagged, who + "every picture says the level it was decoded at (level " + std::to_string(i) + ")");
        check(!at[i].sentBidirectional, who + "no packet goes to ffmpeg as AVDISCARD_BIDIR (level " + std::to_string(i) + ")");
        // Only NonIntra and up say which pictures they keep; below that the kept flag is never cleared.
        if (levels[i] >= PictureSkip::NonIntra) {
            check(at[i].kept, who + "every picture is of a type its level keeps (level " + std::to_string(i) + ")");
        }
    }

    const auto describe = [](const std::vector<std::pair<int, bool>>& sends) {
        std::string out;
        for (size_t i = 0; i < sends.size() && i < 40; ++i) out += " " + std::to_string(sends[i].first) + (sends[i].second ? "K" : "");
        return out;
    };
    // Software frame threads read ahead by as many packets as there are threads, so where the next packet
    // falls depends on the host: the switch is moved until it lands on one that is not a key packet.
    const auto nextPacketAfter = [&](PictureSkip from, std::initializer_list<int> after, PictureSkip to) {
        std::vector<std::pair<int, bool>> sends;
        for (const int pictures : after) {
            sends = sendsAfterSwitch(device, file, from, pictures, to, 4);
            if (!sends.empty() && !sends.front().second) break;
        }
        return sends;
    };
    const std::vector<std::pair<int, bool>> fromUnref =
        nextPacketAfter(PictureSkip::Unreferenced, {6, 10, 14, 19, 25, 31, 38}, PictureSkip::Nothing);
    std::printf("video_decoder_probe (%s): from unreferenced to every picture, the sends:%s\n", label, describe(fromUnref).c_str());
    check(!fromUnref.empty() && !fromUnref.front().second,
          who + "a switch from unreferenced pictures skipped was made with a packet that is not a key packet next");
    check(!fromUnref.empty() && fromUnref.front().first == AVDISCARD_DEFAULT,
          who + "leaving unreferenced pictures skipped does not wait for a key packet");
    if (hardware) {
        // Asked for key pictures only, the device sends unreferenced skipping and nothing deeper.
        const std::vector<std::pair<int, bool>> capped = sendsAfterSwitch(device, file, PictureSkip::Nothing, 6, PictureSkip::NonKey, 60);
        const bool onlyNonRef = std::all_of(capped.begin(), capped.end(), [](const auto& s) { return s.first == AVDISCARD_NONREF; });
        const SkipDecode atKey = decodeSkipping(device, file, PictureSkip::NonKey);
        std::printf("video_decoder_probe (%s): asked for key pictures only, %zu sends:%s; %d pictures (%d at unreferenced)\n", label,
                    capped.size(), describe(capped).c_str(), atKey.pictures, at[1].pictures);
        check(capped.size() >= 20, who + "at least twenty sends were recorded after asking for key pictures only (" +
                                       std::to_string(capped.size()) + ")");
        check(capped.size() >= 20 && onlyNonRef, who + "asked for key pictures only, the device is sent only AVDISCARD_NONREF");
        check(atKey.pictures == at[1].pictures, who + "asked for key pictures only, the device drops nothing beyond unreferenced pictures (" +
                                                    std::to_string(atKey.pictures) + " of " + std::to_string(at[1].pictures) + ")");
        return;
    }

    const std::vector<std::pair<int, bool>> down = sendsAfterSwitch(device, file, PictureSkip::NonIntra, 2, PictureSkip::Nothing, 30);
    size_t firstAll = down.size();
    for (size_t i = 0; i < down.size(); ++i) {
        if (down[i].first == AVDISCARD_DEFAULT) {
            firstAll = i;
            break;
        }
    }
    bool heldAtNonIntra = true;
    for (size_t i = 0; i < firstAll; ++i) {
        if (down[i].first != AVDISCARD_NONINTRA || down[i].second) heldAtNonIntra = false;
    }
    std::printf("video_decoder_probe (%s): from intra only to every picture, the sends:%s\n", label, describe(down).c_str());
    check(firstAll > 0 && firstAll < down.size(),
          who + "after leaving intra only, packets were sent at the old level before one skipping nothing");
    check(firstAll < down.size() && down[firstAll].second && heldAtNonIntra,
          who + "a lower level is sent only from a key packet, the packets before it still at the old level");

    // From intra only up to key only, and from key only down to intra only: neither starts decoding a
    // picture that references a skipped one, so each goes through at once.
    const std::vector<std::pair<int, bool>> up = sendsAfterSwitch(device, file, PictureSkip::NonIntra, 2, PictureSkip::NonKey, 2);
    std::printf("video_decoder_probe (%s): from intra only to key only, the sends:%s\n", label, describe(up).c_str());
    check(!up.empty() && !up.front().second && up.front().first == AVDISCARD_NONKEY,
          who + "a higher level is sent with the very next packet, not a key one");
    const std::vector<std::pair<int, bool>> keyToIntra = nextPacketAfter(PictureSkip::NonKey, {2, 3, 4, 5, 6}, PictureSkip::NonIntra);
    std::printf("video_decoder_probe (%s): from key only to intra only, the sends:%s\n", label, describe(keyToIntra).c_str());
    check(!keyToIntra.empty() && !keyToIntra.front().second,
          who + "a switch from key only to intra only was made with a packet that is not a key packet next");
    check(!keyToIntra.empty() && keyToIntra.front().first == AVDISCARD_NONINTRA,
          who + "from key only down to intra only goes through with the very next packet");

    // Open GOP: every key packet after the first is an I picture that is not an IDR.
    if (!haveAssets({"counter_opengop_long.mp4"})) {
        g_notVerified = true;
        return;
    }
    VideoDecoder open(device, asset("counter_opengop_long.mp4"));
    g_sends.clear();
    g_sendsIdr.clear();
    g_recordSends = true;
    for (int i = 0; i < 2 && open.next(PictureSkip::NonIntra); ++i) {
    }
    const size_t switchAt = g_sends.size();
    int pictures = 0;
    while (pictures < 1000 && open.next(PictureSkip::Nothing)) ++pictures;
    g_recordSends = false;
    const bool idrAtStart = !g_sendsIdr.empty() && g_sendsIdr.front();
    int openKeys = 0;
    bool loweredAtOpenKey = false;
    int previous = AVDISCARD_NONINTRA;
    for (size_t i = switchAt; i < g_sends.size(); ++i) {
        const bool openKey = g_sends[i].second && !g_sendsIdr[i];
        if (openKey && previous == AVDISCARD_NONINTRA) ++openKeys;
        if (g_sends[i].first < previous && !g_sendsIdr[i]) loweredAtOpenKey = true;
        previous = g_sends[i].first;
    }
    std::printf("video_decoder_probe (%s): open GOP, from intra only to every picture: %zu sends after the switch, %d "
                "open-GOP key packets met at intra only, %d pictures\n",
                label, g_sends.size() - switchAt, openKeys, pictures);
    check(idrAtStart, who + "the open-GOP file's first packet was recognised as an IDR");
    check(openKeys >= 1, who + "at least one open-GOP key packet was sent after the switch while at intra only (" +
                             std::to_string(openKeys) + ")");
    check(!loweredAtOpenKey, who + "on an open-GOP file no lower level is sent at a packet that is not an IDR");
}

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
    // CONTROL: control_batch.sh 2026-10-15, reverted, video_decoder_hardware (RTX 3080 Ti), renderer/video/video_decoder.cpp:
    //       decode() never setting `m_path = Path::Hardware` -> "FAIL: a stream this device accepts did not
    //       report Path::Hardware"; DecodedFrame::storage() always HostPlanes -> "FAIL: the hardware path
    //       produced a frame held in host memory" (and the probe then threw at deviceImage(), reported).
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
    //       "... colour range is not the limited range ...". Via control_batch.sh 2026-10-15, reverted, video_decoder_hardware (RTX):
    //       (3) extent() returning the visible size -> "FAIL: the coded extent is not the 320x240 the
    //       fixture's macroblocks round up to"; (4) bitDepth() plus 2 -> "FAIL: an 8-bit stream did not report a
    //       bit depth of 8"; (5) pixelFormat() returning AV_PIX_FMT_VULKAN for a device frame, not the frames
    //       context's sw_format -> "FAIL: the frame reports pixel format vulkan" (an `!= AV_PIX_FMT_NONE` check
    //       stayed green for that, hence NV12, the format an 8-bit 4:2:0 stream decodes to on the device).
    check(first->extent().width == 320 && first->extent().height == 240,
          "the coded extent is not the 320x240 the fixture's macroblocks round up to");
    check(first->visible().extent.width == 320 && first->visible().extent.height == 238,
          "the visible rectangle is the coded extent, so the crop the stream declares was not read");
    check(first->bitDepth() == 8, "an 8-bit stream did not report a bit depth of 8");
    if (first->pixelFormat() != AV_PIX_FMT_NV12) {
        const char* const name = av_get_pix_fmt_name(first->pixelFormat());
        std::fprintf(stderr, "FAIL: the frame reports pixel format %s\n", name != nullptr ? name : "none");
        fail("the frame does not report the NV12 an 8-bit 4:2:0 stream decodes to on the device");
    }
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
    //       does not describe itself the same way after a batch has read it". Via control_batch.sh 2026-10-15, reverted,
    //       video_decoder_hardware (RTX): (3) submit() adding no timeline wait or signal and not advancing
    //       the stored value -> "FAIL: the frame's batch carried no VkTimelineSemaphoreSubmitInfo" and "...
    //       does not wait on and signal exactly one semaphore"; (4) the signal moved to a fresh timeline
    //       semaphore, the stored value left -> "FAIL: the frame's batch waits on one semaphore and signals
    //       another". The value lines index only when the batch carried values, so (3) reports, not overreads.
    //       Re-run 2026-10-19 (T7), video_decoder_hardware (RTX), submit() now reading the image under the frame's
    //       lock: (1) and (2) held, the same lines.
    // COVERAGE-ASSERT: each bracket asserts that exactly one submit was recorded inside it, so a
    //       future change that routes the frame's work somewhere else -- or lets ffmpeg's own
    //       submissions fall into the window -- fails here rather than grading the wrong batch.
    const DecodedFrame::DeviceImage before = first->deviceImage();

    g_submits.clear();
    g_recordingSubmits = true;
    Device::SubmitTicket ticket = first->submit([](VkCommandBuffer, const DecodedFrame::DeviceImage&) {}, VK_PIPELINE_STAGE_TRANSFER_BIT);
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
    }
    if (!firstSubmit.waitValues.empty() && !firstSubmit.signalValues.empty()) {
        check(firstSubmit.signalValues[0] == firstSubmit.waitValues[0] + 1,
              "the frame's batch does not signal exactly one above the value it waits at");
    }
    // GOAL: S13.41 -- the batch waits at the pipeline stage the caller named, not an earlier one, which would
    //       leave the image's read unordered against ffmpeg's decode write.
    // CONTROL: control_batch.sh 2026-10-15, reverted, video_decoder_hardware (RTX 3080 Ti), renderer/video/video_decoder.cpp:
    //       DecodedFrame::submit() waiting at VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, not `readStage` -> "FAIL: the
    //       frame's batch does not wait at the stage the caller named". A batch without exactly one stage is
    //       already red at the one-semaphore check above.
    //       Re-run 2026-10-19 (T7), RTX: held, the same line.
    if (firstSubmit.waitStages.size() == 1) {
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
    ticket = first->submit([](VkCommandBuffer, const DecodedFrame::DeviceImage&) {}, VK_PIPELINE_STAGE_TRANSFER_BIT);
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
    //       Re-run 2026-10-19 (T7), control_batch.sh, RTX: held, the same line, the hang included.
    bool submitThrew = false;
    g_failNextSubmitOnce = true;
    try {
        (void)first->submit([](VkCommandBuffer, const DecodedFrame::DeviceImage&) {}, VK_PIPELINE_STAGE_TRANSFER_BIT);
    } catch (const std::exception&) {
        submitThrew = true;
    }
    g_failNextSubmitOnce = false;
    // CONTROL: control_batch.sh 2026-10-15, reverted, video_decoder_hardware (RTX), renderer/video/video_decoder.cpp: submit()
    //       catching the Device's exception and returning lastSubmission() -> "FAIL: an injected vkQueueSubmit
    //       failure did not reach the caller as an exception".
    //       Re-run 2026-10-19 (T7), RTX: held, the same line.
    check(submitThrew, "an injected vkQueueSubmit failure did not reach the caller as an exception");

    g_submits.clear();
    g_recordingSubmits = true;
    ticket = first->submit([](VkCommandBuffer, const DecodedFrame::DeviceImage&) {}, VK_PIPELINE_STAGE_TRANSFER_BIT);
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
    // GOAL: S13.38 -- the path stays Hardware for the whole stream, not only for its first picture.
    // CONTROL: control_batch.sh 2026-10-15, reverted, video_decoder_hardware (RTX), renderer/video/video_decoder.cpp: decode() setting
    //       SoftwareCodec for every device picture after the first -> "FAIL: the path changed away from Hardware
    //       part-way through the stream".
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

    const auto waitedAt = [](const RecordedSubmit& submit) {
        return submit.waitValues.empty() ? std::string("none") : std::to_string(submit.waitValues[0]);
    };
    std::printf("video_decoder_probe (hardware): %s decoded %d frames of 320x238 BT.709 on the device; semaphore "
                "advanced %s -> %s -> %s\n",
                device.info().gpuName.c_str(), hardwareFrames, waitedAt(firstSubmit).c_str(),
                waitedAt(secondSubmit).c_str(), waitedAt(afterFailure).c_str());
    runPictureSkipChecks(device, "hardware", true);
    if (haveAssets({"counter_long.mp4"})) runThinnedReopenCheck(device);
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
    // CONTROL (paths): control_batch.sh 2026-10-15, reverted, video_decoder_refused (RTX), renderer/video/video_decoder.cpp:
    //       attachHardwareFrames()'s unnamed-refusal branch setting SoftwareCodec -> "FAIL: 10-bit H.264 did not
    //       report Path::SoftwareRefused"; openDecoder()'s unadvertised-codec branch setting SoftwareRefused ->
    //       "FAIL: MPEG-4 Part 2 did not report Path::SoftwareCodec". decode() discarding every picture not on
    //       the device -> "FAIL: the refused stream produced no frames at all" and "FAIL: the unsupported-codec
    //       stream produced no frames at all" (the same break reddens the frame lines of no-decoder and
    //       refused-after-setup). The "produced a frame on the device" lines have no break: a host frame claimed
    //       as a device one is refused by DecodedFrame's own constructor before any count sees it.
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
    uint32_t softwareFirstReadSince = 0;
    {
        Device software(/*enableValidation=*/false);
        const DecodeResult never = decodeAll(software, asset("h264_10bit.mp4"));
        softwareFrames = never.frames;
        softwareFirstReadSince = never.firstReadSince;
    }
    g_hideVideoExtensions = false;

    // GOAL: S15.626 -- the first picture after a resume's seek counts no packet read before it: the
    //       software reopen of the refused stream reports, for its first picture, the readSince() a decoder
    //       that never tried hardware reports for the same file's first picture.
    // WHY-THIS-SHAPE: a resume by timestamp cannot carry this: admit() drops every picture a Seeked resume
    //       decodes first, and each drop zeroes the count itself. This reopen ends in a rewind with nothing to
    //       drop, after the hardware attempt read packet 1, so only seekToResume()'s own reset keeps that read
    //       out. Compared with a never-hardware decode, not a number: frame threads delay the first picture.
    // CONTROL: control_batch.sh 2026-10-15, reverted, video_decoder_refused (RTX): renderer/video/video_decoder.cpp
    //       seekToResume() without `m_readSinceReturned = 0` -> "FAIL: the reopen's first picture reports 11
    //       packet(s) read where a never-hardware decode reports 10".
    if (refused.firstReadSince != softwareFirstReadSince) {
        std::fprintf(stderr,
                     "FAIL: the reopen's first picture reports %u packet(s) read where a never-hardware decode reports "
                     "%u\n",
                     refused.firstReadSince, softwareFirstReadSince);
        fail("the first picture after the software reopen counted packets read before it");
    }

    if (refused.frames != softwareFrames) {
        std::fprintf(stderr,
                     "FAIL: the software reopen produced %d frames where a never-hardware decode of the same file "
                     "produces %d\n",
                     refused.frames, softwareFrames);
        fail("the reopen did not decode the stream from its first packet");
    }

    std::printf("video_decoder_probe (refused): 10-bit H.264 -> %s (%d frames, against %d from a never-hardware "
                "decode of the same file); MPEG-4 -> %s (%d frames); first picture read %u packet(s), against %u\n",
                pathName(refused.path), refused.frames, softwareFrames, pathName(codec.path), codec.frames,
                refused.firstReadSince, softwareFirstReadSince);
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
    // CONTROL: control_batch.sh 2026-10-15, reverted, video_decoder_no_decoder_lavapipe, renderer/video/video_decoder.cpp:
    //       extent() returning the visible size -> "FAIL: a software frame does not report the coded extent";
    //       the constructor's FrameColour default-constructed -> "FAIL: a software frame does not report the
    //       stream's colour matrix".
    VideoDecoder decoder(device, asset("h264_crop.mp4"));
    std::optional<DecodedFrame> frame = decoder.next();
    if (!frame) {
        fail("the software decoder produced no first frame to inspect");
        return;
    }
    // GOAL: S13.40 -- a software frame hands out every plane of its planar Y'CbCr format, each with data
    //       and a stride.
    // CONTROL: control_batch.sh 2026-10-15, reverted, video_decoder_no_decoder_lavapipe, renderer/video/video_decoder.cpp:
    //       hostPlanes() reporting no planes -> "FAIL: a planar Y'CbCr software frame reports fewer than two
    //       planes"; the second plane's stride 0 -> "FAIL: a software frame reports a plane with no data or no
    //       stride".
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
    // CONTROL: control_batch.sh 2026-10-15, reverted, video_decoder_refused_after_setup (RTX), renderer/video/video_decoder.cpp:
    //       attachHardwareFrames()'s av_hwframe_ctx_init failure on a healthy device setting SoftwareCodec ->
    //       "FAIL: a hardware failure on a device that still answers did not select Path::SoftwareRefused".
    //       Earlier: renderer/video/video_decoder.cpp, attachHardwareFrames()'s `if (!deviceHealthy())`
    //       changed to `if (false)`. Observed: the fault entry printed "FAIL: a hardware failure
    //       on a device that no longer answers did not raise GpuFailure". The mirrored mutation,
    //       `if (true)`, makes the refusal entry print "FAIL: a hardware failure on a device that
    //       still answers raised GpuFailure". Both reverted.
    //       Re-run 2026-10-19 (T7), `if (false)` at av_hwframe_ctx_init's refusal, deviceHealthy() now routed
    //       through deviceAnswers(): held, the fault entry's line. The SoftwareCodec and `if (true)` breaks
    //       re-run 2026-10-19 (T7b) on the refusal entry: each held, its line.
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
// CONTROL: the refusal now lives in renderer/video/ffmpeg_fd_io.h's readOnlyThrough(); via
//       control_batch.sh, 2026-10-11, its protocol list made "file,pipe" -> both ffconcat cases red (the
//       pipe one parked and released by the watchdog, the ordinary one "was opened", not refused as a
//       content error, not described as a list of other files), the HLS case green. Reverted.
//       Earlier, in renderer/video/video_decoder.cpp before the move, three runs, all reverted.
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

// S15.904 and S15.908 -- what each picture says about the thinning it was decoded under. Software, where the
// deepest level is NonKey, so a tag carrying the grid's added level would read as a level of its own.
void runThinningTagChecks(Device& device) {
    using namespace std::chrono_literals;
    using wallpaperx::NumberedGrids;
    using wallpaperx::PictureSkip;
    using wallpaperx::ShowGrids;
    const std::string label = "content, thinning";
    const std::string file = asset("counter_long.mp4");  // 30 a second, 321 pictures, three B pictures a run
    constexpr std::chrono::nanoseconds kSpacing = std::chrono::nanoseconds(2s) / 30;  // one picture in two shown
    constexpr int kPictures = 321;

    // GOAL: S15.904 -- a picture not thinned reports no number: with no grids at all every picture's
    //       thinnedUnder() is empty.
    // CONTROL: control_batch.sh 2026-10-20, reverted, video_decoder_content_lavapipe, renderer/video/video_decoder.cpp: sentThinnedUnder() ignoring the thinned
    //       bit -> "FAIL: ... none reports a thinning number (321 of 321 did)", and the first-picture line.
    int unthinnedReported = 0, decodedPlain = 0;
    {
        VideoDecoder plain(device, file);
        while (std::optional<DecodedFrame> frame = plain.next(PictureSkip::Nothing)) {
            ++decodedPlain;
            if (frame->thinnedUnder()) ++unthinnedReported;
        }
    }
    check(decodedPlain == kPictures && unthinnedReported == 0,
          label + ": with no grids every picture is decoded and none reports a thinning number (" +
              std::to_string(unthinnedReported) + " of " + std::to_string(decodedPlain) + " did)");

    VideoDecoder decoder(device, file);
    const auto numbered = [&](uint8_t number) {
        const std::array<std::chrono::nanoseconds, 1> spacings{kSpacing};
        return NumberedGrids{ShowGrids(decoder.timelineStart(), spacings), number};
    };
    struct Stop {
        ~Stop() {
            g_recordThinning = false;
            g_eagainOnThinned = false;
            g_thinDecoder = nullptr;
            g_renumberTo.reset();
        }
    } stop;
    g_numberAtSend.clear();
    g_thinningTags.clear();
    g_refusedTag.reset();
    g_resendTag.reset();
    g_numberInForce = 7;
    g_thinDecoder = &decoder;
    g_recordThinning = true;
    decoder.leaveUnshown(numbered(7));

    struct Returned {
        std::optional<std::chrono::nanoseconds> at;
        PictureSkip skip;
        std::optional<uint8_t> under;
    };
    std::vector<Returned> pictures;
    std::optional<uint8_t> afterRestart;
    bool restarted = false;
    while (std::optional<DecodedFrame> frame = decoder.next(PictureSkip::Nothing)) {
        if (restarted) {
            afterRestart = frame->thinnedUnder();
            break;
        }
        pictures.push_back({frame->presentationTime(), frame->skip(), frame->thinnedUnder()});
        if (pictures.size() == 100) {
            g_renumberTo = numbered(8);
            g_eagainOnThinned = true;
        }
        if (pictures.size() == 200) {
            decoder.restart();
            restarted = true;
        }
    }

    // GOAL: S15.904 -- the skip tag reports only the level next() was asked for: a stream thinned at level 0
    //       returns every picture tagged Nothing, though the grid left pictures out at Unreferenced.
    // WHY-THIS-SHAPE: the stream is shown to be thinned (pictures left out, pictures reporting a number), so a
    //       tag that never could have carried Unreferenced does not pass by default.
    // COVERAGE-ASSERT: at least 50 pictures reporting a number, and fewer pictures than an unthinned decode
    //       by the 200th returned. Not itself observed red: no break here leaves the grid unapplied alone.
    // CONTROL: control_batch.sh 2026-10-20, reverted, video_decoder_content_lavapipe, renderer/video/video_decoder.cpp: tagSent() given `applied` in place of
    //       m_skipping -> "FAIL: ... returns every picture tagged Nothing (127 of 200)".
    size_t taggedNothing = 0, thinned = 0;
    for (const Returned& r : pictures) {
        if (r.skip == PictureSkip::Nothing) ++taggedNothing;
        if (r.under) ++thinned;
    }
    const std::optional<std::chrono::nanoseconds> lastAt = pictures.empty() ? std::nullopt : pictures.back().at;
    const bool leftOut = lastAt && *lastAt - pictures.front().at.value_or(0ns) > 210 * (std::chrono::nanoseconds(1s) / 30);
    check(taggedNothing == pictures.size(), label + ": a stream thinned at level 0 returns every picture tagged Nothing (" +
                                                std::to_string(taggedNothing) + " of " + std::to_string(pictures.size()) + ")");
    check(thinned >= 50 && leftOut, label + ": the grid thinned the stream (" + std::to_string(thinned) +
                                        " pictures report a number; 200 pictures reach " +
                                        std::to_string(lastAt ? lastAt->count() / 1'000'000 : -1) + " ms)");
    check(!pictures.empty() && !pictures.front().under, label + ": the stream's first picture, before the grid applies, reports no number");

    // GOAL: S15.904 -- thinnedUnder() is the number of the grids in force when the picture's packet was first
    //       sent: pictures already in flight when the number changes keep the old one.
    // WHY-THIS-SHAPE: the number in force is the test's own, recorded at each packet's first send by the
    //       interposer and matched by presentation time; a decoder reporting the number at return would
    //       pass every picture sent after the change and fail only those in flight across it.
    // COVERAGE-ASSERT: at least one picture returned after the change reports the old number, and one the new.
    // CONTROL: control_batch.sh 2026-10-20, reverted, video_decoder_content_lavapipe, renderer/video/video_decoder.cpp: a returned picture given the grids' number
    //       now in force (m_unshown->number) -> "FAIL: ... reports the grids' number in force when its packet was
    //       sent (17 of 181 did not)" and the coverage line (0 old). Green: 181 compared, 17 old and 83 new.
    size_t compared = 0, mismatched = 0, oldAfterChange = 0, newAfterChange = 0;
    for (size_t i = 0; i < pictures.size(); ++i) {
        const Returned& r = pictures[i];
        if (!r.under || !r.at) continue;
        const auto sent = g_numberAtSend.find(r.at->count());
        if (sent == g_numberAtSend.end()) continue;
        ++compared;
        if (*r.under != sent->second) {
            ++mismatched;
            if (mismatched <= 5) {
                std::fprintf(stderr, "  picture %zu at %lld ms reports %d, sent under %d\n", i,
                             static_cast<long long>(r.at->count() / 1'000'000), *r.under, sent->second);
            }
        }
        if (i >= 100) ++(*r.under == 7 ? oldAfterChange : newAfterChange);
    }
    check(mismatched == 0, label + ": every thinned picture reports the grids' number in force when its packet was sent (" +
                               std::to_string(mismatched) + " of " + std::to_string(compared) + " did not)");
    check(compared >= 50 && oldAfterChange >= 1 && newAfterChange >= 1,
          label + ": the number check compared pictures in flight across the change (" + std::to_string(compared) +
              " compared; after it " + std::to_string(oldAfterChange) + " old, " + std::to_string(newAfterChange) + " new)");

    // GOAL: S15.908 -- a packet ffmpeg refuses with EAGAIN keeps its verdict and number when sent again, even
    //       when the grids changed in between.
    // WHY-THIS-SHAPE: the grids are renumbered inside the refused send itself, so a resend judged afresh
    //       carries 8 where the kept verdict carries 7; without that change a fresh judgement would match.
    // CONTROL: control_batch.sh 2026-10-20, reverted, video_decoder_content_lavapipe, renderer/video/video_decoder.cpp: judgeGrid() run on every send, not once a
    //       packet -> "FAIL: ... keeps its verdict and grids' number (tag 60 then 68)". Green: 60 then 60.
    const bool refusedThinned = g_refusedTag && (*g_refusedTag & kTagThinnedBit) != 0;
    check(refusedThinned && g_resendTag.has_value(), label + ": a thinned packet was refused with EAGAIN and sent again");
    if (refusedThinned && g_resendTag) {
        const uintptr_t low = (uintptr_t{1} << kTagPacketShift) - 1;
        check((*g_resendTag & low) == (*g_refusedTag & low),
              label + ": a packet sent again after EAGAIN keeps its verdict and grids' number (tag " +
                  std::to_string(*g_refusedTag & low) + " then " + std::to_string(*g_resendTag & low) + ")");
    }

    std::printf("video_decoder_probe (%s): %zu pictures before the restart, %zu thinned, %zu numbers compared (%zu old and "
                "%zu new after the change); refused tag %lld, resent %lld; after the restart %d\n",
                label.c_str(), pictures.size(), thinned, compared, oldAfterChange, newAfterChange,
                g_refusedTag ? static_cast<long long>(*g_refusedTag & 2047) : -1LL,
                g_resendTag ? static_cast<long long>(*g_resendTag & 2047) : -1LL, afterRestart ? int{*afterRestart} : -1);

    // GOAL: S15.908 -- after a rewind (restart) the packet the rewind leaves held is judged afresh: the first
    //       picture of the new pass reports no number, though the packet sent before the restart was thinned.
    // CONTROL: control_batch.sh 2026-10-20, reverted, video_decoder_content_lavapipe, renderer/video/video_decoder.cpp: rewind() putting the held packet's
    //       verdict back after its read -> "FAIL: ... the first picture after a restart reports no thinning number
    //       (it reported 8)".
    check(restarted && pictures.size() == 200 && pictures.back().under.has_value(),
          label + ": the picture before the restart was thinned");
    check(restarted && !afterRestart.has_value(),
          label + ": the first picture after a restart reports no thinning number" +
              (afterRestart ? " (it reported " + std::to_string(*afterRestart) + ")" : std::string()));
}

// S15.908 -- the software reopen of a thinned stream: what the packets read for the resume are judged.
void runThinnedReopenCheck(Device& device) {
    using namespace std::chrono_literals;
    const std::string label = "hardware, thinned reopen";
    const std::string file = asset("counter_long.mp4");  // 30 a second, 321 pictures, three B pictures a run
    constexpr std::chrono::nanoseconds kSpacing = std::chrono::nanoseconds(2s) / 30;  // one picture in two shown
    VideoDecoder decoder(device, file);
    const std::array<std::chrono::nanoseconds, 1> spacings{kSpacing};
    decoder.leaveUnshown(wallpaperx::NumberedGrids{wallpaperx::ShowGrids(decoder.timelineStart(), spacings), 7});
    struct Disarm {
        ~Disarm() {
            g_externalOnThinned = false;
            g_externalFired = false;
        }
    } disarm;

    // GOAL: S15.908 -- after a software reopen while thinning (the hardware decoder failing mid-stream on a
    //       device that still answers), the packets read to resume are judged afresh, not thinned while
    //       resuming: the reopen's first picture reports no thinning number, though the packet that failed
    //       was thinned; and thinning comes back once resumed.
    // WHY-THIS-SHAPE: the failure is injected on a thinned packet's send (AVERROR_EXTERNAL, as ffmpeg's Vulkan
    //       decoder reports one), so a verdict carried over the reopen would be a thinned one. "Thinned again
    //       later" shows the software decoder still thins, so "no number" is not a stream that stopped thinning.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, renderer/video/video_decoder.cpp: judgeGrid() without
    //       its `std::holds_alternative<std::monostate>(m_resume)` condition -> "FAIL: ... the first picture after a
    //       software reopen reports no thinning number (it reported 7)". The failed packet's verdict kept over the
    //       reopen stays green: the reopen seeks with no packet held, and the one packet the kept verdict reaches
    //       decodes a picture before the resume point, which is dropped.
    // COVERAGE-ASSERT: the failure fired on hardware, the stream reopened in software, and the picture before it
    //       was thinned.
    int returned = 0;
    std::optional<uint8_t> lastBefore, firstAfter;
    bool sawBefore = false, reopened = false, thinnedAgain = false;
    VideoDecoder::Path pathBefore = VideoDecoder::Path::SoftwareCodec;
    while (std::optional<DecodedFrame> frame = decoder.next(wallpaperx::PictureSkip::Nothing)) {
        ++returned;
        if (!g_externalFired) {
            sawBefore = true;
            lastBefore = frame->thinnedUnder();
            pathBefore = decoder.path();
            if (returned == 100) g_externalOnThinned = true;
            continue;
        }
        if (!reopened) {
            reopened = true;
            firstAfter = frame->thinnedUnder();
            continue;
        }
        if (frame->thinnedUnder()) {
            thinnedAgain = true;
            break;
        }
    }
    if (pathBefore != VideoDecoder::Path::Hardware) {
        std::printf("video_decoder_probe (%s): %s decodes this file in %s, so a hardware failure cannot be staged -- "
                    "not answerable here\n",
                    label.c_str(), device.info().gpuName.c_str(), pathName(pathBefore));
        g_notVerified = true;
        return;
    }
    check(g_externalFired && reopened && decoder.path() == VideoDecoder::Path::SoftwareRefused,
          label + ": the staged failure fired on a thinned packet and the stream reopened in software (premise)");
    check(sawBefore && lastBefore.has_value(), label + ": the picture before the failure was thinned (premise)");
    check(reopened && !firstAfter.has_value(),
          label + ": the first picture after a software reopen reports no thinning number" +
              (firstAfter ? " (it reported " + std::to_string(*firstAfter) + ")" : std::string()));
    check(thinnedAgain, label + ": the reopened stream is thinned again once resumed");
    std::printf("video_decoder_probe (%s): %d pictures, reopened after the 100th; first after %d, thinned again %s\n",
                label.c_str(), returned, firstAfter ? int{*firstAfter} : -1, thinnedAgain ? "yes" : "no");
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
    // COVERAGE-ASSERT: the count refused is asserted against the literal kIrregularKinds, so a deleted case reds.
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

    g_hideVideoExtensions = true;
    int cleanFrames = -1;
    {
        Device software(/*enableValidation=*/false);
        cleanFrames = decodeAll(software, asset("h264_raw.h264")).frames;
    }
    g_hideVideoExtensions = false;
    check(cleanFrames > 0, "the reference stream the injections are compared with decoded to nothing");
    std::printf("video_decoder_probe (content): %d files refused as content errors; the clean stream decodes to %d "
                "frames\n",
                refusedAsContent, cleanFrames);

    // GOAL: S13.42 -- each of the three answers ffmpeg can give that this decoder is written to
    //       absorb is actually absorbed: AVERROR_INVALIDDATA from the send, AVERROR_INVALIDDATA
    //       from the receive, and an EAGAIN that means "show me that packet again".
    // WHY-THIS-SHAPE: injected from ffmpeg's own entry points rather than produced by damaging a
    //       file, because damaging a file does not reach any of them -- see the note on the
    //       injection counters above. Without the
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

        // GOAL: S15.429 -- a software decode whose stream offers only hardware formats is refused as the
        //       wallpaper's fault ("decodes to no format software can hold") within a few packets, rather than
        //       read to its end with every slice skipped; and a later file, on a decoder of its own, decodes in
        //       full.
        // WHY-THIS-SHAPE: no real file offers a software codec only hardware formats, so the decoder's own
        //       look-ups of format descriptors are made to answer "hardware" (the interposer below, keyed on the
        //       caller being this executable, so ffmpeg's own look-ups are untouched). counter_long.mp4 has 320
        //       packets: a decoder that ignores the refusal reads them all, which the packet bound tells apart.
        // CONTROL: via control_run.sh, video_decoder_content_lavapipe, 2026-10-09, each reverted. chooseFormat()
        //       (renderer/video/video_decoder.cpp) no longer setting m_softwareFormatRefused -> "FAIL: a stream
        //       offering software only hardware formats is refused as the file's, naming why (said: )" and "FAIL:
        //       that refusal comes within 16 packets, ... (321 sent)", 297 pictures. m_softwareFormatRefused made
        //       `static inline` (renderer/video/video_decoder.h) with openDecoder()'s clear removed -> "FAIL: a
        //       later file, on a decoder of its own, decodes in full after that refusal (-1 of 20)". The clear
        //       alone removed stays green: the flag is set only on a software codec, and no decoder ever opens
        //       a second codec after a software one, so nothing can observe it.
        // COVERAGE-ASSERT: the interposer answered "hardware" to the decoder at least once.
        constexpr int kFewPackets = 16;
        if (haveAssets({"counter_long.mp4"})) {
            std::string said;
            bool asContent = false;
            int frames = 0;
            try {
                VideoDecoder decoder(software, asset("counter_long.mp4"));
                g_sendsWhileHardwareOnly = 0;
                g_formatsShownAsHardware = 0;
                g_everyFormatHardware = true;
                while (decoder.next()) ++frames;
            } catch (const WallpaperContentError& e) {
                asContent = true;
                said = e.what();
            } catch (const std::exception& e) {
                said = e.what();
            }
            g_everyFormatHardware = false;
            const int sends = g_sendsWhileHardwareOnly.load();
            std::printf("video_decoder_probe (content): every format shown as hardware -> %d packet(s) sent, %d "
                        "picture(s), %d look-up(s) answered; %s\n",
                        sends, frames, g_formatsShownAsHardware.load(), said.empty() ? "no refusal" : said.c_str());
            check(g_formatsShownAsHardware.load() > 0,
                  "the decoder looked a format up while every format read as hardware (the premise)");
            check(asContent && said.find("decodes to no format software can hold") != std::string::npos,
                  "a stream offering software only hardware formats is refused as the file's, naming why (said: " +
                      said + ")");
            check(sends <= kFewPackets, "that refusal comes within " + std::to_string(kFewPackets) + " packets, not at "
                                            "the end of the file (" + std::to_string(sends) + " sent)");
            int later = -1;
            try {
                later = decodeAll(software, asset("h264_raw.h264")).frames;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "  the later file raised: %s\n", e.what());
            }
            check(later == cleanFrames, "a later file, on a decoder of its own, decodes in full after that refusal (" +
                                            std::to_string(later) + " of " + std::to_string(cleanFrames) + ")");
        } else {
            g_notVerified = true;
        }
        runPictureSkipChecks(software, "content, software", false);
        if (haveAssets({"counter_long.mp4"})) runThinningTagChecks(software);
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

// The descriptor ffmpeg holds, or while g_everyFormatHardware a copy flagged as a hardware format -- for a
// caller in this executable only (the decoder under test); ffmpeg's own look-ups get the real one.
extern "C" const AVPixFmtDescriptor* av_pix_fmt_desc_get(AVPixelFormat format) {
    using Fn = const AVPixFmtDescriptor* (*)(AVPixelFormat);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "av_pix_fmt_desc_get"));
    if (real == nullptr) {
        std::fprintf(stderr, "video_decoder_probe: cannot resolve the real av_pix_fmt_desc_get\n");
        std::abort();
    }
    const AVPixFmtDescriptor* descriptor = real(format);
    if (!g_everyFormatHardware.load() || descriptor == nullptr) return descriptor;
    Dl_info caller{};
    Dl_info self{};
    if (dladdr(__builtin_return_address(0), &caller) == 0 ||
        dladdr(reinterpret_cast<void*>(&av_pix_fmt_desc_get), &self) == 0 || caller.dli_fbase != self.dli_fbase) {
        return descriptor;
    }
    thread_local AVPixFmtDescriptor shown;
    shown = *descriptor;
    shown.flags |= AV_PIX_FMT_FLAG_HWACCEL;
    g_formatsShownAsHardware.fetch_add(1);
    return &shown;
}

namespace {
// An MP4 H.264 packet is NAL units each behind a 4-byte big-endian length; type 5 is an IDR slice.
bool holdsIdrSlice(const AVPacket& packet) {
    for (int at = 0; packet.data != nullptr && at + 4 < packet.size;) {
        const uint32_t length = (uint32_t{packet.data[at]} << 24) | (uint32_t{packet.data[at + 1]} << 16) |
                                (uint32_t{packet.data[at + 2]} << 8) | uint32_t{packet.data[at + 3]};
        if ((packet.data[at + 4] & 0x1f) == 5) return true;
        if (length == 0 || length > static_cast<uint32_t>(packet.size - at - 4)) return false;
        at += 4 + static_cast<int>(length);
    }
    return false;
}
}  // namespace

extern "C" int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    ++g_ffmpegDecodeCalls;
    if (g_everyFormatHardware.load() && packet != nullptr && context != nullptr && context->opaque != nullptr) {
        g_sendsWhileHardwareOnly.fetch_add(1);
    }
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
    if (g_recordThinning && context != nullptr && packet != nullptr) {
        const auto tag = reinterpret_cast<uintptr_t>(packet->opaque);
        if (packet->pts != AV_NOPTS_VALUE && context->pkt_timebase.num > 0) {
            g_numberAtSend.emplace(av_rescale_q(packet->pts, context->pkt_timebase, AVRational{1, 1'000'000'000}),
                                   g_numberInForce);
        }
        if (g_refusedTag && !g_resendTag && (tag >> kTagPacketShift) == (*g_refusedTag >> kTagPacketShift)) g_resendTag = tag;
        if (g_eagainOnThinned && (tag & kTagThinnedBit) != 0) {
            g_eagainOnThinned = false;
            g_refusedTag = tag;
            if (g_thinDecoder != nullptr && g_renumberTo) {
                g_numberInForce = g_renumberTo->number;
                g_thinDecoder->leaveUnshown(g_renumberTo);
            }
            return AVERROR(EAGAIN);
        }
        g_thinningTags.push_back(tag);
    }
    if (g_externalOnThinned && context != nullptr && packet != nullptr && context->hw_device_ctx != nullptr &&
        (reinterpret_cast<uintptr_t>(packet->opaque) & kTagThinnedBit) != 0) {
        g_externalOnThinned = false;
        g_externalFired = true;
        return AVERROR_EXTERNAL;
    }
    if (g_recordSends && context != nullptr && packet != nullptr) {
        g_sends.push_back({context->skip_frame, (packet->flags & AV_PKT_FLAG_KEY) != 0});
        g_sendsIdr.push_back(holdsIdrSlice(*packet));
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
