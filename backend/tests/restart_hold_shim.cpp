// SPDX-License-Identifier: GPL-2.0-or-later

// Preloaded into the real daemon by device_loss_restart_test's planned-restart phases, to stage what
// no request can: a resume record that cannot be written, and a planned restart held still at a
// chosen point so an outside stop can be sent into it. Off unless its variable is set; every call is
// forwarded.
//
//   WALLPAPERX_TEST_RESUME_RENAME_FAIL=1  renaming anything onto ".../resume.json" fails with EIO.
//   WALLPAPERX_TEST_RESTART_HOLD=<point>  once the record has been renamed into place, the thread
//                                         reaching <point> creates <dir>/held and waits (at most
//                                         10 s) for <dir>/release:
//                                           claim      just after that rename, before the stop is claimed
//                                           wake       the first SIGUSR1 pthread_kill after it, before sending
//                                           afterwake  the same, after sending
//                                           joined     main's pthread_join of the thread that called
//                                                      sigwait, after it returns
//                                           unblock    the sigwait thread unblocking SIGTERM, after it does
//                                           final      the first fflush(nullptr) after that join, after it
//   WALLPAPERX_TEST_EXIT_HOLD=<mode>      once the record is renamed, holds an exit -- the daemon's exit
//                                         flush (its poll(nullptr, 0, 1) wait) or _Exit -- creating
//                                         <dir>/exitheld (and <dir>/exitheld-watch when the thread is
//                                         the sigwait one) and waiting at most 10 s for <dir>/exitrelease;
//                                         <dir>/exitexpired when that ran out:
//                                           watch      every exit flush: the sigwait thread's waits for
//                                                      exitrelease, any other thread's for
//                                                      exitrelease-other
//                                           alarm      the first exit flush on any other thread
//                                           first      the first exit flush on any thread
//                                           exit       the first _Exit on any other thread
//   WALLPAPERX_TEST_RESTART_HOLD_DIR=dir  where the files go. <dir>/unblocked is created when a thread
//                                         unblocks SIGTERM after the rename (the signal thread starting
//                                         to listen for a stop during the restart), and
//                                         <dir>/unblocked-watch too when that thread is the sigwait one;
//                                         <dir>/watchwaiting when the sigwait thread first starts a timed
//                                         condition wait after the rename (its teardown deadline);
//                                         <dir>/interrupted when a signal handler ran on a held thread.
//
// Holds run inside signal handlers, so the paths are built once at load and a hold only calls
// async-signal-safe functions. Not linked into anything shipped.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {

std::atomic<bool> g_recordRenamed{false};
std::atomic<bool> g_held{false};
std::atomic<bool> g_exitHeld{false};
std::atomic<bool> g_watchJoined{false};
std::atomic<bool> g_finalHeld{false};
// The thread that called sigwait (the daemon's signal-watch thread); 0 until one has.
std::atomic<pthread_t> g_sigwaiter{};

const char* g_restartHold = nullptr;
const char* g_exitHold = nullptr;
char g_dir[PATH_MAX] = {};

const char* var(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

using PollFn = int (*)(struct pollfd*, nfds_t, int);
using ExitFn = void (*)(int);
PollFn g_poll = nullptr;
ExitFn g_exit = nullptr;

__attribute__((constructor)) void readSettings() {
    g_restartHold = var("WALLPAPERX_TEST_RESTART_HOLD");
    g_exitHold = var("WALLPAPERX_TEST_EXIT_HOLD");
    if (const char* dir = var("WALLPAPERX_TEST_RESTART_HOLD_DIR"); dir != nullptr && std::strlen(dir) < PATH_MAX - 64) {
        std::strcpy(g_dir, dir);
    }
    // Resolved here, not on first use: their first call may be inside a signal handler.
    g_poll = reinterpret_cast<PollFn>(::dlsym(RTLD_NEXT, "poll"));
    g_exit = reinterpret_cast<ExitFn>(::dlsym(RTLD_NEXT, "_Exit"));
}

bool holdAt(const char* point) {
    return g_restartHold != nullptr && std::strcmp(g_restartHold, point) == 0 && g_dir[0] != '\0';
}

bool exitHoldIs(const char* mode) {
    return g_exitHold != nullptr && std::strcmp(g_exitHold, mode) == 0 && g_dir[0] != '\0' && g_recordRenamed.load();
}

bool onWatchThread() { return pthread_equal(pthread_self(), g_sigwaiter.load()) != 0; }

// <dir>/<name> into `out`; async-signal-safe.
void pathOf(const char* name, char (&out)[PATH_MAX]) {
    std::strcpy(out, g_dir);
    std::strcat(out, "/");
    std::strcat(out, name);
}

void createFile(const char* name) {
    char path[PATH_MAX];
    pathOf(name, path);
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) ::close(fd);
}

// Sleeps in short steps until <dir>/<release> exists, so a signal handler run on this thread returns
// into it; at most 10 s. False when that ran out.
bool waitFor(const char* release) {
    char path[PATH_MAX];
    pathOf(release, path);
    for (int step = 0; step < 5000; ++step) {
        if (::access(path, F_OK) == 0) return true;
        const timespec two{0, 2'000'000};
        if (::nanosleep(&two, nullptr) != 0 && errno == EINTR) createFile("interrupted");
    }
    return false;
}

// Once per process.
void hold() {
    if (g_held.exchange(true)) return;
    const int saved = errno;
    createFile("held");
    waitFor("release");
    errno = saved;
}

void exitHold(const char* release) {
    const int saved = errno;
    if (onWatchThread()) createFile("exitheld-watch");
    createFile("exitheld");
    if (!waitFor(release)) createFile("exitexpired");
    errno = saved;
}

