// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wallpaperx {

// Where one screen's video stood when the daemon stopped for a planned restart, so the next start
// opens the file at the same picture. The file's identity is what makes the position mean anything:
// a position read back against other bytes would land wherever its timestamp falls in them.
struct ResumePoint {
    int monitorId = 0;
    std::string path;
    uint64_t device = 0;
    uint64_t inode = 0;
    int64_t size = 0;
    int64_t modifiedNs = 0;
    // StreamPosition::stored().
    int64_t position = 0;
};

// <stateDir>/resume.json. Empty when `stateDir` is.
std::filesystem::path resumeRecordPath(const std::filesystem::path& stateDir);

// Replaces the record whole, through a temporary file and a rename, so a reader never sees half of
// one. Returns the reason it could not, empty on success.
std::string writeResumeRecord(const std::filesystem::path& stateDir, const std::vector<ResumePoint>& points);

// Every well-formed point in the record; none when there is no record or it cannot be read.
std::vector<ResumePoint> readResumeRecord(const std::filesystem::path& stateDir);

void removeResumeRecord(const std::filesystem::path& stateDir);

}
