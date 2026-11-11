// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string_view>

namespace wallpaperx::manifest {

// Refuses what could reach outside a folder: absolute, empty parts, ".", "..", backslashes, NUL.
inline bool isPlainRelativePath(std::string_view name) {
    if (name.empty() || name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    for (std::size_t from = 0;;) {
        const std::size_t slash = name.find('/', from);
        const std::string_view part = name.substr(from, slash == std::string_view::npos ? slash : slash - from);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string_view::npos) return true;
        from = slash + 1;
    }
}

} // namespace wallpaperx::manifest
