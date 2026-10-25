// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "processor_check.h"

#include <filesystem>
#include <string>
#include <vector>

namespace wallpaperx {

// A video file and what the processor check's scan found in it, kept across daemon starts so an unchanged file is
// not read again, whichever screens show it. Only the file's own answer: what the device makes of it -- whether it decodes the format,
// and whether it takes parameter sets inline -- is asked of the device every run, so a new card or driver needs
// nothing invalidated here.
struct SavedScan {
    std::string path;
    FileIdentity file;
    SoftwareStretches found;
};

// <stateDir>/processor-scans.json. Empty when `stateDir` is.
std::filesystem::path scanRecordPath(const std::filesystem::path& stateDir);

// Replaces the record whole, stamped with the scan's rule and the FFmpeg libraries it follows. Returns the reason it
// could not, empty on success.
std::string writeScanRecord(const std::filesystem::path& stateDir, const std::vector<SavedScan>& scans);

// Every well-formed entry; none when there is no record, it cannot be read, or another scan rule or FFmpeg build
// wrote it. An entry is a claim about the bytes its identity names, never about whatever the path names now.
std::vector<SavedScan> readScanRecord(const std::filesystem::path& stateDir);

void removeScanRecord(const std::filesystem::path& stateDir);

}
