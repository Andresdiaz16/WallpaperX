// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What a packet's own bitstream says about its first picture, read from the H.264, HEVC and AV1
// specifications and not from renderer/video/reset_point.cpp, so a probe can grade resetsDecoder() and
// where a start on the hardware decoder began against a reading that shares no code with it.

// <cstdint> first: libavutil refuses C++ that has not defined UINT64_C before it.
#include <cstddef>
#include <cstdint>
#include <optional>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
}

namespace wallpaperx::testing {

// The NAL length prefix an avcC or hvcC record declares, or 0 for start codes. Both records begin with
// configurationVersion 1; Annex B extradata begins with a start code.
inline int nalLengthSize(const AVCodecParameters& stream) {
    const uint8_t* extra = stream.extradata;
    const int size = stream.extradata_size;
    if (size < 1 || extra[0] != 1) return 0;
    if (stream.codec_id == AV_CODEC_ID_H264) return size > 4 ? (extra[4] & 3) + 1 : 0;
    return size > 21 ? (extra[21] & 3) + 1 : 0;
}

// The first NAL unit that carries a picture: its header's offset in the packet, and its type. H.264
// slices are types 1-5; HEVC's are 0-31, in layer 0.
struct FirstSlice {
    size_t offset;
    int type;
};

inline std::optional<FirstSlice> firstSlice(const uint8_t* data, size_t size, AVCodecID codec, int lengthSize) {
    auto slice = [&](size_t at) -> std::optional<FirstSlice> {
        if (codec == AV_CODEC_ID_H264) {
            const int type = data[at] & 0x1f;
            return type >= 1 && type <= 5 ? std::optional<FirstSlice>(FirstSlice{at, type}) : std::nullopt;
        }
        if (at + 1 >= size) return std::nullopt;
        const int type = (data[at] >> 1) & 0x3f;
        const int layer = ((data[at] & 1) << 5) | (data[at + 1] >> 3);
        return type <= 31 && layer == 0 ? std::optional<FirstSlice>(FirstSlice{at, type}) : std::nullopt;
    };
    if (lengthSize > 0) {
        for (size_t at = 0; at + static_cast<size_t>(lengthSize) < size;) {
            size_t length = 0;
            for (int i = 0; i < lengthSize; ++i) length = (length << 8) | data[at + static_cast<size_t>(i)];
            at += static_cast<size_t>(lengthSize);
            if (length == 0 || length > size - at) return std::nullopt;
            if (const std::optional<FirstSlice> found = slice(at)) return found;
            at += length;
        }
        return std::nullopt;
    }
    for (size_t at = 0; at + 3 < size; ++at) {
        if (data[at] != 0 || data[at + 1] != 0 || data[at + 2] != 1) continue;
        if (const std::optional<FirstSlice> found = slice(at + 3)) return found;
        at += 2;
    }
    return std::nullopt;
}

// The first frame or frame-header OBU of an AV1 temporal unit: the offset of its payload's first byte,
// whose top bits are show_existing_frame, frame_type (2 bits) and show_frame for a stream without a
// reduced still-picture header.
inline std::optional<size_t> av1FirstFrameHeader(const uint8_t* data, size_t size) {
    constexpr int kFrameHeader = 3;
    constexpr int kFrame = 6;
    for (size_t at = 0; at < size;) {
        const uint8_t header = data[at++];
        const int type = (header >> 3) & 0xf;
        if ((header & 0x04) != 0) ++at;
        size_t length = at < size ? size - at : 0;
        if ((header & 0x02) != 0) {
            length = 0;
            for (int shift = 0;; shift += 7) {
                if (at >= size || shift > 56) return std::nullopt;
                const uint8_t byte = data[at++];
                length |= static_cast<size_t>(byte & 0x7f) << shift;
                if ((byte & 0x80) == 0) break;
            }
        }
        if (at > size || length > size - at) return std::nullopt;
        if (type == kFrameHeader || type == kFrame) return length > 0 ? std::optional<size_t>(at) : std::nullopt;
        at += length;
    }
    return std::nullopt;
}

constexpr int kH264Idr = 5;
constexpr int kHevcIdrWithLeading = 19;
constexpr int kHevcIdrNoLeading = 20;
constexpr int kHevcCra = 21;

// Whether a decoder handed this packet first decodes it and everything shown after it from the stream
// alone: an H.264 IDR, an HEVC IDR, or an AV1 key frame that is shown.
inline bool referenceResets(const AVCodecParameters& stream, const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return false;
    if (stream.codec_id == AV_CODEC_ID_H264 || stream.codec_id == AV_CODEC_ID_HEVC) {
        const std::optional<FirstSlice> first = firstSlice(data, size, stream.codec_id, nalLengthSize(stream));
        if (!first) return false;
        return stream.codec_id == AV_CODEC_ID_H264 ? first->type == kH264Idr
                                                   : first->type == kHevcIdrWithLeading || first->type == kHevcIdrNoLeading;
    }
    if (stream.codec_id == AV_CODEC_ID_AV1) {
        const std::optional<size_t> at = av1FirstFrameHeader(data, size);
        if (!at) return false;
        const uint8_t bits = data[*at];
        return (bits & 0x80) == 0 && ((bits >> 5) & 3) == 0 && (bits & 0x10) != 0;
    }
    return false;
}

}  // namespace wallpaperx::testing
