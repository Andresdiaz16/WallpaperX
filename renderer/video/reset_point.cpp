// SPDX-License-Identifier: GPL-2.0-or-later

#include "reset_point.h"

#include "nal_bits.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace wallpaperx {

namespace {

// What one NAL unit says about its picture: nothing (not a base-layer slice), or whether it resets.
using Verdict = std::optional<bool>;

Verdict h264Unit(const uint8_t* unit, size_t size) {
    if (size < 1) return std::nullopt;
    const int type = h264Type(unit);
    if (type < h264::kSlice || type > h264::kIdr) return std::nullopt;
    return type == h264::kIdr;
}

Verdict hevcUnit(const uint8_t* unit, size_t size) {
    if (size < 2) return std::nullopt;
    const int type = hevcType(unit);
    if (type >= hevc::kVps || hevcLayer(unit) != 0) return std::nullopt;
    return hevc::isIdr(type);
}

Verdict sliceUnit(AVCodecID codec, const uint8_t* unit, size_t size) {
    return codec == AV_CODEC_ID_H264 ? h264Unit(unit, size) : hevcUnit(unit, size);
}

// The length-prefix size an avcC or hvcC header in `extra` declares, 0 for start codes, and nothing for
// a header too short to hold it. The tests are ffmpeg's own for telling the two layouts apart.
std::optional<int> lengthSize(AVCodecID codec, const uint8_t* extra, size_t extraSize) {
    if (extra == nullptr) return 0;
    if (codec == AV_CODEC_ID_H264) {
        if (extraSize < 1 || extra[0] != 1) return 0;
        if (extraSize < 7) return std::nullopt;
        return (extra[4] & 3) + 1;
    }
    if (extraSize <= 3 || (extra[0] == 0 && extra[1] == 0 && extra[2] <= 1)) return 0;
    if (extraSize < 23) return std::nullopt;
    return (extra[21] & 3) + 1;
}

std::optional<int> lengthSize(const CodecHeader& header) {
    return lengthSize(header.stream->codec_id, header.extradata, header.size);
}

// Where the next 00 00 01 at or after `from` begins, or `size`.
size_t startCode(const uint8_t* data, size_t size, size_t from) {
    for (size_t at = from + 2; at < size;) {
        const auto* one = static_cast<const uint8_t*>(std::memchr(data + at, 1, size - at));
        if (one == nullptr) break;
        at = static_cast<size_t>(one - data);
        if (data[at - 1] == 0 && data[at - 2] == 0) return at - 2;
        ++at;
    }
    return size;
}

// Hands `unit` each NAL unit in turn until it returns true. Units are prefixed by their big-endian
// length in `prefix` bytes, the MP4 and Matroska layout, or separated by start codes when it is 0, where
// the zero bytes before the next start code belong to the byte stream, not the unit. False when a
// length runs past the data.
template <typename Unit>
bool eachUnit(const uint8_t* data, size_t size, int prefix, Unit unit) {
    if (prefix > 0) {
        size_t at = 0;
        while (size - at >= static_cast<size_t>(prefix)) {
            size_t length = 0;
            for (int i = 0; i < prefix; ++i) length = (length << 8) | data[at + i];
            at += static_cast<size_t>(prefix);
            if (length > size - at) return false;
            if (unit(data + at, length)) return true;
            at += length;
        }
        return true;
    }
    for (size_t code = startCode(data, size, 0); code < size;) {
        const size_t start = code + 3;
        const size_t next = startCode(data, size, start);
        size_t end = next;
        while (end > start && data[end - 1] == 0) --end;
        if (unit(data + start, end - start)) return true;
        code = next;
    }
    return true;
}

// Hands `unit` each NAL unit an avcC, hvcC or start-code header holds, until it returns true. False when
// the header cannot all be read. An avcC's trailing high-profile fields hold no unit.
template <typename Unit>
bool eachHeaderUnit(AVCodecID codec, const uint8_t* data, size_t size, Unit unit) {
    const std::optional<int> prefix = lengthSize(codec, data, size);
    if (!prefix) return false;
    if (*prefix == 0) return eachUnit(data, size, 0, unit);
    size_t at = codec == AV_CODEC_ID_H264 ? 5 : 22;
    const unsigned groups = codec == AV_CODEC_ID_H264 ? 2 : data[at++];
    for (unsigned group = 0; group < groups; ++group) {
        if (at >= size) return false;
        unsigned count = 0;
        if (codec == AV_CODEC_ID_H264) {
            count = group == 0 ? data[at++] & 0x1f : data[at++];
        } else {
            if (size - at < 3) return false;
            count = (data[at + 1] << 8) | data[at + 2];
            at += 3;
        }
        for (unsigned i = 0; i < count; ++i) {
            if (size - at < 2) return false;
            const size_t length = (data[at] << 8) | data[at + 1];
            at += 2;
            if (length > size - at) return false;
            if (unit(data + at, length)) return true;
            at += length;
        }
    }
    return true;
}

struct Unit {
    const uint8_t* data;
    size_t size;
};

std::optional<Unit> firstSliceUnit(const CodecHeader& header, const AVPacket& packet) {
    const AVCodecID codec = header.stream->codec_id;
    if ((codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_HEVC) || packet.data == nullptr || packet.size <= 0) {
        return std::nullopt;
    }
    const std::optional<int> prefix = lengthSize(header);
    if (!prefix) return std::nullopt;
    std::optional<Unit> found;
    eachUnit(packet.data, static_cast<size_t>(packet.size), *prefix, [&](const uint8_t* unit, size_t size) {
        if (sliceUnit(codec, unit, size)) found = Unit{unit, size};
        return found.has_value();
    });
    return found;
}

bool isParameterSet(AVCodecID codec, const uint8_t* unit, size_t size) {
    if (codec == AV_CODEC_ID_H264) {
        if (size < 1) return false;
        const int type = h264Type(unit);
        return type == h264::kSps || type == h264::kPps;
    }
    if (size < 2 || hevcLayer(unit) != 0) return false;
    const int type = hevcType(unit);
    return type >= hevc::kVps && type <= hevc::kPps;
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

// Whether an SEI unit carries a recovery point, or may: one that cannot be read through says nothing.
bool h264RecoveryPoint(const uint8_t* unit, size_t size) {
    constexpr uint32_t kRecoveryPoint = 6;
    NalBits bits(unit, size, 1, size);
    // Past the last message only the stop bit and its padding are left.
    while (bits.bitsLeft() > 8) {
        const auto extended = [&] {
            uint32_t value = 0;
            uint32_t byte = 0;
            do {
                byte = bits.u(8);
                value += byte;
            } while (byte == 0xff && bits.ok());
            return value;
        };
        const uint32_t type = extended();
        const uint32_t payload = extended();
        if (!bits.ok() || type == kRecoveryPoint || size_t{payload} * 8 > bits.bitsLeft()) return true;
        bits.skip(size_t{payload} * 8);
    }
    return false;
}

// Whether ffmpeg could start outputting pictures at an H.264 unit, or at its picture: see neverShownBeforeReset().
bool h264UnitMayShow(const uint8_t* unit, size_t size) {
    if (size < 1) return false;
    constexpr int kLastPartition = 4;
    const int type = h264Type(unit);
    if (type == h264::kSei) return h264RecoveryPoint(unit, size);
    if (type == h264::kIdr || (type > h264::kSlice && type <= kLastPartition)) return true;
    if (type != h264::kSlice) return false;
    NalBits bits(unit, size, 1, 16);
    bits.ue();  // first_mb_in_slice
    const uint32_t sliceType = bits.ue() % 5;
    constexpr uint32_t kI = 2;
    constexpr uint32_t kSI = 4;
    return !bits.ok() || sliceType == kI || sliceType == kSI;
}

}  // namespace

CodecHeader CodecHeader::of(const AVCodecParameters& stream) {
    const bool held = stream.extradata != nullptr && stream.extradata_size > 0;
    return {&stream, held ? stream.extradata : nullptr, held ? static_cast<size_t>(stream.extradata_size) : 0};
}

bool resetsDecoder(const CodecHeader& header, const AVPacket& packet) {
    if (packet.data == nullptr || packet.size <= 0) return false;
    switch (header.stream->codec_id) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC: {
            const std::optional<Unit> slice = firstSliceUnit(header, packet);
            return slice && *sliceUnit(header.stream->codec_id, slice->data, slice->size);
        }
        case AV_CODEC_ID_AV1:
            return av1TemporalUnit(packet.data, static_cast<size_t>(packet.size));
        default:
            return false;
    }
}

