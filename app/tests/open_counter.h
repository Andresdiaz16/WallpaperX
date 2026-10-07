// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>

#include <dirent.h>

namespace wallpaperx::apptest {

// Called after every opendir() in the test binary, with its path and result, from whichever thread
// opened; null to stop. For staging what happens to a folder right after it was listed.
using OpendirHook = void (*)(const char* path, DIR* result);
void setOpendirHook(OpendirHook hook);

struct OpenCounts {
    // opendir() of the folder off the main thread: one per scan, which runs on the thread pool.
    int scans = 0;
    // opendir() of the folder on the main thread: the watcher's walk before each scan, and each
    // poll when the folder is polled.
    int watcherOpens = 0;
};

// Counts what this process opens in one folder, by replacing opendir() for the test binary
// (open_counter.cpp). An inotify IN_OPEN watch would also count other processes, such as a file
// indexer, opening the same scratch folder. One counter at a time; its callers assert it saw the
// first scan, so a dead counter goes red, never "idle".
class OpenCounter {
public:
    explicit OpenCounter(const QString& directory);
    ~OpenCounter();
    OpenCounter(const OpenCounter&) = delete;
    OpenCounter& operator=(const OpenCounter&) = delete;

    // Whether this counter is the one counting; false when another was already alive.
    bool ok() const { return m_ok; }
    // Opens since the last call.
    OpenCounts take();

private:
    bool m_ok = false;
};

} // namespace wallpaperx::apptest
