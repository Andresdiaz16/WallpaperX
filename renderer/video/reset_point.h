// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

struct AVCodecParameters;
struct AVPacket;

namespace wallpaperx {

// Whether a decoder handed `packet` first, with nothing before it, decodes it and every picture shown
// after it from the stream alone: H.264 IDR, HEVC IDR, an AV1 key frame that is shown. Read from the
// bitstream, because a container's keyframe flag also marks pictures that do not reset: H.264 I
// pictures with a recovery point, and HEVC CRA and BLA pictures. Started at one of those, ffmpeg hands
// the hardware decoder stand-ins for references it never decoded.
//
// BLA is excluded although the standard resets there: ffmpeg 9.0.2 still builds the picture's
// reference set, so it binds the same never-decoded stand-ins as at a CRA. False for any other codec,
// and for a packet whose first picture cannot be read.
bool resetsDecoder(const AVCodecParameters& stream, const AVPacket& packet);

}
