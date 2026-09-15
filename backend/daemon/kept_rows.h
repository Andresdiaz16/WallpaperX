// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "config_store.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wallpaperx {

// A bound connector name as the file records it: the monitor it answers to, and the size its
// screen last reported.
struct OutputBinding {
    int monitorId = 0;
    std::optional<OutputSize> size;
};

// What the persistence side needs to know about live monitor state, and all it needs. Passed
// as a value rather than read from the manager's fields: which rows the file will hold is
// decided here, and what each monitor is showing is decided there.
struct LiveWallpapers {
    // One per monitor actually holding a wallpaper.
    std::vector<PersistedWallpaper> rows;

    // Connector name -> binding, and the only authority on which is which.
    std::map<std::string, OutputBinding> outputs;

    // Whether a live monitor holds a wallpaper for this id, which is what decides that a kept
    // row for the same id will not survive the next save.
    bool holds(int monitorId) const;
};

// The rows this run could not apply, and the tally of everything the config lost along the
// way. Two questions live here because they are one question asked twice: what will the file
// hold after the next save, and what is the user told about the difference.
//
// Holds no lock. The manager calls it under its own, which is also what makes the live state
// it is handed a consistent snapshot.
class KeptRows {
public:
    // `maxMonitors` is this build's limit: an id at or above it cannot be bound here, which is
    // the only reason a row is carried rather than applied.
    explicit KeptRows(int maxMonitors) : m_maxMonitors(maxMonitors) {}

    // Rows a restore could not apply, carried so a save does not delete them.
    std::vector<PersistedWallpaper>& carried() { return m_carried; }
    const std::vector<PersistedWallpaper>& carried() const { return m_carried; }

    // Clears the wallpaper half of a kept row, answering whether there was one to clear -- the
    // caller reports the loss, and only when there was one. Takes the row, not an id: one
    // caller holds a copy not yet carried. Says nothing about `output`: a lost name is a
    // different loss, counted separately.
    static bool dropWallpaper(PersistedWallpaper& row);

    // Losses no restart brings back, counted so they are reported at all. Name-only rows
    // counted apart: this report is the user's notice that a picture did not come back, and a
    // name never was one.
    void countDroppedEntry() { ++m_droppedEntries; }
    void countDroppedName() { ++m_droppedNames; }

    // The two a client can cause repeatedly. Counted rather than written into the note, which
    // is appended to for the life of the process while the detail is truncated at its tail --
    // so a run of them would crowd out everything after.
    void countReplacedKept() { ++m_replacedKept; }
    void countTakenOverName() { ++m_takenOverNames; }

    // Whether anything was lost for good, which is what decides the report is worth rebuilding.
    bool anyDropped() const { return m_droppedEntries > 0 || m_droppedNames > 0; }

    // Facts about the config that outlive the operation that discovered them. Every detail is
    // rebuilt through these, or the next save would report only itself and drop the pointer to
    // the preserved copy.
    void note(const std::string& text);

    // The rows the next save will write, merging what is live with what is carried. Live state
    // wins per monitor: without that, a save rebuilt from live state alone would delete a
    // wallpaper a newer build set the moment anything else is set here.
    std::vector<PersistedWallpaper> rowsToWrite(const LiveWallpapers& live) const;

    // The report the user reads, ahead of `base`. Counts only what the next save will really
    // write, which is why it needs live state: a client can take over a carried row's id or
    // claim its name later, and rowsToWrite() then drops the carried copy -- so counting it
    // here would go on offering back a wallpaper, or a name, the file no longer holds.
    std::string detail(const std::string& base, const LiveWallpapers& live) const;

private:
    const int m_maxMonitors;

    std::vector<PersistedWallpaper> m_carried;

    // Raw; sanitized once where it is recorded.
    std::string m_persistenceNote;

    int m_droppedEntries = 0;
    int m_droppedNames = 0;
    int m_replacedKept = 0;
    int m_takenOverNames = 0;
};

}
