// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "wallpaper_position.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace wallpaperx {

// A region of a source image, in its texels: [x0, x1) by [y0, y1).
struct Crop {
    int32_t x0, y0, x1, y1;
};

// The same, in fractions of a texel.
struct Area {
    double x0, y0, x1, y1;
};

// Which texels of a source are scaled onto which pixels of a target, the target in whole pixels.
// `exact` is where the target's edges fall in the source; `source` is that rounded to whole texels,
// for a blit, which cannot start mid-texel. Neither is ever empty.
struct Region {
    Area exact;
    Crop source;
    Crop target;
};

namespace placement_detail {

struct Span {
    double exact0, exact1;
    int32_t source0, source1, target0, target1;
};

// One axis of a source `source` texels long, scaled by `scale` and starting at target pixel `start`.
// An edge inside the target lands on the nearest pixel with the whole edge texel; an edge the
// target cuts is exactly where the cut falls, and the nearest whole texel for a blit. A span on the
// target thinner than a pixel still covers one; nothing only when it misses the target entirely.
inline std::optional<Span> placeSpan(double start, double scale, uint32_t source, uint32_t target) {
    const double end = start + source * scale;
    const double limit = target;
    const double whole = source;
    if (!(end > 0 && start < limit)) return std::nullopt;
    double target0 = std::clamp(std::round(start), 0.0, limit);
    double target1 = std::clamp(std::round(end), 0.0, limit);
    if (target0 == target1) {
        if (target1 < limit) {
            target1 += 1;
        } else {
            target0 -= 1;
        }
    }

    const double exact0 = start < 0 ? std::min(-start / scale, whole) : 0.0;
    const double exact1 = end > limit ? std::clamp((limit - start) / scale, exact0, whole) : whole;
    const double source0 = std::clamp(std::round(exact0), 0.0, whole - 1);
    const double source1 = std::clamp(std::round(exact1), source0 + 1, whole);
    return Span{exact0,
                exact1,
                static_cast<int32_t>(source0),
                static_cast<int32_t>(source1),
                static_cast<int32_t>(target0),
                static_cast<int32_t>(target1)};
}

// Where a source at its own size starts so that it is centred, on whole pixels.
inline double centredStart(uint32_t source, uint32_t target) {
    return std::floor((static_cast<double>(target) - source) / 2);
}

}  // namespace placement_detail

// Where `placement` puts a source on a target, or nothing when none of it lands there -- which only
// Free mode can do; every other mode covers at least a pixel on each axis. Tile gives the copy Centre would draw, the one the repetition starts from.
// Every extent must be non-zero and fit in int32_t, and placementAllowed(placement).
inline std::optional<Region> placedRegion(const Placement& placement, uint32_t sourceWidth, uint32_t sourceHeight,
                                          uint32_t targetWidth, uint32_t targetHeight) {
    using placement_detail::placeSpan;
    const double widthRatio = static_cast<double>(targetWidth) / sourceWidth;
    const double heightRatio = static_cast<double>(targetHeight) / sourceHeight;

    double scale = 1.0;
    double left = 0.0;
    double top = 0.0;
    switch (placement.mode) {
        case PositionMode::Stretch:
            return Region{{0.0, 0.0, static_cast<double>(sourceWidth), static_cast<double>(sourceHeight)},
                          {0, 0, static_cast<int32_t>(sourceWidth), static_cast<int32_t>(sourceHeight)},
                          {0, 0, static_cast<int32_t>(targetWidth), static_cast<int32_t>(targetHeight)}};
        case PositionMode::Centre:
        case PositionMode::Tile:
            left = placement_detail::centredStart(sourceWidth, targetWidth);
            top = placement_detail::centredStart(sourceHeight, targetHeight);
            break;
        case PositionMode::Fit:
            scale = std::min(widthRatio, heightRatio);
            left = (targetWidth - sourceWidth * scale) / 2;
            top = (targetHeight - sourceHeight * scale) / 2;
            break;
        case PositionMode::Cover:
        case PositionMode::Free: {
            // Cover is Free at its defaults, by construction.
            const Placement framing = placement.mode == PositionMode::Free ? placement : Placement{};
            scale = framing.zoom * std::max(widthRatio, heightRatio);
            left = targetWidth * (0.5 + framing.x) - sourceWidth * scale / 2;
            top = targetHeight * (0.5 + framing.y) - sourceHeight * scale / 2;
            break;
        }
    }

    const auto x = placeSpan(left, scale, sourceWidth, targetWidth);
    const auto y = placeSpan(top, scale, sourceHeight, targetHeight);
    if (!x || !y) return std::nullopt;
    return Region{{x->exact0, y->exact0, x->exact1, y->exact1},
                  {x->source0, y->source0, x->source1, y->source1},
                  {x->target0, y->target0, x->target1, y->target1}};
}

}
