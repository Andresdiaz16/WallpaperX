// SPDX-License-Identifier: GPL-2.0-or-later

// Preloaded into the real daemon by daemon_system_test and device_loss_restart_test, to stage what
// no request to the daemon can produce. Each is off unless its variable is set, and every other
// call is forwarded:
//
//   WALLPAPERX_TEST_WEDGE_FREE=1            a decoder freed on the MAIN thread never returns --
//                                           only ~WallpaperManager frees one there, after the
//                                           render worker has been joined, so the stop sticks in
//                                           the teardown and nowhere earlier. It says so on stderr
//                                           first, so a test can tell a wedged stop from a quick one.
//   WALLPAPERX_TEST_DELAY_PLAYING_OPEN_MS=n creating <stateDir>/playing.json takes n milliseconds,
//                                           which widens the startup window between opening that
//                                           record and posting the restored renders from
//                                           microseconds to something a death can land inside.
//   WALLPAPERX_TEST_CRASH_IN_PLAYBACK=1     the next picture asked of the daemon's own decoder AFTER
//                                           it delivered its first faults on the thread that asked:
//                                           a real SIGSEGV inside the clock's work on a video, which
//                                           is where the fatal-signal handler is supposed to find a
//                                           name. Keyed on the decoder's own context and on its
//                                           having delivered a picture, not on a packet number,
//                                           because how many packets the first picture costs differs
//                                           between the hardware and software decoders.
//   WALLPAPERX_TEST_CRASH_MARK=path         with the above, the file created just before the fault:
//                                           the moment of the crash, for a test whose stderr is
//                                           stalled and so cannot carry the line that says so.
//   WALLPAPERX_TEST_LINE_AT_EXIT=1          one line on descriptor 2 as the library loads, and one
//                                           from its destructor, which runs after main() returns.
//   WALLPAPERX_TEST_SLOW_EXIT_MS=n          that destructor says so on descriptor 2, then sleeps n
//                                           milliseconds: a static destruction slower than the
//                                           daemon's teardown deadline.
//   WALLPAPERX_TEST_FAIL_SHUTDOWN_WATCH=1   the first mutex lock a thread makes after sigwait()
//                                           returns fails with EINVAL, which std::mutex throws: the
//                                           daemon's signal thread fails as it starts a stop.
//
// Not linked into anything shipped.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

bool enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

long number(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? 0 : std::strtol(value, nullptr, 10);
}

bool endsWith(const char* text, const char* suffix) {
    const size_t textLength = std::strlen(text);
    const size_t suffixLength = std::strlen(suffix);
    return textLength >= suffixLength && std::strcmp(text + textLength - suffixLength, suffix) == 0;
}

// VideoDecoder marks its own context by setting `opaque`; the contexts ffmpeg opens while probing a
// file leave it null, and those decode too -- during the video's OPEN, which is the span the
// applying record covers rather than the one being staged here.
bool isDecodersContext(const AVCodecContext* context) { return context != nullptr && context->opaque != nullptr; }

std::mutex g_deliveredMutex;
std::map<const AVCodecContext*, int> g_delivered;

bool hasDelivered(const AVCodecContext* context) {
    std::lock_guard<std::mutex> lock(g_deliveredMutex);
    const auto it = g_delivered.find(context);
    return it != g_delivered.end() && it->second > 0;
}

void writeLine(const char* line) {
    const ssize_t ignored = ::write(STDERR_FILENO, line, std::strlen(line));
    (void)ignored;
}

__attribute__((constructor)) void announceLoad() {
    if (enabled("WALLPAPERX_TEST_LINE_AT_EXIT")) writeLine("wedge_shim: loaded, with a line to write at exit\n");
}

__attribute__((destructor)) void writeAtExit() {
    if (enabled("WALLPAPERX_TEST_LINE_AT_EXIT")) writeLine("wedge_shim: written after main returned\n");
    if (const long slowMs = number("WALLPAPERX_TEST_SLOW_EXIT_MS"); slowMs > 0) {
        writeLine("wedge_shim: sleeping in a library destructor, after main returned\n");
        const timespec wait{slowMs / 1000, (slowMs % 1000) * 1000000};
        ::nanosleep(&wait, nullptr);
    }
}

