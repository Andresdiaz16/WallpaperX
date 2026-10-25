// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

struct AVCodecParameters;
struct AVPacket;

namespace wallpaperx {

// Readers of an H.264 or HEVC stream's NAL units, laid out as the extradata in effect says: length-prefixed
// as avcC or hvcC declares, or separated by start codes. Only the base layer is read, the one decoded.

// A codec and the extradata in effect for it: the stream's own, or AV_PKT_DATA_NEW_EXTRADATA a packet
// carried since, which ffmpeg's H.264 and HEVC decoders adopt -- the length-prefix size included. The
// bytes are borrowed.
struct CodecHeader {
    const AVCodecParameters* stream;  // for its codec only
    const uint8_t* extradata;
    size_t size;
    static CodecHeader of(const AVCodecParameters& stream);
};

// Whether a decoder handed `packet` first, with nothing before it, decodes it and every picture shown
// after it from the stream alone: H.264 IDR, HEVC IDR, an AV1 key frame that is shown. Read from the
// bitstream, because a container's keyframe flag also marks pictures that do not reset: H.264 I
// pictures with a recovery point, and HEVC CRA and BLA pictures. Started at one of those, ffmpeg hands
// the hardware decoder stand-ins for references it never decoded.
//
// BLA is excluded although the standard resets there: ffmpeg 9.0.2 still builds the picture's
// reference set, so it binds the same never-decoded stand-ins as at a CRA. False for any other codec,
// and for a packet whose first picture cannot be read.
bool resetsDecoder(const CodecHeader& header, const AVPacket& packet);
bool resetsDecoder(const AVCodecParameters& stream, const AVPacket& packet);

// The NAL unit of `packet`'s first slice, from its header: at least one byte of it for H.264, two for
// HEVC. Null for any other codec and when none is read.
const uint8_t* firstSlice(const CodecHeader& header, const AVPacket& packet);

// sps_max_sub_layers_minus1 + 1 of an HEVC stream's first SPS in the extradata, or hvcC's own count
// where it is set; 0 when neither can be read.
uint8_t hevcSubLayers(const CodecHeader& header);

// Whether `packet` carries AV_PKT_DATA_NEW_EXTRADATA that differs byte for byte from `header`'s extradata,
// for H.264 or HEVC: the codecs whose ffmpeg decoders adopt it. False for any other codec.
bool changesExtradata(const CodecHeader& header, const AVPacket& packet);

// Whether two H.264 or HEVC extradata hold different parameter sets: they differ in anything but the NAL
// length-prefix size an avcC or hvcC declares, which ffmpeg's decoder applies itself and a hardware decoder
// never sees. False for any other codec.
bool differInParameterSets(const CodecHeader& a, const CodecHeader& b);

// changesExtradata() where the side data's parameter sets differ from `header`'s, as differInParameterSets().
bool changesParameterSets(const CodecHeader& header, const AVPacket& packet);

// Whether `packet` gives an H.264 or HEVC decoder opened from `header` a parameter set it was not opened
// with: changesExtradata(), or an SPS, PPS or VPS unit that is not one of the extradata's byte for byte.
// A packet whose units cannot all be read gives none: ffmpeg rejects the whole packet.
bool beyondExtradata(const CodecHeader& header, const AVPacket& packet);

// Hands `unit` each NAL unit, header included, until it returns true: of `packet` laid out as `header` says, or
// of `header`'s own extradata. False when the extradata's layout cannot be read or a length runs past the
// bytes. H.264 and HEVC only; false for any other codec.
using NalUnit = std::function<bool(const uint8_t* unit, size_t size)>;
bool eachPacketUnit(const CodecHeader& header, const AVPacket& packet, const NalUnit& unit);
bool eachExtradataUnit(const CodecHeader& header, const NalUnit& unit);

// Whether an H.264 decoder that has met no IDR since it was opened or flushed can be spared `packet` without a
// picture it outputs changing, provided an IDR follows before any packet this is false for: it holds no IDR, I or
// SI slice and no recovery point SEI, and gives no parameter set beyond `header`. FFmpeg 9.0.2 outputs a picture
// only from the first of those on -- an I picture can be its unmarked random access point -- and decodes an IDR
// from nothing before it. Data partitions, which FFmpeg does not decode, count as slices. False for any other codec
// and for a packet whose units cannot all be read.
bool neverShownBeforeReset(const CodecHeader& header, const AVPacket& packet);

}
