// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaper_properties.h"

#include "log_escape.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wallpaperx {
namespace {

constexpr const char* kSpeed = "speed";
constexpr const char* kMute = "mute";
constexpr const char* kVolume = "volume";

std::optional<std::uint8_t> hexByte(char high, char low) {
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const int h = digit(high);
    const int l = digit(low);
    if (h < 0 || l < 0) return std::nullopt;
    return static_cast<std::uint8_t>(h * 16 + l);
}

bool keyCharacter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.';
}

std::string propertyError(const std::string& key, const PropertyValue& value) {
    if (key.empty() || key.size() > kMaxPropertyKeyBytes || !std::all_of(key.begin(), key.end(), keyCharacter)) {
        return "property name " + sanitizeForLog(key, 128) + " must be 1 to " + std::to_string(kMaxPropertyKeyBytes) +
               " ASCII letters, digits, '_', '-' or '.'";
    }
    const double* number = std::get_if<double>(&value);
    if (number != nullptr && !std::isfinite(*number)) return "property " + key + " is not a finite number";
    if (key == kSpeed && (number == nullptr || *number <= 0 || *number > kMaxPlaybackSpeed)) {
        static_assert(kMaxPlaybackSpeed == 2.0, "the message below names the bound");
        return "property speed must be a number above 0 and at most 2";
    }
    if (key == kMute && !std::holds_alternative<bool>(value)) return "property mute must be a boolean";
    if (key == kVolume && (number == nullptr || *number < 0 || *number > 1)) {
        return "property volume must be a number from 0 to 1";
    }
    return {};
}

nlohmann::json toJson(const PropertyValue& value) {
    if (const auto* colour = std::get_if<Colour>(&value)) return toString(*colour);
    if (const auto* flag = std::get_if<bool>(&value)) return *flag;
    return std::get<double>(value);
}

}  // namespace

std::optional<Colour> parseColour(std::string_view text) {
    if (text.size() != 7 || text[0] != '#') return std::nullopt;
    const auto red = hexByte(text[1], text[2]);
    const auto green = hexByte(text[3], text[4]);
    const auto blue = hexByte(text[5], text[6]);
    if (!red || !green || !blue) return std::nullopt;
    return Colour{*red, *green, *blue};
}

std::string toString(Colour colour) {
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", colour.red, colour.green, colour.blue);
    return text;
}

std::string propertiesError(const WallpaperProperties& properties) {
    if (properties.size() > kMaxProperties) {
        return "at most " + std::to_string(kMaxProperties) + " properties, got " + std::to_string(properties.size());
    }
    for (const auto& [key, value] : properties) {
        if (std::string why = propertyError(key, value); !why.empty()) return why;
    }
    return {};
}

WallpaperProperties usableProperties(const WallpaperProperties& properties, int& dropped, int& clampedVolumes) {
    WallpaperProperties usable;
    for (const auto& [key, read] : properties) {
        PropertyValue value = read;
        double* volume = key == kVolume ? std::get_if<double>(&value) : nullptr;
        const bool clamped = volume != nullptr && std::isfinite(*volume) && (*volume < 0 || *volume > 1);
        if (clamped) *volume = std::clamp(*volume, 0.0, 1.0);
        if (usable.size() < kMaxProperties && propertyError(key, value).empty()) {
            usable.emplace(key, std::move(value));
            if (clamped) ++clampedVolumes;
        } else {
            ++dropped;
        }
    }
    return usable;
}

double playbackSpeed(const WallpaperProperties& properties) {
    const auto it = properties.find(kSpeed);
    if (it == properties.end()) return 1.0;
    const double* speed = std::get_if<double>(&it->second);
    if (speed == nullptr || !std::isfinite(*speed)) return 1.0;
    return std::clamp(*speed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
}

bool wallpaperMuted(const WallpaperProperties& properties) {
    const auto it = properties.find(kMute);
    const bool* mute = it == properties.end() ? nullptr : std::get_if<bool>(&it->second);
    return mute != nullptr && *mute;
}

nlohmann::json toJson(const WallpaperProperties& properties) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, value] : properties) object[key] = toJson(value);
    return object;
}

// Not toJson(properties).dump(): destroying a non-empty nlohmann object allocates inside its
// noexcept destructor, and this runs on every manager call that saves, where an allocation
// failure must stay a std::bad_alloc rather than std::terminate. Scalars allocate nothing when
// destroyed, so each key and value is still dumped by nlohmann, in the map order dump() uses.
std::string toJsonText(const WallpaperProperties& properties) {
    if (properties.empty()) return {};
    std::string text = "{";
    for (const auto& [key, value] : properties) {
        if (text.size() > 1) text += ',';
        text += nlohmann::json(key).dump();
        text += ':';
        text += toJson(value).dump();
    }
    text += '}';
    return text;
}

WallpaperProperties propertiesFromJson(const std::string& text, int& dropped) {
    WallpaperProperties properties;
    if (text.empty()) return properties;
    const nlohmann::json field = nlohmann::json::parse(text, nullptr, false);
    if (!field.is_object()) {
        ++dropped;
        return properties;
    }
    for (const auto& [key, value] : field.items()) {
        if (value.is_number()) {
            properties.emplace(key, value.get<double>());
        } else if (value.is_boolean()) {
            properties.emplace(key, value.get<bool>());
        } else if (const auto colour = value.is_string() ? parseColour(value.get<std::string>()) : std::nullopt) {
            properties.emplace(key, *colour);
        } else {
            ++dropped;
        }
    }
    return properties;
}

}  // namespace wallpaperx
