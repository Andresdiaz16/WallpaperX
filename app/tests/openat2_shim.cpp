// SPDX-License-Identifier: GPL-2.0-or-later

// These definitions take precedence over libc's for every call in the test binary, including the
// library sources compiled into it and Qt; each forwards to libc's own through dlsym(RTLD_NEXT).

#include "openat2_shim.h"

#include <QFile>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

std::atomic<int> g_refused{0};
std::atomic<int> g_calls{0};

// Written only while nothing counts; read only after g_counting is seen set.
std::atomic<bool> g_counting{false};
char g_directory[PATH_MAX];
size_t g_directoryLength = 0;
std::atomic<int> g_fileOpens{0};

// libc's own function, looked up on first use. Not a function-local static: its thread-safe
// initialiser waits on a contended first call with syscall(SYS_futex), which is the replacement
// below, so the wait would re-enter the initialiser it waits on. Two threads racing here both look
// the same symbol up, which is harmless.
template <typename Fn>
Fn libc(std::atomic<Fn>& slot, const char* name) {
    Fn fn = slot.load(std::memory_order_acquire);
    if (fn == nullptr) {
        fn = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
        slot.store(fn, std::memory_order_release);
    }
    return fn;
}

// Counts `fd` when it is a file, not a folder, below the counted folder. Leaves errno alone.
void noteOpened(int fd) {
    if (fd < 0 || !g_counting.load(std::memory_order_acquire)) {
        return;
    }
    const int saved = errno;
    char link[32] = "/proc/self/fd/";
    const size_t prefix = std::strlen(link);
    *std::to_chars(link + prefix, link + sizeof link - 1, fd).ptr = '\0';
    char target[PATH_MAX];
    const ssize_t length = ::readlink(link, target, sizeof target);
    struct stat st {};
    if (length > ssize_t(g_directoryLength) && size_t(length) < sizeof target && std::memcmp(target, g_directory, g_directoryLength) == 0 &&
        target[g_directoryLength] == '/' && ::fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) {
        ++g_fileOpens;
    }
    errno = saved;
}

int modeOf(int flags, va_list args) {
    return ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) ? va_arg(args, int) : 0;
}

using OpenFn = int (*)(const char*, int, ...);
using OpenAtFn = int (*)(int, const char*, int, ...);
using Open2Fn = int (*)(const char*, int);
using OpenAt2Fn = int (*)(int, const char*, int);
std::atomic<long (*)(long, ...)> g_syscall{nullptr};
std::atomic<OpenFn> g_open{nullptr};
std::atomic<OpenFn> g_open64{nullptr};
std::atomic<OpenAtFn> g_openat{nullptr};
std::atomic<OpenAtFn> g_openat64{nullptr};
std::atomic<Open2Fn> g_open2{nullptr};
std::atomic<Open2Fn> g_open64_2{nullptr};
std::atomic<OpenAt2Fn> g_openat2fortified{nullptr};
std::atomic<OpenAt2Fn> g_openat64_2{nullptr};

} // namespace

// glibc's syscall() takes the number and up to six word-sized arguments; all six are forwarded,
// as its own stub loads all six registers whatever the call needs.
extern "C" long syscall(long number, ...) {
    const auto real = libc(g_syscall, "syscall");
    va_list args;
    va_start(args, number);
    long a[6];
    for (long& arg : a) {
        arg = va_arg(args, long);
    }
    va_end(args);
    if (number == SYS_openat2) {
        ++g_calls;
        if (const int refused = g_refused.load(); refused != 0) {
            errno = refused;
            return -1;
        }
        const long fd = real(number, a[0], a[1], a[2], a[3], a[4], a[5]);
        noteOpened(int(fd));
        return fd;
    }
    return real(number, a[0], a[1], a[2], a[3], a[4], a[5]);
}

extern "C" int open(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    const int fd = libc(g_open, "open")(path, flags, mode);
    noteOpened(fd);
    return fd;
}

extern "C" int open64(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    const int fd = libc(g_open64, "open64")(path, flags, mode);
    noteOpened(fd);
    return fd;
}

extern "C" int openat(int dirFd, const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    const int fd = libc(g_openat, "openat")(dirFd, path, flags, mode);
    noteOpened(fd);
    return fd;
}

extern "C" int openat64(int dirFd, const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    const int fd = libc(g_openat64, "openat64")(dirFd, path, flags, mode);
    noteOpened(fd);
    return fd;
}

// What _FORTIFY_SOURCE builds call when the flags are not known at compile time.
extern "C" int __open_2(const char* path, int flags) {
    const int fd = libc(g_open2, "__open_2")(path, flags);
    noteOpened(fd);
    return fd;
}

extern "C" int __open64_2(const char* path, int flags) {
    const int fd = libc(g_open64_2, "__open64_2")(path, flags);
    noteOpened(fd);
    return fd;
}

extern "C" int __openat_2(int dirFd, const char* path, int flags) {
    const int fd = libc(g_openat2fortified, "__openat_2")(dirFd, path, flags);
    noteOpened(fd);
    return fd;
}

extern "C" int __openat64_2(int dirFd, const char* path, int flags) {
    const int fd = libc(g_openat64_2, "__openat64_2")(dirFd, path, flags);
    noteOpened(fd);
    return fd;
}

namespace wallpaperx::apptest {

void setOpenat2Refused(int error) {
    g_refused.store(error);
}

int takeOpenat2Calls() {
    return g_calls.exchange(0);
}

FileOpens::FileOpens(const QString& directory) {
    char resolved[PATH_MAX];
    if (g_counting.load() || ::realpath(QFile::encodeName(directory).constData(), resolved) == nullptr) {
        return;
    }
    g_directoryLength = std::strlen(resolved);
    std::memcpy(g_directory, resolved, g_directoryLength + 1);
    g_fileOpens = 0;
    g_counting.store(true, std::memory_order_release);
    m_ok = true;
}

FileOpens::~FileOpens() {
    if (m_ok) {
        g_counting.store(false);
    }
}

int FileOpens::take() {
    return g_fileOpens.exchange(0);
}

} // namespace wallpaperx::apptest
