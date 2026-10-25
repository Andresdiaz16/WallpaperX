// SPDX-License-Identifier: GPL-2.0-or-later
//
// resetsDecoder(), packet by packet, against support/reset_point_reference.h's reading of the same
// bitstreams. No Device: the classifier reads bytes only. Real fixtures in both NAL layouts and AV1, and
// copies of real packets with their first picture rewritten into the kinds that must not count.

#include "video/reset_point.h"
#include "video/software_scan.h"

#include "support/check.h"
#include "support/reset_point_reference.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

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


// A packet holding `bytes`, with `side` as AV_PKT_DATA_NEW_EXTRADATA when it is not empty.
struct OwnedPacket {
    AVPacket* packet = av_packet_alloc();
    OwnedPacket(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& side) {
        if (packet == nullptr || av_new_packet(packet, static_cast<int>(bytes.size())) < 0) return;
        std::memcpy(packet->data, bytes.data(), bytes.size());
        if (!side.empty()) {
            auto* copy = static_cast<uint8_t*>(av_malloc(side.size()));
            if (copy == nullptr) return;
            std::memcpy(copy, side.data(), side.size());
            if (av_packet_add_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, copy, side.size()) < 0) av_free(copy);
        }
    }
    ~OwnedPacket() { av_packet_free(&packet); }
    bool ok() const { return packet != nullptr && packet->data != nullptr; }
};

