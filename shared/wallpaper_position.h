// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// How a wallpaper is laid onto its screen: the "position" properties a wallpaper carries on each
// screen, shared by the app and the daemon so both accept exactly the same values. Header-only:
// the app does not link the daemon.

#include <cmath>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace wallpaperx {

// Every mode leaves black wherever the picture does not reach.
enum class PositionMode {
    Cover,    // scaled to fill the screen, the overflow cropped centrally; the default
    Fit,      // scaled to show all of it, centred
    Centre,   // at its own size, centred, cropped where larger than the screen
    Tile,     // at its own size, repeated, one copy where Centre puts it; still images only
    Stretch,  // scaled to fill the screen exactly, its proportions not kept
    Free,     // the user's own zoom and offset, below
};

// Every mode: a new one goes here as well as in toString()'s switch.
inline constexpr PositionMode kPositionModes[] = {PositionMode::Cover, PositionMode::Fit,  PositionMode::Centre,
                                                  PositionMode::Tile,  PositionMode::Stretch, PositionMode::Free};

// The names on the wire, in config.json and in GetStatus.
inline const char* toString(PositionMode mode) {
    switch (mode) {
        case PositionMode::Cover: return "cover";
        case PositionMode::Fit: return "fit";
        case PositionMode::Centre: return "center";
        case PositionMode::Tile: return "tile";
        case PositionMode::Stretch: return "stretch";
        case PositionMode::Free: return "free";
    }
    return "cover";
}

inline std::optional<PositionMode> positionModeFromName(std::string_view name) {
    for (const PositionMode mode : kPositionModes) {
        if (name == toString(mode)) return mode;
    }
    return std::nullopt;
}

// A video is blitted, and a blit cannot repeat.
inline bool positionModeShowsVideo(PositionMode mode) { return mode != PositionMode::Tile; }

// Whether a wallpaper of `type`, a package's type name, may be laid out in `mode`.
inline bool positionModeAllowed(std::string_view type, PositionMode mode) {
    return type == "static" || positionModeShowsVideo(mode);
}

// The property keys. "position" is a string naming the mode; the other three are numbers read only
// in Free mode, and are kept whatever the mode, so switching away and back restores the framing.
inline constexpr std::string_view kPositionKey = "position";
inline constexpr std::string_view kPositionZoomKey = "position.zoom";
inline constexpr std::string_view kPositionXKey = "position.x";
inline constexpr std::string_view kPositionYKey = "position.y";

// Free mode, in units that survive a change of screen size. Zoom is relative to Cover: 1 is the
// size Cover draws, 0.5 half that. The offset moves the picture's centre from the screen's centre
// by a fraction of the screen's own width (x, rightwards) and height (y, downwards). So 1, 0, 0
// frames the picture as Cover does. A still image moves by fractions of a texel; a video is blitted,
// and a blit cannot start mid-texel, so at high zoom it moves in steps of one source texel.
inline constexpr double kMinPositionZoom = 0.1;
inline constexpr double kMaxPositionZoom = 10.0;
inline constexpr double kMaxPositionOffset = 10.0;

// False for anything not a finite number in range, NaN included.
inline bool positionZoomAllowed(double zoom) { return zoom >= kMinPositionZoom && zoom <= kMaxPositionZoom; }
inline bool positionOffsetAllowed(double offset) { return std::fabs(offset) <= kMaxPositionOffset; }

// What the four properties say, absent ones at their defaults.
struct Placement {
    PositionMode mode = PositionMode::Cover;
    double zoom = 1.0;
    double x = 0.0;
    double y = 0.0;

    bool operator==(const Placement&) const = default;
};

inline bool placementAllowed(const Placement& placement) {
    return positionZoomAllowed(placement.zoom) && positionOffsetAllowed(placement.x) &&
           positionOffsetAllowed(placement.y);
}

// kPositionModes' names for a sentence, the last joined by `last`: "cover, fit, ... or free".
inline std::string positionModesText(std::string_view last) {
    std::string text;
    constexpr std::size_t count = std::size(kPositionModes);
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) text += i + 1 < count ? std::string(", ") : " " + std::string(last) + " ";
        text += toString(kPositionModes[i]);
    }
    return text;
}

}  // namespace wallpaperx
