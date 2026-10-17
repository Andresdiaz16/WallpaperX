// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The playback speeds, shared by the app and the daemon so a package's speed and a saved one land on
// the same choice in both. Header-only: the app does not link the daemon.

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace wallpaperx {

// The speeds a video plays at, and the only values "speed" takes.
inline constexpr std::array<double, 3> kPlaybackSpeeds{0.5, 1.0, 2.0};

// The one of `among` nearest `speed` by ratio, so 0.7 plays at 1 and 1.5 at 2. A speed that is not a
// positive finite number is taken as 1, the speed of a video without one. `among` must not be empty.
inline double nearestPlaybackSpeed(double speed, std::span<const double> among = kPlaybackSpeeds)
{
    const double from = std::isfinite(speed) && speed > 0 ? speed : 1.0;
    return *std::ranges::min_element(among, {}, [from](double to) { return std::fabs(std::log(from / to)); });
}

// kPlaybackSpeeds for a sentence, the last joined by `last`: "0.5, 1 or 2".
inline std::string playbackSpeedsText(std::string_view last)
{
    std::string text;
    for (std::size_t i = 0; i < kPlaybackSpeeds.size(); ++i) {
        if (i > 0) {
            text += i + 1 < kPlaybackSpeeds.size() ? std::string(", ") : std::format(" {} ", last);
        }
        text += std::format("{}", kPlaybackSpeeds[i]);
    }
    return text;
}

} // namespace wallpaperx
