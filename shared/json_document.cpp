// SPDX-License-Identifier: GPL-2.0-or-later

#include "json_document.h"

#include <format>

namespace wallpaperx {

namespace {

using Json = nlohmann::ordered_json;

// Whether `json` opens at most `limit` arrays and objects inside one another, read without recursing.
// Brackets inside strings are skipped, so up to the first byte the parser refuses, this is the depth
// it would build.
bool nestsWithin(std::string_view json, int limit) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : json) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > limit) return false;
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return true;
}

} // namespace

std::expected<Json, std::string> parseJsonDocument(std::string_view json, std::string_view fileName) {
    if (!nestsWithin(json, kMaxJsonDepth)) {
        return std::unexpected(std::format("{} is nested more than {} levels deep.", fileName, kMaxJsonDepth));
    }
    try {
        return Json::parse(json.begin(), json.end());
    } catch (const Json::parse_error& e) {
        return std::unexpected(std::format("{} is not valid JSON (at byte {}).", fileName, e.byte));
    } catch (const Json::out_of_range&) {
        // How the parser reports a number literal too large for a double, such as 1e999.
        return std::unexpected(std::format("{} holds a number too large to read.", fileName));
    } catch (const Json::exception& e) {
        return std::unexpected(std::format("{} cannot be read: {}.", fileName, e.what()));
    }
}

} // namespace wallpaperx
