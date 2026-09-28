// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>

namespace wallpaperx::apptest {

struct OpenCounts {
    // opendir() of the folder: one per scan, and one per poll when the folder is polled.
    int folderOpens = 0;
    // A *.wpx directly in the folder opened read-only: only a scan (or a preview read) does that.
    int packageOpens = 0;
};

// Counts what this process opens in one folder, by replacing opendir() and open() for the test
// binary (open_counter.cpp). An inotify IN_OPEN watch would also count other processes, such as
// a file indexer, opening the same scratch folder. One counter at a time; its callers assert it
// saw the first scan, so a dead counter goes red, never "idle".
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
