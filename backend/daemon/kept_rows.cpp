// SPDX-License-Identifier: GPL-2.0-or-later

#include "kept_rows.h"

#include <set>
#include <utility>

namespace wallpaperx {

bool LiveWallpapers::holds(int monitorId) const {
    // Strict rather than "the id is present": nothing populates a live monitor without a
    // wallpaper today, and if something ever does, this keeps that monitor's carried row in
    // the file rather than dropping it.
    for (const PersistedWallpaper& row : rows) {
        if (row.monitorId != monitorId) continue;
        return !row.type.empty() || !row.path.empty();
    }
    return false;
}

bool KeptRows::dropWallpaper(PersistedWallpaper& row) {
    row.propertiesJson.clear();
    if (row.type.empty() && row.path.empty()) return false;
    row.type.clear();
    row.path.clear();
    return true;
}

void KeptRows::note(const std::string& text) {
    if (!m_persistenceNote.empty()) m_persistenceNote += "; ";
    m_persistenceNote += text;
}

std::vector<PersistedWallpaper> KeptRows::rowsToWrite(const LiveWallpapers& live) const {
    // Keyed by monitor id, not appended: a wallpaper and a name for one monitor belong on
    // one row, and two rows would be two monitors claiming one screen on the next load.
    std::map<int, PersistedWallpaper> rows;
    for (const PersistedWallpaper& row : live.rows) {
        if (row.type.empty() || row.path.empty()) continue;
        rows.emplace(row.monitorId, row);
    }

    // Entries this build could not apply are still the user's. Live state wins per monitor.
    for (const PersistedWallpaper& entry : m_carried) {
        if (!live.holds(entry.monitorId)) rows.emplace(entry.monitorId, entry);
    }

    // live.outputs is the authority: a carried name is written only for an id this build
    // cannot bind, and only while nothing else claims it. This runs on every save and cannot
    // report, so a name it wrote twice would be one screen lost in silence on the next load.
    std::set<std::string> written;
    for (auto& [monitorId, row] : rows) {
        if (row.output.empty()) continue;
        if (monitorId < m_maxMonitors || live.outputs.count(row.output) != 0 ||
            !written.insert(row.output).second) {
            row.output.clear();
            row.outputSizeJson.clear();
        }
    }
    for (const auto& [name, binding] : live.outputs) {
        PersistedWallpaper& row = rows[binding.monitorId];
        // Explicit: operator[] default-constructs monitorId 0 for a named, wallpaper-less
        // screen.
        row.monitorId = binding.monitorId;
        row.output = name;
        row.outputSizeJson = binding.size ? toJsonText(*binding.size) : std::string();
    }

    // The same authority rule as names: a carried row speaks for an id only where this build
    // cannot bind one.
    for (auto& [monitorId, row] : rows) {
        if (monitorId < m_maxMonitors) {
            row.pauseJson.clear();
            row.soundJson.clear();
        }
    }
    for (const auto& [monitorId, text] : live.pause) {
        PersistedWallpaper& row = rows[monitorId];
        row.monitorId = monitorId;
        row.pauseJson = text;
    }
    for (const auto& [monitorId, text] : live.sound) {
        PersistedWallpaper& row = rows[monitorId];
        row.monitorId = monitorId;
        row.soundJson = text;
    }

    std::vector<PersistedWallpaper> entries;
    entries.reserve(rows.size());
    for (auto& [monitorId, row] : rows) {
        // A row the clearing loop emptied has nothing left to say. Written out, the next
        // start reads it back and reports it as a wallpaper that did not come back.
        if (row.type.empty() && row.path.empty() && row.output.empty() && !row.holdsMonitorSettings()) continue;
        entries.push_back(std::move(row));
    }
    return entries;
}

std::string KeptRows::detail(const std::string& base, const LiveWallpapers& live) const {
    // Notes before the path: the recording site caps the detail and truncation drops the tail,
    // so a long config path must not be what silences them.
    std::string detail;
    if (!m_persistenceNote.empty()) detail = m_persistenceNote + "; ";

    // Two independent facts about one row, not a partition: this text is the user's notice
    // that a picture did not come back, and a row can have lost a name as well as one.
    int keptWallpapers = 0;
    int keptNames = 0;
    for (const PersistedWallpaper& entry : m_carried) {
        if ((!entry.type.empty() || !entry.path.empty()) && !live.holds(entry.monitorId)) {
            ++keptWallpapers;
        }
        if (!entry.output.empty() && live.outputs.count(entry.output) == 0) ++keptNames;
    }
    if (keptWallpapers > 0) {
        detail += std::to_string(keptWallpapers) + " entries this build cannot apply, kept; ";
    }
    if (keptNames > 0) {
        detail += std::to_string(keptNames) +
                  " screen names this build's monitor limit excludes, kept; ";
    }
    // Reported for the same reason the kept ones are: a wallpaper that did not come back is
    // otherwise invisible, and these are the ones no restart will bring back.
    if (m_droppedEntries > 0) {
        detail += std::to_string(m_droppedEntries) + " entries dropped, no build can apply them; ";
    }
    if (m_droppedNames > 0) {
        detail += std::to_string(m_droppedNames) + " screen names dropped, no build can apply them; ";
    }
    if (m_replacedKept > 0) {
        detail += std::to_string(m_replacedKept) +
                  " entries this build cannot apply, replaced by new wallpapers for those screens; ";
    }
    if (m_takenOverNames > 0) {
        detail += std::to_string(m_takenOverNames) +
                  " screen names this build cannot bind, taken over by new names; ";
    }
    return detail + base;
}

}