thread_local bool t_failNextLock = false;

// Plain pointers, not function-local statics: a static's guard may itself lock a mutex.
using SigwaitFn = int (*)(const sigset_t*, int*);
using MutexLockFn = int (*)(pthread_mutex_t*);
std::atomic<SigwaitFn> g_realSigwait{nullptr};
std::atomic<MutexLockFn> g_realMutexLock{nullptr};

}  // namespace

extern "C" int sigwait(const sigset_t* set, int* sig) {
    SigwaitFn real = g_realSigwait.load();
    if (real == nullptr) {
        real = reinterpret_cast<SigwaitFn>(::dlsym(RTLD_NEXT, "sigwait"));
        g_realSigwait.store(real);
    }
    if (real == nullptr) return EINVAL;
    const int result = real(set, sig);
    if (result == 0 && enabled("WALLPAPERX_TEST_FAIL_SHUTDOWN_WATCH")) t_failNextLock = true;
    return result;
}

extern "C" int pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (t_failNextLock) {
        t_failNextLock = false;
        return EINVAL;
    }
    MutexLockFn real = g_realMutexLock.load();
    if (real == nullptr) {
        real = reinterpret_cast<MutexLockFn>(::dlsym(RTLD_NEXT, "pthread_mutex_lock"));
        g_realMutexLock.store(real);
    }
    return real != nullptr ? real(mutex) : EINVAL;
}

extern "C" void avcodec_free_context(AVCodecContext** context) {
    using Fn = void (*)(AVCodecContext**);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avcodec_free_context"));
    if (enabled("WALLPAPERX_TEST_WEDGE_FREE") && ::gettid() == ::getpid()) {
        static constexpr char kHeld[] = "wedge_shim: holding a decoder free on the main thread\n";
        const ssize_t ignored = ::write(STDERR_FILENO, kHeld, sizeof(kHeld) - 1);
        (void)ignored;
        for (;;) ::pause();
    }
    if (real != nullptr) real(context);
}

extern "C" int avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    using Fn = int (*)(AVCodecContext*, AVFrame*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avcodec_receive_frame"));
    if (real == nullptr) return -1;
    const int result = real(context, frame);
    if (result == 0 && isDecodersContext(context)) {
        std::lock_guard<std::mutex> lock(g_deliveredMutex);
        ++g_delivered[context];
    }
    return result;
}

extern "C" int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    using Fn = int (*)(AVCodecContext*, const AVPacket*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avcodec_send_packet"));
    if (enabled("WALLPAPERX_TEST_CRASH_IN_PLAYBACK") && isDecodersContext(context) && hasDelivered(context)) {
        static constexpr char kDying[] = "wedge_shim: faulting on the decoder's thread\n";
        const ssize_t ignored = ::write(STDERR_FILENO, kDying, sizeof(kDying) - 1);
        (void)ignored;
        // After the line, so a test that sees the file knows the line was already written.
        if (const char* mark = std::getenv("WALLPAPERX_TEST_CRASH_MARK"); mark != nullptr && *mark != '\0') {
            const int fd = open(mark, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
            if (fd >= 0) ::close(fd);
        }
        // A store to a page mapped PROT_NONE, not raise(): a fault the thread took, which is the
        // case the daemon's handler is written for.
        void* page = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page != MAP_FAILED) *static_cast<volatile char*>(page) = 1;
    }
    return real != nullptr ? real(context, packet) : -1;
}

extern "C" int open(const char* path, int flags, ...) {
    using Fn = int (*)(const char*, int, mode_t);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "open"));
    if (real == nullptr) return -1;
    mode_t mode = 0;
    if ((flags & (O_CREAT | O_TMPFILE)) != 0) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, unsigned int));
        va_end(args);
    }
    const long delayMs = number("WALLPAPERX_TEST_DELAY_PLAYING_OPEN_MS");
    if (delayMs > 0 && path != nullptr && (flags & O_CREAT) != 0 && endsWith(path, "/playing.json")) {
        const timespec wait{delayMs / 1000, (delayMs % 1000) * 1000000};
        ::nanosleep(&wait, nullptr);
    }
    return real(path, flags, mode);
}
