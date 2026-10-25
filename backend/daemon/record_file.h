// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>

#include <sys/types.h>

namespace wallpaperx {

// The whole of a record this daemon could have written, or nothing: a file larger than `maxBytes`,
// not regular, or a symlink is not one of ours. Never blocks on a FIFO.
std::string readRecordFile(const std::filesystem::path& file, off_t maxBytes);

// Writes all of `text`, retrying an interrupted or short write. False on any other failure.
bool writeAll(int fd, const std::string& text);

}
