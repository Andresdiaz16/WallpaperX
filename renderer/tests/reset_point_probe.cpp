// SPDX-License-Identifier: GPL-2.0-or-later
//
// resetsDecoder(), packet by packet, against support/reset_point_reference.h's reading of the same
// bitstreams. No Device: the classifier reads bytes only. Real fixtures in both NAL layouts and AV1, and
// copies of real packets with their first picture rewritten into the kinds that must not count.

#include "video/reset_point.h"

#include "support/check.h"
#include "support/reset_point_reference.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>

namespace {

using wallpaperx::resetsDecoder;
using wallpaperx::testing::fail;

constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

std::string asset(const char* name) { return (std::filesystem::path(WALLPAPERX_VIDEO_ASSET_DIR) / name).string(); }

struct Counts {
    int packets = 0;
    int resets = 0;
    int openKeyframes = 0;  // keyframe-flagged, and not a reset point
    int mismatches = 0;
    int rewritten = 0;
};

std::string firstPicture(const AVCodecParameters& stream, const AVPacket& packet) {
    if (stream.codec_id == AV_CODEC_ID_AV1) {
        const std::optional<size_t> at = wallpaperx::testing::av1FirstFrameHeader(packet.data, static_cast<size_t>(packet.size));
        if (!at) return "no frame header";
        const uint8_t bits = packet.data[*at];
        return "show_existing " + std::to_string(bits >> 7) + ", frame_type " + std::to_string((bits >> 5) & 3) +
               ", show_frame " + std::to_string((bits >> 4) & 1);
    }
    const auto slice = wallpaperx::testing::firstSlice(packet.data, static_cast<size_t>(packet.size), stream.codec_id,
                                                       wallpaperx::testing::nalLengthSize(stream));
    return slice ? "first slice NAL type " + std::to_string(slice->type) : std::string("no slice");
}

// A copy of `packet` with `edit` applied to its bytes; resetsDecoder() must answer `expected`.
void gradeRewrite(const char* file, const AVCodecParameters& stream, const AVPacket& packet, int index,
                  const std::string& kind, bool expected, const std::function<void(uint8_t*)>& edit, Counts& counts) {
    AVPacket* copy = av_packet_alloc();
    if (copy == nullptr || av_new_packet(copy, packet.size) < 0) {
        av_packet_free(&copy);
        fail("the harness could not copy a packet to rewrite, which is not a verdict on resetsDecoder()");
        return;
    }
    std::memcpy(copy->data, packet.data, static_cast<size_t>(packet.size));
    edit(copy->data);
    const bool reference = wallpaperx::testing::referenceResets(stream, copy->data, static_cast<size_t>(copy->size));
    const bool got = resetsDecoder(stream, *copy);
    ++counts.rewritten;
    if (reference != expected) {
        std::fprintf(stderr, "FAIL: reset_point_probe: %s packet %d rewritten as %s: the reference reads it as %s\n", file,
                     index, kind.c_str(), reference ? "a reset point" : "not one");
        fail("the harness's rewrite did not produce the picture kind it names, which is not a verdict on resetsDecoder()");
    } else if (got != expected) {
        std::fprintf(stderr, "FAIL: reset_point_probe: %s packet %d rewritten as %s: resetsDecoder() said %s\n", file,
                     index, kind.c_str(), got ? "it resets the decoder" : "it does not reset the decoder");
        fail(expected ? "resetsDecoder() refuses a picture that resets the decoder"
                      : "resetsDecoder() takes a picture that does not reset the decoder as a reset point");
    }
    av_packet_free(&copy);
}

// Every video packet of `file`, and the rewrites its codec calls for.
Counts gradeFile(const char* file, AVCodecID codec, int lengthSize) {
    Counts counts;
    const std::string path = asset(file);
    if (!std::filesystem::exists(path)) {
        std::printf("reset_point_probe: fixture %s was not generated -- not answerable\n", file);
        g_notVerified = true;
        return counts;
    }
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
    int stream = -1;
    if (packet != nullptr && avformat_open_input(&format, path.c_str(), nullptr, nullptr) >= 0 &&
        avformat_find_stream_info(format, nullptr) >= 0) {
        stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }
    if (stream < 0) {
        std::fprintf(stderr, "FAIL: reset_point_probe: cannot read the video packets of %s\n", file);
        fail("the harness could not read a fixture, which is not a verdict on resetsDecoder()");
        return counts;
    }
    const AVCodecParameters& parameters = *format->streams[stream]->codecpar;
    if (parameters.codec_id != codec || wallpaperx::testing::nalLengthSize(parameters) != lengthSize) {
        std::fprintf(stderr, "FAIL: reset_point_probe: %s is codec %d with NAL length prefix %d, not %d and %d\n", file,
                     static_cast<int>(parameters.codec_id), wallpaperx::testing::nalLengthSize(parameters),
                     static_cast<int>(codec), lengthSize);
        fail("a fixture is not the codec and layout this probe grades it as");
        return counts;
    }
    for (int index = 0; av_read_frame(format, packet) >= 0; av_packet_unref(packet)) {
        if (packet->stream_index != stream) continue;
        const bool reference = wallpaperx::testing::referenceResets(parameters, packet->data, static_cast<size_t>(packet->size));
        const bool got = resetsDecoder(parameters, *packet);
        const bool key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        ++counts.packets;
        counts.resets += reference ? 1 : 0;
        counts.openKeyframes += key && !reference ? 1 : 0;
        if (got != reference && counts.mismatches++ < 4) {
            std::fprintf(stderr, "FAIL: reset_point_probe: %s packet %d (%s%s): resetsDecoder() said %s\n", file, index,
                         firstPicture(parameters, *packet).c_str(), key ? ", keyframe-flagged" : "",
                         got ? "it resets the decoder" : "it does not reset the decoder");
        }
        // HEVC: a CRA, and the same packet rewritten to each BLA type and each IDR type. The IDR rewrites
        // show the rewrite reaches the classifier; without them a rewrite that missed would read as a
        // refused BLA.
        if (codec == AV_CODEC_ID_HEVC && key && !reference) {
            const auto slice = wallpaperx::testing::firstSlice(packet->data, static_cast<size_t>(packet->size), codec, lengthSize);
            if (slice && slice->type == wallpaperx::testing::kHevcCra) {
                for (const int type : {16, 17, 18, 19, 20}) {
                    gradeRewrite(file, parameters, *packet, index, "NAL type " + std::to_string(type), type >= 19,
                                 [at = slice->offset, type](uint8_t* data) {
                                     data[at] = static_cast<uint8_t>((data[at] & 0x81) | (type << 1));
                                 },
                                 counts);
                }
            }
        }
        // AV1: a shown key frame made hidden, made a show-existing frame, and made intra-only; and unchanged.
        if (codec == AV_CODEC_ID_AV1 && reference) {
            const std::optional<size_t> at = wallpaperx::testing::av1FirstFrameHeader(packet->data, static_cast<size_t>(packet->size));
            const size_t o = at.value_or(0);
            gradeRewrite(file, parameters, *packet, index, "an unchanged copy", true, [](uint8_t*) {}, counts);
            gradeRewrite(file, parameters, *packet, index, "a hidden key frame", false,
                         [o](uint8_t* data) { data[o] = static_cast<uint8_t>(data[o] & ~0x10); }, counts);
            gradeRewrite(file, parameters, *packet, index, "a show-existing frame", false,
                         [o](uint8_t* data) { data[o] = static_cast<uint8_t>(data[o] | 0x80); }, counts);
            gradeRewrite(file, parameters, *packet, index, "an intra-only frame", false,
                         [o](uint8_t* data) { data[o] = static_cast<uint8_t>((data[o] & ~0x60) | (2 << 5)); }, counts);
        }
        ++index;
    }
    // CONTROL: test-side, by hand, 2026-10-01: counter_opengop.ts replaced by its first 564 bytes (its tables
    //       name an H.264 stream; no video packet follows) -> "FAIL: reset_point_probe: counter_opengop.ts
    //       opens with no video packets". Restored.
    if (counts.packets == 0) {
        std::fprintf(stderr, "FAIL: reset_point_probe: %s opens with no video packets\n", file);
        fail("a fixture holds no video packets, so nothing in it was graded");
        return counts;
    }
    if (counts.mismatches > 0) {
        std::fprintf(stderr, "FAIL: reset_point_probe: %s: resetsDecoder() disagreed on %d of %d packets\n", file,
                     counts.mismatches, counts.packets);
        fail("resetsDecoder() answers as the bitstream says, for every packet");
    }
    std::printf("reset_point_probe: %s: %d packets, %d reset points, %d keyframe-flagged packets that do not reset, "
                "%d rewritten copies\n",
                file, counts.packets, counts.resets, counts.openKeyframes, counts.rewritten);
    return counts;
}

}  // namespace