// GOAL: S15.926 (the readers' half) -- the NAL walkers take the length-prefix size from the extradata they are
//       handed, so a NEW_EXTRADATA declaring 2-byte lengths reads 2-byte packets: resetsDecoder(), firstSlice()
//       and beyondExtradata() on an avcC and an hvcC declaring 2; hevcSubLayers() reads the SPS's count; and
//       changesExtradata() marks a same-size NEW_EXTRADATA one byte different, and not a byte-identical one.
//       False if a walker assumes 4-byte lengths, or the comparison is by size or by presence.
// WHY-THIS-SHAPE: hand-built bytes, because no encoder writes 2-byte lengths and the decoder-level half (the
//       decoder adopting the side data, a rewind dropping it) needs a fixture this round did not build -- see
//       docs/efficiency/G2b_report.md. Each 2-byte reading is paired with the same packet under a 4-byte header,
//       which must not read, so a fixture whose bytes happened to parse either way could not pass.
// CONTROL: control_batch.sh 2026-10-21, each alone, reverted, reset_point_detector, renderer/video/reset_point.cpp:
//       lengthSize() returning 4 for avcC -> "avcC declaring 2-byte lengths: resetsDecoder() reads ..." (and the
//       firstSlice and beyond lines); for hvcC -> "hvcC declaring 2-byte lengths: firstSlice() finds ...";
//       changesExtradata() by size only -> "a same-size NEW_EXTRADATA with one byte changed ..."; by presence
//       only -> "a byte-identical NEW_EXTRADATA does not change ..."; hevcSubLayers() reading unit[2] & 7 ->
//       "... reads 3 sub-layers from the SPS (got 6)" (re-run 2026-10-24, G5, on the hevcType() form; restored).
void runLengthSizeChecks() {
    using wallpaperx::testing::check;
    auto avcC = [](uint8_t lengthByte) {
        return std::vector<uint8_t>{1, 0x42, 0, 0x1e, lengthByte, 0xe1, 0, 4, 0x67, 0x42, 0, 0x1e, 1, 0, 2, 0x68, 0xce};
    };
    auto hvcC = [](uint8_t lengthByte) {
        std::vector<uint8_t> record(23, 0);
        record[0] = 1;
        record[21] = lengthByte;  // numTemporalLayers 0: the count comes from the SPS
        record[22] = 1;
        const std::vector<uint8_t> sps = {0x21, 0, 1, 0, 3, 0x42, 0x01, 0x05};  // SPS, max_sub_layers_minus1 = 2
        record.insert(record.end(), sps.begin(), sps.end());
        return record;
    };
    AVCodecParameters h264{};
    h264.codec_id = AV_CODEC_ID_H264;
    AVCodecParameters hevc{};
    hevc.codec_id = AV_CODEC_ID_HEVC;
    const std::vector<uint8_t> avc2 = avcC(0xfd), avc4 = avcC(0xff), hvc2 = hvcC(0xc1), hvc4 = hvcC(0xc3);
    auto header = [](const AVCodecParameters& stream, const std::vector<uint8_t>& extra) {
        return wallpaperx::CodecHeader{&stream, extra.data(), extra.size()};
    };

    const OwnedPacket idr({0, 3, 0x65, 0x88, 0x84}, {});
    const OwnedPacket newSps({0, 4, 0x67, 0x42, 0, 0x1f, 0, 3, 0x65, 0x88, 0x84}, {});
    const OwnedPacket heldSps({0, 4, 0x67, 0x42, 0, 0x1e, 0, 3, 0x65, 0x88, 0x84}, {});
    const OwnedPacket hevcSlice({0, 3, 0x00, 0x01, 0xaf}, {});
    std::vector<uint8_t> changed = avc2;
    changed[3] = 0x1f;
    const OwnedPacket sideSame({0, 3, 0x65, 0x88, 0x84}, avc2);
    const OwnedPacket sideChanged({0, 3, 0x65, 0x88, 0x84}, changed);
    if (!idr.ok() || !newSps.ok() || !heldSps.ok() || !hevcSlice.ok() || !sideSame.ok() || !sideChanged.ok()) {
        fail("the harness could not build its packets, which is not a verdict on the NAL readers");
        return;
    }
    check(resetsDecoder(header(h264, avc2), *idr.packet),
          "avcC declaring 2-byte lengths: resetsDecoder() reads a 2-byte-prefixed IDR as a reset point");
    check(wallpaperx::firstSlice(header(h264, avc2), *idr.packet) == idr.packet->data + 2,
          "avcC declaring 2-byte lengths: firstSlice() finds the slice after a 2-byte prefix");
    check(!resetsDecoder(header(h264, avc4), *idr.packet),
          "the same 2-byte packet under an avcC declaring 4 is not read as a reset point (the fixture discriminates)");
    check(wallpaperx::beyondExtradata(header(h264, avc2), *newSps.packet),
          "avcC declaring 2-byte lengths: an SPS the extradata does not hold is beyond it");
    check(!wallpaperx::beyondExtradata(header(h264, avc2), *heldSps.packet),
          "avcC declaring 2-byte lengths: the extradata's own SPS is not beyond it");
    check(wallpaperx::firstSlice(header(hevc, hvc2), *hevcSlice.packet) == hevcSlice.packet->data + 2,
          "hvcC declaring 2-byte lengths: firstSlice() finds the HEVC slice after a 2-byte prefix");
    check(wallpaperx::firstSlice(header(hevc, hvc4), *hevcSlice.packet) == nullptr,
          "the same 2-byte HEVC packet under an hvcC declaring 4 holds no readable slice (the fixture discriminates)");
    check(wallpaperx::hevcSubLayers(header(hevc, hvc2)) == 3,
          "hvcC with no declared count: hevcSubLayers() reads 3 sub-layers from the SPS (got " +
              std::to_string(wallpaperx::hevcSubLayers(header(hevc, hvc2))) + ")");
    check(!wallpaperx::changesExtradata(header(h264, avc2), *sideSame.packet),
          "a byte-identical NEW_EXTRADATA does not change the extradata");
    check(wallpaperx::changesExtradata(header(h264, avc2), *sideChanged.packet),
          "a same-size NEW_EXTRADATA with one byte changed changes the extradata");
    check(!wallpaperx::changesExtradata(header(h264, avc2), *idr.packet), "a packet with no NEW_EXTRADATA changes nothing");
}

