// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace wallpaperx {

// Where VideoDecoder, on a device that decodes the stream's codec, profile and size, still leaves the device for
// software, as read from the file's packet headers without decoding anything. H.264 and HEVC only: no other
// codec has such a stretch.
struct SoftwareStretches {
    // ffmpeg would make up a picture the stream lacks -- a frame_num gap, a reference set naming a picture never
    // decoded, a start part way into a group or on an HEVC CRA -- which VideoDecoder decodes in software up to the
    // next reset point after it (Path::SoftwareMissingReference). Not for the pictures before an H.264 stream's first
    // IDR that VideoDecoder reads past undecoded (neverShownBeforeReset()).
    bool missingReference = false;
    // Only with missingReference: the first picture already lacks one and no reset point follows it, so no picture
    // of any pass reaches the device.
    bool missingReferenceThroughout = false;
    // A packet that is not a reset point carries AV_PKT_DATA_NEW_EXTRADATA with other parameter sets than the
    // codec holds, which leaves a device without Device::videoInlineParameters() for the rest of the pass.
    bool parameterSetsChangeOffReset = false;

    // Whether that change sends pictures to software on a device whose Device::videoInlineParameters() is
    // `inlineParameters`; not known without a device to ask.
    std::optional<bool> parameterSetsToSoftware(std::optional<bool> inlineParameters) const {
        if (!parameterSetsChangeOffReset) return false;
        if (!inlineParameters) return std::nullopt;
        return !*inlineParameters;
    }
};

// Reads `file`'s video packets once, front to back, as the decoder's first pass would meet them, and stops at
// the first missing reference: past it the answer is the same on every device. When that is the first picture, it
// reads on to the first reset point, without parsing, for missingReferenceThroughout. The packets before an H.264
// stream's first IDR are read twice where the decoder, having read past them, rewinds to send them after all
// (neverShownBeforeReset()). Nothing when `stop` was seen
// true, which is read once a packet. Throws WallpaperContentError for a file that cannot be opened or read
// and ResourceExhausted when out of memory, as VideoDecoder's constructor does.
//
// What it follows is FFmpeg 9.0.2's own bookkeeping, not the standard's: H.264 frame_num against the last
// picture decoded, HEVC reference sets against the pictures held; and a picture whose parameter sets were never
// seen is one FFmpeg drops, so it makes no reference of its own. Not followed: H.264 memory management
// operation 5, after which a later frame_num reads as a gap that is not one.
std::optional<SoftwareStretches> scanSoftwareStretches(const std::string& file, const std::atomic<bool>& stop);

// Raised whenever scanSoftwareStretches() could answer otherwise for the same bytes, so that an answer saved under
// another is not reused.
inline constexpr uint32_t kSoftwareScanRule = 2;

}
