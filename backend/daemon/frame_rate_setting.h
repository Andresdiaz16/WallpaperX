// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace wallpaperx {

// The most pictures a second a wallpaper is drawn at: the config root's setting, and a monitor's that
// replaces it. Only these, so a screen refreshing at 60 or 120 Hz holds every picture equally long.
inline constexpr int kFrameRates[] = {30, 60, 120};
inline constexpr int kDefaultFrameRate = 60;

inline bool isFrameRate(int rate) {
    for (const int known : kFrameRates) {
        if (rate == known) return true;
    }
    return false;
}

// "30, 60 or 120", for a refusal.
inline std::string frameRateChoices() {
    std::string text;
    for (size_t i = 0; i < std::size(kFrameRates); ++i) {
        if (i != 0) text += i + 1 == std::size(kFrameRates) ? " or " : ", ";
        text += std::to_string(kFrameRates[i]);
    }
    return text;
}

// The `frame_rate` field as config.json carries it: JSON text, empty when absent. At the root, absent is
// kDefaultFrameRate, which is written as absent so a config that never changed it stays byte-identical;
// on a monitor's row, absent means the root's applies.
inline std::string frameRateJsonText(int rate) { return std::to_string(rate); }

// Nothing for a value this build does not know, which a newer build may have written: it is kept as read
// and the setting above it applies -- the root's on a monitor, kDefaultFrameRate at the root.
inline std::optional<int> frameRateFromJson(std::string_view text) {
    for (const int rate : kFrameRates) {
        if (text == frameRateJsonText(rate)) return rate;
    }
    return std::nullopt;
}

// The root's `keep_higher_frame_rate`: whether a screen is drawn at its frame-rate setting even where
// that is above what the screen refreshes at. Absent means no, as sound_setting.h treats its default.
inline std::string keepHigherJsonText(bool keep) { return keep ? std::string("true") : std::string(); }

inline std::optional<bool> keepHigherFromJson(std::string_view text) {
    if (text.empty() || text == "false") return false;
    if (text == "true") return true;
    return std::nullopt;
}

// A refresh rate a client reports, in refreshes a second; 0 is a screen whose rate is not known.
inline constexpr double kMaxRefreshRate = 1000.0;

// Why a reported refresh rate is unusable, or empty when it is fine.
inline std::string refreshRateError(double rate) {
    if (rate == 0.0 || (std::isfinite(rate) && rate >= 1.0 && rate <= kMaxRefreshRate)) return {};
    return "refresh rate must be 0 (not known) or 1 to " + std::to_string(static_cast<int>(kMaxRefreshRate)) +
           " times a second";
}

}  // namespace wallpaperx
