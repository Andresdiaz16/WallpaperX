// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <string_view>

namespace wallpaperx {

// Only A-Z is folded: the files compared this way are not in any one encoding.
inline bool equalsIgnoringAsciiCase(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

} // namespace wallpaperx