[[noreturn]] void unresolved(const char* name) {
    constexpr const char kHead[] = "restart_hold_shim: cannot resolve the real ";
    ssize_t ignored = ::write(STDERR_FILENO, kHead, sizeof kHead - 1);
    ignored = ::write(STDERR_FILENO, name, std::strlen(name));
    ignored = ::write(STDERR_FILENO, "\n", 1);
    (void)ignored;
    ::_exit(70);
}

template <typename Fn>
Fn real(const char* name) {
    void* found = ::dlsym(RTLD_NEXT, name);
    if (found == nullptr) unresolved(name);
    return reinterpret_cast<Fn>(found);
}

bool isResumeRecord(const char* path) {
    constexpr const char* kSuffix = "/resume.json";
    const size_t length = std::strlen(path);
    const size_t suffix = std::strlen(kSuffix);
    return length >= suffix && std::strcmp(path + length - suffix, kSuffix) == 0;
}

}  // namespace

extern "C" int rename(const char* from, const char* to) {
    using Fn = int (*)(const char*, const char*);
    static const Fn next = real<Fn>("rename");
    if (to != nullptr && isResumeRecord(to)) {
        if (var("WALLPAPERX_TEST_RESUME_RENAME_FAIL") != nullptr && std::strcmp(var("WALLPAPERX_TEST_RESUME_RENAME_FAIL"), "1") == 0) {
            errno = EIO;
            return -1;
        }
        const int result = next(from, to);
        if (result == 0) {
            g_recordRenamed.store(true);
            if (holdAt("claim")) hold();
        }
        return result;
    }
    return next(from, to);
}

extern "C" int pthread_kill(pthread_t thread, int sig) {
    using Fn = int (*)(pthread_t, int);
    static const Fn next = real<Fn>("pthread_kill");
    const bool ours = sig == SIGUSR1 && g_recordRenamed.load() && !g_held.load();
    if (ours && holdAt("wake")) hold();
    const int result = next(thread, sig);
    if (ours && holdAt("afterwake")) hold();
    return result;
}

extern "C" int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) {
    using Fn = int (*)(int, const sigset_t*, sigset_t*);
    static const Fn next = real<Fn>("pthread_sigmask");
    const int result = next(how, set, old);
    if (result == 0 && how == SIG_UNBLOCK && set != nullptr && sigismember(set, SIGTERM) == 1 && g_recordRenamed.load() &&
        g_dir[0] != '\0') {
        createFile("unblocked");
        if (onWatchThread()) createFile("unblocked-watch");
        if (holdAt("unblock") && onWatchThread()) hold();
    }
    return result;
}

// Marks the sigwait thread reaching its deadline wait, which in program order follows its unblock.
static void markWatchWaiting() {
    static std::atomic<bool> marked{false};
    if (g_recordRenamed.load() && g_dir[0] != '\0' && onWatchThread() && !marked.exchange(true)) createFile("watchwaiting");
}

extern "C" int pthread_cond_clockwait(pthread_cond_t* cond, pthread_mutex_t* mutex, clockid_t clock, const struct timespec* at) {
    using Fn = int (*)(pthread_cond_t*, pthread_mutex_t*, clockid_t, const struct timespec*);
    static const Fn next = real<Fn>("pthread_cond_clockwait");
    markWatchWaiting();
    return next(cond, mutex, clock, at);
}

extern "C" int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* at) {
    using Fn = int (*)(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
    static const Fn next = real<Fn>("pthread_cond_timedwait");
    markWatchWaiting();
    return next(cond, mutex, at);
}

extern "C" int sigwait(const sigset_t* set, int* sig) {
    using Fn = int (*)(const sigset_t*, int*);
    static const Fn next = real<Fn>("sigwait");
    g_sigwaiter.store(pthread_self());
    return next(set, sig);
}

extern "C" int pthread_join(pthread_t thread, void** result) {
    using Fn = int (*)(pthread_t, void**);
    static const Fn next = real<Fn>("pthread_join");
    const int joined = next(thread, result);
    if (joined == 0 && g_recordRenamed.load() && pthread_equal(thread, g_sigwaiter.load()) != 0) {
        g_watchJoined.store(true);
        if (holdAt("joined")) hold();
    }
    return joined;
}

extern "C" int fflush(FILE* stream) {
    using Fn = int (*)(FILE*);
    static const Fn next = real<Fn>("fflush");
    const int result = next(stream);
    if (stream == nullptr && g_watchJoined.load() && holdAt("final") && !g_finalHeld.exchange(true)) hold();
    return result;
}

// The daemon's exit flush is the only poll with no descriptors and a 1 ms timeout.
extern "C" int poll(struct pollfd* fds, nfds_t count, int timeout) {
    if (g_poll == nullptr) unresolved("poll");
    if (fds == nullptr && count == 0 && timeout == 1) {
        if (exitHoldIs("watch")) {
            exitHold(onWatchThread() ? "exitrelease" : "exitrelease-other");
        } else if ((exitHoldIs("alarm") && !onWatchThread()) || exitHoldIs("first")) {
            if (!g_exitHeld.exchange(true)) exitHold("exitrelease");
        }
    }
    return g_poll(fds, count, timeout);
}

extern "C" [[noreturn]] void _Exit(int status) {
    if (g_exit == nullptr) unresolved("_Exit");
    if (exitHoldIs("exit") && !onWatchThread() && !g_exitHeld.exchange(true)) exitHold("exitrelease");
    g_exit(status);
    __builtin_unreachable();
}
