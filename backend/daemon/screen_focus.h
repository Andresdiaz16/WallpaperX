// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>

namespace wallpaperx {

// What holds the keyboard focus on a screen, as its plugin reports it. Desktop is the screen's own
// desktop window -- the one its wallpaper is drawn in -- holding it, which the compositor arranges
// when the user clicks the desktop, shows the desktop, or closes the last window there.
enum class ScreenFocus { None, Window, Desktop };

inline constexpr ScreenFocus kScreenFocuses[] = {ScreenFocus::None, ScreenFocus::Window, ScreenFocus::Desktop};

// The names on the wire and in GetStatus.
inline const char* toString(ScreenFocus focus) {
    switch (focus) {
        case ScreenFocus::None: return "none";
        case ScreenFocus::Window: return "window";
        case ScreenFocus::Desktop: return "desktop";
    }
    return "none";
}

inline std::optional<ScreenFocus> screenFocusFromName(std::string_view name) {
    for (const ScreenFocus focus : kScreenFocuses) {
        if (name == toString(focus)) return focus;
    }
    return std::nullopt;
}

}  // namespace wallpaperx
