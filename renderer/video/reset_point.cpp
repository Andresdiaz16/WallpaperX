// SPDX-License-Identifier: GPL-2.0-or-later

#include "reset_point.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
}

#include <cstddef>
#include <cstdint>
#include <optional>

namespace wallpaperx {

namespace {

// What one NAL unit says about its picture: nothing (not a slice), or whether it resets.
using Verdict = std::optional<bool>;

Verdict h264Unit(const uint8_t* unit, size_t size) {
    if (size < 1) return std::nullopt;
    const int type = unit[0] & 0x1f;
    if (type < 1 || type > 5) return std::nullopt;
    return type == 5;
}

Verdict hevcUnit(const uint8_t* unit, size_t size) {
    if (size < 2) return std::nullopt;
    const int type = (unit[0] >> 1) & 0x3f;
    const int layer = ((unit[0] & 1) << 5) | (unit[1] >> 3);
    if (type >= 32 || layer != 0) return std::nullopt;
    constexpr int kIdrWithLeading = 19;
    constexpr int kIdrNoLeading = 20;
    return type == kIdrWithLeading || type == kIdrNoLeading;
}

// The first slice's verdict. Units are prefixed by their big-endian length in `lengthSize` bytes, the
// MP4 and Matroska layout, or separated by start codes when it is 0.
template <typename Unit>
bool firstSlice(const uint8_t* data, size_t size, int lengthSize, Unit unit) {
    if (lengthSize > 0) {
        size_t at = 0;
        while (size - at >= static_cast<size_t>(lengthSize)) {
            size_t length = 0;
            for (int i = 0; i < lengthSize; ++i) length = (length << 8) | data[at + i];
            at += static_cast<size_t>(lengthSize);
            if (length > size - at) return false;
            if (const Verdict verdict = unit(data + at, length)) return *verdict;
            at += length;
        }
        return false;
    }
    for (size_t at = 0; at + 3 <= size; ++at) {
        if (data[at] != 0 || data[at + 1] != 0 || data[at + 2] != 1) continue;
        const size_t start = at + 3;
        size_t end = start;
        while (end + 3 <= size && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1)) ++end;
        if (end + 3 > size) end = size;
        if (const Verdict verdict = unit(data + start, end - start)) return *verdict;
        at = end - 1;
    }
    return false;
}

// The length-prefix size an avcC or hvcC header declares, 0 for start codes, and nothing for a header
// too short to hold it. The tests are ffmpeg's own for telling the two layouts apart.
std::optional<int> lengthSize(const AVCodecParameters& stream) {
    const uint8_t* extra = stream.extradata;
    const int extraSize = stream.extradata_size;
    if (stream.codec_id == AV_CODEC_ID_H264) {
        if (extraSize < 1 || extra[0] != 1) return 0;
        if (extraSize < 7) return std::nullopt;
        return (extra[4] & 3) + 1;
    }
    if (extraSize <= 3 || (extra[0] == 0 && extra[1] == 0 && extra[2] <= 1)) return 0;
    if (extraSize < 23) return std::nullopt;
    return (extra[21] & 3) + 1;
}

bool readLeb128(const uint8_t* data, size_t size, size_t& at, uint64_t& value) {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        if (at >= size) return false;
        const uint8_t byte = data[at++];
        value |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

// A temporal unit of low-overhead OBUs, judged by its first frame header. Its first bits are
// show_existing_frame, frame_type and show_frame unless the sequence uses a reduced still-picture
// header, where every frame is a shown key frame and any answer is safe.
bool av1TemporalUnit(const uint8_t* data, size_t size) {
    constexpr int kFrameHeader = 3;
    constexpr int kFrame = 6;
    constexpr int kKeyFrame = 0;
    size_t at = 0;
    while (at < size) {
        const uint8_t header = data[at++];
        const int type = (header >> 3) & 0xf;
        if ((header & 0x04) != 0) ++at;  // extension byte
        uint64_t length = size > at ? size - at : 0;
        if ((header & 0x02) != 0 && !readLeb128(data, size, at, length)) return false;
        if (at > size || length > size - at) return false;
        if (type == kFrameHeader || type == kFrame) {
            if (length < 1) return false;
            const uint8_t bits = data[at];
            const bool showExisting = (bits & 0x80) != 0;
            const int frameType = (bits >> 5) & 3;
            const bool shown = (bits & 0x10) != 0;
            return !showExisting && frameType == kKeyFrame && shown;
        }
        at += length;
    }
    return false;
}

}  // namespace

bool resetsDecoder(const AVCodecParameters& stream, const AVPacket& packet) {
    if (packet.data == nullptr || packet.size <= 0) return false;
    const auto size = static_cast<size_t>(packet.size);
    switch (stream.codec_id) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC: {
            const std::optional<int> prefix = lengthSize(stream);
            if (!prefix) return false;
            return stream.codec_id == AV_CODEC_ID_H264 ? firstSlice(packet.data, size, *prefix, h264Unit)
                                                       : firstSlice(packet.data, size, *prefix, hevcUnit);
        }
        case AV_CODEC_ID_AV1:
            return av1TemporalUnit(packet.data, size);
        default:
            return false;
    }
}

}
