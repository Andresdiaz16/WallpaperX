// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wallpaperx {

// When a monitor's video stops on the picture it shows. A setting of the screen, not of the
// wallpaper on it: it outlives every SetWallpaper.
enum class PauseMode { Play, Always, WhileWindowFocused, WhileWindowMaximized };

// Every mode: a new one goes here as well as in toString()'s switch.
inline constexpr PauseMode kPauseModes[] = {PauseMode::Play, PauseMode::Always, PauseMode::WhileWindowFocused,
                                            PauseMode::WhileWindowMaximized};

// The names on the wire, in config.json and in GetStatus.
inline const char* toString(PauseMode mode) {
    switch (mode) {
        case PauseMode::Play: return "play";
        case PauseMode::Always: return "always_paused";
        case PauseMode::WhileWindowFocused: return "paused_while_focused";
        case PauseMode::WhileWindowMaximized: return "paused_while_maximized";
    }
    return "play";
}

inline std::optional<PauseMode> pauseModeFromName(std::string_view name) {
    for (const PauseMode mode : kPauseModes) {
        if (name == toString(mode)) return mode;
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
