// SPDX-License-Identifier: GPL-2.0-or-later

// This definition takes precedence over libc's for every call in the test binary, including the
// library sources compiled into it; it forwards to libc's own through dlsym(RTLD_NEXT).

#include "open_counter.h"

#include <QDir>
#include <QFile>

#include <atomic>
#include <cstring>
#include <mutex>

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

namespace {

// Plain storage, so a call made during static initialisation finds a valid (empty) state.
std::mutex g_lock;
std::atomic<bool> g_active{false};
char g_directory[4096];
int g_scans = 0;
int g_watcherOpens = 0;

void noteFolder(const char* path) {
    if (path == nullptr || !g_active.load()) {
        return;
    }
    const std::scoped_lock lock(g_lock);
    if (g_active.load() && std::strcmp(path, g_directory) == 0) {
        // The main thread's id is the process id.
        ++(::gettid() == ::getpid() ? g_watcherOpens : g_scans);
    }
}

} // namespace

extern "C" DIR* opendir(const char* path) {
    static const auto real = reinterpret_cast<DIR* (*)(const char*)>(::dlsym(RTLD_NEXT, "opendir"));
    noteFolder(path);
    return real(path);
}

namespace wallpaperx::apptest {

OpenCounter::OpenCounter(const QString& directory) {
    const QByteArray path = QFile::encodeName(QDir::cleanPath(QDir(directory).absolutePath()));
    const std::scoped_lock lock(g_lock);
    if (g_active.load() || size_t(path.size()) >= sizeof g_directory) {
        return;
    }
    std::memcpy(g_directory, path.constData(), size_t(path.size()) + 1);
    g_scans = 0;
    g_watcherOpens = 0;
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
    const OpenCounts counts{g_scans, g_watcherOpens};
    g_scans = 0;
    g_watcherOpens = 0;
    return counts;
}

} // namespace wallpaperx::apptest
