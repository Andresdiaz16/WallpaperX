// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wallpaperx {

// A sound on/off setting -- the config root's or a monitor's -- as config.json carries it: JSON
// text, empty when the field is absent. Absent means on, so a config that never switched sound off
// is byte-identical to what a build without these fields writes.
inline std::string soundJsonText(bool on) { return on ? std::string() : std::string("false"); }

// Nothing for a value this build does not know, which a newer build may have written: it is kept
// as read and sound stays on.
inline std::optional<bool> soundFromJson(std::string_view text) {
    if (text.empty() || text == "true") return true;
    if (text == "false") return false;
    return std::nullopt;
}

inline bool soundOn(std::string_view text) { return soundFromJson(text).value_or(true); }

}  // namespace wallpaperx
