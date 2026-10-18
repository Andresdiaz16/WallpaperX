// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "playback_speed.h"

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

// Why `properties` cannot be stored, or empty when it can. Keys are 1 to kMaxPropertyKeyBytes of
// ASCII letters, digits, '_', '-' and '.', so they are safe to log and publish unescaped; numbers
// are finite; "speed" is one of kPlaybackSpeeds, "mute" a bool and "volume" a number from 0 to 1,
// the keys the daemon gives a meaning.
std::string propertiesError(const WallpaperProperties& properties);

// `properties` without the entries propertiesError() would refuse, and without those past
// kMaxProperties -- "speed", "mute" and "volume" counted first -- adding how many were left out to `dropped`. For a config file, where one bad
// value must not cost the wallpaper it rides on. Two are moved instead, each counted: a numeric
// "volume" outside 0 to 1 to the nearer end, in `clampedVolumes`, since a build before "volume" was
// reserved stored whatever range the wallpaper declared; and a positive "speed" that is not one of
// kPlaybackSpeeds to the nearest of all of them, in `snappedSpeeds`, since a config file may hold any
// speed the wallpaper declared. Not the nearest the wallpaper offers: the daemon does not know its range.
WallpaperProperties usableProperties(const WallpaperProperties& properties, int& dropped, int& clampedVolumes,
                                     int& snappedSpeeds);

// What "speed" says a video plays at: 1 without one, else one of kPlaybackSpeeds.
double playbackSpeed(const WallpaperProperties& properties);

// The wallpaper's own sound level: "mute" switches it off, unset means on.
bool wallpaperMuted(const WallpaperProperties& properties);

// What "volume" says the sound plays at, 0 to 1: 1 without one.
float wallpaperVolume(const WallpaperProperties& properties);

// An object of numbers, booleans and "#rrggbb" strings: the form both the config file and
// GetStatus carry.
nlohmann::json toJson(const WallpaperProperties& properties);

// toJson() as text, the form a config row carries; empty when there are no properties.
std::string toJsonText(const WallpaperProperties& properties);

// The reverse: each value by its JSON type, adding to `dropped` one for each value none of the three
// can hold and one for text that is not an object. Types only; usableProperties() applies the rules.
WallpaperProperties propertiesFromJson(const std::string& text, int& dropped);

}  // namespace wallpaperx