// With no argument, the H.264 and HEVC fixtures, which are always generated; with `av1`, the AV1 one, which
// needs an ffmpeg with libaom and is skipped without. Two entries, so a missing optional file does not turn
// the main entry's pass into a skip.
int main(int argc, char** argv) {
    const bool av1Entry = argc > 1 && std::string(argv[1]) == "av1";
    if (argc > 1 && !av1Entry) {
        std::fprintf(stderr, "usage: %s [av1]\n", argv[0]);
        return 1;
    }
    // GOAL: S15.165 -- resetsDecoder() takes a packet as a reset point exactly when its first picture resets
    //       the decoder: an H.264 IDR, in start-code and avcC layout; an HEVC IDR and never a CRA or BLA, in
    //       start-code and hvcC layout; an AV1 key frame only when it is shown and not a show-existing frame.
    // WHY-THIS-SHAPE: graded against a reading of the bitstream written from the specifications here, on
    //       every packet of real encoder output, rather than on hand-built byte strings that could share the
    //       classifier's own misreading of a layout. No x265 setting emits BLA and no libaom one emits a hidden
    //       key frame, so those are real packets rewritten in place, each kind paired with a rewrite to a type
    //       that must count, which shows the rewrite reaches the classifier.
    // CONTROL: via control_batch.sh, 2026-09-30, each alone, reverted, in renderer/video/reset_point.cpp:
    //       hevcUnit() also accepting type 21 -> "counter_opengop_hevc.ts packet 45 (first slice NAL type 21,
    //       keyframe-flagged): resetsDecoder() said it resets the decoder"; lengthSize() returning 0 for avcC ->
    //       "counter_opengop.mp4 packet 0 (first slice NAL type 5, keyframe-flagged): resetsDecoder() said it
    //       does not reset the decoder"; hevcUnit() also accepting types 16-18 -> "counter_opengop_hevc.ts
    //       packet 45 rewritten as NAL type 16: resetsDecoder() said it resets the decoder"; av1TemporalUnit()
    //       ignoring show_frame -> on reset_point_detector_av1, "counter_av1.mp4 packet 0 rewritten as a hidden
    //       key frame: resetsDecoder() said it resets the decoder".
    // COVERAGE-ASSERT: each fixture must hold reset points and keyframe-flagged packets that are not, and the
    //       HEVC and AV1 rewrites must have run -- red with counter_long.mp4 in counter_opengop.mp4's place
    //       (test-side, by hand, 2026-09-30), "14 reset points, 0 open keyframes, 0 rewrites". Each rewrite
    //       is checked against the reference first: the HEVC rewrite made to write a CRA -> "rewritten as NAL
    //       type 19: the reference reads it as not one", a harness fault, not a verdict.
    struct Fixture {
        const char* file;
        AVCodecID codec;
        int lengthSize;
    };
    const std::vector<Fixture> main = {Fixture{"counter_opengop.ts", AV_CODEC_ID_H264, 0},
                                       Fixture{"counter_opengop.mp4", AV_CODEC_ID_H264, 4},
                                       Fixture{"counter_opengop_hevc.ts", AV_CODEC_ID_HEVC, 0},
                                       Fixture{"counter_opengop_hevc.mp4", AV_CODEC_ID_HEVC, 4},
                                       Fixture{"counter_two_idr.mp4", AV_CODEC_ID_H264, 4}};
    const std::vector<Fixture> av1 = {Fixture{"counter_av1.mp4", AV_CODEC_ID_AV1, 0}};
    for (const Fixture& fixture : av1Entry ? av1 : main) {
        const Counts counts = gradeFile(fixture.file, fixture.codec, fixture.lengthSize);
        // Already reported: a fixture not generated is a skip, one unreadable or empty a FAIL.
        if (counts.packets == 0) continue;
        const bool av1 = fixture.codec == AV_CODEC_ID_AV1;
        // AV1 has no open keyframe here; its negatives are the non-key frames and the rewrites.
        const bool structure = counts.resets >= (av1 ? 2 : 1) && (av1 ? counts.resets < counts.packets : counts.openKeyframes >= 1) &&
                               (fixture.codec == AV_CODEC_ID_H264 || counts.rewritten >= 4);
        if (!structure) {
            std::fprintf(stderr, "FAIL: reset_point_probe: %s: %d reset points, %d open keyframes, %d rewrites -- not the "
                                 "structure this probe grades\n",
                         fixture.file, counts.resets, counts.openKeyframes, counts.rewritten);
            fail("a fixture lost the reset points or open keyframes the classifier is graded on");
        }
    }

    const int failures = wallpaperx::testing::reportFailures("reset_point_probe");
    if (failures > 0) return 1;
    if (g_notVerified) {
        std::printf("reset_point_probe: NOT VERIFIED here -- reported to ctest as a skip, not a pass\n");
        return kNotVerifiedExitCode;
    }
    std::printf("reset_point_probe: all checks passed\n");
    return 0;
}
