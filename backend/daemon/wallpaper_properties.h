// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json_fwd.hpp>

namespace wallpaperx {

// A wallpaper.json "color": "#rrggbb", no alpha.
struct Colour {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    bool operator==(const Colour&) const = default;
};

// "#rrggbb" in either case, or nothing.
std::optional<Colour> parseColour(std::string_view text);
// Lower case, the form the app stores.
std::string toString(Colour colour);

// wallpaper.json's three property types: float, bool, color.
using PropertyValue = std::variant<double, bool, Colour>;

// A wallpaper's edited property values by key, as a client set them. The daemon does not read
// wallpaper.json, so a value is checked for what it is and not against the range its wallpaper
// declares: that is the app's to enforce, and nothing here depends on it.
using WallpaperProperties = std::map<std::string, PropertyValue>;

// Bounds on what a client can make the daemon store and write into every save: 32 monitors at
// these limits stay far under the config file's 1 MiB read cap, past which the whole file is
// refused at the next start.
constexpr std::size_t kMaxProperties = 64;
constexpr std::size_t kMaxPropertyKeyBytes = 64;

// The range a video is actually played at. A speed above the top is refused; a positive one below
// the bottom is stored as set and played at the bottom, where the wait for one picture would
// otherwise grow without bound.
constexpr double kMinPlaybackSpeed = 0.01;
constexpr double kMaxPlaybackSpeed = 2.0;

// Why `properties` cannot be stored, or empty when it can. Keys are 1 to kMaxPropertyKeyBytes of
// ASCII letters, digits, '_', '-' and '.', so they are safe to log and publish unescaped; numbers
// are finite; "speed" is above 0 and at most kMaxPlaybackSpeed and "mute" a bool, the two the
// daemon applies.
std::string propertiesError(const WallpaperProperties& properties);

// `properties` without the entries propertiesError() would refuse, and without those past
// kMaxProperties, adding how many were left out to `dropped`. For a config file, where one bad
// value must not cost the wallpaper it rides on.
WallpaperProperties usableProperties(const WallpaperProperties& properties, int& dropped);

// What "speed" says a video plays at: 1 without one, else within the playback range.
double playbackSpeed(const WallpaperProperties& properties);

// An object of numbers, booleans and "#rrggbb" strings: the form both the config file and
// GetStatus carry.
nlohmann::json toJson(const WallpaperProperties& properties);

// toJson() as text, the form a config row carries; empty when there are no properties.
std::string toJsonText(const WallpaperProperties& properties);

// The reverse: each value by its JSON type, adding to `dropped` one for each value none of the three
// can hold and one for text that is not an object. Types only; usableProperties() applies the rules.
WallpaperProperties propertiesFromJson(const std::string& text, int& dropped);

}  // namespace wallpaperx
