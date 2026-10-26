// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_decoder.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace wallpaperx::ycbcr {

// A planar Y'CbCr layout as ffmpeg describes it: Y' on its own plane, Cb and Cr either
// interleaved on a second plane or on a plane each, every sample the same depth.
struct Layout {
    uint32_t planeCount;
    uint32_t bitDepth;
    // Whether a code sits in the top bits of its 16-bit sample, as P010 and every Vulkan X6
    // format store it, rather than the bottom bits as ffmpeg's yuv420p10 does.
    bool codeInTopBits;
    uint32_t chromaShiftX;
    uint32_t chromaShiftY;
    // The plane and the component within it that hold Cb and Cr.
    uint32_t cbPlane, crPlane;
    VkComponentSwizzle cbComponent, crComponent;
};

std::optional<Layout> layoutFor(AVPixelFormat format);

// The single-plane format a plane is held in, and what a sample of it multiplies to for the
// code value.
struct PlaneFormat {
    VkFormat format;
    uint32_t texelBytes;
    float sampleToCode;
};

// The texels of a plane subsampled by the given shifts that any pixel of `visible` shares, and no
// others. `visible` must lie inside the coded picture.
VkRect2D planeRegion(VkRect2D visible, uint32_t shiftX, uint32_t shiftY);

// The one multi-planar VkFormat a decoder's image of this layout is held in, as Vulkan names
// them, or UNDEFINED for one never read from a decoder's image.
VkFormat deviceFormatFor(const Layout& layout);

struct Planes {
    Layout layout;
    PlaneFormat luma;
    PlaneFormat chroma;
};

// How a picture in `format` from `storage` has its planes held on this device, each plane format
// supporting `planeFeatures` with optimal tiling, or why it cannot. A device image's planes take
// the plane's own compatible format, which above 8 bits is an X6 or X4 format whose samples are
// normalised over the code's bits; a host plane takes whichever format its bit placement matches.
std::variant<Planes, std::string> planesFor(const Device& device, AVPixelFormat format, DecodedFrame::Storage storage,
                                            VkFormatFeatureFlags2 planeFeatures);

// rgb = matrix * sampled + offset, for plane samples in [0, 1]: range, depth and sample
// alignment folded in. Columns padded to vec4 as std430 lays a mat3 out.
struct ColourMap {
    float matrix[3][4];
    float offset[4];
};

// Throws WallpaperContentError, its message led by `component`, unless primaries, transfer and
// matrix are each one this map accepts by name: it does no gamut, transfer or tone conversion.
ColourMap colourMapFor(const DecodedFrame& frame, const Layout& layout, float sampleToCode, const char* component);

// Where a chroma sample sits right of and below the luma sample it shares a texel with, in luma
// pixels. Untagged is left, which is what H.264 and H.265 infer.
std::array<float, 2> chromaSiting(AVChromaLocation location, const Layout& layout);

}
