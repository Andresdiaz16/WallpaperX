// SPDX-License-Identifier: GPL-2.0-or-later

// These definitions take precedence over libc's for every call in the test binary, including the
// library sources compiled into it; each forwards to libc's own through dlsym(RTLD_NEXT).

#include "open_counter.h"

#include <QDir>
#include <QFile>

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <mutex>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <strings.h>

namespace {

// Plain storage, so a call made during static initialisation finds a valid (empty) state.
std::mutex g_lock;
std::atomic<bool> g_active{false};
char g_directory[4096];
size_t g_directoryLength = 0;
int g_folderOpens = 0;
int g_packageOpens = 0;

void noteFolder(const char* path) {
    if (path == nullptr || !g_active.load()) {
        return;
    }
    const std::scoped_lock lock(g_lock);
    if (g_active.load() && std::strcmp(path, g_directory) == 0) {
        ++g_folderOpens;
    }
}

void notePackage(const char* path, int flags) {
    if (path == nullptr || (flags & O_ACCMODE) != O_RDONLY || !g_active.load()) {
        return;
    }
    const std::scoped_lock lock(g_lock);
    if (!g_active.load() || std::strncmp(path, g_directory, g_directoryLength) != 0 || path[g_directoryLength] != '/') {
        return;
    }
    const char* name = path + g_directoryLength + 1;
    const size_t length = std::strlen(name);
    if (std::strchr(name, '/') == nullptr && name[0] != '.' && length > 4 && strcasecmp(name + length - 4, ".wpx") == 0) {
        ++g_packageOpens;
    }
}

template <typename Fn>
Fn next(const char* symbol) {
    return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, symbol));
}

int modeOf(int flags, va_list args) {
    return ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) ? va_arg(args, int) : 0;
}

} // namespace

extern "C" DIR* opendir(const char* path) {
    static const auto real = next<DIR* (*)(const char*)>("opendir");
    noteFolder(path);
    return real(path);
}

extern "C" int open(const char* path, int flags, ...) {
    static const auto real = next<int (*)(const char*, int, ...)>("open");
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    notePackage(path, flags);
    return real(path, flags, mode);
}

extern "C" int open64(const char* path, int flags, ...) {
    static const auto real = next<int (*)(const char*, int, ...)>("open64");
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    notePackage(path, flags);
    return real(path, flags, mode);
}

namespace wallpaperx::apptest {

OpenCounter::OpenCounter(const QString& directory) {
    const QByteArray path = QFile::encodeName(QDir::cleanPath(QDir(directory).absolutePath()));
    const std::scoped_lock lock(g_lock);
    if (g_active.load() || size_t(path.size()) >= sizeof g_directory) {
        return;
    }
    std::memcpy(g_directory, path.constData(), size_t(path.size()) + 1);
    g_directoryLength = size_t(path.size());
    g_folderOpens = 0;
    g_packageOpens = 0;
    g_active.store(true);
    m_ok = true;
}

OpenCounter::~OpenCounter() {
    if (m_ok) {
        const std::scoped_lock lock(g_lock);
        g_active.store(false);
    }
}

OpenCounts OpenCounter::take() {
    const std::scoped_lock lock(g_lock);
    const OpenCounts counts{g_folderOpens, g_packageOpens};
    g_folderOpens = 0;
    g_packageOpens = 0;
    return counts;
}

} // namespace wallpaperx::apptest
