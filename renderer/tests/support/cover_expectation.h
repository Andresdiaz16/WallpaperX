// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The region of a source that "scaled to cover, cropped centrally" shows, stated in real
// arithmetic for the probes that grade it. Deliberately NOT renderers/cover_crop.h's
// integer-product form: a copy of the implementation would agree with it however both were
// wrong. The two differ by under a texel, which each caller's tolerance accounts for.

#include <algorithm>
#include <cstdint>

namespace wallpaperx::testing {

struct CropRect {
    double x0 = 0, y0 = 0, width = 0, height = 0;
};

inline CropRect expectedCover(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t targetWidth,
                              uint32_t targetHeight) {
    const double sourceAspect = static_cast<double>(sourceWidth) / sourceHeight;
    const double targetAspect = static_cast<double>(targetWidth) / targetHeight;

    double width = sourceWidth;
    double height = sourceHeight;
    if (sourceAspect > targetAspect) {
        width = std::max(1.0, sourceHeight * targetAspect);
    } else if (sourceAspect < targetAspect) {
        height = std::max(1.0, sourceWidth / targetAspect);
    }
    return {(sourceWidth - width) / 2.0, (sourceHeight - height) / 2.0, width, height};
}

}  // namespace wallpaperx::testing
