// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Whether this process is on the private session bus isolated_run.sh starts, for the Qt-side plugin
// tests that own the daemon's name on it. No bus library: the answer comes from the environment the
// wrapper sets up, so a test can ask before it has touched any bus.

#include "tests/test_report.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include <unistd.h>

namespace wallpaperx::testing {

// Whether the session bus this process was given is the private one isolated_run.sh starts, and so
// one a test may watch or own names on; why not, when it is not. isolated_run.sh exports the caller's
// own bus address and network namespace before it starts the private bus, so a process it did not
// start has neither, and one it did start sees its private bus differ from the caller's.
inline bool onPrivateTestBus(std::string& why) {
    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    const char* callerNet = std::getenv("WALLPAPERX_CALLER_NET_NS");
    const char* callerBus = std::getenv("WALLPAPERX_CALLER_DBUS_SESSION_BUS_ADDRESS");
    if (address == nullptr || *address == '\0') {
        why = "DBUS_SESSION_BUS_ADDRESS is not set";
        return false;
    }
    if (callerNet == nullptr || *callerNet == '\0') {
        why = "this process was not started by isolated_run.sh, so its session bus may be the live one";
        return false;
    }
    if (callerBus != nullptr && std::strcmp(callerBus, address) == 0) {
        why = "the session bus is the one the caller had";
        return false;
    }
    const std::string live = "unix:path=/run/user/" + std::to_string(::getuid()) + "/bus";
    if (std::strncmp(address, live.c_str(), live.size()) == 0) {
        why = "the session bus is the user's live one (" + live + ")";
        return false;
    }
    return true;
}

// For a plugin test's main(), before it owns a name or sends anything: off the private bus, a harness
// fault and the status to exit with; nothing when the bus is the private one.
inline std::optional<int> refuseOffPrivateTestBus() {
    std::string why;
    if (onPrivateTestBus(why)) return std::nullopt;
    harnessFailure("this test runs only on the private bus isolated_run.sh starts: " + why);
    (void)reportHarnessFaults("in this test file, not in the plugin");
    return 1;
}

}  // namespace wallpaperx::testing