bool resetsDecoder(const AVCodecParameters& stream, const AVPacket& packet) {
    return resetsDecoder(CodecHeader::of(stream), packet);
}

const uint8_t* firstSlice(const CodecHeader& header, const AVPacket& packet) {
    const std::optional<Unit> slice = firstSliceUnit(header, packet);
    return slice ? slice->data : nullptr;
}

uint8_t hevcSubLayers(const CodecHeader& header) {
    const std::optional<int> prefix = lengthSize(header);
    if (header.stream->codec_id != AV_CODEC_ID_HEVC || !prefix) return 0;
    if (*prefix != 0) {
        if (const uint8_t declared = (header.extradata[21] >> 3) & 7; declared != 0) return declared;
    }
    // The SPS NAL header is two bytes; the first payload byte holds the count, unescaped.
    uint8_t layers = 0;
    eachHeaderUnit(AV_CODEC_ID_HEVC, header.extradata, header.size, [&](const uint8_t* unit, size_t size) {
        if (size >= 3 && hevcType(unit) == hevc::kSps) layers = static_cast<uint8_t>(((unit[2] >> 1) & 7) + 1);
        return layers != 0;
    });
    return layers;
}

bool changesExtradata(const CodecHeader& header, const AVPacket& packet) {
    const AVCodecID codec = header.stream->codec_id;
    if (codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_HEVC) return false;
    size_t sideSize = 0;
    const uint8_t* side = av_packet_get_side_data(&packet, AV_PKT_DATA_NEW_EXTRADATA, &sideSize);
    return side != nullptr && sideSize > 0 &&
           (sideSize != header.size || std::memcmp(side, header.extradata, sideSize) != 0);
}

