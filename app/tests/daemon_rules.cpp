// SPDX-License-Identifier: GPL-2.0-or-later

#include "daemon_rules.h"

#include "daemon/pause_mode.h"
#include "daemon/wallpaper_properties.h"

#include <algorithm>

namespace wallpaperx::apptest {

namespace {

// Empty `why` when every value converted.
WallpaperProperties toDaemon(const std::vector<std::pair<std::string, DaemonValue>>& properties, std::string& why) {
    WallpaperProperties converted;
    for (const auto& [key, value] : properties) {
        if (const auto* text = std::get_if<std::string>(&value)) {
            const auto parsed = propertyFromText(*text);
            if (!parsed) {
                why = "property " + key + " is neither a colour nor a position mode";
                return converted;
            }
            converted.insert_or_assign(key, *parsed);
        } else if (const auto* number = std::get_if<double>(&value)) {
            converted.insert_or_assign(key, *number);
        } else {
            converted.insert_or_assign(key, std::get<bool>(value));
        }
    }
    return converted;
}

} // namespace

std::string daemonPropertiesError(const std::vector<std::pair<std::string, DaemonValue>>& properties) {
    std::string why;
    const WallpaperProperties converted = toDaemon(properties, why);
    // A duplicate key would be one entry on the wire as well; the count is what the daemon sees.
    return why.empty() ? propertiesError(converted) : why;
}

std::string daemonShowError(const std::vector<std::pair<std::string, DaemonValue>>& properties, const std::string& type) {
    std::string why;
    const WallpaperProperties converted = toDaemon(properties, why);
    if (why.empty()) {
        why = propertiesError(converted);
    }
    return why.empty() ? positionError(converted, type) : why;
}

// Each mode through toString(), the function the daemon puts on the wire. toString() reads the same
// kPauseModes rows, so this guards nothing against the table itself reading wrong in this program:
// pause_mode_link_test is what catches that.
std::vector<std::string> daemonPauseModeNames() {
    std::vector<std::string> names;
    for (const PauseModeNames& row : kPauseModes) {
        names.emplace_back(toString(row.mode));
    }
    return names;
}

} // namespace wallpaperx::apptest
