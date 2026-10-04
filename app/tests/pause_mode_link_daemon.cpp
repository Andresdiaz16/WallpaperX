// SPDX-License-Identifier: GPL-2.0-or-later

// Test-only: backend/daemon/pause_mode.h's kPauseModes as the program this is linked into reads it,
// for pause_mode_link_test.cpp. A file of its own because the daemon's header and the app's
// daemon_client.h declare the same names and cannot meet in one translation unit.

#include "daemon/pause_mode.h"

#include <string>
#include <vector>

std::vector<std::string> daemonArrayNames() {
    const wallpaperx::PauseMode* modes = wallpaperx::kPauseModes;
    // Read from memory, through the symbol the linker kept: a compiler that folded the constant
    // array would show this file's own definition whatever the program holds.
    asm volatile("" : "+r"(modes));
    std::vector<std::string> names;
    for (int i = 0; i < 4; ++i) {
        names.emplace_back(wallpaperx::toString(modes[i]));
    }
    return names;
}
