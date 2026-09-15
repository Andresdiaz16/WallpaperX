// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "daemon/wallpaper_manager.h"

#include <string>
#include <vector>

namespace wallpaperx::testing {

// WallpaperManager::setMonitorOutput, returning the monitors it says are owed a redraw, for the
// manager-level tests that name a screen only as setup.
inline std::vector<int> nameScreen(WallpaperManager& manager, int monitorId, const std::string& name) {
    std::vector<int> redraw;
    manager.setMonitorOutput(monitorId, name, redraw);
    return redraw;
}

}  // namespace wallpaperx::testing