// GOAL: S15.952/S15.953 -- scanSoftwareStretches() finds a missing reference exactly where the decoder sends shown
//       pictures to software: an H.264 frame_num gap (counter_gap.mp4), a cut on a non-IDR I (counter_cut_nonidr.mp4),
//       an HEVC dropped reference or CRA start (counter_hevc_gap.mp4, counter_cut_cra.mp4); and none for healthy files or for an
//       H.264 transport stream cut mid-group whose lead-in ends at an IDR and is never shown (counter_cut_start.ts).
//       A lead-in ending at a non-IDR I with no recovery point (counter_lead_then_i.mp4) may be shown, so it is read
//       as the decoder sends it, and its missing references are found.
// WHY-THIS-SHAPE: answers graded on real cut and damaged files, both directions, rather than on the follower's
//       internals: the property is the scan's agreement with the decoder, which video_pipeline_damaged_device grades
//       on the same fixtures.
// CONTROL: control_batch.sh 2026-10-24 (T18b), each alone, reverted, renderer/video/software_scan.cpp: the lead-in
//       skip off in scanOnce() -> "counter_cut_start.ts: missing reference found, none due"; the H.264 frame_num
//       rule's `return true` made false -> "counter_gap.mp4: no missing reference found"; the HEVC rule's `return
//       true` made false -> "counter_hevc_gap.mp4: no missing reference found". control_batch.sh 2026-10-24 (T18d),
//       restored, renderer/video/reset_point.cpp h264UnitMayShow() not counting an I slice ->
//       "counter_lead_then_i.mp4: no missing reference found".
// COVERAGE-ASSERT: each row is graded or accounted for: a missing fixture sets g_notVerified (main() then exits
//       kNotVerifiedExitCode, a ctest skip, not a pass), and a scan that throws or gives no answer calls fail().
void runScanChecks() {
    struct Row {
        const char* file;
        bool missingReference;
    };
    const Row rows[] = {{"counter_gap.mp4", true},         {"counter_cut_nonidr.mp4", true},
                        {"counter_hevc_gap.mp4", true},
                        {"counter_cut_cra.mp4", true},     {"counter_cut_start.ts", false},
                        {"counter_lead_then_i.mp4", true},
                        {"counter_opengop.ts", false},     {"counter_two_idr.mp4", false},
                        {"counter_nonidr_src.mp4", false}, {"counter_opengop_hevc.mp4", false}};
    for (const Row& row : rows) {
        const std::string path = asset(row.file);
        if (!std::filesystem::exists(path)) {
            std::printf("reset_point_probe: NOT VERIFIED -- %s was not generated, so the scan is not graded on it\n",
                        row.file);
            g_notVerified = true;
            continue;
        }
        const std::atomic<bool> stop{false};
        std::optional<wallpaperx::SoftwareStretches> found;
        try {
            found = wallpaperx::scanSoftwareStretches(path, stop);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: reset_point_probe: %s: the scan threw %s\n", row.file, e.what());
            fail("scanSoftwareStretches() reads every fixture through");
            continue;
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: reset_point_probe: %s: the scan gave no answer without being stopped\n", row.file);
            fail("scanSoftwareStretches() answers when not stopped");
            continue;
        }
        if (found->missingReference != row.missingReference) {
            std::fprintf(stderr, "FAIL: reset_point_probe: %s: %s\n", row.file,
                         row.missingReference ? "no missing reference found" : "missing reference found, none due");
            fail("the scan finds a missing reference exactly where the decoder makes one up");
        }
        if (found->parameterSetsChangeOffReset) {
            std::fprintf(stderr, "FAIL: reset_point_probe: %s: a parameter-set change off a reset point found\n", row.file);
            fail("the scan finds no parameter-set change in a file without one");
        }
    }

    // GOAL: S15.952 -- a parameter-set change off a reset point sends pictures to software only on a device without
    //       inline parameters, and is not known without a device.
    // CONTROL: control_batch.sh 2026-10-24 (T18b), reverted, renderer/video/software_scan.h
    //       parameterSetsToSoftware(): `return !*inlineParameters` made `return *inlineParameters` -> "FAIL:
    //       parameterSetsToSoftware() answers software only for a change on a device without inline parameters".
    wallpaperx::SoftwareStretches change;
    change.parameterSetsChangeOffReset = true;
    const wallpaperx::SoftwareStretches none;
    const bool rule = change.parameterSetsToSoftware(false) == true && change.parameterSetsToSoftware(true) == false &&
                      !change.parameterSetsToSoftware(std::nullopt).has_value() &&
                      none.parameterSetsToSoftware(false) == false && none.parameterSetsToSoftware(std::nullopt) == false;
    if (!rule) fail("parameterSetsToSoftware() answers software only for a change on a device without inline parameters");
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
    //       key frame: resetsDecoder() said it resets the decoder". The two hevcUnit() breaks re-run 2026-10-24
    //       (G5) on its hevc::isIdr() form via control_batch.sh, the same lines; restored.
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

    if (!av1Entry) runLengthSizeChecks();
    if (!av1Entry) runScanChecks();

    const int failures = wallpaperx::testing::reportFailures("reset_point_probe");
    if (failures > 0) return 1;
    if (g_notVerified) {
        std::printf("reset_point_probe: NOT VERIFIED here -- reported to ctest as a skip, not a pass\n");
        return kNotVerifiedExitCode;
    }
    std::printf("reset_point_probe: all checks passed\n");
    return 0;
}
