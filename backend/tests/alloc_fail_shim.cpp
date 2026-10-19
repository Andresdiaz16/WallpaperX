// SPDX-License-Identifier: GPL-2.0-or-later

// Preloaded into the real daemon by daemon_system_test's frameAllocationFailure phase: fails one
// chosen operator new on the thread that sends GetFrame replies, so an allocation failure on the
// bus path of a hand-over can be staged in the shipped binary. Off unless
//
//   WALLPAPERX_TEST_ALLOC_FAIL=path   names a file holding an AllocFailControl (alloc_fail_control.h),
//                                     which the test arms.
//   WALLPAPERX_TEST_ALLOC_FAIL_TRACE=1  each failure prints its stack on descriptor 2.
//
// The thread is learned rather than named: the first to append a Unix descriptor to a D-Bus message
// is the render worker answering a GetFrame, since nothing else this daemon sends carries one until a
// test asks for an acquire timeline. Other threads' allocations are never failed, so a failure lands
// only where the frame reply is built.
//
// Under a sanitizer GCC links as a shared runtime, a preloaded library comes first in symbol lookup:
// under ThreadSanitizer this shim's operator new runs and forwards to the runtime's; under
// AddressSanitizer the runtime must itself be first in the preload list, or the daemon refuses to
// start, which the test's CMake entry arranges. Only a runtime that binds operator new inside itself
// leaves this shim inert, and the test then reports that nothing fired. Not linked into anything shipped.

#include "tests/alloc_fail_control.h"

#include "support/sanitizer_ignored_caller.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

struct sd_bus_message;

namespace {

wallpaperx::testing::AllocFailControl* g_control = nullptr;

__attribute__((constructor)) void mapControl() {
    const char* path = std::getenv("WALLPAPERX_TEST_ALLOC_FAIL");
    if (path == nullptr || *path == '\0') return;
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    void* mapped = ::mmap(nullptr, sizeof(wallpaperx::testing::AllocFailControl), PROT_READ | PROT_WRITE, MAP_SHARED,
                          fd, 0);
    ::close(fd);
    // The unwinder is loaded on the first call, which allocates; never inside operator new.
    void* frames[2];
    static_cast<void>(::backtrace(frames, 2));
    if (mapped != MAP_FAILED) g_control = static_cast<wallpaperx::testing::AllocFailControl*>(mapped);
}

bool onTheWorker() {
    return g_control != nullptr && static_cast<int>(::gettid()) == g_control->workerTid.load(std::memory_order_relaxed);
}

// Whether the allocation is the daemon's own or sdbus-c++'s, never a Vulkan layer's or driver's:
// those allocate through C++ too, and a std::bad_alloc thrown out of one crosses the Vulkan C API,
// which no real driver does -- it answers VK_ERROR_OUT_OF_HOST_MEMORY. Decided by the first frame
// outside this library and libstdc++, since a layer's strings are built by libstdc++'s out-of-line
// code. backtrace() is primed in mapControl(), so this does not allocate.
bool calledByTheDaemon() {
    void* frames[24];
    const int depth = ::backtrace(frames, 24);
    for (int i = 0; i < depth; ++i) {
        Dl_info info{};
        if (::dladdr(frames[i], &info) == 0 || info.dli_fname == nullptr) return false;
        if (std::strstr(info.dli_fname, "alloc-fail-shim") != nullptr || std::strstr(info.dli_fname, "libstdc++") != nullptr) {
            continue;
        }
        return std::strstr(info.dli_fname, "wallpaperx-daemon") != nullptr ||
               std::strstr(info.dli_fname, "libsdbus-c++") != nullptr;
    }
    return false;
}

[[gnu::noinline]] bool failThisOne() {
    if (g_control == nullptr || g_control->countdown.load(std::memory_order_relaxed) <= 0) return false;
    if (!onTheWorker()) return false;
    if (!calledByTheDaemon()) return false;
    if (g_control->countdown.fetch_sub(1) != 1) return false;
    g_control->fired.fetch_add(1);
    // For finding where a failure lands: the stack, on descriptor 2, without allocating.
    if (const char* trace = std::getenv("WALLPAPERX_TEST_ALLOC_FAIL_TRACE"); trace != nullptr && *trace == '1') {
        void* frames[48];
        ::backtrace_symbols_fd(frames, ::backtrace(frames, 48), 2);
    }
    return true;
}

// Whether the waiting code is the daemon's own: the first frame outside this library and libstdc++
// (whose condition_variable::wait the daemon's worker reaches the wait through) is in the daemon. A
// driver's or a library's wait in the middle of a call -- a fence wait built on a condition variable
// -- is not the worker going back for its next job.
bool waitedByTheDaemon() {
    void* frames[16];
    const int depth = ::backtrace(frames, 16);
    for (int i = 0; i < depth; ++i) {
        Dl_info info{};
        if (::dladdr(frames[i], &info) == 0 || info.dli_fname == nullptr) return false;
        if (std::strstr(info.dli_fname, "alloc-fail-shim") != nullptr || std::strstr(info.dli_fname, "libstdc++") != nullptr) {
            continue;
        }
        return std::strstr(info.dli_fname, "wallpaperx-daemon") != nullptr;
    }
    return false;
}

// The worker starting to wait on a condition variable of the daemon's own: the end of whatever it was
// doing. Any other wait on that thread is only counted, for the test to report.
void noteWait() {
    if (!onTheWorker()) return;
    if (waitedByTheDaemon()) {
        g_control->callsDone.fetch_add(1);
    } else {
        g_control->otherWaits.fetch_add(1);
    }
}

// The default version of a pthread symbol: glibc keeps an older pthread_cond_* beside the current
// one, and the daemon binds the current.
template <class Fn>
Fn nextPthread(const char* symbol) {
    if (void* versioned = ::dlvsym(RTLD_NEXT, symbol, "GLIBC_2.3.2")) return reinterpret_cast<Fn>(versioned);
    return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, symbol));
}

