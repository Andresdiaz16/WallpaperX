// SPDX-License-Identifier: GPL-2.0-or-later

#include "log_pump.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// Present only when a sanitizer runtime is linked in.
extern "C" void __sanitizer_set_death_callback(void (*callback)()) __attribute__((weak));

namespace wallpaperx::logpump {
namespace {

// Per stream. At the daemon's few lines a second this covers a reader that falls behind for
// minutes; past it the newest lines are dropped, so what is kept stays in order.
constexpr size_t kQueueLimit = 256 * 1024;
// A line no longer than PIPE_BUF reaches a pipe in one piece whoever else writes to it; longer
// ones are split.
constexpr size_t kMaxLine = PIPE_BUF;
constexpr char kDroppedFormat[] = "wallpaperx-daemon: %llu log line(s) dropped while output was blocked\n";
constexpr size_t kNoticeRoom = 96;

// How a destination is written to without blocking.
enum class Sink {
    Discard,      // absent at launch, or failed for good
    NonBlocking,  // a pipe or terminal, through a description of its own that has O_NONBLOCK
    Socket,       // journald's stream: send() with MSG_DONTWAIT
    File,         // nothing to fall behind; shares the original offset
    Guarded,      // a blocking description (no /proc): at most PIPE_BUF, only once poll says it fits
};

struct Destination {
    int fd = -1;  // never closed: writeErrNow may use it until the process is gone
    std::atomic<Sink> sink{Sink::Discard};
};
static_assert(std::atomic<Sink>::is_always_lock_free);

void openDestination(Destination& out, int fd) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) return;
    Sink sink = Sink::Guarded;
    int own = -1;
    if (S_ISSOCK(st.st_mode)) {
        sink = Sink::Socket;
    } else if (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)) {
        sink = Sink::File;
    } else {
        // Not O_NONBLOCK on fd itself: that description is shared with whoever else writes to it.
        char path[32];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        own = ::open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
        if (own >= 0) sink = Sink::NonBlocking;
    }
    out.fd = own >= 0 ? own : ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (out.fd >= 0) out.sink.store(sink);
}

// What put() returns in place of a byte count.
constexpr ssize_t kGone = -1;     // nothing reads the destination any more
constexpr ssize_t kRefused = -2;  // this write failed (a full disk, an I/O error); a later one may not

