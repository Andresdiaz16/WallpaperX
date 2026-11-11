// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Steam's own list of its library folders, steamapps/libraryfolders.vdf, in Steam's text key-value
// format. Standard library only.

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wallpaperx::steam {

inline constexpr const char* kLibraryListName = "libraryfolders.vdf";
inline constexpr std::size_t kMaxLibraryListBytes = 1 << 20;

// One key and either a text value or a block of further keys, in the file's order. Keys repeat.
struct KeyValue {
    std::string key;
    std::variant<std::string, std::vector<KeyValue>> value;
};

// Keys and values are double-quoted strings (\\, \", \n and \t escaped) or bare words; a block is
// braces; // starts a comment running to the end of the line. The error is a sentence.
std::expected<std::vector<KeyValue>, std::string> parseKeyValues(std::string_view text);

// The "path" of each block inside the top-level "libraryfolders" block, and each path a numbered key
// there names directly (Steam's older layout), in the file's order, keys compared without case.
// Paths that are not absolute are left out.
std::expected<std::vector<std::string>, std::string> libraryFolders(std::string_view vdf);

} // namespace wallpaperx::steam