// Where a condition wait goes. Under ThreadSanitizer, dlvsym above never matches the runtime's
// unversioned interceptor, so it is libc's own wait: the sanitizer would never see the mutex released
// and retaken inside it, and every access the daemon makes under that mutex reads as a race. So the
// daemon's waits go to the interceptor (dlsym finds it; it calls the current version), and only a wait
// from a library the sanitizer ignores keeps libc's, as the sanitizer itself would treat it.
template <class Fn>
Fn waitFor(Fn libc, Fn sanitizer, const void* caller) {
#if defined(__SANITIZE_THREAD__)
    if (sanitizer != nullptr && !wallpaperx::testing::ignored_caller::fromIgnoredLibrary(caller)) return sanitizer;
#else
    (void)sanitizer;
    (void)caller;
#endif
    return libc;
}

}  // namespace

extern "C" int sd_bus_message_append_basic(sd_bus_message* message, char type, const void* value) {
    using Fn = int (*)(sd_bus_message*, char, const void*);
    static const auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "sd_bus_message_append_basic"));
    if (type == 'h' && g_control != nullptr) {
        int unset = 0;
        g_control->workerTid.compare_exchange_strong(unset, static_cast<int>(::gettid()));
    }
    return real(message, type, value);
}

extern "C" int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    using Fn = int (*)(pthread_cond_t*, pthread_mutex_t*);
    static const Fn libc = nextPthread<Fn>("pthread_cond_wait");
    static const Fn sanitizer = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "pthread_cond_wait"));
    noteWait();
    return waitFor(libc, sanitizer, __builtin_return_address(0))(cond, mutex);
}

extern "C" int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* at) {
    using Fn = int (*)(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
    static const Fn libc = nextPthread<Fn>("pthread_cond_timedwait");
    static const Fn sanitizer = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "pthread_cond_timedwait"));
    noteWait();
    return waitFor(libc, sanitizer, __builtin_return_address(0))(cond, mutex, at);
}

extern "C" int pthread_cond_clockwait(pthread_cond_t* cond, pthread_mutex_t* mutex, clockid_t clock,
                                      const struct timespec* at) {
    using Fn = int (*)(pthread_cond_t*, pthread_mutex_t*, clockid_t, const struct timespec*);
    static const Fn sanitizer = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "pthread_cond_clockwait"));
    static const Fn libc = [] {
#if defined(__SANITIZE_THREAD__)
        return wallpaperx::testing::ignored_caller::libcSymbol<Fn>("pthread_cond_clockwait");
#else
        return sanitizer;
#endif
    }();
    noteWait();
    return waitFor(libc != nullptr ? libc : sanitizer, sanitizer, __builtin_return_address(0))(cond, mutex, clock, at);
}

// Only the two forms libstdc++ does not route through these itself: its operator new[] and the
// nothrow forms call operator new (aligned or not) through the PLT, so they reach these, and an
// override of each would count one allocation twice.
void* operator new(std::size_t size) {
    using Fn = void* (*)(std::size_t);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "_Znwm"));
    if (failThisOne()) throw std::bad_alloc();
    return real(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    using Fn = void* (*)(std::size_t, std::align_val_t);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "_ZnwmSt11align_val_t"));
    if (failThisOne()) throw std::bad_alloc();
    return real(size, alignment);
}
