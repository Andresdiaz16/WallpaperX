// SPDX-License-Identifier: GPL-2.0-or-later

// Linked into log_pump_test only: takes libc's place for write(2) in that binary, so the log writer
// (backend/daemon/log_pump.cpp, statically linked in) meets a destination that refuses a write as a
// full disk does, and so the writes it attempts on one destination can be counted. Every other
// write passes straight to the kernel.

#include <atomic>
#include <cerrno>

#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
std::atomic<bool> g_targeted{false};
std::atomic<dev_t> g_dev{0};
std::atomic<ino_t> g_ino{0};
std::atomic<bool> g_refuse{false};
std::atomic<unsigned long long> g_attempts{0};
}  // namespace

// The file `fd` refers to becomes the one watched: any descriptor for it, the log writer's own
// reopen included. False if it could not be identified.
bool wallpaperxTestWatchWrites(int fd) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) return false;
    g_dev.store(st.st_dev);
    g_ino.store(st.st_ino);
    g_targeted.store(true);
    return true;
}

// While on, every write to the watched file fails with ENOSPC.
void wallpaperxTestRefuseWrites(bool refuse) { g_refuse.store(refuse); }

// Writes attempted on the watched file so far, refused or not.
unsigned long long wallpaperxTestWriteAttempts() { return g_attempts.load(); }

extern "C" ssize_t write(int fd, const void* data, size_t size) {
    if (g_targeted.load()) {
        struct stat st {};
        if (::fstat(fd, &st) == 0 && st.st_dev == g_dev.load() && st.st_ino == g_ino.load()) {
            g_attempts.fetch_add(1);
            if (g_refuse.load()) {
                errno = ENOSPC;
                return -1;
            }
        }
    }
    return static_cast<ssize_t>(::syscall(SYS_write, fd, data, size));
}
