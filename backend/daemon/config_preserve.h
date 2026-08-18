// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>

namespace wallpaperx {

// Where the bytes are and what went wrong, kept apart: a failed removal of the original is a
// complaint about the original, not about the preservation, and one value for both let the
// second erase the first.
struct PreservedCopy {
    std::string kept;  // empty: nothing was preserved
    std::string why;   // every directory tried, or what still needs saying about a copy made
};

// Keeps a copy of a config file this run could not use, so a rewrite that follows does not
// destroy rows held nowhere else. `move` removes the original once the bytes are safe.
//
// Preferred location is $XDG_STATE_HOME/wallpaperx, else ~/.local/state/wallpaperx: out of the
// directory the user hand-edits, so tidying there cannot take the copy with the file it was
// made for. Falling back to the config's own directory, because a machine with no home offered
// nowhere at all, and a copy beside the file is worth more than none.
PreservedCopy preserveConfig(const std::filesystem::path& configFile, bool move);

}
