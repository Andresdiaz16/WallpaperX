// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// For the readers in shared/ only: it brings in nlohmann, which their public headers do not.

#include <nlohmann/json.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace wallpaperx {

// Json::parse does not recurse, but an ordered_json object copies its members when it grows, and
// copying a value recurses once per level, so a deeper document is refused before it is parsed.
inline constexpr int kMaxJsonDepth = 64;

// `json` parsed, any value at its top. The error is a sentence starting with `fileName`.
std::expected<nlohmann::ordered_json, std::string> parseJsonDocument(std::string_view json, std::string_view fileName);

} // namespace wallpaperx
