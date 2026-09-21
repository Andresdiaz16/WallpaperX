// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace wallpaperx {

// Opens a wallpaper's own file for reading without ever parking the calling thread in the kernel,
// which is what a named pipe with no writer, a device or a socket do to an ordinary open -- and
// every renderer here runs on the daemon's one GPU thread, so one such path stops every monitor.
// O_NONBLOCK covers the open and the descriptor is then what says what was opened: only a regular
// file is accepted, and reading through the descriptor rather than opening the path a second time
// leaves no moment for the path to become something else.
//
// A path validated when the wallpaper was chosen is not enough on its own: the file is opened again
// on every render, when a stopped video's session is reopened, and when a screen reports a new size,
// and the path may name something different by then.
//
// Returns -1 with errno set, as open(2) does, so a caller can phrase that failure in its own words.
// Throws WallpaperContentError, worded by describeRejection(), for a file that is not a regular
// one. The descriptor is the caller's to close.
int openWallpaperFile(const std::string& file, const char* component);

// How a rejection naming a wallpaper file is worded: `component: reason: path`. The path goes last
// because the daemon keeps only the head of a recorded error, and a long path ahead of the reason
// would be all that survived. Build every such message here rather than by hand.
std::string describeRejection(const char* component, const std::string& reason, const std::string& path);

}  // namespace wallpaperx
