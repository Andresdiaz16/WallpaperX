// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What the daemon stores of a wallpaper's properties: its bounds on SetWallpaperWithProperties, and a
// colour's form. One definition for the daemon and for what reads wallpaper.json (the app and wpxctl),
// so a wallpaper the daemon would refuse is refused when it is read rather than when it is set.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace wallpaperx {

// Bounds on what a client can make the daemon store and write into every save: 32 monitors at
// these limits stay far under the config file's 1 MiB read cap, past which the whole file is
// refused at the next start.
inline constexpr std::size_t kMaxProperties = 65;
inline constexpr std::size_t kMaxPropertyKeyBytes = 64;

// 1 to kMaxPropertyKeyBytes of ASCII letters, digits, '_', '-' and '.': safe to log and publish
// unescaped.
inline bool isPropertyKey(std::string_view key) {
    return !key.empty() && key.size() <= kMaxPropertyKeyBytes && std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.';
    });
}

// A wallpaper.json "color": "#rrggbb", no alpha.
struct Colour {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    bool operator==(const Colour&) const = default;
};

// "#rrggbb" in either case, or nothing.
inline std::optional<Colour> parseColour(std::string_view text) {
    const auto byte = [&text](std::size_t at) -> std::optional<std::uint8_t> {
        const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int high = digit(text[at]);
        const int low = digit(text[at + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        return static_cast<std::uint8_t>(high * 16 + low);
    };
    if (text.size() != 7 || text[0] != '#') return std::nullopt;
    const auto red = byte(1);
    const auto green = byte(3);
    const auto blue = byte(5);
    if (!red || !green || !blue) return std::nullopt;
    return Colour{*red, *green, *blue};
}

// Lower case, the form the app stores.
inline std::string toString(Colour colour) {
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", colour.red, colour.green, colour.blue);
    return text;
}

}  // namespace wallpaperx