// Bytes taken; 0 when it would block; kGone or kRefused. Async-signal-safe.
ssize_t put(const Destination& out, const char* data, size_t size) noexcept {
    for (;;) {
        ssize_t n = 0;
        switch (out.sink.load(std::memory_order_relaxed)) {
        case Sink::Discard:
            return static_cast<ssize_t>(size);
        case Sink::Socket:
            n = ::send(out.fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
            break;
        case Sink::Guarded: {
            pollfd ready{out.fd, POLLOUT, 0};
            if (::poll(&ready, 1, 0) != 1) return 0;
            if ((ready.revents & POLLOUT) == 0) return kGone;
            n = ::write(out.fd, data, std::min(size, kMaxLine));
            break;
        }
        case Sink::NonBlocking:
        case Sink::File:
            n = ::write(out.fd, data, size);
            break;
        }
        if (n >= 0) return n;
        switch (errno) {
        case EINTR:
            continue;
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return 0;
        case EPIPE:
        case ECONNRESET:
        case ENOTCONN:
        case EBADF:
            return kGone;
        default:
            return kRefused;
        }
    }
}

// How many lines a dropped chunk of whole lines stood for: a dropped-lines notice among them
// counts the lines it reported.
unsigned long long linesIn(const char* data, size_t size) {
    constexpr std::string_view kNoticeStart = "wallpaperx-daemon: ";
    unsigned long long lines = 0;
    for (const char* end = data + size; data < end;) {
        const auto* newline = static_cast<const char*>(std::memchr(data, '\n', static_cast<size_t>(end - data)));
        const char* next = newline != nullptr ? newline + 1 : end;
        const std::string_view line(data, static_cast<size_t>(next - data));
        unsigned long long count = 0;
        if (line.starts_with(kNoticeStart) &&
            std::from_chars(line.data() + kNoticeStart.size(), line.data() + line.size(), count).ec == std::errc()) {
            char notice[kNoticeRoom];
            const int n = std::snprintf(notice, sizeof(notice), kDroppedFormat, count);
            if (n <= 0 || std::string_view(notice, std::min(static_cast<size_t>(n), sizeof(notice) - 1)) != line) {
                count = 0;
            }
        }
        lines += count != 0 ? count : 1;
        data = next;
    }
    return lines;
}

struct Stream {
    int in = -1;  // read end of the pipe now on descriptor 1 or 2
    Destination out;
    std::string line;   // read since the last newline
    std::string queue;  // whole lines, written from `sent`
    size_t sent = 0;
    unsigned long long dropped = 0;

    size_t pending() const { return queue.size() - sent; }

    // Never grows past what install() reserved, so the pump does not allocate.
    void makeRoom(size_t size) {
        if (sent == queue.size()) {
            queue.clear();
            sent = 0;
        } else if (queue.size() + size > queue.capacity()) {
            queue.erase(0, sent);
            sent = 0;
        }
    }

    void appendNotice() {
        char text[kNoticeRoom];
        const int n = std::snprintf(text, sizeof(text), kDroppedFormat, dropped);
        queue.append(text, static_cast<size_t>(std::clamp(n, 0, static_cast<int>(sizeof(text)) - 1)));
        dropped = 0;
    }

    void admit(const std::string& whole) {
        if (out.sink.load(std::memory_order_relaxed) == Sink::Discard) return;
        noticeIfRoom();
        if (dropped != 0 || pending() + whole.size() > kQueueLimit - kNoticeRoom) {
            ++dropped;
            return;
        }
        makeRoom(whole.size());
        queue.append(whole);
    }

    // Once dropping, lines stay dropped until the destination has taken half the queue: resuming at
    // the first gap would interleave a count with every line that happened to fit. The count then
    // takes the place of the lines it stands for.
    void noticeIfRoom() {
        if (dropped == 0 || pending() > kQueueLimit / 2) return;
        makeRoom(kNoticeRoom);
        appendNotice();
    }

    void ingest(const char* data, size_t size) {
        while (size > 0) {
            const size_t room = kMaxLine - 1 - line.size();  // leaves space for a newline
            const auto* newline = static_cast<const char*>(std::memchr(data, '\n', std::min(size, room + 1)));
            size_t taken = size;
            if (newline != nullptr) {
                taken = static_cast<size_t>(newline - data) + 1;
                line.append(data, taken);
            } else if (size > room) {
                taken = room;
                line.append(data, taken);
                line.push_back('\n');
            } else {
                line.append(data, taken);
                return;
            }
            admit(line);
            line.clear();
            data += taken;
            size -= taken;
        }
    }

    void endLine() {
        if (line.empty()) return;
        line.push_back('\n');
        admit(line);
        line.clear();
    }

    void drain(char* buffer, size_t size) {
        for (;;) {
            const ssize_t n = ::read(in, buffer, size);
            if (n > 0) {
                ingest(buffer, static_cast<size_t>(n));
            } else if (n == 0) {
                ::close(in);  // every writer closed it
                in = -1;
                return;
            } else if (errno != EINTR) {
                return;
            }
        }
    }

    // Whole lines at a time, so a pipe takes each chunk entirely or not at all.
    void write() {
        while (pending() > 0) {
            const char* start = queue.data() + sent;
            const size_t window = std::min(pending(), kMaxLine);
            const auto* last = static_cast<const char*>(::memrchr(start, '\n', window));
            const size_t chunk = last != nullptr ? static_cast<size_t>(last - start) + 1 : window;
            const ssize_t n = put(out, start, chunk);
            if (n == 0) return;
            if (n == kGone) {
                out.sink.store(Sink::Discard);
                queue.clear();
                sent = 0;
                dropped = 0;
                return;
            }
            // Dropped rather than retried: a file polls writable while its disk stays full, so a
            // retry would spin. The count reports it once a write succeeds.
            if (n == kRefused) {
                dropped += linesIn(start, chunk);
                sent += chunk;
                continue;
            }
            sent += static_cast<size_t>(n);
        }
    }
};

struct Pump {
    // stdout's, then stderr's. When both reach the same file they share the first, so the file gets
    // their lines in the order they were written.
    Stream streams[2];
    size_t used = 2;
    // Descriptors 1 and 2 as the process was started with them, for release().
    int originals[2] = {-1, -1};
    int wake = -1;  // eventfd: a flush was asked for
    // Passes are numbered; the last to end with nothing left to write. Atomics and not a condition
    // variable, so a signal handler can wait on them.
    std::atomic<uint64_t> started{0};
    std::atomic<uint64_t> lastIdle{0};
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::atomic<pid_t> thread{0};  // run()'s, which flush() must not wait on

    void run() noexcept {
        thread.store(::gettid());
        char buffer[16 * 1024];
        for (;;) {
            try {
                pass(buffer, sizeof(buffer));
            } catch (...) {  // NOLINT(bugprone-empty-catch) -- nothing to report it with but itself
            }
        }
    }

    std::span<Stream> active() { return {streams, used}; }
    Stream& err() { return streams[used - 1]; }

    void writeAll() {
        for (Stream& s : active()) s.write();
    }

    void pass(char* buffer, size_t size) {
        pollfd fds[5];
        nfds_t n = 0;
        fds[n++] = {wake, POLLIN, 0};
        for (const Stream& s : active()) {
            if (s.in >= 0) fds[n++] = {s.in, POLLIN, 0};
            if (s.pending() > 0) fds[n++] = {s.out.fd, POLLOUT, 0};
        }
        if (::poll(fds, n, -1) < 0) return;
        // Numbered before reading: a flush that saw a lower number wrote before this pass reads.
        const uint64_t number = started.fetch_add(1) + 1;
        const bool flushing = (fds[0].revents & POLLIN) != 0;
        if (flushing) {
            uint64_t count = 0;
            ssize_t ignored = ::read(wake, &count, sizeof(count));
            (void)ignored;
        }
        for (Stream& s : active()) {
            if (s.in >= 0) s.drain(buffer, size);
            if (flushing) s.endLine();
        }
        writeAll();
        // After the writes, which are what make room for a count.
        bool noticed = false;
        for (Stream& s : active()) {
            noticed = noticed || s.dropped != 0;
            s.noticeIfRoom();
        }
        if (noticed) writeAll();
        const auto idle = [](const Stream& s) { return s.pending() == 0 && s.line.empty(); };
        if (std::ranges::all_of(active(), idle)) lastIdle.store(number);
    }
};

// Set once, before any other thread exists, and never freed: the pump thread and signal handlers
// use it until the process is gone.
Pump* g_pump = nullptr;

bool sameFile(int a, int b) {
    struct stat x {}, y {};
    return ::fstat(a, &x) == 0 && ::fstat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

}  // namespace

bool install() {
    bool absent[3] = {};
    for (int fd = 0; fd <= 2; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1 || errno != EBADF) continue;
        absent[fd] = true;
        // The lowest free number, so this one; if it fails, a pipe below could take the number.
        if (::open("/dev/null", O_RDWR) != fd) return false;
    }

    auto* pump = new (std::nothrow) Pump;
    if (pump == nullptr) return false;
    int writeEnds[2] = {-1, -1};
    try {
        const bool shared = !absent[STDOUT_FILENO] && !absent[STDERR_FILENO] && sameFile(STDOUT_FILENO, STDERR_FILENO);
        pump->used = shared ? 1 : 2;
        for (size_t i = 0; i < pump->used; ++i) {
            Stream& s = pump->streams[i];
            const int fd = STDOUT_FILENO + static_cast<int>(i);
            if (!absent[fd]) openDestination(s.out, fd);
            int ends[2];
            if (::pipe2(ends, O_CLOEXEC) != 0) return false;
            ::fcntl(ends[0], F_SETFL, O_NONBLOCK);
            s.in = ends[0];
            writeEnds[i] = ends[1];
            s.line.reserve(kMaxLine);
            s.queue.reserve(kQueueLimit + kNoticeRoom);
        }
        pump->wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (pump->wake < 0) return false;

        // Every signal blocked on the pump: it must never be the thread a process-directed stop
        // signal is delivered to. Not SIGPIPE, which its own writes raise: blocked, an ignored
        // SIGPIPE is still queued, and would stay pending on this thread for good. Nor the faults
        // the thread raises on itself: the kernel meets a blocked one with the default action,
        // skipping the crash handler.
        sigset_t all, previous;
        sigfillset(&all);
        for (const int sig : {SIGPIPE, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS}) {
            sigdelset(&all, sig);
        }
        pthread_sigmask(SIG_SETMASK, &all, &previous);
        try {
            std::thread([pump] { pump->run(); }).detach();
        } catch (...) {
            pthread_sigmask(SIG_SETMASK, &previous, nullptr);
            throw;
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    } catch (const std::exception&) {
        // std::exception, not system_error: the thread's state is allocated too.
        return false;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    pump->originals[0] = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    pump->originals[1] = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    ::dup2(writeEnds[0], STDOUT_FILENO);
    ::dup2(writeEnds[pump->used - 1], STDERR_FILENO);
    for (size_t i = 0; i < pump->used; ++i) ::close(writeEnds[i]);
    // A pipe would make stdout fully buffered, holding lines back from the pump.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    g_pump = pump;
    // A sanitizer's fatal report is written to descriptor 2 and followed by _exit, not by anything
    // that would flush. Not release(): its report is in the pipe by now, and the originals would
    // let a write after it block the exit on a stalled destination.
    if (__sanitizer_set_death_callback != nullptr) __sanitizer_set_death_callback([] { flush(kExitFlush); });
    return true;
}

void flush(std::chrono::milliseconds limit) noexcept {
    Pump* pump = g_pump;
    if (pump == nullptr || ::gettid() == pump->thread.load()) return;
    const uint64_t before = pump->started.load();
    const uint64_t one = 1;
    ssize_t ignored = ::write(pump->wake, &one, sizeof(one));
    (void)ignored;
    const auto now = [] {
        timespec t {};
        ::clock_gettime(CLOCK_MONOTONIC, &t);
        return std::chrono::seconds(t.tv_sec) + std::chrono::nanoseconds(t.tv_nsec);
    };
    const auto deadline = now() + limit;
    while (pump->lastIdle.load() <= before && now() < deadline) ::poll(nullptr, 0, 1);
}

void release() noexcept {
    Pump* pump = g_pump;
    if (pump == nullptr) return;
    // Restored first, so nothing written from here on can be caught in the pipe as the process ends.
    for (int fd = STDOUT_FILENO; fd <= STDERR_FILENO; ++fd) {
        if (const int original = pump->originals[fd - STDOUT_FILENO]; original >= 0) ::dup2(original, fd);
    }
    flush(kExitFlush);
}

void writeErrNow(const char* line, size_t size) noexcept {
    Pump* pump = g_pump;
    if (pump != nullptr) (void)put(pump->err().out, line, size);
}

}  // namespace wallpaperx::logpump