bool differInParameterSets(const CodecHeader& a, const CodecHeader& b) {
    const AVCodecID codec = a.stream->codec_id;
    if (codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_HEVC) return false;
    if (a.size != b.size) return true;
    if (a.size == 0) return false;
    // The byte holding lengthSizeMinusOne in its low two bits, where both are an avcC or hvcC.
    std::optional<size_t> lengthByte;
    const std::optional<int> aPrefix = lengthSize(a);
    const std::optional<int> bPrefix = lengthSize(b);
    if (aPrefix && bPrefix && *aPrefix != 0 && *bPrefix != 0) lengthByte = codec == AV_CODEC_ID_H264 ? 4 : 21;
    for (size_t i = 0; i < a.size; ++i) {
        const uint8_t mask = lengthByte == i ? 0xfc : 0xff;
        if ((a.extradata[i] & mask) != (b.extradata[i] & mask)) return true;
    }
    return false;
}

bool changesParameterSets(const CodecHeader& header, const AVPacket& packet) {
    size_t sideSize = 0;
    const uint8_t* side = av_packet_get_side_data(&packet, AV_PKT_DATA_NEW_EXTRADATA, &sideSize);
    return side != nullptr && sideSize > 0 && differInParameterSets(header, CodecHeader{header.stream, side, sideSize});
}

bool beyondExtradata(const CodecHeader& header, const AVPacket& packet) {
    const AVCodecID codec = header.stream->codec_id;
    if (codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_HEVC) return false;
    if (changesExtradata(header, packet)) return true;
    const std::optional<int> prefix = lengthSize(header);
    if (packet.data == nullptr || packet.size <= 0 || !prefix) return false;
    const auto inExtradata = [&](const uint8_t* unit, size_t size) {
        bool found = false;
        eachHeaderUnit(codec, header.extradata, header.size, [&](const uint8_t* held, size_t heldSize) {
            found = heldSize == size && std::memcmp(held, unit, size) == 0;
            return found;
        });
        return found;
    };
    bool beyond = false;
    const bool read = eachUnit(packet.data, static_cast<size_t>(packet.size), *prefix, [&](const uint8_t* unit, size_t size) {
        beyond = beyond || (isParameterSet(codec, unit, size) && !inExtradata(unit, size));
        return false;
    });
    return read && beyond;
}

bool eachPacketUnit(const CodecHeader& header, const AVPacket& packet, const NalUnit& unit) {
    const AVCodecID codec = header.stream->codec_id;
    if (codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_HEVC) return false;
    const std::optional<int> prefix = lengthSize(header);
    if (!prefix || packet.data == nullptr || packet.size <= 0) return false;
    return eachUnit(packet.data, static_cast<size_t>(packet.size), *prefix, unit);
}

bool eachExtradataUnit(const CodecHeader& header, const NalUnit& unit) {
    const AVCodecID codec = header.stream->codec_id;
    if (codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_HEVC) return false;
    if (header.extradata == nullptr) return true;
    return eachHeaderUnit(codec, header.extradata, header.size, unit);
}

bool neverShownBeforeReset(const CodecHeader& header, const AVPacket& packet) {
    if (header.stream->codec_id != AV_CODEC_ID_H264 || packet.data == nullptr || packet.size <= 0) return false;
    const std::optional<int> prefix = lengthSize(header);
    if (!prefix || beyondExtradata(header, packet)) return false;
    bool shows = false;
    const bool read = eachUnit(packet.data, static_cast<size_t>(packet.size), *prefix, [&](const uint8_t* unit, size_t size) {
        shows = h264UnitMayShow(unit, size);
        return shows;
    });
    return read && !shows;
}

}
