// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace wallpaperx {

// What the last run was applying to a monitor when it died. Read at startup, before the saved
// wallpapers are applied: a file that kills the daemon while it is being applied would otherwise
// be re-applied by the restart that follows, and kill it again.
struct CrashRecord {
    std::string type;
    std::string path;

    // The signal the applying thread died of. Empty when the process ended with no handler
    // running -- SIGKILL, the teardown alarm, a power cut -- which says only that it did not
    // survive applying this.
    std::optional<int> signal;
};

// <stateDir>/applying-<monitorId>.json. Empty when `stateDir` is.
std::filesystem::path crashRecordPath(const std::filesystem::path& stateDir, int monitorId);

// The record the last run left for this monitor, or nothing when there is none or it cannot be
// read. Unreadable is treated as absent: a refusal on a record that says nothing would refuse a
// wallpaper on no evidence.
std::optional<CrashRecord> readCrashRecord(const std::filesystem::path& stateDir, int monitorId);

void removeCrashRecord(const std::filesystem::path& stateDir, int monitorId);

// The reason a client reads, raw: sanitized where it is recorded.
std::string describeCrash(const CrashRecord& crash);

// On disk from construction to destruction, on the thread that is applying the wallpaper. One at
// a time, enforced: the fatal-signal handler has a single slot, and a record constructed while
// another is live gets no slot and writes no file. A record that could not be written is
// silently absent -- there is nothing to report it to that the next start would read.
class ApplyingRecord {
public:
    ApplyingRecord(const std::filesystem::path& stateDir, int monitorId, const std::string& type,
                   const std::string& path);
    ~ApplyingRecord();

    ApplyingRecord(const ApplyingRecord&) = delete;
    ApplyingRecord& operator=(const ApplyingRecord&) = delete;

private:
    std::string m_file;
    int m_fd = -1;
};

// SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT: stamps the live record with the signal when the
// dying thread is the one applying, removes it when another thread dies -- the wallpaper being
// applied is not what killed that one -- then hands the signal to whatever handler was there
// before and re-raises it. Call once, before any thread exists.
void installFatalSignalHandlers();

}
