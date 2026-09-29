// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>

// Test-only: stands in for a kernel without openat2 (before 5.6), or a sandbox that filters it,
// by replacing syscall() in the test binary (openat2_shim.cpp). Every other system call is passed
// through unchanged. The same file counts the files opened below one folder.

namespace wallpaperx::apptest {

// While not 0, openat2 fails with this errno without reaching the kernel: ENOSYS as an old kernel
// answers, EPERM or ENOSYS as a seccomp filter does.
void setOpenat2Refused(int error);
// openat2 calls since the last call, refused or not.
int takeOpenat2Calls();

// Counts, while alive, every file that is not a folder which this process opens below `directory`,
// on any thread, through open, open64, openat, openat64 (and their fortified forms) or openat2 --
// O_PATH included. The binary's own replacements of those calls do the counting, so they are all
// seen, whoever makes them; a file is placed by the path the kernel reports for the descriptor.
// One at a time.
class FileOpens {
public:
    explicit FileOpens(const QString& directory);
    ~FileOpens();
    FileOpens(const FileOpens&) = delete;
    FileOpens& operator=(const FileOpens&) = delete;

    // Whether this counter is the one counting: the folder exists and no other counter was alive.
    bool ok() const { return m_ok; }
    // Opens since the last call.
    int take();

private:
    bool m_ok = false;
};

} // namespace wallpaperx::apptest
