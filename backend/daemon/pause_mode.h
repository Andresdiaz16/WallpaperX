// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace wallpaperx {

// When a monitor's video stops on the picture it shows. A setting of the screen, not of the
// wallpaper on it: it outlives every SetWallpaper.
enum class PauseMode { Play, Always, WhileWindowFocused, WhileWindowMaximized };

struct PauseModeNames {
    PauseMode mode;
    const char* name;        // on the wire, in config.json and in GetStatus
    const char* shortName;   // what wpxctl takes and shows
    const char* description; // what wpxctl pause --help says of it
};

// Every mode, row i holding enum value i. A new mode also needs its case in isPauseMode() below,
// WallpaperManager's pause decision, and its name in the app's own list (app/daemon/daemon_client.h).
inline constexpr PauseModeNames kPauseModes[] = {
    {PauseMode::Play, "play", "play", "the video always plays"},
    {PauseMode::Always, "always_paused", "always", "the video is always paused"},
    {PauseMode::WhileWindowFocused, "paused_while_focused", "focused",
     "paused while a window has the focus on that screen"},
    {PauseMode::WhileWindowMaximized, "paused_while_maximized", "maximized",
     "paused while a window on that screen is maximized or fullscreen"},
};

// Whether `mode` is one of the enum's values. The switch names every one, and the diagnostic made an
// error refuses to build it while one is missing: that is what ties the table's length to the enum.
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
constexpr bool isPauseMode(PauseMode mode) {
    switch (mode) {
        case PauseMode::Play:
        case PauseMode::Always:
        case PauseMode::WhileWindowFocused:
        case PauseMode::WhileWindowMaximized:
            return true;
    }
    return false;
}
#pragma GCC diagnostic pop

static_assert([] {
    for (std::size_t i = 0; i < std::size(kPauseModes); ++i) {
        if (kPauseModes[i].mode != static_cast<PauseMode>(i) || !isPauseMode(kPauseModes[i].mode)) return false;
    }
    return !isPauseMode(static_cast<PauseMode>(std::size(kPauseModes)));
}(), "kPauseModes needs one row per PauseMode, in the enum's order");

// `mode` must be one of the enum's values: anything but pauseModeFrom*() gives none other.
inline const PauseModeNames& namesOf(PauseMode mode) { return kPauseModes[static_cast<std::size_t>(mode)]; }

inline const char* toString(PauseMode mode) { return namesOf(mode).name; }
inline const char* toShortName(PauseMode mode) { return namesOf(mode).shortName; }

// One column of the table as a sentence's list: "a, b, c or d".
inline std::string pauseModesText(const char* PauseModeNames::*column) {
    std::string text;
    for (std::size_t i = 0; i < std::size(kPauseModes); ++i) {
        if (i > 0) text += i + 1 < std::size(kPauseModes) ? ", " : " or ";
        text += kPauseModes[i].*column;
    }
    return text;
}

inline std::optional<PauseMode> pauseModeFromName(std::string_view name) {
    for (const PauseModeNames& row : kPauseModes) {
        if (name == row.name) return row.mode;
    }
    return std::nullopt;
}

inline std::optional<PauseMode> pauseModeFromShortName(std::string_view name) {
    for (const PauseModeNames& row : kPauseModes) {
        if (name == row.shortName) return row.mode;
    }
    return std::nullopt;
}

// The `pause` field of a config row as JSON text. Every name is plain ASCII, so this is exactly
// what nlohmann's dump() produces for it -- which is what lets pauseModeFromJson() compare text
// rather than parse it.
inline std::string toJsonText(PauseMode mode) { return std::string("\"") + toString(mode) + "\""; }

// Nothing for a value this build does not know, which a newer build may have written: the row
// carries it as read and the monitor plays.
inline std::optional<PauseMode> pauseModeFromJson(std::string_view text) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') return std::nullopt;
    return pauseModeFromName(text.substr(1, text.size() - 2));
}

}  // namespace wallpaperx
