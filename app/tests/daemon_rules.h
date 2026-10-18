// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Test-only: the daemon's own rules, compiled from backend/daemon, so an app check can be graded
// against what the daemon will actually accept rather than against a copy of it. Standard types
// only: the daemon's headers and the app's declare the same names inside namespace wallpaperx
// (kMaxProperties, kPauseModes), which cannot both be declared in one translation unit.

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace wallpaperx::apptest {

// A property value as SetWallpaperWithProperties carries it: double, bool, or "#rrggbb" or a
// position mode's name.
using DaemonValue = std::variant<double, bool, std::string>;

// backend/daemon/wallpaper_properties.cpp's propertiesError(): empty when the daemon would store
// `properties`, otherwise its reason. A string that is neither comes back as a reason too.
std::string daemonPropertiesError(const std::vector<std::pair<std::string, DaemonValue>>& properties);
// The same, then positionError() for a wallpaper of `type` ("static", "video", ...): empty when the
// daemon would show a wallpaper of that type with `properties`.
std::string daemonShowError(const std::vector<std::pair<std::string, DaemonValue>>& properties, const std::string& type);

// backend/daemon/pause_mode.h: the names SetMonitorPause takes, in the daemon's order.
std::vector<std::string> daemonPauseModeNames();

} // namespace wallpaperx::apptest
