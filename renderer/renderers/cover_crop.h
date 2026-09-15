// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>

namespace wallpaperx {

// A region of a source image, in its texels: [x0, x1) by [y0, y1).
struct Crop {
    int32_t x0, y0, x1, y1;
};

// The centred region of a source with the target's aspect ratio: what "scaled and cropped" shows
// once the source is scaled uniformly to cover the target. Products rather than divided ratios
// so the comparison is exact; the region is never larger than the source and never empty,
// whatever the two shapes are. Every extent must be non-zero and fit in int32_t.
inline Crop coverCrop(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t targetWidth, uint32_t targetHeight) {
    const uint64_t sourceWide = static_cast<uint64_t>(sourceWidth) * targetHeight;
    const uint64_t targetWide = static_cast<uint64_t>(targetWidth) * sourceHeight;

    uint64_t width = sourceWidth;
    uint64_t height = sourceHeight;
    if (sourceWide > targetWide) {
        width = std::max<uint64_t>(1, static_cast<uint64_t>(sourceHeight) * targetWidth / targetHeight);
    } else if (sourceWide < targetWide) {
        height = std::max<uint64_t>(1, static_cast<uint64_t>(sourceWidth) * targetHeight / targetWidth);
    }

    const uint64_t x0 = (sourceWidth - width) / 2;
    const uint64_t y0 = (sourceHeight - height) / 2;
    return {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(x0 + width),
            static_cast<int32_t>(y0 + height)};
}

}
