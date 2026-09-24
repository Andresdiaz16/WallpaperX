// SPDX-License-Identifier: GPL-2.0-or-later

// Step 13 Stages E1, E2 and E3: the daemon draws a `video` wallpaper's first picture, keeps one
// decode session per monitor rather than building one inside every render pass, and plays it.
//
// Each phase has a manager of its own, because they need the manager in three different shapes --
// with a bus, with a render worker and no bus, and with neither -- and the Device pins itself to
// the thread that created it.
//
//   over the bus        a video is drawn, served and reported (S13.114, S13.115)
//   no bus, no worker   an undecodable video fails like an undecodable image (S13.116)
//   a worker, no bus    a resize decodes nothing, a new wallpaper decodes before the rebuild, and
//                       an overtaken pass leaves the published frame reachable (S13.117, S13.118)
//   a worker, no bus    the crash record covers the decode and a session's teardown (S13.119)
//   no bus, no worker   which passes reuse and drop a session, the eight-session ceiling and what
//                       it refuses (S13.123-S13.126, S13.132)
//   a worker, no bus    what a failed pass does with its session, and which teardowns are under a
//                       record (S13.127-S13.130, S13.138)
//   a worker, no bus    shutdown frees sessions with no record, and the next start draws (S13.131)
//   a worker, no bus    the clock: rate, order, looping, pausing for a render, a still beside it
//                       (S13.133, S13.134, S13.137, S13.143)
//   a worker, no bus    a one-picture file is shown once and parks (S13.135)
//   no bus, no worker   a hand-over and a refusal are announced once per consumer (S13.144)
//   no bus, no worker   an open that runs out of memory frees an idle session and retries once
//                       (S13.139)
//   a worker, no bus    playback that keeps failing is reopened three times and then settles
//                       (S13.140)
//   a worker, no bus    a played picture does not clear GPU health, and a GPU fault during
//                       playback is never retried (S13.141, S13.142)
//   a worker, no bus    running short is the daemon's fault, with a reopen budget of its own
//                       (S13.193)
//   no bus, no worker   picking a wallpaper opens it, with the draw's filesystem checks (S13.195)
//   a worker, no bus    a video's death count is cleared by play, not by time (S13.189)
//   children            which death a video answers for: this binary re-executed as a daemon-like
//                       start that dies of a real fault on its render thread (S13.184, S13.185,
//                       S13.190)
//
// TWO FIXTURES, AND THE SECOND IS LOAD-BEARING. wallpaper_bands.mp4 holds ten pictures and plays;
// wallpaper_one_picture.mp4 holds one, and a session opened on it parks after its first picture and
// is never decoded from again. Every phase that counts what ONE RENDER PASS opened or decoded uses
// the parked fixture, because the clock runs on the same thread between a counter reset and the
// read, and no counter can say which of the two sent a packet.
//
// WHICH FILES ARE OPEN is read from open(2) and close(2), hooked below: a session holds its file
// open for exactly its lifetime, so the set of open wallpaper files is the set of resident
// sessions. Status cannot answer that -- a monitor reports the same frame whether its session was
// kept or rebuilt.
//
// The kernel's calls and not ffmpeg's, because the daemon no longer lets ffmpeg near the path: it
// opens the file itself, refuses anything that is not a regular file, and gives the decoder an
// AVIOContext reading that descriptor. An avio_open2 hook -- what this file used before -- now
// records nothing at all, and "nothing was opened" is what several checks here expect to see, so
// that shape does not fail, it goes vacuous. Only paths the ledger was told to watch are recorded,
// or every shader, ICD and /proc file the process opens would land in the same counters.
//
// THE VALIDATION ORACLE IS NARROWED HERE, AND ONLY HERE. ffmpeg's Vulkan decoder binds one
// reference slot per DPB picture where this driver's profile wants a single layered image, so
// every hardware decode raises VUID-VkVideoBeginCodingInfoKHR-flags-07244 -- reproducible with
// the stock `ffmpeg -hwaccel vulkan` command line, so a defect in a dependency. This process
// excuses that one message id through the validation layer's own filter and says so on every run.
// It is a separate binary from dbus_service_test for exactly that reason: a whole process is the
// smallest thing the filter can apply to, and dbus_service_test must keep grading every message.
//
// An excuse that stops working fails LOUDLY rather than quietly: if the filter is not applied the
// count is non-zero and each phase's grade goes red. There is no path where a broken excuse turns
// into a green run.
//
// WHY A GATE RATHER THAN A SLEEP. Several of these properties are about a span inside one render
// pass -- what has been decoded by the time the images are rebuilt, what is on disk while a
// decoder is torn down. A check before or after the pass cannot see that span at all, and a sleep
// would be an assertion on the scheduler. The daemon's own calls are hooked in this executable
// (tests/client/call_gate.h) and the render thread is held inside them.

#include "daemon/crash_record.h"
#include "daemon/dbus_service.h"
#include "daemon/render_worker.h"
#include "daemon/wallpaper_manager.h"

#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

#include "support/cover_expectation.h"
#include "support/decoder_message_id.h"
#include "support/exported_frame_map.h"
#include "support/test_image.h"
#include "support/validation_grade.h"
#include "tests/client/call_gate.h"
#include "tests/client/daemon_client.h"
#include "tests/client/scratch_dir.h"
#include "tests/manager_output.h"
#include "tests/self_exec.h"
#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <cstdarg>
#include <limits>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using wallpaperx::DbusService;
using wallpaperx::RenderWorker;
using wallpaperx::WallpaperManager;
using wallpaperx::testing::CallGate;
using wallpaperx::testing::check;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::notVerified;

// The ffmpeg entry points this file hooks. The header is needed, not just the declarations:
// telling the decoder's own codec context apart from the ones ffmpeg opens while probing the file
// means reading a field of it, and that distinction is load-bearing -- see isDecodersContext.
extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

// Where the render thread is held or failed, inside the daemon's own work. Unarmed, each costs the
// real call one atomic load.
// The two framebuffer gates are armed with an extent ("96x48") and fire only on a framebuffer of
// that size. Unfiltered they fire on the first framebuffer of the pass, which for a pass that opens a
// video is the frame converter's, built during the first decode -- not the monitor's images.
CallGate g_framebufferGate;      // the framebuffer that finishes a rebuilt image of the armed size
CallGate g_framebufferFailGate;  // the same call, failed; checked after the hold, so both can apply
CallGate g_framebufferLostGate;  // the same call, failed as the DEVICE itself: a GPU fault, not a shortage
CallGate g_decodeGate;           // a packet handed to the decoder: the span the picture is decoded in
CallGate g_freeCodecGate;        // the decoder's codec context being freed: a session's teardown
CallGate g_submitFailGate;       // a queue submission, failed
CallGate g_closeFileGate;        // a wallpaper file's descriptor being closed, by path: a session's end

// Queue submissions to be failed as VK_ERROR_DEVICE_LOST -- which the daemon sorts as the GPU's
// own fault, where the shortage g_submitFailGate injects is sorted as a resource running out. The
// difference is the whole of "a GPU fault never retries", so the two cannot share a gate.
std::atomic<int> g_submitDeviceLost{0};

// Image creations to be failed as VK_ERROR_OUT_OF_DEVICE_MEMORY. Only the daemon's own: ffmpeg
// reaches Vulkan through a function table it fetched from the loader, which no definition here can
// interpose, so an armed count is spent on the picture converter's image inside the decode that
// opens a session -- which is the shortage the retry-once path exists for.
std::atomic<int> g_failImageCreates{0};

// Packets sent to the wallpaper's decoder since the last reset. What makes "the decode happened
// before the images were rebuilt" an observation rather than an inference.
std::atomic<int> g_decodeCalls{0};
std::atomic<int> g_codecFrees{0};

// The wallpaper files the daemon has open, keyed by the descriptor it reads each through, and how
// often each path was opened and closed since the last reset.
//
// Keyed on the descriptor, which the kernel reuses after a close. That is safe only because every
// close in this process goes through the hook below, so a number is removed from m_live before it
// can be handed out again; a descriptor closed some other way (close_range, dup2 over it) would
// leave a stale entry, and nothing here does either.
class FileLedger {
public:
    // The paths worth recording. Everything else this process opens -- shaders, the ICD's files,
    // /proc -- is ignored, so totalOpens() stays the number several checks read it as: how many
    // times a wallpaper file was opened.
    void watch(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_watched.insert(path);
        m_armed.store(true, std::memory_order_relaxed);
    }

    // True when this open was recorded. Cheap when unarmed: one relaxed load, which is what every
    // open on every thread of this process pays.
    bool opened(int fd, const char* path) {
        if (!m_armed.load(std::memory_order_relaxed) || path == nullptr || fd < 0) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto watched = m_watched.find(path);
        if (watched == m_watched.end()) return false;
        m_live[fd] = *watched;
        ++m_opens[*watched];
        return true;
    }

    // The path `fd` was opened on, or empty for a descriptor this ledger never saw.
    std::string closing(int fd) {
        if (!m_armed.load(std::memory_order_relaxed) || fd < 0) return {};
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_live.find(fd);
        if (it == m_live.end()) return {};
        std::string path = it->second;
        m_live.erase(it);
        ++m_closes[path];
        return path;
    }

    void resetCounts() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_opens.clear();
        m_closes.clear();
    }

    int opens(const std::string& path) const { return count(m_opens, path); }
    int closes(const std::string& path) const { return count(m_closes, path); }
    int totalOpens() const { return total(m_opens); }
    int totalCloses() const { return total(m_closes); }

    std::multiset<std::string> live() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::multiset<std::string> paths;
        for (const auto& [fd, path] : m_live) paths.insert(path);
        return paths;
    }

private:
    int count(const std::map<std::string, int>& from, const std::string& path) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = from.find(path);
        return it == from.end() ? 0 : it->second;
    }
    int total(const std::map<std::string, int>& from) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        int sum = 0;
        for (const auto& [path, n] : from) sum += n;
        return sum;
    }

    std::atomic<bool> m_armed{false};
    mutable std::mutex m_mutex;
    std::set<std::string> m_watched;
    std::map<int, std::string> m_live;
    std::map<std::string, int> m_opens;
    std::map<std::string, int> m_closes;
};

FileLedger g_files;

// VideoDecoder's own context, which it marks by setting `opaque`; avformat_find_stream_info opens
// contexts of its own and decodes through the same entry points to work out what is in the file.
//
// Counting those too is not a detail. They are sent while the VideoPipeline is being CONSTRUCTED,
// which happens before the images are rebuilt whichever order the decode is in -- so a counter
// that included them was non-zero at the rebuild either way, and the ordering check below passed
// with the production code deliberately reordered. Measured, not reasoned: that is what the
// control for S13.117 first produced.
bool isDecodersContext(const AVCodecContext* context) { return context != nullptr && context->opaque != nullptr; }

// Fails every picture a decoder is asked for AFTER the first one it delivered, for as long as it is
// armed. Not a CallGate: a gate fires once, and what the bounded retry is about is a file that
// fails again after every reopen -- one failure would establish nothing about the bound.
//
// Keyed on the codec context and on how many pictures that context has already delivered, because
// the open and the clock decode through the same call: the open must SUCCEED for a reopen to be a
// reopen, and it is the picture after it -- the clock's -- that must fail. Counting per context is
// what separates them without the test having to know how many packets a driver needs for a
// picture, which differs between the hardware and software decoders.
class PlaybackFailure {
public:
    // `fileFaults` of the failures are EIO -- the file's fault -- and every one after them ENOMEM,
    // which the decoder reports as the daemon running short.
    void arm(int fileFaults = std::numeric_limits<int>::max()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fileFaultsLeft = fileFaults;
        m_armed.store(true);
    }
    void disarm() { m_armed.store(false); }

    // The error to fail this picture with, or 0 to let it through.
    int failureFor(const AVCodecContext* context) {
        if (!m_armed.load()) return 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_delivered.find(context);
        if (it == m_delivered.end() || it->second == 0) return 0;
        ++m_injected;
        if (m_fileFaultsLeft > 0) {
            --m_fileFaultsLeft;
            // EIO, not AVERROR_INVALIDDATA: the decoder treats a damaged packet as something to skip
            // past, which is a picture lost rather than playback failing.
            return AVERROR(EIO);
        }
        ++m_shortagesInjected;
        return AVERROR(ENOMEM);
    }

    bool hasDelivered(const AVCodecContext* context) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_delivered.find(context);
        return it != m_delivered.end() && it->second > 0;
    }

    int shortagesInjected() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_shortagesInjected;
    }

    void delivered(const AVCodecContext* context) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_delivered[context];
    }

    // A freed context must not leave its count behind: ffmpeg reuses the address, and a reopened
    // decoder inheriting "has already delivered one" would have its FIRST picture failed, which is
    // an open that fails rather than playback that fails.
    void forget(const AVCodecContext* context) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_delivered.erase(context);
    }

    int injected() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_injected;
    }

private:
    std::atomic<bool> m_armed{false};
    mutable std::mutex m_mutex;
    std::map<const AVCodecContext*, int> m_delivered;
    int m_injected = 0;
    int m_fileFaultsLeft = 0;
    int m_shortagesInjected = 0;
};

PlaybackFailure g_playbackFailure;

// Opens of one path made to fail with a chosen errno, `count` times -- EMFILE for the daemon running
// out of descriptors (S13.193), EACCES for a file stat() calls readable that cannot be opened
// (S13.195).
class OpenFailure {
public:
    void arm(const std::string& path, int error, int count) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_path = path;
        m_error = error;
        m_left = count;
        m_armed.store(count > 0);
    }
    void disarm() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_left = 0;
        m_armed.store(false);
    }
    // The errno to fail this open with, or 0.
    int failureFor(const char* path) {
        if (!m_armed.load(std::memory_order_relaxed) || path == nullptr) return 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_left <= 0 || m_path != path) return 0;
        if (--m_left == 0) m_armed.store(false);
        ++m_spent;
        return m_error;
    }
    int spent() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_spent;
    }
    int left() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_left;
    }

private:
    std::atomic<bool> m_armed{false};
    mutable std::mutex m_mutex;
    std::string m_path;
    int m_error = 0;
    int m_left = 0;
    int m_spent = 0;
};

OpenFailure g_openFailure;

// A mount table read in place of /proc/self/mountinfo while set: how a phase puts a wallpaper on a
// network filesystem without mounting one (S13.194, S13.195). Empty is the machine's own.
std::mutex g_mountTableMutex;
std::string g_mountTable;
std::atomic<bool> g_mountTableSet{false};

// Which wallpaper file each decoder context reads, so a death can be staged on one video's work
// and not another's. Learned at the context's first packet from the last wallpaper file opened on
// that thread: a decoder opens its file and then decodes from it on the same thread, with no other
// wallpaper opened in between.
thread_local std::string t_lastWallpaperOpened;
std::mutex g_contextFilesMutex;
std::map<const AVCodecContext*, std::string> g_contextFiles;

std::string fileOfContext(const AVCodecContext* context) {
    std::lock_guard<std::mutex> lock(g_contextFilesMutex);
    const auto it = g_contextFiles.find(context);
    return it == g_contextFiles.end() ? std::string() : it->second;
}

// The death a --blame-child stages: a real memory fault on the render thread, in the decoder's work
// on one file. Decode: a packet sent for that file's next picture, after its first was delivered --
// the clock's work, never the open's. Free: that file's decoder being freed -- a session's teardown.
//
// Only on the render worker's thread, whose id the child records: a free on any other thread is the
// manager's own destruction as the child unwinds -- a stop, not the teardown being staged -- and a
// death there would be graded as the staged one.
enum class CrashOn { Nothing, Decode, Free };
std::atomic<CrashOn> g_crashOn{CrashOn::Nothing};
std::atomic<pid_t> g_crashThread{0};
std::mutex g_crashPathMutex;
std::string g_crashPath;

void armCrash(CrashOn on, const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(g_crashPathMutex);
        g_crashPath = path;
    }
    g_crashOn.store(on);
}

bool crashDueFor(CrashOn on, const AVCodecContext* context) {
    if (g_crashOn.load() != on || ::gettid() != g_crashThread.load()) return false;
    std::string target;
    {
        std::lock_guard<std::mutex> lock(g_crashPathMutex);
        target = g_crashPath;
    }
    return !target.empty() && fileOfContext(context) == target;
}

// A store to a page mapped PROT_NONE: a genuine SIGSEGV on the calling thread, for
// crash_record_test's reason (a raise() would be a signal the thread sent itself).
[[noreturn]] void faultNow() {
    void* page = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page != MAP_FAILED) *static_cast<volatile char*>(page) = 1;
    std::_Exit(120);
}

}  // namespace

extern "C" VkResult vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
                                        const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer) {
    using Fn = VkResult (*)(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "vkCreateFramebuffer"));
    if (real == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    char extent[32] = "";
    if (info != nullptr) std::snprintf(extent, sizeof(extent), "%ux%u", info->width, info->height);
    if (g_framebufferGate.enter(extent)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (g_framebufferFailGate.enter(extent)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (g_framebufferLostGate.enter(extent)) return VK_ERROR_DEVICE_LOST;
    return real(device, info, allocator, framebuffer);
}

extern "C" VkResult vkCreateImage(VkDevice device, const VkImageCreateInfo* info,
                                  const VkAllocationCallbacks* allocator, VkImage* image) {
    using Fn = VkResult (*)(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "vkCreateImage"));
    if (real == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    for (int armed = g_failImageCreates.load(); armed > 0;) {
        if (g_failImageCreates.compare_exchange_weak(armed, armed - 1)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return real(device, info, allocator, image);
}

static std::string extentOf(uint32_t width, uint32_t height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

// The daemon's own submissions only: ffmpeg's decoder reaches the driver through its own table.
extern "C" VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
    using Fn = VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "vkQueueSubmit"));
    if (real == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (g_submitFailGate.enter()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (int armed = g_submitDeviceLost.load(); armed > 0;) {
        if (g_submitDeviceLost.compare_exchange_weak(armed, armed - 1)) return VK_ERROR_DEVICE_LOST;
    }
    return real(queue, count, infos, fence);
}

// The daemon opens a wallpaper file itself, through openWallpaperFile(), and closes that
// descriptor as the last step of a session's teardown. These two hooks are where the ledger and
// the close gate see it.
//
// The mode argument is forwarded whether or not it was passed: the real open ignores it unless
// O_CREAT or O_TMPFILE is set, and reading a va_arg that is never used is not the undefined part.
// Forwarding it blindly keeps this one definition correct for every other opener in the process,
// the ICD's and sdbus's included.
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
    if (const int error = g_openFailure.failureFor(path); error != 0) {
        errno = error;
        return -1;
    }
    if (g_mountTableSet.load(std::memory_order_relaxed) && path != nullptr &&
        std::strcmp(path, "/proc/self/mountinfo") == 0) {
        std::string table;
        {
            std::lock_guard<std::mutex> lock(g_mountTableMutex);
            table = g_mountTable;
        }
        if (!table.empty()) return real(table.c_str(), flags, mode);
    }
    const int fd = real(path, flags, mode);
    if (fd >= 0 && g_files.opened(fd, path)) t_lastWallpaperOpened = path;
    return fd;
}

extern "C" int close(int fd) {
    using Fn = int (*)(int);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "close"));
    if (real == nullptr) return -1;
    // The gate is entered only for a wallpaper file: every descriptor in the process passes
    // through here, and a gate armed on a path would otherwise be asked about each of them.
    if (const std::string path = g_files.closing(fd); !path.empty()) g_closeFileGate.enter(path.c_str());
    return real(fd);
}

extern "C" int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    using Fn = int (*)(AVCodecContext*, const AVPacket*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avcodec_send_packet"));
    if (real == nullptr) return -1;
    if (isDecodersContext(context)) {
        {
            std::lock_guard<std::mutex> lock(g_contextFilesMutex);
            g_contextFiles.try_emplace(context, t_lastWallpaperOpened);
        }
        if (crashDueFor(CrashOn::Decode, context) && g_playbackFailure.hasDelivered(context)) faultNow();
        g_decodeCalls.fetch_add(1, std::memory_order_relaxed);
        g_decodeGate.enter();
    }
    return real(context, packet);
}

extern "C" void avcodec_free_context(AVCodecContext** context) {
    using Fn = void (*)(AVCodecContext**);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avcodec_free_context"));
    if (real == nullptr) return;
    // The same filter, for the same reason: ffmpeg frees its probing contexts while the pipeline is
    // being built, so a gate that fired on the first free of any context would hold the thread
    // during construction and never see a session's teardown at all.
    if (context != nullptr && isDecodersContext(*context)) {
        if (crashDueFor(CrashOn::Free, *context)) faultNow();
        g_codecFrees.fetch_add(1, std::memory_order_relaxed);
        g_playbackFailure.forget(*context);
        {
            std::lock_guard<std::mutex> lock(g_contextFilesMutex);
            g_contextFiles.erase(*context);
        }
        g_freeCodecGate.enter();
    }
    real(context);
}

extern "C" int avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    using Fn = int (*)(AVCodecContext*, AVFrame*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "avcodec_receive_frame"));
    if (real == nullptr) return -1;
    if (isDecodersContext(context)) {
        if (const int failure = g_playbackFailure.failureFor(context); failure != 0) return failure;
    }
    const int result = real(context, frame);
    if (result == 0 && isDecodersContext(context)) g_playbackFailure.delivered(context);
    return result;
}

namespace {

using Clock = std::chrono::steady_clock;
using wallpaperx::testing::getFrameFd;
using wallpaperx::testing::getStatusOrExplain;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::StatusReply;
using wallpaperx::testing::validationWaived;
using wallpaperx::testing::wallpaperStatus;

// Deliberately not square, and deliberately a different aspect from the fixture's: a monitor whose
// shape matches the clip's is covered without cropping anything, and every wrong crop would then
// produce the right picture.
constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 24;

// What a screen reports for the resize phases. Different in both axes, so a rebuild is forced.
constexpr uint32_t kResizedWidth = 96;
constexpr uint32_t kResizedHeight = 48;
constexpr uint32_t kResizedAgainWidth = 80;
constexpr uint32_t kResizedAgainHeight = 40;

// The fixture: four horizontal bands over 320x240. Covering a 64x24 monitor keeps the middle half
// of the rows, so a correct draw shows green then yellow and none of the red or blue.
constexpr uint32_t kClipWidth = 320;
constexpr uint32_t kClipHeight = 240;

constexpr auto kGateLimit = std::chrono::seconds(30);

struct Band {
    uint32_t firstRow;
    uint32_t lastRow;  // exclusive
    int red;
    int green;
    int blue;
    const char* name;
};

// The bands the fixture is generated with, in backend/CMakeLists.txt. Stated once here and read by
// the pixel check, so a fixture recipe that changed without this changing is a failed check rather
// than a check that quietly grades a different picture.
constexpr Band kBands[] = {
    {0, 60, 255, 0, 0, "red"},
    {60, 120, 0, 255, 0, "green"},
    {120, 180, 255, 255, 0, "yellow"},
    {180, 240, 0, 0, 255, "blue"},
};

const Band* bandAtRow(double sourceRow) {
    for (const Band& band : kBands) {
        if (sourceRow >= band.firstRow && sourceRow < band.lastRow) return &band;
    }
    return nullptr;
}

std::string testAssetDir() { return std::string(WALLPAPERX_TEST_IMAGE_DIR); }
std::string clipPath() { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_bands.mp4"; }

// The same bands in a file holding exactly one picture. The clock parks a session opened on it
// after that picture and never decodes from it again, so on the render worker's thread the pass
// under test is the only thing that can open a file or send a packet while a phase is counting.
std::string onePicturePath() { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_one_picture.mp4"; }

// The same bands again at one picture every two seconds, well over twice the unwatched interval
// this file's daemon runs with. The only fixture that can tell "the wait runs from the oldest uncollected
// picture" apart from "the wait runs from the last hand-over": at ten pictures a second the two
// rules agree.
std::string slowClipPath() { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_slow.mp4"; }

// The two fixtures the interval clamps are graded with, and the only ones whose declared timing is
// outside what the daemon will honour: 240 pictures timed at a thousand a second, and three
// pictures eight seconds apart.
std::string fastClipPath() { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_fast.mp4"; }
std::string gapClipPath() { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_gap.mp4"; }
std::string rate120ClipPath() { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_rate120_ms.mkv"; }

// How many pictures each fixture holds, as backend/CMakeLists.txt generates them. Stated here so a
// recipe that changed without this changing is a failed check rather than a check grading a
// different file.
constexpr int kClipPictures = 10;
constexpr int kFastClipPictures = 240;
constexpr double kFastClipDeclaredFps = 1000.0;
constexpr double kGapClipDeclaredGapSeconds = 8.0;

// The clamps the daemon puts on the gap between two pictures, mirrored from
// backend/daemon/wallpaper_manager.cpp's file-local constexprs -- there is no header to include
// and adding one for a test's benefit is forbidden. The mirror is load-bearing: the fixtures above
// are placed outside these bounds on purpose, so a change to either constant in production alone
// moves the real edge off the fixtures and reddens the phase that uses them.
constexpr double kMinPictureIntervalSeconds = 1.0 / 120.0;
constexpr double kMaxPictureIntervalSeconds = 5.0;
static_assert(1.0 / kFastClipDeclaredFps < kMinPictureIntervalSeconds,
              "the fast fixture must declare a gap SHORTER than the floor, or the floor is never reached and the "
              "rate it caps is never established");
static_assert(kGapClipDeclaredGapSeconds > kMaxPictureIntervalSeconds,
              "the gap fixture must declare a gap LONGER than the ceiling, or the ceiling is never reached");

// A file that exists, is a regular file and is readable, and is not a video. SetWallpaper applies
// the readable-regular-file check to every known type, so a path that was never created is
// rejected on the reply and never reaches the failed-render state S13.116 is about.
std::string writeUndecodable(const std::string& name) {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "this is not a video, and never was";
    return path;
}

// Runs an empty task on the render worker and waits for it. The worker runs one clock round after
// every job, so a drain that returns means: the queue was emptied, and at least one clock round has
// finished since the job before this task. That is the barrier every count-inside-a-pass check
// below is opened with -- a sleep would be an assertion on the scheduler, and there is no state the
// test can read that says "the clock is between rounds".
bool drainWorker(RenderWorker& worker) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> future = done->get_future();
    worker.postTask([done](RenderWorker::TaskState) { done->set_value(); });
    return future.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
}

std::optional<wallpaperx::MonitorStatus> monitorStatus(const WallpaperManager& manager, int monitorId) {
    const WallpaperManager::Snapshot snap = manager.snapshot();
    const auto it = snap.monitors.find(monitorId);
    if (it == snap.monitors.end()) return std::nullopt;
    return it->second;
}

// A phase's own Device graded for validation messages, in a destructor so an exception out of the
// phase cannot skip it, and with the counter snapshotted as the manager's scope unwinds -- the
// layer's teardown complaints come from ~WallpaperManager, and reading a live accessor before the
// manager is gone exempts the whole destruction half.
class PhaseGrade {
public:
    explicit PhaseGrade(std::string label) : m_label(std::move(label)) {}
    PhaseGrade(const PhaseGrade&) = delete;
    PhaseGrade& operator=(const PhaseGrade&) = delete;

    void capture(const WallpaperManager& manager) {
        m_counter = manager.validationCounter();
        m_layerActive = manager.validationLayerActive();
    }

    // For a phase that legitimately never built a Device and has already said so.
    void expectNoDevice() { m_deviceOptional = true; }

    ~PhaseGrade() {
        int failures = 0;
        int ungraded = 0;
        {
            wallpaperx::testing::ValidationGrade grade("video_wallpaper_test (" + m_label + ")", failures);
            if (validationWaived() || m_deviceOptional) grade.expectNoDevice();
            grade.reportUngradedTo(ungraded);
            grade.adopt(m_counter, m_layerActive);
        }
        if (ungraded > 0) {
            std::fprintf(stderr,
                         "video_wallpaper_test: %s's validation grade could not be made -- "
                         "VK_LAYER_KHRONOS_validation is not loaded. The phase itself is still judged.\n",
                         m_label.c_str());
        }
        check(failures == 0, ("video_wallpaper_test: " + m_label + "'s own device passed its validation grade").c_str());
        // Printed, never asserted: how long each phase takes is what says where this entry's run
        // time goes, and a run that fails intermittently under load needs it to say which phase.
        std::printf("video_wallpaper_test: %s took %.2f s\n", m_label.c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count());
        std::fflush(stdout);
    }

private:
    std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
    std::string m_label;
    std::shared_ptr<const std::atomic<int>> m_counter;
    bool m_layerActive = false;
    bool m_deviceOptional = false;
};

struct CaptureGradeOnExit {
    PhaseGrade& grade;
    const WallpaperManager& manager;
    ~CaptureGradeOnExit() { grade.capture(manager); }
};

// Points the validation layer at a settings file that excuses ffmpeg's one known message by id.
//
// Constructed before any Device: the layer reads its settings at vkCreateInstance, so an override
// set afterwards changes nothing for an instance already made.
class DecoderMessageExcuse {
public:
    explicit DecoderMessageExcuse(const std::filesystem::path& dir) {
        m_file = dir / "vk_layer_settings.txt";
        std::ofstream out(m_file);
        out << "khronos_validation.message_id_filter = " << wallpaperx::testing::kExcusedDecoderMessageId << "\n";
        out.close();
        if (!out) return;
        m_applied = ::setenv("VK_LAYER_SETTINGS_PATH", m_file.c_str(), 1) == 0;
    }

    bool applied() const { return m_applied; }
    const std::filesystem::path& file() const { return m_file; }

private:
    std::filesystem::path m_file;
    bool m_applied = false;
};

// --------------------------------------------------------------------------- //
// Over the bus: a video wallpaper is drawn, served and reported
// --------------------------------------------------------------------------- //

constexpr int32_t kMonitorClip = 0;
constexpr int32_t kMonitorStill = 1;

// What a hand-over's bytes said, or why they could not be read.
struct PixelVerdict {
    bool answerable = false;
    std::string reason;
    int compared = 0;
    int wrong = 0;
    std::string firstWrong;
};

// What the pixel check needs from a hand-over, whether it came over the bus or straight from the
// manager.
struct FrameLayout {
    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    int handleType = 0;  // the wire's numbering: 1 is a dma_buf
    uint64_t modifier = 0;
    bool hasPlane = false;
    uint64_t planeOffset = 0;
    uint64_t rowPitch = 0;
};

FrameLayout layoutOf(const wallpaperx::testing::Frame& frame) {
    FrameLayout layout{frame.fd.get(), frame.width, frame.height, frame.memorySize, frame.handleType,
                       frame.drmModifier};
    if (!frame.planes.empty()) {
        layout.hasPlane = true;
        layout.planeOffset = frame.planes[0].get<0>();
        layout.rowPitch = frame.planes[0].get<1>();
    }
    return layout;
}

FrameLayout layoutOf(const wallpaperx::ExportedFrame& frame) {
    FrameLayout layout{frame.fd,
                       frame.width,
                       frame.height,
                       frame.memorySize,
                       frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf ? 1 : 0,
                       frame.modifier};
    if (!frame.planes.empty()) {
        layout.hasPlane = true;
        layout.planeOffset = frame.planes[0].offset;
        layout.rowPitch = frame.planes[0].rowPitch;
    }
    return layout;
}

PixelVerdict gradeCoverCrop(const FrameLayout& frame) {
    PixelVerdict verdict;
    if (frame.handleType != 1 || frame.modifier != wallpaperx::testing::kDrmModifierLinear || !frame.hasPlane) {
        verdict.reason = "this driver's export is not a LINEAR dma_buf (handle type " +
                         std::to_string(frame.handleType) + ", modifier 0x" +
                         [&] {
                             char buf[32];
                             std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(frame.modifier));
                             return std::string(buf);
                         }() +
                         "), so no client can address its pixels";
        return verdict;
    }
    const wallpaperx::testing::MappedFrame mapped(frame.fd, frame.memorySize);
    if (!mapped.valid()) {
        verdict.reason = "the exported fd could not be mapped: " + mapped.error();
        return verdict;
    }
    const uint64_t planeOffset = frame.planeOffset;
    const uint64_t rowPitch = frame.rowPitch;
    const wallpaperx::testing::CropRect crop =
        wallpaperx::testing::expectedCover(kClipWidth, kClipHeight, frame.width, frame.height);

    for (uint32_t row = 0; row < frame.height; ++row) {
        // Where this monitor row samples the clip, from the cover rule in real arithmetic --
        // never from the renderer's own integer form, which would agree with itself however
        // both were wrong.
        const double sourceRow = crop.y0 + (row + 0.5) * crop.height / frame.height;
        const Band* band = bandAtRow(sourceRow);
        if (band == nullptr) continue;
        // A band boundary lands between two rows of chroma, so the two rows either side of one
        // are a blend of both colours and say nothing. Eight source rows is generous against a
        // 60-row band: what is skipped is the seam, not the band.
        const double toBoundary =
            std::min(sourceRow - static_cast<double>(band->firstRow), static_cast<double>(band->lastRow) - sourceRow);
        if (toBoundary < 8.0) continue;

        const unsigned char* pixels = mapped.row(planeOffset, rowPitch, row, static_cast<size_t>(frame.width) * 4);
        if (pixels == nullptr) {
            verdict.reason = "the frame's own published layout puts row " + std::to_string(row) +
                             " outside the memory it exported";
            return verdict;
        }
        const size_t column = frame.width / 2;
        const int blue = pixels[column * 4 + 0];
        const int green = pixels[column * 4 + 1];
        const int red = pixels[column * 4 + 2];
        ++verdict.compared;
        // 60 codes. The bands differ from each other by 255 in at least one channel, so a wrong band
        // cannot pass at any tolerance below 128; what this has to absorb is the 4:2:0 limited-range
        // round trip, which moves a primary by ~40 before the renderer's own conversion.
        const bool matches = std::abs(red - band->red) <= 60 && std::abs(green - band->green) <= 60 &&
                             std::abs(blue - band->blue) <= 60;
        if (!matches) {
            ++verdict.wrong;
            if (verdict.firstWrong.empty()) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "monitor row %u samples clip row %.1f, which is %s (%d,%d,%d), but reads (%d,%d,%d)",
                              row, sourceRow, band->name, band->red, band->green, band->blue, red, green, blue);
                verdict.firstWrong = buf;
            }
        }
    }
    verdict.answerable = true;
    return verdict;
}

void runBusChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& clip, const std::string& still) {
    // The still first, so it is already showing when the video is set: what "a still wallpaper on
    // another monitor is unaffected" is measured against.
    setWallpaper(proxy, "static", still, kMonitorStill);
    if (!signals.wait(kMonitorStill, 1)) {
        notVerified("S13.114 and S13.115",
                    "the still wallpaper this phase compares against never rendered on this host");
        return;
    }
    const StatusReply beforeClip = getStatusOrExplain(proxy);
    const nlohmann::json* stillBefore = wallpaperStatus(beforeClip, std::to_string(kMonitorStill));
    const uint64_t stillGeneration = stillBefore != nullptr ? stillBefore->value("generation", uint64_t{0}) : 0;

    // GOAL: S13.114 -- a `video` wallpaper is drawn. SetWallpaper("video", ...) commits a frame,
    //       GetFrameFd serves it, and GetStatus reports the monitor as renderable with a frame and
    //       no error. False for a build that accepts a video and then never draws one, which is
    //       exactly what this daemon did until Stage E1 and what every other type it knows still
    //       does.
    // WHY-THIS-SHAPE: the FrameReady signal is what says the frame exists, not a sleep and not a
    //       poll: SetWallpaper returns before the render is attempted, so any check made on the
    //       reply alone is equally true of a daemon that draws nothing. `unsupported` is asserted
    //       to be false as well as the frame being served, because those are two published
    //       surfaces derived from one predicate and a client branches on either.
    // CONTROL: drawableType() in backend/daemon/wallpaper_manager.h with its `video` arm removed, which
    //       is the daemon one commit before this stage -> "FAIL: a video wallpaper commits a frame" and
    //       five more, including the undecodable-video comparison and the decode gate; the phases that
    //       then had nothing to set up added no FAIL line. Observed 2026-09-15, and again 2026-09-16
    //       with the session phases present. Reverted.
    const bool drawn = [&] {
        setWallpaper(proxy, "video", clip, kMonitorClip);
        return signals.wait(kMonitorClip, 1);
    }();
    check(drawn, "a video wallpaper commits a frame");
    if (!drawn) {
        const StatusReply failed = getStatusOrExplain(proxy);
        if (const nlohmann::json* entry = wallpaperStatus(failed, std::to_string(kMonitorClip))) {
            std::fprintf(stderr, "  the video monitor reports: unsupported=%s last_error=%s\n",
                         entry->value("unsupported", false) ? "true" : "false",
                         entry->value("last_error", std::string("(none)")).c_str());
        }
        return;
    }

    const StatusReply after = getStatusOrExplain(proxy);
    const nlohmann::json* clipEntry = wallpaperStatus(after, std::to_string(kMonitorClip));
    check(clipEntry != nullptr, "GetStatus carries the video monitor");
    if (clipEntry != nullptr) {
        check(!clipEntry->value("unsupported", true), "GetStatus does not call a video wallpaper unsupported");
        check(clipEntry->value("has_frame", false), "GetStatus reports the video monitor as having a frame");
        check(!clipEntry->contains("last_error"), "a video wallpaper that drew leaves no last_error");
        check(clipEntry->value("type", std::string()) == "video", "and the recorded type is still the one asked for");
    }

    const wallpaperx::testing::Frame frame = getFrameFd(proxy, kMonitorClip);
    check(frame.fd.get() >= 0, "GetFrameFd serves the video monitor's frame");
    check(frame.generation >= 1, "the served frame names a generation");

    // GOAL: S13.115's first half -- the picture is drawn at the monitor's target size, not at the
    //       clip's. False for a daemon that sizes the frame from the decoded picture, which would
    //       hand the plugin a 320x240 frame for a 64x24 screen.
    // WHY-THIS-SHAPE: the frame's own published width and height, which is what an importer builds
    //       its texture from; the target size is deliberately a different aspect AND both axes
    //       differ from the clip's, so a swap or a passthrough cannot satisfy it.
    // CONTROL: no mutation of the crop reddens this one -- a stretched picture is still served at the
    //       target size -- so it is controlled by the same removal of drawableType()'s `video` arm as the
    //       check above, which leaves no frame to measure at all. What it forbids is a frame sized from
    //       the decoded picture, and nothing in this build produces one to mutate toward.
    const bool sized = frame.width == kTargetWidth && frame.height == kTargetHeight;
    check(sized, "the video frame is served at the monitor's target size, not the clip's");
    if (!sized) {
        std::fprintf(stderr, "  served %ux%u, target is %ux%u, the clip is %ux%u\n", frame.width, frame.height,
                     kTargetWidth, kTargetHeight, kClipWidth, kClipHeight);
    }

    // GOAL: S13.115's second half -- the picture is cover-cropped, as a still image is: scaled
    //       uniformly to cover the monitor, centred, the overflow cropped. False for a stretched,
    //       letterboxed, corner-anchored or mirrored draw.
    // WHY-THIS-SHAPE: the fixture is four bands, and a 64x24 monitor covered by a 320x240 clip
    //       shows only the middle two, so every wrong rule puts a band on screen that the right
    //       one crops away -- a stretch shows all four, a corner anchor shows red, a vertical
    //       mirror swaps green and yellow. The expected band per row comes from
    //       support/cover_expectation.h, the cover rule in real arithmetic, never from the
    //       renderer's own crop. Sampling one column is enough because the bands span the width.
    // CONTROL: renderer/renderers/cover_crop.h's coverCrop() made to return the whole source rectangle,
    //       which is the stretch commit 602281a replaced -> on the Lavapipe entry "FAIL: the video
    //       picture is cover-cropped to the monitor, like a still image", printing "8 of 16 sampled rows
    //       show the wrong band; first: monitor row 2 samples clip row 72.5, which is green (0,255,0),
    //       but reads (255,24,0)" -- the red band that a correct crop removes. On the NVIDIA entry the
    //       same mutation printed the abstention below instead of passing. Observed 2026-09-15. Reverted.
    // COVERAGE-ASSERT: the number of rows actually compared is asserted, so a crop that moved the
    //       sampled rows onto a band boundary (where the check abstains) cannot quietly empty it.
    const PixelVerdict pixels = gradeCoverCrop(layoutOf(frame));
    if (!pixels.answerable) {
        notVerified("S13.115's pixel half", pixels.reason);
    } else {
        check(pixels.compared >= 16,
              "enough monitor rows fell inside a band to grade the crop (the pixel check is not empty)");
        check(pixels.wrong == 0, "the video picture is cover-cropped to the monitor, like a still image");
        if (pixels.wrong > 0) {
            std::fprintf(stderr, "  %d of %d sampled rows show the wrong band; first: %s\n", pixels.wrong,
                         pixels.compared, pixels.firstWrong.c_str());
        } else {
            std::printf("video_wallpaper_test: %d monitor rows checked against the cover crop, all correct\n",
                        pixels.compared);
        }
    }

    // GOAL: S13.114's last clause -- drawing a video on one monitor leaves a still wallpaper on
    //       another untouched: same generation, still served. False if the new drawer disturbs
    //       another monitor's images or its published frame.
    // WHY-THIS-SHAPE: the generation is compared against the value read before the video was set,
    //       and the frame is re-fetched -- not just read from status. A monitor whose images were
    //       released still reports its old generation, so only asking for the frame back
    //       distinguishes "unaffected" from "still says it has one".
    const StatusReply stillAfter = getStatusOrExplain(proxy);
    const nlohmann::json* stillEntry = wallpaperStatus(stillAfter, std::to_string(kMonitorStill));
    const bool untouched = stillEntry != nullptr && stillEntry->value("has_frame", false) &&
                           stillEntry->value("generation", uint64_t{0}) == stillGeneration &&
                           !stillEntry->contains("last_error");
    check(untouched, "a still wallpaper on another monitor is unchanged by the video render");
    const wallpaperx::testing::Frame stillFrame = getFrameFd(proxy, kMonitorStill);
    check(stillFrame.fd.get() >= 0 && stillFrame.generation == stillGeneration,
          "and its frame is still served, at the generation it had before");
    check(signals.countFor(kMonitorStill) == 1, "and nothing re-rendered it");
}

// --------------------------------------------------------------------------- //
// No bus, no worker: an undecodable video fails like an undecodable image
// --------------------------------------------------------------------------- //

constexpr int kMonitorBadClip = 0;
constexpr int kMonitorBadImage = 1;

struct FailureShape {
    bool recorded = false;
    std::string lastError;
    bool daemonFault = false;
    bool hasFrame = true;
    wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
};

void reportUnless(bool ok, const char* what, const std::string& detail) {
    check(ok, what);
    if (!ok) std::fprintf(stderr, "  %s\n", detail.c_str());
}

FailureShape failOnce(WallpaperManager& manager, int monitorId, const char* type, const std::string& path) {
    FailureShape shape;
    manager.requestWallpaper(type, path, monitorId);
    manager.renderPending(monitorId);
    const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, monitorId);
    if (!status) return shape;
    shape.recorded = true;
    shape.lastError = status->lastError;
    shape.daemonFault = status->lastErrorIsDaemonFault;
    shape.hasFrame = status->hasFrame;
    shape.reason = manager.exportCurrentFrame(monitorId).reason;
    return shape;
}

void runUndecodableChecks(const WallpaperManager::Config& config, const std::string& junk) {
    const char* kGoal = "S13.116 (an undecodable video fails like an undecodable image)";
    PhaseGrade grade("the undecodable phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    // GOAL: S13.116 -- a file that is not decodable as a video fails exactly as an undecodable
    //       image does: last_error is set, no frame, GetFrameFd answers RenderFailed, and the GPU
    //       is NOT marked failed. False for a daemon that reports a bad file as its own fault --
    //       which would make /health fail and a watchdog restart a daemon that is working -- and
    //       false for one that keeps a stale frame beside the error.
    // WHY-THIS-SHAPE: the two types are driven through the same call and their answers compared
    //       field by field, rather than each being asserted against a literal. The property is
    //       "the same as a still", so stating it as a comparison is what makes a video that fails
    //       in some *other* correct-looking way red. The GPU health is read from the manager's own
    //       snapshot: GetStatus publishes the device's name and tier but not its health, so over
    //       the bus this clause is not observable at all.
    // CONTROL: renderer/video/video_decoder.cpp's throwFileFailure() raising GpuFailure instead of
    //       WallpaperContentError, so the video fails as a fault while the still image still fails as
    //       content -> "FAIL: and is the file's fault, not
    //       the daemon's, exactly as an undecodable image is" and "FAIL: a video the daemon cannot
    //       decode does not mark the GPU failed". Re-observed 2026-09-21. Reverted.
    const FailureShape clip = failOnce(manager, kMonitorBadClip, "video", junk);
    const FailureShape image = failOnce(manager, kMonitorBadImage, "static", junk);
    if (!clip.recorded || !image.recorded) {
        notVerified(kGoal, "neither request was recorded, so there is nothing to compare");
        return;
    }
    if (image.lastError.empty()) {
        // The comparison is against the still image's behaviour, so a host where the still does
        // not fail either has nothing to compare with -- and that is a harness fact, not a video
        // defect.
        grade.expectNoDevice();
        notVerified(kGoal, "an undecodable image did not fail on this host, so the shape being compared to is absent");
        return;
    }

    check(!clip.lastError.empty(), "an undecodable video records a last_error");
    check(!clip.hasFrame, "and leaves no frame");
    check(clip.reason == wallpaperx::NoFrameReason::RenderFailed, "and a frame request answers RenderFailed");
    check(clip.reason == image.reason, "the same no-frame reason an undecodable image gives");
    check(clip.daemonFault == image.daemonFault && !clip.daemonFault,
          "and is the file's fault, not the daemon's, exactly as an undecodable image is");

    const WallpaperManager::Snapshot snap = manager.snapshot();
    check(snap.health.gpu.status != wallpaperx::HealthStatus::Fail,
          "a video the daemon cannot decode does not mark the GPU failed");
    if (snap.health.gpu.status == wallpaperx::HealthStatus::Fail) {
        std::fprintf(stderr, "  gpu health: fail -- %s\n", snap.health.gpu.detail.c_str());
    }
    std::printf("video_wallpaper_test: an undecodable video reports -- %s\n", clip.lastError.c_str());

    // GOAL: S13.182 at the daemon -- a rejection keeps its REASON however long the path is. The
    //       daemon keeps only the first 1024 bytes of a recorded error, so the reason has to come
    //       before the path: a path-first message about a deeply nested file records nothing but
    //       the path, and the client that asks why its wallpaper failed is told only where it is.
    //       Both renderers, because each builds its own messages.
    // WHY-THIS-SHAPE: a real path longer than the kept head -- five directories of 220 characters,
    //       about 1150 bytes -- rather than asserting on word order in a short message: order is
    //       the mechanism, surviving truncation is the property, and only a path that is actually
    //       cut can tell a message that merely looks right from one that is right.
    // CONTROL: renderer/wallpaper_file.cpp, describeRejection() returning `component: path: reason`
    //       -> both red, "a video rejected at the end of a long path keeps its reason" and "and so
    //       does a still", each recorded error holding nothing but the component and the path.
    //       Reverted.
    {
        std::string deep = testAssetDir() + "/";
        for (int i = 0; i < 5; ++i) deep += std::string(220, static_cast<char>('a' + i)) + "/";
        std::error_code ec;
        std::filesystem::create_directories(deep, ec);
        const std::string longClip = deep + "not_a_clip.mp4";
        const std::string longImage = deep + "not_an_image.png";
        if (!ec) {
            std::ofstream(longClip) << "this is not a video, and never was";
            std::ofstream(longImage) << "this is not an image, and never was";
        }
        struct RemoveDeep {
            std::string root;
            ~RemoveDeep() {
                std::error_code ignored;
                std::filesystem::remove_all(root, ignored);
            }
        } removeDeep{testAssetDir() + "/" + std::string(220, 'a')};
        if (ec || longClip.size() <= 1024) {
            wallpaperx::testing::harnessFailure("could not make a wallpaper path longer than the kept head of an error");
        } else {
            constexpr int kMonitorLongClip = 4;
            constexpr int kMonitorLongImage = 5;
            const FailureShape longVideo = failOnce(manager, kMonitorLongClip, "video", longClip);
            const FailureShape longStill = failOnce(manager, kMonitorLongImage, "static", longImage);
            std::printf("video_wallpaper_test: with a %zu-byte path the recorded errors begin \"%.60s\" and "
                        "\"%.60s\"\n",
                        longClip.size(), longVideo.lastError.c_str(), longStill.lastError.c_str());
            std::fflush(stdout);
            // Found anywhere, not at a fixed place: what is graded is that the reason SURVIVED the
            // cut, which a path-first message with a short path would pass as well.
            reportUnless(longVideo.lastError.find("cannot open the wallpaper") != std::string::npos,
                         "a video rejected at the end of a long path keeps its reason, which comes before the path",
                         longVideo.lastError.substr(0, 120));
            reportUnless(longStill.lastError.find("not a decodable image") != std::string::npos,
                         "and so does a still",
                         longStill.lastError.substr(0, 120));
        }
    }
}

// --------------------------------------------------------------------------- //
// A worker, no bus: what a resize costs, and what an overtaken pass leaves behind
// --------------------------------------------------------------------------- //

constexpr int kMonitorOrder = 0;
constexpr int kMonitorRace = 1;

struct Exported {
    bool served = false;
    uint32_t width = 0;
    uint32_t height = 0;
    wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
    std::string error;
};

std::string describe(const Exported& e) {
    if (e.served) return std::to_string(e.width) + "x" + std::to_string(e.height);
    if (!e.error.empty()) return "an exception: " + e.error;
    return "no frame, reason " + std::to_string(static_cast<int>(e.reason));
}

void runResizeChecks(const WallpaperManager::Config& config, const std::string& clip) {
    const char* kGoals = "S13.117 (a resize decodes nothing; a new wallpaper decodes before the images are "
                         "rebuilt), S13.118 (an overtaken video pass leaves the published frame reachable) and "
                         "S13.123's retry clause";
    PhaseGrade grade("the resize phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    // Before the worker is destroyed, on every exit: a gate left armed or holding would park it.
    struct Unblock {
        RenderWorker& worker;
        ~Unblock() {
            g_framebufferGate.release();
            g_decodeGate.release();
            worker.stop();
        }
    } unblock{worker};

    // Waits for the frame AND for the clock round that follows it, which is the round that parks a
    // one-picture session. After this returns, nothing but a render pass can open a file or send a
    // packet -- which is what every "opens and decodes nothing" count below rests on.
    const auto firstFrame = [&](int monitorId, const std::string& name) {
        wallpaperx::testing::nameScreen(manager, monitorId, name);
        manager.requestWallpaper("video", clip, monitorId);
        worker.post(monitorId);
        return signals.wait(monitorId, 1) && drainWorker(worker);
    };
    const auto resize = [&](const std::string& name, uint32_t width, uint32_t height) {
        std::vector<int> redraw;
        manager.setOutputSize(name, width, height, redraw);
        for (const int id : redraw) worker.post(id);
    };
    const auto exportOnWorker = [&worker, &manager](int monitorId) {
        auto answer = std::make_shared<std::promise<Exported>>();
        std::future<Exported> future = answer->get_future();
        worker.postTask([answer, &manager, monitorId](RenderWorker::TaskState taskState) {
            Exported out;
            if (taskState != RenderWorker::TaskState::Run) {
                out.error = "the worker did not run the export";
            } else {
                try {
                    wallpaperx::FrameExport e = manager.exportCurrentFrame(monitorId, "");
                    if (e.handle) {
                        out.served = true;
                        out.width = e.handle->frame.width;
                        out.height = e.handle->frame.height;
                        if (e.handle->frame.fd >= 0) close(e.handle->frame.fd);
                        if (e.handle->acquireFd >= 0) close(e.handle->acquireFd);
                    } else {
                        out.reason = e.reason;
                    }
                } catch (const std::exception& ex) {
                    out.error = ex.what();
                }
            }
            answer->set_value(std::move(out));
        });
        return future;
    };

    if (!firstFrame(kMonitorOrder, "DP-ORDER")) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame to resize");
        return;
    }

    // GOAL: S13.117's first half -- a redraw at a new size of a monitor whose session is resident
    //       opens no file and decodes nothing, before the rebuild or after it, and still commits.
    //       False for a daemon that builds a decode session inside every pass.
    // WHY-THIS-SHAPE: read at the rebuild AND after the commit, because "nothing before the rebuild"
    //       alone is also true of a pass that rebuilds first and decodes after. The render thread is
    //       held inside the creation of a framebuffer of the new size, which only the monitor's
    //       rebuilt images have, so the gate firing proves this pass really rebuilt.
    //       THE FIXTURE IS THE ONE-PICTURE FILE ON PURPOSE. The obvious alternative -- the ten-picture
    //       clip these checks used before the clock existed -- cannot be counted: the clock decodes
    //       from a playing session on this same thread between the counter reset and the read, so a
    //       non-zero count no longer says the pass did it. A file whose session parks leaves the pass
    //       as the only possible opener or decoder, and firstFrame() above waits for the round that
    //       parks it.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, dropStaleVideoSessionLocked()'s keep-it
    //       `return` disabled (`&& false`), so every pass reopens its file -> "FAIL: a resize of a
    //       monitor with a resident session opens and decodes nothing", printing "1 open(s), 1
    //       packet(s) by the rebuild; 1 open(s), 1 packet(s) by the commit". Observed 2026-09-18.
    //       Reverted.
    // COVERAGE-ASSERT: the gate is asserted to have fired, so "nothing was decoded" is never
    //       concluded from a rebuild that never happened.
    g_decodeCalls.store(0);
    g_files.resetCounts();
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedWidth, kResizedHeight));
    resize("DP-ORDER", kResizedWidth, kResizedHeight);
    const bool rebuilding = g_framebufferGate.waitFired(kGateLimit);
    check(rebuilding, "a new size makes the render thread rebuild the video monitor's images");
    const int opensAtRebuild = g_files.totalOpens();
    const int decodesAtRebuild = g_decodeCalls.load();
    g_framebufferGate.release();
    const bool resizedCommit = signals.wait(kMonitorOrder, 2);
    check(resizedCommit, "and the redraw commits once the rebuild is let go");
    if (rebuilding && resizedCommit) {
        const bool nothing = opensAtRebuild == 0 && decodesAtRebuild == 0 && g_files.totalOpens() == 0 &&
                             g_decodeCalls.load() == 0;
        check(nothing, "a resize of a monitor with a resident session opens and decodes nothing");
        if (!nothing) {
            std::fprintf(stderr, "  %d open(s), %d packet(s) by the rebuild; %d open(s), %d packet(s) by the commit\n",
                         opensAtRebuild, decodesAtRebuild, g_files.totalOpens(), g_decodeCalls.load());
        }
    }

    // GOAL: S13.117's second half -- a pass that must open a session AND rebuild the images (a new
    //       wallpaper arriving with a new size) decodes before it rebuilds, so the frameless window
    //       costs a blit and not a decode. False for the order a single decode-and-draw call forces.
    // WHY-THIS-SHAPE: the new wallpaper and the new size are recorded before the one post, so one
    //       pass carries both. Two filters keep it honest: the gate fires only on a framebuffer of the
    //       new size, since the frame converter builds its own framebuffer during the first decode and
    //       an unfiltered gate stops there instead; and the packet counter takes only the decoder's own
    //       context, since ffmpeg's probing packets precede the rebuild in either order.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, `clip = &videoSessionLocked(...)` moved below
    //       `if (ensureTargets(monitorId, size)) rebuilt = true;` -> "FAIL: a new video wallpaper is
    //       decoded before the monitor's images are rebuilt", printing "no packet had been decoded when
    //       the image rebuild was reached". Observed 2026-09-16 on both driver entries. Reverted.
    // COVERAGE-ASSERT: the gate is asserted to have fired, as above.
    g_decodeCalls.store(0);
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedAgainWidth, kResizedAgainHeight));
    manager.requestWallpaper("video", clip, kMonitorOrder);
    resize("DP-ORDER", kResizedAgainWidth, kResizedAgainHeight);
    const bool rebuildingNew = g_framebufferGate.waitFired(kGateLimit);
    check(rebuildingNew, "a new wallpaper at a new size makes the render thread rebuild the images");
    if (rebuildingNew) {
        const int decodedFirst = g_decodeCalls.load();
        check(decodedFirst > 0, "a new video wallpaper is decoded before the monitor's images are rebuilt");
        if (decodedFirst == 0) {
            std::fprintf(stderr, "  no packet had been decoded when the image rebuild was reached\n");
        }
    }
    g_framebufferGate.release();
    check(signals.wait(kMonitorOrder, 3), "and that redraw commits once the rebuild is let go");
    // This monitor's session is new, so the round that parks it has still to run; the counters
    // below must not carry its decode.
    check(drainWorker(worker), "the worker keeps serving after the reopened session's first frame");

    // GOAL: S13.118 -- a video pass overtaken by a newer request after it replaced the monitor's
    //       images never leaves that monitor without a frame: an export queued behind it is
    //       answered with a frame, the old one or the new, and never with NotRendered.
    // WHY-THIS-SHAPE: the newer size lands while the first pass is held inside its image rebuild,
    //       and the export is queued between that pass and the newer size's own post -- the one
    //       order in which the export runs before anything but the overtaken pass could draw.
    //       Asserting on status instead would not do: has_frame is read from a different lock than
    //       the one the export takes, and it is the export that a client actually depends on.
    // CONTROL: WallpaperManager::renderPending's attempt loop (backend/daemon/wallpaper_manager.cpp)
    //       reduced to a single pass -> "FAIL: an export racing an overtaken video pass gets a frame,
    //       never none", printing "the export got no frame, reason 1" (NotRendered -- the answer this
    //       goal forbids). Re-observed 2026-09-18. Reverted.
    if (!firstFrame(kMonitorRace, "DP-RACE")) {
        check(false, "the monitor the overtaken video pass is about is drawn first");
        return;
    }
    g_decodeCalls.store(0);
    g_files.resetCounts();
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedWidth, kResizedHeight));
    resize("DP-RACE", kResizedWidth, kResizedHeight);
    const bool raceHeld = g_framebufferGate.waitFired(kGateLimit);
    check(raceHeld, "the video pass to be overtaken is held inside its image rebuild");
    if (raceHeld) {
        std::future<Exported> racing = exportOnWorker(kMonitorRace);
        resize("DP-RACE", kResizedAgainWidth, kResizedAgainHeight);
        g_framebufferGate.release();
        const bool answered = racing.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
        check(answered, "the export queued behind the overtaken video pass is answered");
        if (answered) {
            const Exported got = racing.get();
            check(got.served, "an export racing an overtaken video pass gets a frame, never none");
            if (!got.served) std::fprintf(stderr, "  the export got %s\n", describe(got).c_str());
        }

        // GOAL: S13.123's retry clause -- the passes renderPending() runs after an overtaken rebuild
        //       draw with the session already resident: no file opened, nothing decoded.
        // WHY-THIS-SHAPE: waits for generation 2, which only the retry inside the overtaken
        //       renderPending() call can commit -- the newer size's own post finds its request
        //       serviced -- so the counters cover exactly the overtaken pass and its retry. The
        //       one-picture fixture again: a playing session would decode on this thread inside the
        //       same window and no counter could say which of the two did it.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, `m_videoSessions.erase(monitorId);` added
        //       before the superseded `return RenderPass{{}, true};` in the commit block -> "FAIL: the
        //       retry after an overtaken rebuild opens and decodes nothing", printing "1 open(s), 3
        //       packet(s) across the overtaken pass and its retry". Observed 2026-09-18. Reverted.
        const bool retried = signals.wait(kMonitorRace, 2);
        check(retried, "the retry after the overtaken rebuild commits");
        if (retried) {
            const bool nothing = g_files.totalOpens() == 0 && g_decodeCalls.load() == 0;
            check(nothing, "the retry after an overtaken rebuild opens and decodes nothing");
            if (!nothing) {
                std::fprintf(stderr, "  %d open(s), %d packet(s) across the overtaken pass and its retry\n",
                             g_files.totalOpens(), g_decodeCalls.load());
            }
        }
    }
    g_framebufferGate.release();
}

// --------------------------------------------------------------------------- //
// A worker, no bus: what the crash record covers for a video
// --------------------------------------------------------------------------- //

constexpr int kMonitorRecord = 0;

bool recordOnDisk(const std::filesystem::path& stateDir, int monitorId) {
    std::error_code ec;
    return std::filesystem::exists(wallpaperx::crashRecordPath(stateDir, monitorId), ec) && !ec;
}

void runCrashRecordSpanChecks(const WallpaperManager::Config& base, const std::string& clip) {
    const char* kGoals = "S13.119 (the crash record covers a video's decode and a replaced session's teardown)";
    const wallpaperx::testing::ScratchDir state("videostate");
    if (!state.ok()) {
        notVerified(kGoals, "no scratch state directory could be created");
        return;
    }
    PhaseGrade grade("the crash record phase");
    FrameReadyLog signals;
    WallpaperManager::Config config = base;
    config.stateDir = state.path();
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    struct Unblock {
        RenderWorker& worker;
        ~Unblock() {
            g_decodeGate.release();
            g_freeCodecGate.release();
            worker.stop();
        }
    } unblock{worker};

    // GOAL: S13.119's first half -- while a video's picture is being decoded, a crash record naming
    //       that monitor, the type `video` and the file is on disk, so a start after a fatal fault
    //       inside ffmpeg refuses that wallpaper instead of applying it again. False if the record
    //       is written only around the draw, which is what leaves a decoder crash to repeat on
    //       every start -- the crash loop this feature exists to break.
    // WHY-THIS-SHAPE: the render thread is held inside the very call that hands a packet to the
    //       decoder, and the record is read from this thread while it is held. A check before or
    //       after the pass cannot see the span; the record is removed by the end of the pass, so
    //       "it was there afterwards" is not available as evidence. The record's contents are read
    //       too, not just its existence: a record naming the wrong type would refuse nothing.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `const ApplyingRecord applying(...)` line
    //       deleted from videoSessionLocked()'s attempt -> "FAIL: a crash record is on disk while the
    //       video is being decoded", and the eviction phase's positive reading ("and a record naming
    //       the new file IS on disk ..."), nothing else. Re-observed 2026-09-21 on the NVIDIA entry.
    //       Reverted.
    g_decodeGate.arm(CallGate::Mode::Hold);
    wallpaperx::testing::nameScreen(manager, kMonitorRecord, "DP-REC");
    manager.requestWallpaper("video", clip, kMonitorRecord);
    worker.post(kMonitorRecord);
    const bool decoding = g_decodeGate.waitFired(kGateLimit);
    check(decoding, "the render thread reaches the decode of a video wallpaper");
    if (decoding) {
        const bool onDisk = recordOnDisk(state.path(), kMonitorRecord);
        check(onDisk, "a crash record is on disk while the video is being decoded");
        if (onDisk) {
            const std::optional<wallpaperx::CrashRecord> record =
                wallpaperx::readCrashRecord(state.path(), kMonitorRecord);
            const bool names = record.has_value() && record->type == "video" && record->path == clip;
            check(names, "and it names the video and the file being applied");
            if (!names && record.has_value()) {
                std::fprintf(stderr, "  the record names type '%s' path '%s'\n", record->type.c_str(),
                             record->path.c_str());
            }
        }
    }
    g_decodeGate.release();
    const bool committed = signals.wait(kMonitorRecord, 1);
    check(committed, "the held decode goes on to commit its frame once it is let go");
    if (!committed) {
        grade.expectNoDevice();
        notVerified(kGoals, "the video render did not commit on this host");
        return;
    }

    // GOAL: S13.119's second half, RESTATED for Stage E4 -- a session dropped because its monitor
    //       was given a wallpaper again writes NO crash record while its decoder is torn down. What
    //       a record is for is the span in which the FILE'S CONTENT runs: its open, its first decode
    //       and its draw, which all keep theirs. A teardown is past all three, and Stage E4 frees a
    //       session five seconds after any screen goes dark -- so a record here would refuse a
    //       wallpaper that has been playing for hours because the machine lost power during a lock.
    //       False for the Stage E3 behaviour, which wrote one.
    // WHY-THIS-SHAPE: an absent record is only evidence beside a demonstration that one can be
    //       present, so this reads the same state directory and the same monitor that the decode
    //       half above has just proved a record IS written for. Without that pairing the check would
    //       pass for a build whose crash records had stopped working entirely.
    //       The thread is held inside the decoder's codec context being freed, inside
    //       ~VideoPipeline. The same path is requested again on purpose: a new wallpaper with the
    //       same path must still drop the session (S13.124), and the first decoder free in that pass
    //       is that drop, since it runs before the new session is opened. The count of frees is
    //       printed so a run where that stopped being true is visible.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, closeVideoSessionLocked() given back the
    //       Stage E3 record -- `std::optional<ApplyingRecord> applying; try {
    //       applying.emplace(m_config.stateDir, monitorId, "video", session.path); } catch (const
    //       std::exception&) {}` before `session.pipeline.reset();` -> "FAIL: no crash record is on
    //       disk while a replaced session's decoder is torn down", printing "the record on disk:
    //       video <the clip>". Observed 2026-09-20. Reverted.
    // The clock round that parks this phase's one-picture session runs after the job that drew it,
    // and it decodes; the gate below must not be armed while that round is still to come, or the
    // teardown it catches could be the decoder's rather than the replaced session's.
    check(drainWorker(worker), "the worker answers a task before the replaced session is torn down");
    g_codecFrees.store(0);
    g_freeCodecGate.arm(CallGate::Mode::Hold);
    manager.requestWallpaper("video", clip, kMonitorRecord);
    worker.post(kMonitorRecord);
    const bool tearingDown = g_freeCodecGate.waitFired(kGateLimit);
    check(tearingDown, "the render thread reaches the teardown of the replaced session's decoder");
    if (tearingDown) {
        const std::optional<wallpaperx::CrashRecord> record = wallpaperx::readCrashRecord(state.path(), kMonitorRecord);
        check(!record.has_value(), "no crash record is on disk while a replaced session's decoder is torn down");
        if (record.has_value()) {
            std::fprintf(stderr, "  the record on disk: %s %s\n", record->type.c_str(), record->path.c_str());
        }
        std::printf("video_wallpaper_test: %d codec context(s) freed by the time the teardown was reached\n",
                    g_codecFrees.load());
    }
    g_freeCodecGate.release();
    check(signals.wait(kMonitorRecord, 2), "and the pass finishes once the teardown is let go");

    // The record does not outlive the pass that wrote it: a record left behind would refuse this
    // wallpaper at the next start, on a render that succeeded.
    check(!recordOnDisk(state.path(), kMonitorRecord), "and no record is left behind once the pass has finished");
}

// --------------------------------------------------------------------------- //
// Stage E2: one decode session per monitor
// --------------------------------------------------------------------------- //

std::string screenOf(int monitorId) { return "DP-S" + std::to_string(monitorId); }

std::string describeFiles(const std::multiset<std::string>& paths) {
    std::string out;
    for (const std::string& path : paths) {
        if (!out.empty()) out += ", ";
        out += std::filesystem::path(path).filename().string();
    }
    return out.empty() ? "none" : out;
}

bool contains(const std::vector<int>& ids, int id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

// What one pass cost, read from the hooks.
struct PassCost {
    wallpaperx::RenderOutcome outcome;
    int opens = 0;
    int closes = 0;
    int decodes = 0;
};

std::string describe(const PassCost& cost) {
    return std::string(cost.outcome.rendered ? "committed" : "did not commit") + ", " + std::to_string(cost.opens) +
           " open(s), " + std::to_string(cost.closes) + " close(s), " + std::to_string(cost.decodes) + " packet(s)";
}

// On the calling thread, which must be the one the manager's Device is pinned to.
PassCost renderCounted(WallpaperManager& manager, int monitorId) {
    g_files.resetCounts();
    g_decodeCalls.store(0);
    PassCost cost;
    cost.outcome = manager.renderPending(monitorId);
    cost.opens = g_files.totalOpens();
    cost.closes = g_files.totalCloses();
    cost.decodes = g_decodeCalls.load();
    return cost;
}

// The monitor's current frame against the cover crop, where the export can be mapped.
void gradeServedPicture(WallpaperManager& manager, int monitorId, const char* goal, const char* what) {
    wallpaperx::FrameExport exported;
    try {
        exported = manager.exportCurrentFrame(monitorId);
    } catch (const std::exception& e) {
        reportUnless(false, what, std::string("the export threw: ") + e.what());
        return;
    }
    if (!exported.handle) {
        check(false, (std::string(what) + " -- the frame is not even served").c_str());
        return;
    }
    const PixelVerdict verdict = gradeCoverCrop(layoutOf(exported.handle->frame));
    if (exported.handle->frame.fd >= 0) close(exported.handle->frame.fd);
    if (exported.handle->acquireFd >= 0) close(exported.handle->acquireFd);
    if (!verdict.answerable) {
        notVerified(goal, verdict.reason);
        return;
    }
    check(verdict.compared >= 16, (std::string(what) + " -- enough rows were compared to grade it").c_str());
    reportUnless(verdict.wrong == 0, what,
                 std::to_string(verdict.wrong) + " of " + std::to_string(verdict.compared) +
                     " sampled rows show the wrong band; first: " + verdict.firstWrong);
}

// One more than the session ceiling, so the last of them is a session the daemon has no room for.
constexpr int kSessionMonitors[] = {10, 11, 12, 13, 14, 15, 16, 17, 18};
static_assert(std::size(kSessionMonitors) == WallpaperManager::kMaxVideoSessions + 1,
              "the ceiling checks need exactly one monitor past the ceiling");
constexpr int kMonitorStillOnly = 19;
constexpr const char* kConsumer = ":1.42";
// A layout no driver allocates: what a report excluding the committed one is made of.
constexpr uint64_t kUnallocatableModifier = 0x0f00000000000001ull;

// S13.123-S13.126 and S13.132, on the calling thread with no worker: every pass is run by this
// function, so the counters read after one cover that pass and nothing else.
void runSessionChecks(const WallpaperManager::Config& config, const std::vector<std::string>& clips,
                      const std::string& still, const std::string& shaderRow) {
    const char* kGoals = "S13.123-S13.126 and S13.132 (which passes reuse, reopen, evict and drop a session)";
    PhaseGrade grade("the session phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    for (const int id : kSessionMonitors) wallpaperx::testing::nameScreen(manager, id, screenOf(id));
    wallpaperx::testing::nameScreen(manager, kMonitorStillOnly, screenOf(kMonitorStillOnly));
    const auto resize = [&manager](int id, uint32_t width, uint32_t height) {
        std::vector<int> redraw;
        manager.setOutputSize(screenOf(id), width, height, redraw);
        return contains(redraw, id);
    };

    const int v0 = kSessionMonitors[0];
    manager.requestWallpaper("video", clips[0], v0);
    const PassCost first = renderCounted(manager, v0);
    if (!first.outcome.rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }
    // What gives every "opens nothing" below its meaning: the same hooks, on the same thread, see
    // this pass open and decode.
    reportUnless(first.opens == 1 && first.decodes > 0,
                 "a monitor's first video pass opens its file and decodes a picture (the counters work)",
                 describe(first));

    // GOAL: S13.123, a size report -- a re-post for a new size opens, decodes and closes nothing,
    //       and still commits the next generation showing the video.
    // WHY-THIS-SHAPE: counted per pass on the one thread that runs passes, rather than timed. The
    //       picture is graded because "committed" alone is also true of a pass that rebuilt the
    //       images and drew nothing into them.
    // CONTROL: (a) dropStaleVideoSessionLocked() in backend/daemon/wallpaper_manager.cpp made to
    //       drop every session (`&& false` on its keep-it test) -> "FAIL: a size report of a video
    //       monitor opens, decodes and closes nothing", printing "committed, 1 open(s), 1 close(s), 1
    //       packet(s)" (11 packets on Lavapipe). (b) the crop in renderer/renderers/video_renderer.cpp's
    //       render() made `static`, so a session keeps the crop of the first size it drew -> on
    //       Lavapipe "FAIL: after a size report the monitor shows the video, cover-cropped at the new
    //       size", printing "8 of 36 sampled rows show the wrong band"; NVIDIA abstains. Both observed
    //       2026-09-16; reverted.
    reportUnless(resize(v0, kResizedWidth, kResizedHeight), "a new size is owed to the video monitor",
                 "setOutputSize did not name it for a redraw");
    const PassCost resized = renderCounted(manager, v0);
    reportUnless(resized.outcome.rendered && resized.outcome.generation == first.outcome.generation + 1,
                 "a size report of a video monitor commits the next frame", describe(resized));
    reportUnless(resized.opens == 0 && resized.decodes == 0 && resized.closes == 0,
                 "a size report of a video monitor opens, decodes and closes nothing", describe(resized));
    gradeServedPicture(manager, v0, "S13.123's picture after a size report",
                       "after a size report the monitor shows the video, cover-cropped at the new size");
    uint64_t generation = resized.outcome.generation;

    // GOAL: S13.123, an import-modifier report -- a re-post a consumer's layout report causes opens
    //       and decodes nothing, and commits.
    // WHY-THIS-SHAPE: a report can condemn a frame only where the frame has a layout (a dma_buf),
    //       and on Lavapipe the only layout is LINEAR, so a report that excludes it leaves nothing to
    //       draw into. So the frame is condemned by one report -- which posts the monitor -- and made
    //       readable again by a second before the pass runs: the pass is still the report's, and it
    //       commits. An opaque-fd export carries no layout, so no report reaches it there.
    // CONTROL: the (a) mutation above -> on Lavapipe "FAIL: a layout report's re-post of a video
    //       monitor opens, decodes and closes nothing", printing "committed, 1 open(s), 1 close(s), 11
    //       packet(s)". Observed 2026-09-16; the NVIDIA entry abstains. Reverted.
    {
        wallpaperx::FrameExport probe = manager.exportCurrentFrame(v0);
        const bool served = probe.handle.has_value();
        uint64_t committedLayout = wallpaperx::kDrmModifierInvalid;
        bool dmaBuf = false;
        if (served) {
            committedLayout = probe.handle->frame.modifier;
            dmaBuf = probe.handle->frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf;
            if (probe.handle->frame.fd >= 0) close(probe.handle->frame.fd);
            if (probe.handle->acquireFd >= 0) close(probe.handle->acquireFd);
        }
        check(served, "the video monitor's frame is served before the layout report");
        if (served && (!dmaBuf || committedLayout == wallpaperx::kDrmModifierInvalid)) {
            notVerified("S13.123's import-modifier clause",
                        "this host exports opaque file descriptors, which carry no layout for a report to exclude, so "
                        "no report re-posts a monitor that has a frame");
        } else if (served) {
            const uint32_t format = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
            const std::vector<int> condemned =
                manager.setImportModifiers(kConsumer, format, {kUnallocatableModifier}).toRender;
            check(contains(condemned, v0), "a layout report excluding the frame's layout re-posts the video monitor");
            manager.setImportModifiers(kConsumer, format, {committedLayout, kUnallocatableModifier});
            const PassCost reported = renderCounted(manager, v0);
            reportUnless(reported.outcome.rendered && reported.outcome.generation == generation + 1,
                         "a layout report's re-post of a video monitor commits the next frame", describe(reported));
            reportUnless(reported.opens == 0 && reported.decodes == 0 && reported.closes == 0,
                         "a layout report's re-post of a video monitor opens, decodes and closes nothing",
                         describe(reported));
            if (reported.outcome.rendered) generation = reported.outcome.generation;
        }
    }

    // GOAL: S13.123, an image release -- the pass a consumer's release posts, for a render that was
    //       owed, opens and decodes nothing and commits.
    // WHY-THIS-SHAPE: a release posts a monitor only when a render is owed, so one is made owed by a
    //       size that changes and changes back. Neither size report is posted: the release is the
    //       only thing that runs the pass.
    // CONTROL: the (a) mutation above -> "FAIL: the release's pass of a video monitor opens, decodes
    //       and closes nothing", printing "committed, 1 open(s), 1 close(s), 1 packet(s)". Observed
    //       2026-09-16 on both driver entries. Reverted.
    {
        resize(v0, kResizedAgainWidth, kResizedAgainHeight);
        resize(v0, kResizedWidth, kResizedHeight);
        wallpaperx::FrameExport held = manager.exportCurrentFrame(v0, kConsumer);
        check(held.handle.has_value(), "a consumer is handed the video monitor's frame while a render is owed");
        if (held.handle) {
            const uint64_t bufferId = held.handle->frame.bufferId;
            if (held.handle->frame.fd >= 0) close(held.handle->frame.fd);
            if (held.handle->acquireFd >= 0) close(held.handle->acquireFd);
            const std::vector<int> retry = manager.releaseBuffer(kConsumer, v0, bufferId);
            check(contains(retry, v0), "releasing that image posts the video monitor");
            const PassCost released = renderCounted(manager, v0);
            reportUnless(released.outcome.rendered && released.outcome.generation == generation + 1,
                         "the release's pass of a video monitor commits the next frame", describe(released));
            reportUnless(released.opens == 0 && released.decodes == 0 && released.closes == 0,
                         "the release's pass of a video monitor opens, decodes and closes nothing", describe(released));
        }
    }

    // GOAL: S13.124 -- giving a monitor the same video path again opens a new session and closes the
    //       old one, leaving one. False for a daemon that keys the session on the path, which would
    //       keep decoding a file rewritten in place from bytes that are gone.
    // CONTROL: `++state.wallpaperSeq;` deleted from requestWallpaper() in
    //       backend/daemon/wallpaper_manager.cpp -> "FAIL: the same video path given again opens a new
    //       session and closes the old one", printing "committed, 0 open(s), 0 close(s), 0 packet(s)";
    //       S13.117's, S13.119's and S13.128's checks that need a new session went red with it. Observed
    //       2026-09-16 on both driver entries. Reverted.
    manager.requestWallpaper("video", clips[0], v0);
    const PassCost again = renderCounted(manager, v0);
    check(again.outcome.rendered, "the same video given again commits");
    reportUnless(again.opens == 1 && again.closes == 1 && again.decodes > 0,
                 "the same video path given again opens a new session and closes the old one", describe(again));
    reportUnless(g_files.live().count(clips[0]) == 1, "and one session for that monitor remains",
                 "open: " + describeFiles(g_files.live()));

    // GOAL: S13.126, restated for the clock -- eight sessions is the ceiling, and a ninth video
    //       asked for while all eight monitors are showing theirs is REFUSED: nothing is opened,
    //       nothing is closed, and no monitor that is showing a video loses it. False for the E2
    //       behaviour this replaces, which evicted the least recently drawn session whatever it was
    //       doing -- on a desktop with nine video screens that froze one of them for another's sake.
    // WHY-THIS-SHAPE: every monitor plays a different copy of the fixture, so the set of open files
    //       says which monitors hold a session. This phase runs no render worker, so every monitor
    //       that has been rendered here is settled -- a frame of its own wallpaper and no render
    //       owed -- which is exactly the state the daemon calls "playing". The simpler shape, asking
    //       for the ninth on a daemon with a worker running, cannot establish "no playing session is
    //       taken": which session is least recently used would then depend on which clock rounds had
    //       run.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the "skip a session whose monitor is
    //       showing its own current picture" line deleted from evictIdleVideoSessionLocked() --
    //       Stage E4 spells it `if (currentSeqLocked(...) && screenDrawnLocked(monitorId))
    //       continue;`, where Stage E2 had no skip at all -> "FAIL: a ninth video wallpaper asked
    //       for while all eight play opens nothing and commits nothing", and eleven more across
    //       this phase and the eviction-narrowing one. Re-run under the E4 spelling 2026-09-20.
    //       Raising kMaxVideoSessions cannot be used as the control: the static_assert above stops
    //       the build, which is the point of tying the monitor count to the constant. Observed
    //       2026-09-18. Reverted.
    // COVERAGE-ASSERT: each of the eight opens is asserted to have happened, so "eight are resident"
    //       is never read off a phase in which some never opened; and the monitor list is tied to
    //       kMaxVideoSessions by the static_assert above, so a ceiling that moved without this
    //       moving is a failed build rather than a check grading some other number.
    const int refused = kSessionMonitors[std::size(kSessionMonitors) - 1];
    {
        int opened = 0;
        for (size_t i = 1; i + 1 < std::size(kSessionMonitors); ++i) {
            manager.requestWallpaper("video", clips[i], kSessionMonitors[i]);
            const PassCost cost = renderCounted(manager, kSessionMonitors[i]);
            if (cost.outcome.rendered && cost.opens == 1 && cost.closes == 0) ++opened;
            else std::fprintf(stderr, "  monitor %d: %s\n", kSessionMonitors[i], describe(cost).c_str());
        }
        check(opened == static_cast<int>(WallpaperManager::kMaxVideoSessions) - 1,
              "seven more video monitors each open a session and close none");
        reportUnless(g_files.live().size() == WallpaperManager::kMaxVideoSessions,
                     "eight video monitors hold eight sessions", "open: " + describeFiles(g_files.live()));

        const std::multiset<std::string> beforeNinth = g_files.live();
        manager.requestWallpaper("video", clips[std::size(kSessionMonitors) - 1], refused);
        const PassCost ninth = renderCounted(manager, refused);
        reportUnless(!ninth.outcome.rendered && ninth.opens == 0 && ninth.closes == 0,
                     "a ninth video wallpaper asked for while all eight play opens nothing and commits nothing",
                     describe(ninth));
        reportUnless(g_files.live() == beforeNinth, "and no monitor that is showing a video loses its session",
                     "before: " + describeFiles(beforeNinth) + "; after: " + describeFiles(g_files.live()));

        // GOAL: the refusal is the daemon's, and it says what the limit is. False for a refusal
        //       reported as the file's fault, which sends the user to replace a file that is fine,
        //       and false for one whose message does not name the limit that caused it.
        // CONTROL: in classifyFailure() (backend/daemon/wallpaper_manager.cpp), the ResourceExhausted
        //       arm's `daemonFault` set to false -> "FAIL: the refusal is recorded as the daemon's
        //       fault, not the file's". Changing the thrown text in videoSessionLocked() to drop the
        //       count reddens the naming clause instead. Observed 2026-09-18. Reverted.
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, refused);
        const std::string error = status ? status->lastError : std::string();
        reportUnless(error.find("at most " + std::to_string(WallpaperManager::kMaxVideoSessions) + " videos") !=
                         std::string::npos,
                     "the refused monitor's error names the session limit that caused it", "last_error: " + error);
        reportUnless(status.has_value() && status->lastErrorIsDaemonFault,
                     "the refusal is recorded as the daemon's fault, not the file's",
                     status ? "daemon fault: no" : "the monitor has no status");
        reportUnless(manager.exportCurrentFrame(refused).reason == wallpaperx::NoFrameReason::RenderFailed,
                     "and a frame request for it answers RenderFailed", "a different no-frame reason");

        // GOAL: room IS made by closing a session whose monitor is not playing -- here one that owes
        //       a render because its screen reported a new size -- and the sessions that are playing
        //       are left alone. False for a ceiling that refuses whatever is resident, which would
        //       strand the ninth wallpaper for as long as eight entries existed.
        // WHY-THIS-SHAPE: the idle session is made idle by setOutputSize alone, which owes a render
        //       without changing the wallpaper: it isolates "owes a render" as the reason, where
        //       setting a new wallpaper would also have moved the session's wallpaper key.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, playingSeqLocked()'s
        //       `it->second.requestSeq != it->second.servicedSeq` clause deleted, so a monitor owing a
        //       render still counts as playing -> "FAIL: with one monitor owing a render, the ninth
        //       video opens a session and commits", printing "did not commit, 0 open(s), 0 close(s),
        //       0 packet(s)". Observed 2026-09-18. Reverted.
        reportUnless(resize(kSessionMonitors[1], kResizedWidth, kResizedHeight),
                     "a new size is owed to the monitor whose session is to be freed",
                     "setOutputSize did not name it for a redraw");
        manager.requestWallpaper("video", clips[std::size(kSessionMonitors) - 1], refused);
        const PassCost ninthAgain = renderCounted(manager, refused);
        reportUnless(ninthAgain.outcome.rendered && ninthAgain.opens == 1 && ninthAgain.closes == 1,
                     "with one monitor owing a render, the ninth video opens a session and commits",
                     describe(ninthAgain));
        const std::multiset<std::string> afterNinth = g_files.live();
        reportUnless(afterNinth.count(clips[1]) == 0 && afterNinth.count(clips[0]) == 1 &&
                         afterNinth.size() == WallpaperManager::kMaxVideoSessions,
                     "the session closed for it is the one whose monitor owes a render, not a playing monitor's",
                     "open: " + describeFiles(afterNinth));
        gradeServedPicture(manager, refused, "S13.126's picture past the ceiling",
                           "the ninth monitor's frame shows its video, cover-cropped");
    }

    // GOAL: S13.132 -- a still wallpaper's passes neither open, close nor evict a video session:
    //       with the ceiling reached, a still set and resized on another monitor leaves exactly the
    //       same files open and still commits.
    // WHY-THIS-SHAPE: run while the ceiling is reached, so a still pass that counted against it, or
    //       that cleared sessions it does not own, changes the open set.
    // CONTROL: dropStaleVideoSessionLocked() in backend/daemon/wallpaper_manager.cpp given a first
    //       line that closes every session when `kind` is not a video -> "FAIL: setting a still beside
    //       eight video sessions commits and opens or closes no video", printing "8 close(s)", and
    //       "FAIL: the video sessions are all still resident". Observed 2026-09-18. Reverted.
    {
        const std::multiset<std::string> resident = g_files.live();
        manager.requestWallpaper("static", still, kMonitorStillOnly);
        const PassCost set = renderCounted(manager, kMonitorStillOnly);
        reportUnless(set.outcome.rendered && set.opens == 0 && set.closes == 0,
                     "setting a still beside eight video sessions commits and opens or closes no video", describe(set));
        resize(kMonitorStillOnly, kResizedWidth, kResizedHeight);
        const PassCost moved = renderCounted(manager, kMonitorStillOnly);
        reportUnless(moved.outcome.rendered && moved.opens == 0 && moved.closes == 0,
                     "resizing that still commits and opens or closes no video", describe(moved));
        reportUnless(g_files.live() == resident, "the video sessions are all still resident",
                     "before: " + describeFiles(resident) + "; after: " + describeFiles(g_files.live()));
    }

    // GOAL: S13.125 -- a monitor whose wallpaper stops being a video, to a still or to a type this
    //       build cannot draw, holds no session after its pass.
    // WHY-THIS-SHAPE: both kinds of replacement, because they leave through different branches of
    //       the pass: the undrawable one returns before anything is drawn.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the dropStaleVideoSessionLocked() call moved
    //       from the top of renderPassLocked() into its video branch, beside videoSessionLocked() ->
    //       both checks below red, each printing the replaced file still open. Observed 2026-09-16 on
    //       both driver entries. Reverted. `kind` in dropStaleVideoSessionLocked()'s keep-it test has no
    //       control of its own: a type changes only through a request, which moves wallpaperSeq too.
    {
        manager.requestWallpaper("static", still, kSessionMonitors[3]);
        const PassCost toStill = renderCounted(manager, kSessionMonitors[3]);
        check(toStill.outcome.rendered, "a video monitor given a still commits it");
        reportUnless(g_files.closes(clips[3]) == 1 && g_files.live().count(clips[3]) == 0,
                     "a video monitor given a still no longer holds a session",
                     describe(toStill) + "; open: " + describeFiles(g_files.live()));

        manager.requestWallpaper("shader", shaderRow, kSessionMonitors[4]);
        const PassCost toShader = renderCounted(manager, kSessionMonitors[4]);
        reportUnless(g_files.closes(clips[4]) == 1 && g_files.live().count(clips[4]) == 0,
                     "a video monitor given a type this build cannot draw no longer holds a session",
                     describe(toShader) + "; open: " + describeFiles(g_files.live()));
    }
}

// What a start would decide had this process died at this instant: the config as it is now, and
// this monitor's crash record copied as it is now -- unstamped, which is what SIGKILL or the
// teardown alarm leaves.
struct RestartVerdict {
    bool graded = false;
    std::string why;
    bool refused = false;
    std::string lastError;
};

RestartVerdict startAfterDeathNow(const WallpaperManager::Config& running, int monitorId) {
    RestartVerdict verdict;
    const wallpaperx::testing::ScratchDir copy("videorestart");
    if (!copy.ok()) {
        verdict.why = "no scratch directory for the copied state";
        return verdict;
    }
    WallpaperManager::Config config;
    config.width = running.width;
    config.height = running.height;
    config.configFile = copy.path() / "config.json";
    config.stateDir = copy.path() / "state";
    std::error_code ec;
    std::filesystem::copy_file(running.configFile, config.configFile, ec);
    if (ec) {
        verdict.why = "the running config could not be copied: " + ec.message();
        return verdict;
    }
    const std::filesystem::path record = wallpaperx::crashRecordPath(running.stateDir, monitorId);
    if (std::filesystem::exists(record, ec)) {
        std::filesystem::create_directories(config.stateDir, ec);
        if (!ec) std::filesystem::copy_file(record, wallpaperx::crashRecordPath(config.stateDir, monitorId), ec);
        if (ec) {
            verdict.why = "the crash record could not be copied: " + ec.message();
            return verdict;
        }
    }
    try {
        WallpaperManager restarted(config);
        restarted.restoreSaved();
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(restarted, monitorId);
        if (!status) {
            verdict.why = "the copied config did not restore the monitor";
            return verdict;
        }
        verdict.graded = true;
        verdict.refused = !status->lastError.empty() && !status->renderOutstanding;
        verdict.lastError = status->lastError;
    } catch (const std::exception& e) {
        verdict.why = std::string("a manager on the copied state threw: ") + e.what();
    }
    return verdict;
}

bool restartGraded(const RestartVerdict& verdict) {
    if (!verdict.graded) {
        wallpaperx::testing::harnessFailure("a start from the copied state could not be graded: " + verdict.why);
    }
    return verdict.graded;
}

std::string describeRecord(const std::optional<wallpaperx::CrashRecord>& record) {
    return record ? "type '" + record->type + "' path '" + record->path + "'" : "none";
}

// The render worker's thread, with every gate this file arms let go before the worker is stopped:
// a gate left armed or holding would park it.
struct WorkerUnblock {
    RenderWorker& worker;
    ~WorkerUnblock() {
        g_playbackFailure.disarm();
        g_failImageCreates.store(0);
        g_submitDeviceLost.store(0);
        g_framebufferGate.release();
        g_framebufferFailGate.release();
        g_framebufferLostGate.release();
        g_submitFailGate.release();
        g_closeFileGate.release();
        g_freeCodecGate.release();
        g_decodeGate.release();
        worker.stop();
    }
};

constexpr int kMonitorGpuFault = 20;
constexpr int kMonitorOvertaken = 21;
constexpr int kMonitorDrawFault = 22;
constexpr int kMonitorReplaced = 23;

// S13.127-S13.129 and S13.130's replacement half.
void runFailedPassChecks(const WallpaperManager::Config& base, const std::vector<std::string>& clips,
                         const std::string& still) {
    const char* kGoals = "S13.127-S13.130 (what a failed pass does with its session; which teardowns are recorded)";
    const wallpaperx::testing::ScratchDir scratch("videofail");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch directory could be created for the state and config");
        return;
    }
    WallpaperManager::Config config = base;
    config.stateDir = scratch.path() / "state";
    config.configFile = scratch.path() / "config.json";

    PhaseGrade grade("the failed-pass phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    // The drain matters as much as the frame: this phase's clips hold one picture each, and the
    // clock round that parks a session runs after the job that drew it. Until that round has run,
    // the session is still playing and the clock would reach the gates armed below -- the submit
    // this phase fails to fail a DRAW would be spent on the clock's own instead.
    const auto show = [&](int id, const std::string& path) {
        wallpaperx::testing::nameScreen(manager, id, screenOf(id));
        manager.requestWallpaper("video", path, id);
        worker.post(id);
        return signals.wait(id, 1) && drainWorker(worker);
    };
    const auto resize = [&](int id, uint32_t width, uint32_t height) {
        std::vector<int> redraw;
        manager.setOutputSize(screenOf(id), width, height, redraw);
        for (const int posted : redraw) worker.post(posted);
    };

    if (!show(kMonitorGpuFault, clips[0])) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }

    // GOAL: S13.127 -- a pass that fails before the draw for a reason that is not the file's (here
    //       the images failing to build) drops the monitor's session with NO crash record on disk,
    //       so a death inside that teardown does not refuse the video at the next start; and the
    //       next pass reopens the file rather than drawing with the dropped session.
    // WHY-THIS-SHAPE: the thread is held as the session's file is closed, the end of its teardown,
    //       and a start is built from the state copied at that instant -- the consequence, not just
    //       the file. The draw-fault section below builds the same copy while a record IS on disk and
    //       requires it to refuse, so this "not refused" cannot be the copy failing to refuse anything.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, closeFailedVideoSessionLocked() made to free
    //       a non-file failure's pipeline inside a scoped `ApplyingRecord` naming the session's file
    //       -- a record-writing close, which the Stage E2 review found here -> "FAIL: no crash record
    //       is on disk while a session is torn down for a fault that is not the file's", printing the
    //       record naming video_parked_0.mp4, and "FAIL: a start after dying in that teardown applies
    //       the video instead of refusing it", beside the restated S13.129 pair below (the same close).
    //       Observed 2026-09-21 on the NVIDIA entry. Reverted.
    // COVERAGE-ASSERT: the injected failure is asserted to have fired, so the teardown seen is the
    //       one that failure caused.
    g_framebufferFailGate.arm(CallGate::Mode::Fail, extentOf(kResizedWidth, kResizedHeight));
    g_closeFileGate.arm(CallGate::Mode::Hold, clips[0]);
    resize(kMonitorGpuFault, kResizedWidth, kResizedHeight);
    const bool droppedForFault = g_closeFileGate.waitFired(kGateLimit);
    check(droppedForFault, "a pass whose images fail to build tears the monitor's session down");
    if (droppedForFault) {
        check(g_framebufferFailGate.waitFired(std::chrono::seconds(0)),
              "that teardown follows the injected image failure");
        const std::optional<wallpaperx::CrashRecord> record =
            wallpaperx::readCrashRecord(config.stateDir, kMonitorGpuFault);
        reportUnless(!record.has_value(),
                     "no crash record is on disk while a session is torn down for a fault that is not the file's",
                     "the record on disk: " + describeRecord(record));
        const RestartVerdict restart = startAfterDeathNow(config, kMonitorGpuFault);
        if (restartGraded(restart)) {
            reportUnless(!restart.refused, "a start after dying in that teardown applies the video instead of refusing it",
                         "the start refused it: " + restart.lastError);
        }
    }
    g_closeFileGate.release();
    g_framebufferFailGate.release();
    check(drainWorker(worker), "the failed pass finishes once its teardown is let go");
    {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMonitorGpuFault);
        check(status.has_value() && !status->lastError.empty() && status->lastErrorIsDaemonFault,
              "the pass whose images failed reports a failure that is the daemon's");
    }
    g_files.resetCounts();
    resize(kMonitorGpuFault, kResizedAgainWidth, kResizedAgainHeight);
    const bool recovered = signals.wait(kMonitorGpuFault, 2);
    check(recovered, "the monitor draws again at its next pass");
    if (recovered) {
        reportUnless(g_files.opens(clips[0]) == 1, "and that pass reopens the file rather than keeping the dropped session",
                     std::to_string(g_files.opens(clips[0])) + " open(s)");
    }

    // GOAL: S13.128 -- a pass that opened a session, then failed, and was overtaken by a newer
    //       request before it could commit, leaves no session: the newer request's pass opens its
    //       own. False for a daemon that drops a failed session only after the commit block, which
    //       an overtaken pass returns from inside.
    // WHY-THIS-SHAPE: the render thread is held inside the image rebuild of a pass that has just
    //       opened a new wallpaper's session, the rebuild is armed to fail when let go, and the newer
    //       size lands while it is held -- so the failure and the overtaking are both certain, in
    //       that order, with no timing involved. The open count is reset AFTER requestWallpaper():
    //       the request opens the file itself to check it can be read (S13.195), and that open,
    //       counted, would make "the pass opened its session" read two where the pass opened one.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, renderPassLocked()'s first
    //       `if (!error.empty()) closeFailedVideoSessionLocked(monitorId, fault);` -- the one before
    //       the commit block -- deleted -> "FAIL: a failed, overtaken pass leaves no session: the
    //       newer pass opens its own", printing "0 open(s), 0 close(s) after the failure", alone.
    //       Observed 2026-09-21 on the NVIDIA entry. Reverted.
    // COVERAGE-ASSERT: the held pass is asserted to have opened the session, and the injected
    //       failure to have fired.
    if (!show(kMonitorOvertaken, clips[0])) {
        check(false, "the monitor the overtaken failing pass is about is drawn first");
        return;
    }
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedWidth, kResizedHeight));
    g_framebufferFailGate.arm(CallGate::Mode::Fail, extentOf(kResizedWidth, kResizedHeight));
    manager.requestWallpaper("video", clips[1], kMonitorOvertaken);
    g_files.resetCounts();
    resize(kMonitorOvertaken, kResizedWidth, kResizedHeight);
    const bool heldFailing = g_framebufferGate.waitFired(kGateLimit);
    check(heldFailing, "the pass to be overtaken is held inside its image rebuild");
    if (heldFailing) {
        const bool opened = g_files.opens(clips[1]) == 1;
        check(opened, "the pass to be overtaken opened its session before the rebuild");
        resize(kMonitorOvertaken, kResizedAgainWidth, kResizedAgainHeight);
        g_files.resetCounts();
        g_framebufferGate.release();
        const bool redrawn = signals.wait(kMonitorOvertaken, 2);
        check(redrawn, "the newer request's pass commits");
        check(g_framebufferFailGate.waitFired(std::chrono::seconds(0)), "the overtaken pass's rebuild was failed");
        if (opened && redrawn) {
            reportUnless(g_files.opens(clips[1]) == 1 && g_files.closes(clips[1]) == 1,
                         "a failed, overtaken pass leaves no session: the newer pass opens its own",
                         std::to_string(g_files.opens(clips[1])) + " open(s), " +
                             std::to_string(g_files.closes(clips[1])) + " close(s) after the failure");
        }
    }
    g_framebufferGate.release();
    g_framebufferFailGate.release();

    if (!show(kMonitorDrawFault, clips[2])) {
        check(false, "the monitor whose draw is failed is drawn first");
        return;
    }
    // Changes size and back before the one post, so the pass neither rebuilds nor decodes and the
    // draw's submission is its only one.
    const auto repostWithoutRebuild = [&] {
        std::vector<int> notPosted;
        manager.setOutputSize(screenOf(kMonitorDrawFault), kResizedWidth, kResizedHeight, notPosted);
        manager.setOutputSize(screenOf(kMonitorDrawFault), kTargetWidth, kTargetHeight, notPosted);
        worker.post(kMonitorDrawFault);
    };

    // The positive reading the absences in this phase are measured against: while a video's DRAW
    // runs -- the file's content being shown -- a record naming it is on disk. Without it, a build
    // whose records had stopped being written at all would pass every "no record" check here.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, renderPassLocked()'s
    //       `if (!deferred) applying.emplace(m_config.stateDir, monitorId, type, path);` deleted ->
    //       "FAIL: a record naming the video is on disk while its draw is submitted", printing "none",
    //       and "FAIL: a start from the state copied while that record is on disk refuses the video".
    //       Observed 2026-09-21 on the NVIDIA entry. Reverted.
    g_submitFailGate.arm(CallGate::Mode::Hold);
    repostWithoutRebuild();
    const bool drawing = g_submitFailGate.waitFired(kGateLimit);
    check(drawing, "a re-post of a parked video reaches its draw's submission");
    if (drawing) {
        const std::optional<wallpaperx::CrashRecord> record = wallpaperx::readCrashRecord(config.stateDir, kMonitorDrawFault);
        reportUnless(record && record->type == "video" && record->path == clips[2],
                     "a record naming the video is on disk while its draw is submitted",
                     "the record on disk: " + describeRecord(record));
        // And the copied-state start, which S13.127 above and the restated S13.129 below read as "not
        // refused", does refuse when there is a record to refuse on.
        const RestartVerdict restart = startAfterDeathNow(config, kMonitorDrawFault);
        if (restartGraded(restart)) {
            check(restart.refused, "a start from the state copied while that record is on disk refuses the video");
        }
    }
    g_submitFailGate.release();
    check(signals.wait(kMonitorDrawFault, 2) && drainWorker(worker), "and that draw commits once it is let go");

    // GOAL: S13.129, RESTATED for Stage E5 -- a draw that fails for a reason that is NOT the file's
    //       (here the device running out of memory at the submit, which the daemon sorts as its own
    //       shortage) frees the session with NO record naming the video on disk, so a death in that
    //       teardown does not refuse the video at the next start; the monitor reports the failure as
    //       the daemon's. The old rule (a record kept across the teardown) predated the split between
    //       the file's faults and the daemon's, and would refuse a working video after a driver
    //       crash freeing a pipeline on a device that had just failed.
    // WHY-THIS-SHAPE: the thread is held as the session's file is closed -- the end of its teardown
    //       -- and a start is built from the state copied at that instant, for S13.127's reason. The
    //       record reading just above, in the same directory for the same monitor, is what makes
    //       this absence evidence.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, renderPassLocked()'s
    //       `if (!error.empty() && fault != Fault::File) applying.reset();` deleted, so the pass's
    //       record stays on disk through the teardown -> "FAIL: no record naming the video is on disk
    //       while a session whose draw failed for the daemon's reason is torn down", printing the
    //       record naming video_parked_2.mp4, and "FAIL: a start after dying in that teardown applies
    //       the video", printing the "killed while applying" refusal; nothing else. Observed 2026-09-21
    //       on the NVIDIA entry. Reverted.
    g_submitFailGate.arm(CallGate::Mode::Fail);
    g_closeFileGate.arm(CallGate::Mode::Hold, clips[2]);
    repostWithoutRebuild();
    const bool closingAfterDraw = g_closeFileGate.waitFired(kGateLimit);
    check(closingAfterDraw, "a draw that fails tears the monitor's session down");
    if (closingAfterDraw) {
        const std::optional<wallpaperx::CrashRecord> record =
            wallpaperx::readCrashRecord(config.stateDir, kMonitorDrawFault);
        reportUnless(!record.has_value(),
                     "no record naming the video is on disk while a session whose draw failed for the daemon's "
                     "reason is torn down",
                     "the record on disk: " + describeRecord(record));
        const RestartVerdict restart = startAfterDeathNow(config, kMonitorDrawFault);
        if (restartGraded(restart)) {
            reportUnless(!restart.refused, "a start after dying in that teardown applies the video",
                         "the start refused it: " + restart.lastError);
        }
    }
    g_closeFileGate.release();
    g_submitFailGate.release();
    check(drainWorker(worker), "the pass whose draw failed finishes once its teardown is let go");
    {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMonitorDrawFault);
        check(status && status->lastErrorIsDaemonFault, "and the failed draw is reported as the daemon's failure, not "
                                                        "the file's");
    }
    // GOAL: S13.138 -- the failure this monitor reports is the one its own draw hit -- the queue submission --
    //       and not some later failure of the daemon's, so an operator reading last_error is pointed
    //       at what actually went wrong.
    // WHY-THIS-SHAPE: this phase's clips hold one picture each and show() drains the round that parks
    //       them, so the single injected submission failure is spent on THIS pass's draw. With a
    //       playing clip it was spent on a picture of the video clock instead, the clock's retry then
    //       re-rendered the monitor successfully, and the successful render cleared last_error -- the
    //       check read an empty string and could no longer fail for the reason it names.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, recordFailureLocked() made to record a fixed
    //       string instead of `error` -> "FAIL: the failure the monitor reports is the draw's own
    //       submission", printing "last_error: a failure this monitor did not have". Observed
    //       2026-09-18. Reverted.
    {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMonitorDrawFault);
        const std::string error = status ? status->lastError : std::string();
        reportUnless(error.find("vkQueueSubmit") != std::string::npos,
                     "the failure the monitor reports is the draw's own submission", "last_error: " + error);
    }

    // GOAL: S13.130, a replaced video, RESTATED for Stage E4 -- a session dropped because its
    //       monitor was given a still is torn down with NO record on disk: neither the video that
    //       session opened nor the still now asked for. The still's own record is taken later in the
    //       pass, around the span its file is actually read in; a record around this teardown would
    //       blame the next start on a file that had already played.
    // WHY-THIS-SHAPE: a different file and type on purpose -- with the same path, "no record" and
    //       "a record naming the new request" cannot be told apart. The absence is read in the same
    //       state directory where the draw-fault check above has just required a record to be
    //       PRESENT, so a build whose crash records stopped being written altogether fails there
    //       rather than passing here.
    // CONTROL: closeVideoSessionLocked() in backend/daemon/wallpaper_manager.cpp given back the
    //       Stage E3 record (see the same control in runCrashRecordSpanChecks) -> "FAIL: no record
    //       is on disk while a replaced video's session is torn down for a still", printing the
    //       video's own path. Observed 2026-09-20. Reverted.
    if (!show(kMonitorReplaced, clips[3])) {
        check(false, "the monitor whose video is replaced is drawn first");
        return;
    }
    g_closeFileGate.arm(CallGate::Mode::Hold, clips[3]);
    manager.requestWallpaper("static", still, kMonitorReplaced);
    worker.post(kMonitorReplaced);
    const bool replacing = g_closeFileGate.waitFired(kGateLimit);
    check(replacing, "giving a video monitor a still tears its session down");
    if (replacing) {
        const std::optional<wallpaperx::CrashRecord> record = wallpaperx::readCrashRecord(config.stateDir, kMonitorReplaced);
        reportUnless(!record.has_value(), "no record is on disk while a replaced video's session is torn down for a still",
                     "the record on disk: " + describeRecord(record));
    }
    g_closeFileGate.release();
    check(signals.wait(kMonitorReplaced, 2), "and the still then commits");
}

constexpr int kEvictionMonitors[] = {20, 21, 22, 23, 24, 25, 26, 27, 28};
static_assert(std::size(kEvictionMonitors) == WallpaperManager::kMaxVideoSessions + 1,
              "the eviction check needs one monitor past the ceiling");

// S13.130's eviction half, on a manager of its own so which session is least recently drawn
// depends on nothing but this function.
void runEvictionRecordCheck(const WallpaperManager::Config& base, const std::vector<std::string>& clips) {
    const char* kGoal = "S13.130's eviction half (an evicted session is torn down under a record naming its file)";
    const wallpaperx::testing::ScratchDir scratch("videoevict");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch state directory could be created");
        return;
    }
    WallpaperManager::Config config = base;
    config.stateDir = scratch.path();

    PhaseGrade grade("the eviction phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    const auto post = [&](size_t i) {
        wallpaperx::testing::nameScreen(manager, kEvictionMonitors[i], screenOf(kEvictionMonitors[i]));
        manager.requestWallpaper("video", clips[i], kEvictionMonitors[i]);
        worker.post(kEvictionMonitors[i]);
    };
    post(0);
    if (!signals.wait(kEvictionMonitors[0], 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the first video render on this host produced no frame");
        return;
    }
    for (size_t i = 1; i + 1 < std::size(kEvictionMonitors); ++i) {
        post(i);
        if (!signals.wait(kEvictionMonitors[i], 1)) {
            check(false, "eight video monitors are drawn before the ninth");
            return;
        }
    }
    // Every session here is opened on the one-picture fixture, so after this drain each has been
    // parked by a clock round: none of them is playing, all of them are free to close, and the
    // choice below is the least recently drawn of eight equals rather than a race between whichever
    // rounds happened to have run.
    if (!drainWorker(worker)) {
        check(false, "the render worker answers a task once the eight sessions are open");
        return;
    }

    // GOAL: S13.130, eviction, RESTATED for Stage E4 -- the session closed to make room for one past
    //       the ceiling is torn down with NO record on disk at all: not the evicted monitor's, whose
    //       file has already opened, decoded and drawn, and not the monitor being opened either,
    //       whose own record is taken only once the eviction is over. The second half is the
    //       ordering that keeps a death inside another file's teardown from being blamed on this
    //       one. Which session is chosen is unchanged: the least recently drawn that is not
    //       playing, and a file holding a single picture is not playing.
    // WHY-THIS-SHAPE: an absent record says nothing on its own, so the same phase, the same state
    //       directory and the same monitor are then held inside the NEW file's first decode and a
    //       record IS required to be there naming it. The two readings are what make each other
    //       mean something: a build that had stopped writing records anywhere passes the first and
    //       fails the second.
    // CONTROL: (a) closeVideoSessionLocked() in backend/daemon/wallpaper_manager.cpp given back the
    //       Stage E3 record -> "FAIL: nothing names the evicted monitor while its session is torn
    //       down", printing the evicted clip. (b) in videoSessionLocked(), an `ApplyingRecord` naming
    //       the new file declared above the ceiling's eviction loop -> "FAIL: and nothing names the
    //       monitor being opened either, whose own record is taken after the eviction", printing
    //       video_parked_8.mp4 -- declared below that loop instead, it reddens only the shortage-path
    //       eviction in runPlaybackBlameChecks, since the ceiling's eviction runs first. (c) for the
    //       positive half, the `const ApplyingRecord applying(...)` line deleted from the open's
    //       attempt -> "FAIL: and a record naming the new file IS on disk once its own open and first
    //       decode are running". (a) observed 2026-09-20, re-observed 2026-09-21 with the
    //       applying-record control in runPlaybackBlameChecks; (b) and (c) re-observed 2026-09-21 on
    //       the NVIDIA entry; reverted.
    g_closeFileGate.arm(CallGate::Mode::Hold, clips[0]);
    const size_t ninth = std::size(kEvictionMonitors) - 1;
    post(ninth);
    const bool evicting = g_closeFileGate.waitFired(kGateLimit);
    check(evicting, "a video monitor past the ceiling tears the least recently drawn parked session down");
    if (evicting) {
        const std::optional<wallpaperx::CrashRecord> evicted =
            wallpaperx::readCrashRecord(config.stateDir, kEvictionMonitors[0]);
        reportUnless(!evicted.has_value(), "nothing names the evicted monitor while its session is torn down",
                     "the evicted monitor's record: " + describeRecord(evicted));
        const std::optional<wallpaperx::CrashRecord> opening =
            wallpaperx::readCrashRecord(config.stateDir, kEvictionMonitors[ninth]);
        reportUnless(!opening.has_value(),
                     "and nothing names the monitor being opened either, whose own record is taken after the eviction",
                     "the opening monitor's record: " + describeRecord(opening));
    }
    // Armed before the close is let go, so the hold lands on the new file's own first decode and
    // not on a later round's: the pass continues straight from the teardown into the open.
    g_decodeGate.arm(CallGate::Mode::Hold);
    g_closeFileGate.release();
    const bool decodingNew = g_decodeGate.waitFired(kGateLimit);
    check(decodingNew, "the pass past the ceiling goes on to decode the file it opened");
    if (decodingNew) {
        const std::optional<wallpaperx::CrashRecord> opening =
            wallpaperx::readCrashRecord(config.stateDir, kEvictionMonitors[ninth]);
        reportUnless(opening && opening->type == "video" && opening->path == clips[ninth],
                     "and a record naming the new file IS on disk once its own open and first decode are running",
                     "the opening monitor's record: " + describeRecord(opening));
    }
    g_decodeGate.release();
    check(signals.wait(kEvictionMonitors[ninth], 1), "and the monitor past the ceiling then commits");
}

constexpr int kMonitorShutdown = 29;

// S13.131: the manager is destroyed on a thread of this test's, so this thread can read the disk
// while the destruction is held.
void runShutdownChecks(const WallpaperManager::Config& base, const std::string& clip) {
    const char* kGoal = "S13.131 (shutdown frees a video session with no record, and the next start draws it)";
    const wallpaperx::testing::ScratchDir scratch("videoshutdown");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created for the state and config");
        return;
    }
    WallpaperManager::Config config = base;
    config.stateDir = scratch.path() / "state";
    config.configFile = scratch.path() / "config.json";

    {
        PhaseGrade grade("the shutdown phase");
        auto manager = std::make_unique<WallpaperManager>(config);
        struct CaptureIfAlive {
            PhaseGrade& grade;
            std::unique_ptr<WallpaperManager>& manager;
            ~CaptureIfAlive() {
                if (manager) grade.capture(*manager);
            }
        } captureGrade{grade, manager};

        FrameReadyLog signals;
        bool drawn = false;
        {
            RenderWorker worker(*manager, [&signals](int monitorId, uint64_t generation) {
                signals.record(monitorId, generation);
            });
            WorkerUnblock unblock{worker};
            wallpaperx::testing::nameScreen(*manager, kMonitorShutdown, screenOf(kMonitorShutdown));
            manager->requestWallpaper("video", clip, kMonitorShutdown);
            worker.post(kMonitorShutdown);
            drawn = signals.wait(kMonitorShutdown, 1);
        }
        if (!drawn) {
            grade.expectNoDevice();
            notVerified(kGoal, "the video render on this host produced no frame");
            return;
        }
        grade.capture(*manager);

        // GOAL: S13.131, the teardown -- destroying the manager frees a video session with NO crash
        //       record on disk, so a stop abandoned inside that teardown (the shutdown alarm) does
        //       not refuse, at the next start, a wallpaper that drew.
        // WHY-THIS-SHAPE: the destruction runs on a thread of this test's and is held as the file
        //       closes; the disk is read and a start is built from it at that instant.
        // CONTROL: ~WallpaperManager in backend/daemon/wallpaper_manager.cpp given a body that
        //       closes every session through closeVideoSessionLocked() under m_gpuMutex ->
        //       "FAIL: no crash record is on disk while shutdown tears a video session down", printing
        //       the record naming video_session_0.mp4, and "FAIL: a start after a shutdown abandoned in
        //       that teardown applies the video". Observed 2026-09-16 on both driver entries. Reverted.
        g_closeFileGate.arm(CallGate::Mode::Hold, clip);
        std::jthread teardown([&manager] { manager.reset(); });
        struct ReleaseFirst {
            ~ReleaseFirst() { g_closeFileGate.release(); }
        } releaseFirst;
        const bool closing = g_closeFileGate.waitFired(kGateLimit);
        check(closing, "shutting the manager down tears the video session down");
        if (closing) {
            const std::optional<wallpaperx::CrashRecord> record =
                wallpaperx::readCrashRecord(config.stateDir, kMonitorShutdown);
            reportUnless(!record.has_value(), "no crash record is on disk while shutdown tears a video session down",
                         "the record on disk: " + describeRecord(record));
            const RestartVerdict restart = startAfterDeathNow(config, kMonitorShutdown);
            if (restartGraded(restart)) {
                reportUnless(!restart.refused,
                             "a start after a shutdown abandoned in that teardown applies the video",
                             "the start refused it: " + restart.lastError);
            }
        }
        g_closeFileGate.release();
        teardown.join();
    }

    // GOAL: S13.131, the next start -- a daemon stopped while showing a video restores it at the
    //       next start owing a render, and that render opens the file and commits.
    // WHY-THIS-SHAPE: the same config and state directory the stopped manager used, so anything its
    //       shutdown left behind is read.
    // CONTROL: ~WallpaperManager in backend/daemon/wallpaper_manager.cpp given a body that writes a
    //       record file naming each session's video and never removes it -> "FAIL: the next start
    //       restores the video that was showing, owing a render rather than refusing it", printing the
    //       "not applied" refusal, and "FAIL: and that render opens the video and commits". Observed
    //       2026-09-16 on both driver entries. Reverted. Leaking an ApplyingRecord instead cannot
    //       control this: it keeps the process's one record slot, so no later record in the run is
    //       written at all, this phase's included.
    PhaseGrade grade("the start after shutdown");
    WallpaperManager restarted(config);
    CaptureGradeOnExit captureGrade{grade, restarted};
    const std::vector<int> posted = restarted.restoreSaved();
    const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(restarted, kMonitorShutdown);
    reportUnless(contains(posted, kMonitorShutdown) && status && status->lastError.empty() && status->renderOutstanding,
                 "the next start restores the video that was showing, owing a render rather than refusing it",
                 status ? "last_error: " + status->lastError : std::string("the monitor was not restored"));
    const PassCost drawn = renderCounted(restarted, kMonitorShutdown);
    reportUnless(drawn.outcome.rendered && g_files.opens(clip) == 1, "and that render opens the video and commits",
                 describe(drawn));
    // A refused monitor is never drawn, so no Device exists to grade; the checks above are the red.
    if (status && !status->lastError.empty()) grade.expectNoDevice();
}

// --------------------------------------------------------------------------- //
// Stage E3: the video clock
// --------------------------------------------------------------------------- //

// Every FrameReady the worker announced, in order and with the moment it arrived. FrameReadyLog
// answers "did generation N arrive"; the clock's checks need the sequence itself -- that each
// generation was announced once, that the next is exactly one higher, and how long a run of them
// took -- and none of those is a question about one generation.
class PictureLog {
public:
    struct Event {
        int monitorId = 0;
        uint64_t generation = 0;
        Clock::time_point at;
    };

    void record(int monitorId, uint64_t generation) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.push_back(Event{monitorId, generation, Clock::now()});
    }

    std::vector<Event> forMonitor(int monitorId) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Event> out;
        for (const Event& e : m_events) {
            if (e.monitorId == monitorId) out.push_back(e);
        }
        return out;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<Event> m_events;
};

// Waits for a condition the daemon reaches on its own, with a deadline that is reported rather than
// fallen through. Every use below is a state a correct daemon reaches in well under a second; the
// limit is there so a daemon that never reaches it fails a named check instead of hanging.
bool settlesWithin(std::chrono::seconds limit, const std::function<bool()>& reached) {
    const Clock::time_point deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        if (reached()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return reached();
}

// Parks the render worker inside a task of this test's until the returned handle is dropped. The
// barrier every "nothing moved" reading below needs: the worker is the only thread that shows a
// picture, so while it is held here the generation a status read returns is provably the last one
// committed, and a request recorded during the hold provably precedes every later round.
class HeldWorker {
public:
    explicit HeldWorker(RenderWorker& worker) {
        auto entered = std::make_shared<std::promise<void>>();
        std::future<void> arrived = entered->get_future();
        std::shared_future<void> gate = m_release->get_future().share();
        worker.postTask([entered, gate](RenderWorker::TaskState state) {
            entered->set_value();
            // Never wait when the worker is answering a task it will not run: stop() is waiting for
            // this thread, and waiting back is a deadlock that costs the run its diagnostics.
            if (state == RenderWorker::TaskState::Run) gate.wait();
        });
        m_held = arrived.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
    }
    HeldWorker(const HeldWorker&) = delete;
    HeldWorker& operator=(const HeldWorker&) = delete;
    ~HeldWorker() { m_release->set_value(); }

    bool held() const { return m_held; }

private:
    std::shared_ptr<std::promise<void>> m_release = std::make_shared<std::promise<void>>();
    bool m_held = false;
};

// What one hand-over did. Asked for ON THE WORKER'S THREAD wherever a phase has a worker, which is
// forced rather than tidy: Device aborts the process on a GPU call from any thread but the one that
// owns it, and exporting a frame is a GPU call. It is also what the daemon does -- getFrameFd posts
// the export as a task and answers from the worker.
struct Handover {
    bool answered = false;  // the task ran at all, rather than being refused by a stopping worker
    bool served = false;
    uint64_t bufferId = 0;
    uint64_t generation = 0;
    wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
    bool firstRefusal = false;
    std::string threw;
};

std::string describe(const Handover& h) {
    if (!h.answered) return "the worker never ran the export task";
    if (!h.threw.empty()) return "the export threw: " + h.threw;
    if (h.served) return "served image " + std::to_string(h.bufferId) + " at generation " + std::to_string(h.generation);
    return "refused (" + std::to_string(static_cast<int>(h.reason)) + ")";
}

// `keep` decides whether the image is given straight back. A consumer that keeps what it is handed
// is the state a refusal exists for, and it is also the state that must stop a video: those are
// two of the goals below, so the choice cannot be wired one way here.
Handover handOver(WallpaperManager& manager, RenderWorker& worker, int monitorId, const std::string& client,
                  bool keep = false, std::chrono::seconds limit = std::chrono::seconds(60)) {
    auto done = std::make_shared<std::promise<Handover>>();
    std::future<Handover> answer = done->get_future();
    worker.postTask([&manager, monitorId, client, keep, done](RenderWorker::TaskState state) {
        Handover out;
        if (state != RenderWorker::TaskState::Run) {
            done->set_value(out);
            return;
        }
        out.answered = true;
        try {
            const wallpaperx::FrameExport e = manager.exportCurrentFrame(monitorId, client);
            out.reason = e.reason;
            out.firstRefusal = e.firstRefusal;
            if (e.handle) {
                out.served = true;
                out.bufferId = e.handle->frame.bufferId;
                out.generation = e.handle->generation;
                if (e.handle->frame.fd >= 0) close(e.handle->frame.fd);
                if (e.handle->acquireFd >= 0) close(e.handle->acquireFd);
                if (!keep) manager.releaseBuffer(client, monitorId, out.bufferId);
            }
        } catch (const std::exception& ex) {
            out.threw = ex.what();
        }
        done->set_value(out);
    });
    // Reported, never fallen through: a hand-over that never came back must not be graded as a
    // refusal, which is what a default-constructed Handover would look like without `answered`.
    if (answer.wait_for(limit) != std::future_status::ready) return Handover{};
    return answer.get();
}

// A consumer of one monitor's pictures: it asks for a hand-over when one is ANNOUNCED, and gives
// the image straight back.
//
// ONLY ON AN ANNOUNCEMENT, never on a timer, and that is the whole reason this class exists rather
// than a loop. The daemon decides a monitor has stopped being collected from by how long its
// OLDEST uncollected picture has been waiting; the obvious wrong rule -- how long since the last
// hand-over -- is satisfied by a consumer that polls faster than the file's own rate, so a polling
// consumer could not tell the two apart. The KDE plugin fetches on FrameReady, which is this.
//
// The hand-over is posted as a task rather than made here: announcements arrive on the worker's own
// thread, inside its loop, and that thread is the only one that may touch the GPU.
class Collector {
public:
    Collector(WallpaperManager& manager, int monitorId, std::string client)
        : m_manager(manager), m_monitorId(monitorId), m_client(std::move(client)) {}

    // Separate from the constructor because the worker's frame callback has to name this object,
    // so this is built first -- and it must be destroyed after worker.stop(), which is what
    // guarantees no task is still running with a pointer to it.
    void attach(RenderWorker& worker) { m_worker = &worker; }

    void announced(int monitorId, uint64_t) {
        if (monitorId != m_monitorId || m_worker == nullptr) return;
        // One fetch outstanding at a time, as the plugin keeps: without it a burst of
        // announcements would queue a task each and the worker's task slots are finite.
        if (m_inFlight.exchange(true)) return;
        m_worker->postTask([this](RenderWorker::TaskState state) {
            if (state == RenderWorker::TaskState::Run) collectOnce();
            m_inFlight.store(false);
        });
    }

    int served() const { return m_served.load(); }
    int refused() const { return m_refused.load(); }
    std::string threw() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_threw;
    }
    std::string describe() const {
        return std::to_string(served()) + " collected, " + std::to_string(refused()) + " refused" +
               (threw().empty() ? std::string() : ", threw: " + threw());
    }

private:
    void collectOnce() {
        try {
            const wallpaperx::FrameExport e = m_manager.exportCurrentFrame(m_monitorId, m_client);
            if (!e.handle) {
                m_refused.fetch_add(1);
                return;
            }
            const uint64_t bufferId = e.handle->frame.bufferId;
            if (e.handle->frame.fd >= 0) close(e.handle->frame.fd);
            if (e.handle->acquireFd >= 0) close(e.handle->acquireFd);
            m_manager.releaseBuffer(m_client, m_monitorId, bufferId);
            m_served.fetch_add(1);
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_threw.empty()) m_threw = ex.what();
        }
    }

    WallpaperManager& m_manager;
    RenderWorker* m_worker = nullptr;
    int m_monitorId;
    std::string m_client;
    std::atomic<bool> m_inFlight{false};
    std::atomic<int> m_served{0};
    std::atomic<int> m_refused{0};
    mutable std::mutex m_mutex;
    std::string m_threw;
};

// Waits until this monitor has been quiet for `quiet`, and reports how many pictures it had by
// then. False means it never went quiet inside `limit` -- which every caller must branch on: a
// video that never stops is exactly what these phases are looking for, and treating a timeout as
// "it stopped" would make the whole off switch unfalsifiable.
bool quiesced(const FrameReadyLog& signals, int monitorId, std::chrono::milliseconds quiet,
              std::chrono::seconds limit, size_t& finalCount) {
    const Clock::time_point deadline = Clock::now() + limit;
    size_t last = signals.countFor(monitorId);
    Clock::time_point lastChange = Clock::now();
    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const size_t now = signals.countFor(monitorId);
        if (now != last) {
            last = now;
            lastChange = Clock::now();
            continue;
        }
        if (Clock::now() - lastChange >= quiet) {
            finalCount = last;
            return true;
        }
    }
    finalCount = signals.countFor(monitorId);
    return false;
}

// Monitor ids start again at 0 in each of the phases below, and may: every one builds a manager of
// its own, and a manager refuses an id at or past kMaxMonitors (32), which the earlier phases have
// nearly used up between them.
constexpr int kMonitorPlay = 0;
constexpr int kMonitorPlayStill = 1;

// How many pictures the rate and order checks grade. Two and a half passes of the ten-picture
// fixture, so the loop is crossed twice inside the graded run.
constexpr uint64_t kGradedPictures = 25;
// The generation the rate is measured from: past the first pictures, whose spacing carries the
// cost of opening the file.
constexpr uint64_t kRateFrom = 5;

void runClockChecks(const WallpaperManager::Config& config, const std::string& clip, const std::string& still) {
    const char* kGoals = "S13.133-S13.134, S13.137 and S13.143 (a video plays at the file's rate and in order, "
                         "loops, pauses while a render is owed, and leaves a still alone)";
    PhaseGrade grade("the clock phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    // Declared before the worker so it is destroyed after WorkerUnblock has stopped it: a task
    // still running with a pointer to a dead collector is a use-after-free in the harness.
    Collector collector(manager, kMonitorPlay, ":1.700");
    RenderWorker worker(manager, [&signals, &pictures, &collector](int monitorId, uint64_t generation) {
        signals.record(monitorId, generation);
        pictures.record(monitorId, generation);
        collector.announced(monitorId, generation);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};

    wallpaperx::testing::nameScreen(manager, kMonitorPlay, screenOf(kMonitorPlay));
    manager.requestWallpaper("video", clip, kMonitorPlay);
    worker.post(kMonitorPlay);
    if (!signals.wait(kMonitorPlay, 1)) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }
    g_files.resetCounts();

    // GOAL: S13.133, RESTATED for Stage E4 -- a video wallpaper keeps committing pictures FOR A
    //       CONSUMER THAT KEEPS COLLECTING THEM, one generation per picture, each announced exactly
    //       once and each exactly one past the last: none repeated, none skipped. And S13.134 -- it
    //       keeps going past the file's last picture, from the session it already has. False for a
    //       daemon that shows one picture and stops (what Stage E2 did), for one that drops pictures
    //       to catch up, for one that plays the file once and stalls at its end -- and now for one
    //       whose off switch stops a video somebody is watching.
    //
    //       The Stage E3 wording, "with nobody asking", is exactly what Stage E4 reverses: a video
    //       no consumer collects from now stops within a couple of seconds. That direction is graded
    //       in runWatchChecks below; what is left here is that collection keeps it running.
    // WHY-THIS-SHAPE: the consumer fetches only when a picture is ANNOUNCED, which is what the KDE
    //       plugin does. A consumer polling on a timer would satisfy the wrong rule as well -- "how
    //       long since the last hand-over" -- and so could not grade the one the daemon has.
    //       The wait is for an exact generation, so reaching it is what makes the run gradeable, and
    //       a run that does not reach it returns instead of measuring: a stalled clock must not be
    //       graded as a slow one. The order is read from the announcements rather than from
    //       GetStatus, which carries only the newest generation and so cannot say whether the one
    //       before it was ever announced.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, advanceSessionLocked()'s
    //       `round.shown.push_back(...)` deleted -> "FAIL: a video wallpaper keeps committing
    //       pictures for a consumer that collects them", after the 30-second signal limit.
    //       Committing the same generation twice -- `--it->second.generation;` after
    //       commitFrameLocked() -- reddens the ordering check instead. Observed 2026-09-18,
    //       re-observed under this wording 2026-09-20. Reverted.
    // COVERAGE-ASSERT: the consumer is required to have collected most of the run, so a daemon that
    //       ignored `watched` altogether -- and would keep playing for a consumer that had stopped
    //       -- cannot be reported as having been graded against a collecting one. And the number of
    //       successive pairs actually compared is asserted, so a future edit that leaves the loop
    //       with two announcements to look at fails instead of going quiet.
    const bool reached = signals.wait(kMonitorPlay, kGradedPictures);
    check(reached, "a video wallpaper keeps committing pictures for a consumer that collects them");
    if (!reached) {
        std::fprintf(stderr, "  the consumer: %s\n", collector.describe().c_str());
        notVerified(kGoals, "the video did not reach generation " + std::to_string(kGradedPictures) +
                                " on this host, so there is no run of pictures to grade");
        return;
    }
    reportUnless(collector.threw().empty(), "the consumer's hand-overs did not throw", collector.threw());
    reportUnless(static_cast<uint64_t>(collector.served()) * 2 >= kGradedPictures,
                 "the consumer collected most of the pictures, so the run is graded against a watched monitor",
                 collector.describe());
    {
        const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorPlay);
        size_t compared = 0;
        size_t outOfOrder = 0;
        std::string firstWrong;
        for (size_t i = 1; i < shown.size(); ++i) {
            ++compared;
            if (shown[i].generation == shown[i - 1].generation + 1) continue;
            ++outOfOrder;
            if (firstWrong.empty()) {
                firstWrong = std::to_string(shown[i - 1].generation) + " then " + std::to_string(shown[i].generation);
            }
        }
        check(compared + 1 >= kGradedPictures, "enough successive pictures were compared to grade their order");
        reportUnless(outOfOrder == 0,
                     "every picture is announced once and is the next generation -- none repeated, none skipped",
                     std::to_string(outOfOrder) + " of " + std::to_string(compared) +
                         " successive pairs are not consecutive; first: " + firstWrong);

        // GOAL: S13.133's rate -- the pictures are paced by the file's own timestamps, not drawn as
        //       fast as the GPU allows and not at some fixed rate of the daemon's.
        // WHY-THIS-SHAPE: INHERENTLY TEMPORAL -- pacing is a claim about time and nothing else can
        //       stand in for it. Kept stable by measuring across a fixed NUMBER of pictures rather
        //       than counting pictures in a fixed time, and by bounds wide enough (half the file's
        //       rate to four times it) that a loaded machine cannot cross them: at ten pictures a
        //       second the twenty graded intervals are 2.0 s, and the check admits 1.0 s to 8.0 s.
        //       The lower bound is what catches a clock that ignores the file and free-runs, which
        //       would finish the same twenty in the time twenty draws take.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, gapBetween() made to return
        //       kMinPictureInterval always -> "FAIL: the pictures are paced by the file's own rate",
        //       printing "20 pictures took 0.167 s, where the file's own rate makes them 2.000 s".
        //       Re-measured 2026-09-20 after the floor moved from 4 ms to 1/120 s, which is why the
        //       span is 0.167 s and not the 0.08 s this line recorded before.
        const auto at = [&shown](uint64_t generation) -> std::optional<Clock::time_point> {
            for (const PictureLog::Event& e : shown) {
                if (e.generation == generation) return e.at;
            }
            return std::nullopt;
        };
        const std::optional<Clock::time_point> from = at(kRateFrom);
        const std::optional<Clock::time_point> to = at(kGradedPictures);
        if (!from || !to) {
            notVerified("S13.133's rate", "the graded generations were not both announced, so no span can be measured");
        } else {
            const double seconds = std::chrono::duration<double>(*to - *from).count();
            const double nominal = static_cast<double>(kGradedPictures - kRateFrom) / kClipPictures;
            reportUnless(seconds >= nominal / 2 && seconds <= nominal * 4, "the pictures are paced by the file's own rate",
                         std::to_string(kGradedPictures - kRateFrom) + " pictures took " + std::to_string(seconds) +
                             " s, where the file's own rate makes them " + std::to_string(nominal) + " s");
            std::printf("video_wallpaper_test: %llu pictures took %.3f s (the file's rate makes them %.3f s)\n",
                        static_cast<unsigned long long>(kGradedPictures - kRateFrom), seconds, nominal);
            std::fflush(stdout);
        }

        // GOAL: S13.134 -- the video starts the file again from the session it already has, rather
        //       than stopping at its last picture or opening the file a second time.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, videoSessionLocked() given
        //       VideoPipeline::Playback::Once -> "FAIL: and starts it again without reopening the
        //       file", printing 2 opens since the first frame; the picture COUNT stays above ten even
        //       then, because the failure at the file's end posts a render that reopens and goes on,
        //       which is why the reopen count is asserted beside it. Observed 2026-09-18. Reverted.
        reportUnless(shown.size() > kClipPictures,
                     "the video plays past the last picture in the file instead of stopping at it",
                     std::to_string(shown.size()) + " picture(s) from a file holding " +
                         std::to_string(kClipPictures));
        reportUnless(g_files.totalOpens() == 0, "and starts it again without reopening the file",
                     std::to_string(g_files.totalOpens()) + " open(s) since the first frame");
    }

    // GOAL: S13.137 -- a monitor plays only while it is settled. A screen reporting a new size owes
    //       that monitor a render, and until that render has run NO picture is shown; once it has,
    //       the pictures start again. False for a clock that keeps drawing this session's pictures
    //       into a monitor whose images are about to be rebuilt for a different size.
    // WHY-THIS-SHAPE: the worker is parked inside a task of this test's while the new size is
    //       recorded, so the count read either side of that moment is provably not a count taken
    //       mid-round. The simpler shape -- record the size and compare counts -- cannot tell a
    //       picture the clock committed before the size was recorded from one it committed after,
    //       and that is exactly the distinction the goal is about. The quiet window is a second,
    //       ten of this file's picture intervals; a correct daemon shows nothing in it however long
    //       it is, so the length buys strength and cannot buy flakiness.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, playingSeqLocked()'s
    //       `it->second.requestSeq != it->second.servicedSeq` clause deleted -> "FAIL: no picture is
    //       shown while the monitor owes a render", printing about ten pictures shown in the quiet
    //       second. Observed 2026-09-18. Reverted.
    std::vector<int> owed;
    uint64_t pausedAt = 0;
    {
        const HeldWorker hold(worker);
        check(hold.held(), "the render worker can be parked between pictures");
        if (!hold.held()) {
            notVerified(kGoals, "the render worker could not be parked, so nothing can be read at a known moment");
            return;
        }
        manager.setOutputSize(screenOf(kMonitorPlay), kResizedWidth, kResizedHeight, owed);
        pausedAt = signals.countFor(kMonitorPlay);
    }
    reportUnless(contains(owed, kMonitorPlay), "a new size leaves the playing monitor owing a render",
                 "setOutputSize did not name it for a redraw");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    reportUnless(signals.countFor(kMonitorPlay) == pausedAt, "no picture is shown while the monitor owes a render",
                 std::to_string(signals.countFor(kMonitorPlay) - pausedAt) + " picture(s) were shown in a quiet second");

    for (const int id : owed) worker.post(id);
    const bool resumed = signals.waitAtLeast(kMonitorPlay, pausedAt + 3);
    reportUnless(resumed, "and the pictures start again once that render has run",
                 "no third picture arrived after the redraw");
    if (!resumed) return;

    // GOAL: S13.143 -- a still wallpaper is untouched while a video plays beside it: it commits when
    //       it is asked to and never again, and its frame stays exportable. False for a clock whose
    //       rounds announce or redraw monitors that hold no video.
    // WHY-THIS-SHAPE: the still's announcements are counted across a window in which the video is
    //       provably busy -- the video's own count is asserted to have grown -- so "the still did not
    //       move" cannot be read off a window in which nothing moved at all.
    // CONTROL: in backend/daemon/render_worker.cpp, runClock() made to announce each picture for the
    //       NEXT monitor id as well as its own -> "FAIL: a still wallpaper beside a playing video is
    //       left exactly as it was drawn", printing "37 announcements for a still drawn once". Moving
    //       the announcement to the neighbour instead of duplicating it cannot be used: the video's
    //       own announcements stop, so the phase abandons before this check is reached. Observed
    //       2026-09-18. Reverted.
    // COVERAGE-ASSERT: the video's own picture count over the same window is asserted to have grown
    //       by at least five, so the still's steadiness is never read off a stalled daemon.
    wallpaperx::testing::nameScreen(manager, kMonitorPlayStill, screenOf(kMonitorPlayStill));
    manager.requestWallpaper("static", still, kMonitorPlayStill);
    worker.post(kMonitorPlayStill);
    if (!signals.wait(kMonitorPlayStill, 1)) {
        check(false, "the still wallpaper beside the video commits");
        return;
    }
    const size_t videoBefore = signals.countFor(kMonitorPlay);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const size_t videoAfter = signals.countFor(kMonitorPlay);
    reportUnless(videoAfter >= videoBefore + 5, "the video kept playing across the window the still is judged in",
                 std::to_string(videoAfter - videoBefore) + " picture(s) in a second");
    reportUnless(signals.countFor(kMonitorPlayStill) == 1,
                 "a still wallpaper beside a playing video is left exactly as it was drawn",
                 std::to_string(signals.countFor(kMonitorPlayStill)) + " announcements for a still drawn once");
    const std::optional<wallpaperx::MonitorStatus> stillStatus = monitorStatus(manager, kMonitorPlayStill);
    reportUnless(stillStatus && stillStatus->hasFrame && stillStatus->lastError.empty(),
                 "and it still holds its frame with no error",
                 stillStatus ? "last_error: " + stillStatus->lastError : std::string("no status"));
}

constexpr int kMonitorParked = 0;

void runOnePictureChecks(const WallpaperManager::Config& config, const std::string& onePicture) {
    const char* kGoals = "S13.135 (a file holding one picture is shown once and decoded no further, and a resize "
                         "still redraws it)";
    PhaseGrade grade("the one-picture phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    wallpaperx::testing::nameScreen(manager, kMonitorParked, screenOf(kMonitorParked));
    manager.requestWallpaper("video", onePicture, kMonitorParked);
    worker.post(kMonitorParked);
    if (!signals.wait(kMonitorParked, 1) || !drainWorker(worker)) {
        grade.expectNoDevice();
        notVerified(kGoals, "the one-picture video produced no frame on this host");
        return;
    }

    // GOAL: S13.135 -- a video file that yields exactly one picture is shown once and then left
    //       alone: nothing is decoded from it again, no further generation is announced, and the
    //       monitor keeps the frame it was given. False for a clock that asks such a file for its
    //       next picture for ever, which gets the same picture back after every restart and
    //       announces it as new -- a still image redrawn tens of times a second.
    // WHY-THIS-SHAPE: the drain above is what makes the counters mean something. The round that
    //       parks the session runs after the job that drew it, and it decodes once -- that is how
    //       the daemon learns the file has nothing after the first picture. Resetting the counters
    //       before that round had run would count its decode as a failure to park. INHERENTLY
    //       TEMPORAL in one direction only: nothing happening can only be observed by waiting. The
    //       window is half the phase's idleSessionRelease, because that is where "however long" ends
    //       -- the parked session is released one idle interval after it parks, and that release is a
    //       close this window would count.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, advanceSessionLocked()'s
    //       `if (before.startsPass && after.startsPass)` -- the branch that records the file as
    //       parked -- disabled, so the round falls through to the Waiting line below it -> "FAIL: a
    //       video file holding one picture is never decoded again", printing 20 packets in the quiet
    //       second, and "FAIL: and its one picture is announced once, not once per round", printing
    //       10 further announcements, with the overtaken-rebuild count in the resize phase red
    //       beside them. Rewritten for Stage E4, which moved the fact from a clock variant onto the
    //       monitor; observed 2026-09-20. Reverted.
    g_decodeCalls.store(0);
    g_files.resetCounts();
    const size_t announced = signals.countFor(kMonitorParked);
    std::this_thread::sleep_for(config.idleSessionRelease / 2);
    reportUnless(g_decodeCalls.load() == 0, "a video file holding one picture is never decoded again",
                 std::to_string(g_decodeCalls.load()) + " packet(s) in a quiet window");
    reportUnless(signals.countFor(kMonitorParked) == announced,
                 "and its one picture is announced once, not once per round",
                 std::to_string(signals.countFor(kMonitorParked) - announced) + " further announcement(s)");
    reportUnless(g_files.totalOpens() == 0 && g_files.totalCloses() == 0, "and its file is neither reopened nor closed",
                 std::to_string(g_files.totalOpens()) + " open(s), " + std::to_string(g_files.totalCloses()) +
                     " close(s)");
    const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMonitorParked);
    reportUnless(status && status->hasFrame && status->lastError.empty(), "and the monitor still holds its frame",
                 status ? "last_error: " + status->lastError : std::string("no status"));

    // GOAL: S13.135's second half -- a parked session is still a session: a screen reporting a new
    //       size redraws that monitor from the picture the session holds, opening no file and
    //       decoding nothing. False for a daemon that frees a session it will never advance again,
    //       which would make every resize of a one-picture video reopen and decode it.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, dropStaleVideoSessionLocked()'s keep-it
    //       `return` disabled (`&& false`) -> "FAIL: a resize of a parked one-picture video redraws
    //       it from the session, opening and decoding nothing", printing "1 open(s), 1 packet(s)".
    //       Observed 2026-09-18. Reverted.
    g_decodeCalls.store(0);
    g_files.resetCounts();
    std::vector<int> owed;
    manager.setOutputSize(screenOf(kMonitorParked), kResizedWidth, kResizedHeight, owed);
    for (const int id : owed) worker.post(id);
    const bool redrawn = signals.wait(kMonitorParked, 2);
    check(redrawn, "a new size redraws the parked one-picture video");
    if (redrawn) {
        reportUnless(g_files.totalOpens() == 0 && g_decodeCalls.load() == 0,
                     "a resize of a parked one-picture video redraws it from the session, opening and decoding nothing",
                     std::to_string(g_files.totalOpens()) + " open(s), " + std::to_string(g_decodeCalls.load()) +
                         " packet(s)");
    }

    // GOAL: S13.160 -- a one-picture video releases its decoder too. Its session is kept for a
    //       while after it parks, which is what the resize above redraws from, and then freed on
    //       the same terms as any stopped video's: the file is closed and the picture pool goes,
    //       while the monitor keeps the picture on screen. False for a daemon that parks a session
    //       for ever -- a decoder and a picture pool per screen, held for the life of the process
    //       for a wallpaper that is a still image in all but name -- and false for one that frees
    //       it so fast the resize above has to reopen the file.
    // WHY-THIS-SHAPE: INHERENTLY TEMPORAL, and kept stable by waiting for the CLOSE rather than by
    //       sleeping for a fixed span and looking: the wait has a deadline well past the release
    //       interval and a failure branch, so a session that is never freed fails this named check
    //       instead of being timed. The file is asserted to be still open first -- read right after
    //       the resize, inside the interval -- so "it was closed" cannot be satisfied by a daemon
    //       that had already closed it before the resize, which the resize check above would then
    //       be grading against a reopen.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, advanceSessionLocked()'s stopped branch
    //       reduced to `if (!session.idleSince) session.idleSince = now; return std::nullopt;`, so
    //       nothing is ever freed -> "FAIL: a parked one-picture video's decoder and file are
    //       released once it has been idle", printing the fixture still open after the wait.
    //       Observed 2026-09-20. Reverted.
    //       NOT graded here: whether the park branch SCHEDULES that release. Making it return
    //       nothing leaves this check green, because the resize above posts a job and the round
    //       that follows any job starts the countdown anyway. runParkedReleaseChecks below is the
    //       phase that can fail on it -- one monitor and nothing else ever posted.
    {
        const std::multiset<std::string> openNow = g_files.live();
        reportUnless(openNow.count(onePicture) == 1,
                     "the parked one-picture video still holds its file open just after the resize",
                     "open files: " + describeFiles(openNow));
        g_files.resetCounts();
        const bool released = settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(onePicture) >= 1; });
        reportUnless(released, "a parked one-picture video's decoder and file are released once it has been idle",
                     "open files after the wait: " + describeFiles(g_files.live()));
        const std::optional<wallpaperx::MonitorStatus> after = monitorStatus(manager, kMonitorParked);
        reportUnless(after && after->hasFrame, "and the picture it drew stays on the screen through the release",
                     after ? "has_frame: false" : std::string("no status"));
    }
}

// --------------------------------------------------------------------------- //
// Stage E4: the off switch
// --------------------------------------------------------------------------- //

constexpr int kMonitorWatch = 0;

// How long the "nothing moved" windows below are: six of this file's picture intervals, so a
// daemon that had not stopped shows about six pictures in one. It cuts both ways, which is what
// bounds it below: the same window also decides that a video still PLAYING has not stopped, so a
// playing video must never go this long without a picture -- and a 10-a-second clip on the software
// driver under a parallel run has never been seen to pause for more than a small fraction of it.
constexpr auto kQuietWindow = std::chrono::milliseconds(600);

// How long a run of pictures may take before it counts as "did not stop promptly": three of the
// daemon's own unwatched intervals, read from the config the phase was built with. A correct stop
// lands about one interval after the first uncollected picture; three is far past a scheduling
// hiccup and nowhere near a daemon that never stops.
std::chrono::steady_clock::duration stopBy(const WallpaperManager::Config& config) { return 3 * config.unwatchedAfter; }

void runOffSwitchChecks(const WallpaperManager::Config& config, const std::string& clip) {
    const char* kGoals = "S13.146-S13.148 (a video nobody collects stops and keeps its picture, its decoder is "
                         "released after that, and a hand-over starts it again)";
    PhaseGrade grade("the off-switch phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager, [&signals, &pictures](int monitorId, uint64_t generation) {
        signals.record(monitorId, generation);
        pictures.record(monitorId, generation);
    });
    WorkerUnblock unblock{worker};

    wallpaperx::testing::nameScreen(manager, kMonitorWatch, screenOf(kMonitorWatch));
    manager.requestWallpaper("video", clip, kMonitorWatch);
    worker.post(kMonitorWatch);
    if (!signals.wait(kMonitorWatch, 1)) {
        grade.expectNoDevice();
        notVerified(kGoals, "the video render on this host produced no frame, so there was nothing to stop");
        return;
    }

    // GOAL: S13.146 -- a video whose pictures NOTHING COLLECTS stops within about two seconds and
    //       keeps its last picture on the screen. False for Stage E3's clock, which decoded and
    //       committed for ever whether or not any consumer was reading -- a locked screen, or a
    //       desktop whose plugin had gone, cost a decode and a GPU pass per picture for as long as
    //       the daemon ran. Also false for a daemon that stops by clearing the frame, which would
    //       blank the screen instead of freezing it.
    // WHY-THIS-SHAPE: "it stopped" is read as a QUIESCENCE with a deadline and a failure branch --
    //       the count has to stop changing for a full second -- and never as a timeout falling
    //       through into arithmetic. A wait for an exact generation cannot be used in either
    //       direction here: how many pictures fit in the daemon's own interval is not a number this
    //       file may pin, and waiting for one that will never arrive is how a stall gets graded as
    //       a slow drain. The span the pictures took is asserted separately, which is what makes
    //       this "stopped promptly" rather than merely "stopped eventually".
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, playingSeqLocked() made to return `current`
    //       without consulting watchedLocked() -- the Stage E3 predicate -> "FAIL: a video no
    //       consumer is collecting from stops on its own", printing 251 pictures still arriving
    //       after 25 s, and the visibility phase's own stop line red beside it. Observed
    //       2026-09-20. Reverted.
    // COVERAGE-ASSERT: the number of pictures shown before the stop is asserted to be more than
    //       one, so a daemon that never played at all -- or one that stopped before its second
    //       picture, which would make every later check here read off a video that never ran --
    //       fails instead of being reported as having stopped correctly.
    size_t stoppedAt = 0;
    const bool stopped = quiesced(signals, kMonitorWatch, kQuietWindow, std::chrono::seconds(25), stoppedAt);
    check(stopped, "a video no consumer is collecting from stops on its own");
    if (!stopped) {
        std::fprintf(stderr, "  it was still announcing pictures after 25 s; %zu shown\n", stoppedAt);
        notVerified(kGoals, "the video never stopped, so nothing below could be read against a stopped one");
        return;
    }
    check(stoppedAt > 1, "and it had played more than one picture before it stopped");
    {
        const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorWatch);
        if (shown.size() < 2) {
            notVerified(kGoals, "fewer than two pictures were ever announced, so no span can be measured");
            return;
        }
        const auto span = shown.back().at - shown.front().at;
        std::printf("video_wallpaper_test: the unwatched video ran for %.3f s (%zu pictures)\n",
                    std::chrono::duration<double>(span).count(), shown.size());
        std::fflush(stdout);
        reportUnless(span <= stopBy(config), "and it stopped within a few seconds of its first picture, not eventually",
                     std::to_string(std::chrono::duration<double>(span).count()) + " s of pictures");
    }
    const uint64_t frozenAt = static_cast<uint64_t>(stoppedAt);
    {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMonitorWatch);
        reportUnless(status && status->hasFrame && status->generation == frozenAt && status->lastError.empty(),
                     "the stopped monitor keeps the last picture it drew, with no error",
                     status ? "has_frame " + std::string(status->hasFrame ? "true" : "false") + ", generation " +
                                  std::to_string(status->generation) + " against " + std::to_string(frozenAt) +
                                  ", last_error: " + status->lastError
                            : std::string("no status"));
        // GOAL: and GetStatus says WHY it stopped, in the two facts the daemon decided from. A
        //       stopped wallpaper reads identically in every other field, so without these an
        //       operator cannot tell a video that was switched off from one that is broken.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, snapshot()'s
        //       `entry.watched = watchedLocked(monitorId, state);` replaced by `entry.watched =
        //       true;` -> "FAIL: and it says the monitor is no longer watched, which is why".
        //       Observed 2026-09-20. Reverted.
        reportUnless(status && !status->watched, "and it says the monitor is no longer watched, which is why",
                     status ? "watched: true" : std::string("no status"));
        reportUnless(status && status->screenVisible,
                     "while the screen itself is still reported as being drawn, so the reason is not confused",
                     status ? "screen_visible: false" : std::string("no status"));
    }

    // GOAL: S13.147 -- a session that has been stopped for a few seconds is closed: its file is
    //       released and its decoder and picture pool go with it. Not at once, which is the other
    //       half: the interval is what keeps a menu, a notification or a window animation from
    //       costing the file a reopen. The monitor keeps its picture throughout, so nothing on the
    //       screen changes. False for a daemon that holds a decoder per screen for the life of the
    //       process, and false for one that frees it the moment playback stops.
    // WHY-THIS-SHAPE: the file is read from open(2) and close(2), which is the only thing
    //       that can answer whether a session is resident: a stopped monitor reports the same frame
    //       whether its decoder was kept or freed. The "still open" reading comes first and is what
    //       makes the release meaningful -- without it a daemon that had already closed the file
    //       before the video stopped would pass.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, advanceSessionLocked()'s stopped branch
    //       made to `return std::nullopt;` before the release -- so nothing is ever freed -> "FAIL:
    //       a video stopped for a few seconds has its file and decoder released", printing the clip
    //       still open, together with the one-picture phase's release line and the reopen line
    //       below. Making it release immediately (`const SteadyTime releaseAt = *session.
    //       idleSince;`) reddens "the stopped video still holds its file open just after stopping"
    //       -- and a dozen other checks in this file with it, since a session freed the moment it
    //       stops breaks every reuse and resize property Stage E2 established. Both observed
    //       2026-09-20. Reverted.
    {
        const std::multiset<std::string> openNow = g_files.live();
        reportUnless(openNow.count(clip) == 1, "the stopped video still holds its file open just after stopping",
                     "open files: " + describeFiles(openNow));
    }
    g_files.resetCounts();
    const bool releasedSession = settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(clip) >= 1; });
    const Clock::time_point releasedAt = Clock::now();
    check(releasedSession, "a video stopped for a few seconds has its file and decoder released");
    if (!releasedSession) {
        std::fprintf(stderr, "  open files after the wait: %s\n", describeFiles(g_files.live()).c_str());
    }

    // GOAL: S13.147's length -- the release comes the CONFIGURED idleSessionRelease after the
    //       video stopped, not at some other interval. Nothing else in this file reads that setting,
    //       so without this a daemon that ignored it, and released at its compiled-in five seconds
    //       or at once, would satisfy every check above.
    // WHY-THIS-SHAPE: measured from the FIRST picture, which nothing here ever collects: the video
    //       stops once that picture has waited unwatchedAfter, and is released idleSessionRelease
    //       after that, so the close is due at their sum. The lower bound is the sum less a margin
    //       for the poll; the upper bound allows a second and a half of late waking. Both bounds read
    //       the phase's own config.
    // CONTROL: backend/daemon/wallpaper_manager.cpp, the release computed from kIdleSessionRelease
    //       instead of m_config.idleSessionRelease -> "FAIL: the stopped video is released after the
    //       configured idle interval, not sooner or later", released 5.816 s after its first picture
    //       where 2.750 s was due. Reverted.
    if (releasedSession) {
        const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorWatch);
        if (shown.empty()) {
            notVerified("S13.147's length", "no picture was ever announced, so there is nothing to measure from");
        } else {
            const double seconds = std::chrono::duration<double>(releasedAt - shown.front().at).count();
            const double due =
                std::chrono::duration<double>(config.unwatchedAfter + config.idleSessionRelease).count();
            std::printf("video_wallpaper_test: the uncollected video's session was released %.3f s after its first "
                        "picture (due at %.3f s)\n",
                        seconds, due);
            std::fflush(stdout);
            reportUnless(seconds >= due - 0.1 && seconds <= due + 1.5,
                         "the stopped video is released after the configured idle interval, not sooner or later",
                         std::to_string(seconds) + " s after its first picture, where " + std::to_string(due) +
                             " s is due");
        }
    }
    {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMonitorWatch);
        reportUnless(status && status->hasFrame && status->generation == frozenAt,
                     "and the picture stays on the screen across the release, so there is no blank",
                     status ? "has_frame " + std::string(status->hasFrame ? "true" : "false") + ", generation " +
                                  std::to_string(status->generation)
                            : std::string("no status"));
    }

    // GOAL: S13.148 -- a hand-over to a named consumer starts a stopped video again, and a video
    //       whose session was released opens its file afresh to do it. That fresh open is what
    //       makes a resumed video play from the file's beginning: nothing of the old session
    //       survives it, and a VideoPipeline is constructed at the file's first picture (which
    //       renderer/tests/video_pipeline_timing_* grades for S13.145). False for a daemon that
    //       leaves a stopped video stopped -- a desktop that comes back to a frozen wallpaper for
    //       ever -- and false for one that needs a render to be posted before it will reopen,
    //       since nothing posts one: the monitor's request is already serviced.
    // WHY-THIS-SHAPE: nothing but the hand-over is done. No render is posted and no size reported,
    //       so the pictures that follow can only have come from the daemon deciding for itself that
    //       the monitor is watched again. The reopen is counted from a reset taken before the
    //       hand-over, so a file opened during the release above is not credited to it.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `if (!client.empty())
    //       noteHandoverServed(monitorId);` line deleted from exportCurrentFrame() -> "FAIL: a
    //       hand-over to a named consumer starts a stopped video again", after the 30-second signal
    //       limit. Deleting the reopen loop body in reopenVideosLocked() -- `if (!owed) continue;`
    //       changed to `continue;` -- reddens the same line for the other half, since the monitor
    //       becomes watched with no session to play from. Both observed 2026-09-20. Reverted.
    g_files.resetCounts();
    const Handover resume = handOver(manager, worker, kMonitorWatch, ":1.801");
    check(resume.served, "the stopped video's last picture can still be handed over");
    if (!resume.served) {
        std::fprintf(stderr, "  %s\n", describe(resume).c_str());
        notVerified(kGoals, "no hand-over was served, so nothing could have restarted the video");
        return;
    }
    const bool restarted = signals.waitAtLeast(kMonitorWatch, frozenAt + 1);
    check(restarted, "a hand-over to a named consumer starts a stopped video again");
    if (restarted) {
        reportUnless(g_files.opens(clip) == 1,
                     "and a video whose session was released opens its file again to do it, from the beginning",
                     std::to_string(g_files.opens(clip)) + " open(s) of the clip since the hand-over");
    }
}

constexpr int kMonitorParkedIdle = 0;

// S13.160's other half, on a manager whose ONE monitor is posted once and never touched again.
//
// That isolation is the check. A one-picture video parks in the middle of a clock round, and if
// that round asks for nothing further the render worker has no deadline to wake on: it sleeps
// until something else is posted, and the decoder and picture pool stay held for the life of the
// daemon. Every other phase in this file posts something sooner or later -- a resize, another
// monitor -- and a round runs after any job, so the countdown starts whatever the park branch
// returned. Only a phase that posts nothing can tell the two apart.
void runParkedReleaseChecks(const WallpaperManager::Config& config, const std::string& onePicture) {
    const char* kGoal = "S13.160's scheduling half (a one-picture video that parks asks for the round that frees "
                        "its decoder, with nothing else posted)";
    PhaseGrade grade("the parked-release phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    wallpaperx::testing::nameScreen(manager, kMonitorParkedIdle, screenOf(kMonitorParkedIdle));
    manager.requestWallpaper("video", onePicture, kMonitorParkedIdle);
    worker.post(kMonitorParkedIdle);
    if (!signals.wait(kMonitorParkedIdle, 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the one-picture video produced no frame on this host");
        return;
    }

    // GOAL: a one-picture video that parks releases its decoder on its own, without anything else
    //       being asked of the daemon. False for a clock that parks by scheduling nothing: the
    //       worker then has no deadline to wake on, and a screen showing a one-picture video holds
    //       a decoder and a picture pool until the user next changes something -- which on a
    //       desktop is for the rest of the session.
    // WHY-THIS-SHAPE: nothing is posted, drained or resized after the first render, and that is the
    //       whole check. A drain would post a task, and the round that follows any job starts the
    //       countdown regardless -- which is exactly why the one-picture phase above cannot grade
    //       this. INHERENTLY TEMPORAL in one direction: the wait has a deadline well past the
    //       release interval and a failure branch, so a decoder that is never freed fails a named
    //       check rather than being timed.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, advanceSessionLocked()'s park branch made
    //       to `return std::nullopt;` instead of `return now;` -- the Stage E3 shape -> "FAIL: a
    //       one-picture video releases its decoder with nothing else asked of the daemon",
    //       printing the fixture still open after 20 s. Observed 2026-09-20. Reverted.
    g_files.resetCounts();
    const bool released = settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(onePicture) >= 1; });
    check(released, "a one-picture video releases its decoder with nothing else asked of the daemon");
    if (!released) {
        std::fprintf(stderr, "  open files after the wait: %s\n", describeFiles(g_files.live()).c_str());
    }
    const std::optional<wallpaperx::MonitorStatus> after = monitorStatus(manager, kMonitorParkedIdle);
    reportUnless(after && after->hasFrame, "and its picture stays on the screen",
                 after ? "has_frame: false" : std::string("no status"));
}

constexpr int32_t kMonitorVisible = 0;

// Over the bus, because the resume is a chain and only the whole chain is the property: the method
// records the answer, the manager decides the monitor is watchable again, and the worker is WOKEN
// so a round runs at once. A manager-level phase would have to call wake() itself and would then
// be grading the daemon's own plumbing with a stand-in for the part most likely to be missing --
// measured on Stage E3, where a manager change that nothing woke the worker for took effect only
// when something else happened to be posted.
// The newest generation this monitor has been announced at, which over the bus is NOT the number
// of announcements: releasing an image re-announces the frame it frees, so a consumer that
// collects and releases produces a FrameReady per release with no new picture behind it. Counting
// those made the first draft of this phase report five thousand pictures for a stopped video.
uint64_t latestGeneration(const PictureLog& pictures, int monitorId) {
    uint64_t newest = 0;
    for (const PictureLog::Event& e : pictures.forMonitor(monitorId)) newest = std::max(newest, e.generation);
    return newest;
}

// Waits until no NEW picture has been announced for `quiet`. quiesced() above cannot be used over
// the bus for the reason latestGeneration() states: a release re-announces the frame it frees, so
// the number of announcements grows without a picture behind it. False means it never went quiet
// inside `limit`, which every caller must branch on.
bool quiescedByGeneration(const PictureLog& pictures, int monitorId, std::chrono::milliseconds quiet,
                          std::chrono::seconds limit) {
    uint64_t last = latestGeneration(pictures, monitorId);
    Clock::time_point lastChange = Clock::now();
    const Clock::time_point deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const uint64_t now = latestGeneration(pictures, monitorId);
        if (now != last) {
            last = now;
            lastChange = Clock::now();
            continue;
        }
        if (Clock::now() - lastChange >= quiet) return true;
    }
    return false;
}

// `config` is the one the phase's manager was built with: every wait and bound below reads the
// daemon's intervals from it.
void runVisibilityChecks(sdbus::IProxy& proxy, const PictureLog& pictures, const std::string& clip,
                         const WallpaperManager::Config& config) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputVisible;
    const char* kGoals = "S13.154-S13.157 (a collecting consumer keeps a video playing, a covered screen's video stops "
                         "the screen coming back starts it again)";
    const std::string screen = "DP-VIS";

    setMonitorOutput(proxy, kMonitorVisible, screen);
    setWallpaper(proxy, "video", clip, kMonitorVisible);
    if (!settlesWithin(std::chrono::seconds(30), [&] { return latestGeneration(pictures, kMonitorVisible) > 0; })) {
        notVerified(kGoals, "the video render on this host produced no frame, so there was nothing to cover");
        return;
    }
    const auto statusOf = [&proxy]() -> nlohmann::json {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorVisible));
        return entry != nullptr ? *entry : nlohmann::json::object();
    };

    // Collects every announced picture and gives it straight back -- which is what the KDE plugin
    // does -- while watching for the monitor to go quiet. One loop rather than a collector thread
    // because both halves go through the same proxy, and the pair is inseparable anyway: "it
    // stopped" is only attributable to the screen report if something was still collecting when it
    // stopped.
    struct Watched {
        bool quiet = false;      // no NEW picture for the whole quiet window
        uint64_t newest = 0;     // the newest generation announced by the end
        int collected = 0;       // hand-overs served to this consumer inside the window
    };
    const auto collectUntilQuiet = [&proxy, &pictures](std::chrono::milliseconds quiet, std::chrono::seconds limit) {
        Watched out;
        out.newest = latestGeneration(pictures, kMonitorVisible);
        Clock::time_point lastChange = Clock::now();
        const Clock::time_point deadline = Clock::now() + limit;
        while (Clock::now() < deadline) {
            const uint64_t now = latestGeneration(pictures, kMonitorVisible);
            if (now != out.newest) {
                out.newest = now;
                lastChange = Clock::now();
                try {
                    const wallpaperx::testing::Frame frame = getFrameFd(proxy, kMonitorVisible);
                    wallpaperx::testing::releaseBuffer(proxy, kMonitorVisible, frame.bufferId);
                    ++out.collected;
                } catch (const sdbus::Error&) {
                    // A refusal is an ordinary answer and says nothing about these goals.
                }
                continue;
            }
            if (Clock::now() - lastChange >= quiet) {
                out.quiet = true;
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return out;
    };

    // GOAL: S13.154 -- a consumer that keeps collecting keeps the video playing. This is the
    //       starting state everything below is measured against, and it is a claim in its own
    //       right: the off switch must not stop a wallpaper somebody is watching.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, noteHandoverServed() made to set
    //       `uncollectedSince = now` instead of resetting it -- the obvious wrong rule, "how long
    //       since the last hand-over" -> the video still plays here, because this file's pictures
    //       are a tenth of a second apart; it is runSlowPictureChecks that reddens on that
    //       mutation. Deleting the `if (!client.empty()) noteHandoverServed(monitorId);` line
    //       instead -> "FAIL: a video whose pictures are being collected keeps playing". Observed
    //       2026-09-20. Reverted.
    // Three seconds: four of the phase's unwatched intervals, which is what a daemon applying the
    // wrong rule would need to stop a video its consumer is collecting from.
    const Watched whilePlaying = collectUntilQuiet(kQuietWindow, std::chrono::seconds(3));
    check(!whilePlaying.quiet && whilePlaying.collected > 0,
          "a video whose pictures are being collected keeps playing");
    if (whilePlaying.quiet || whilePlaying.collected == 0) {
        std::fprintf(stderr, "  it reached generation %llu, %d collected; the video was not playing for a "
                             "collecting consumer, so covering the screen would establish nothing\n",
                     static_cast<unsigned long long>(whilePlaying.newest), whilePlaying.collected);
        notVerified(kGoals, "the video was not playing for a collecting consumer before the screen was covered");
        return;
    }

    // GOAL: S13.155 -- a screen that reports it is NOT BEING DRAWN stops its video on the next
    //       round, whatever any consumer is doing, and GetStatus says so. This is the half the
    //       daemon cannot work out for itself: a lock screen, a fullscreen window or another
    //       session leaves every other fact about the monitor unchanged. False for a daemon that
    //       only counts hand-overs, which would keep decoding at full rate for a desktop whose
    //       plugin is dutifully fetching frames nobody can see.
    // WHY-THIS-SHAPE: the consumer goes on collecting through the whole stop, so "it stopped" is
    //       attributable to the report and to nothing else -- the check above has just shown that
    //       the same loop keeps this video running. The stop is read as a quiescence with a
    //       deadline and a failure branch, never as a wait that expires.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, screenDrawnLocked() made to `return true;`
    //       -> "FAIL: a screen that says it is not being drawn stops its video", after the 25 s
    //       quiescence limit. Observed 2026-09-20. Reverted.
    // COVERAGE-ASSERT: the check above is the guard -- the same loop, in the same phase, is
    //       required to have kept this video running for three seconds first. Nothing is asserted
    //       about hand-overs INSIDE the stopping window, and deliberately: a correct daemon stops
    //       within a round, so there may be no further picture to collect, and requiring one would
    //       make the check fail for the daemon behaving exactly as the goal says.
    setOutputVisible(proxy, screen, false);
    const Watched whileCovering = collectUntilQuiet(kQuietWindow, std::chrono::seconds(25));
    check(whileCovering.quiet, "a screen that says it is not being drawn stops its video");
    const uint64_t stoppedAt = whileCovering.newest;
    if (!whileCovering.quiet) {
        std::fprintf(stderr, "  it was still announcing pictures 25 s after the screen reported itself dark\n");
        notVerified(kGoals, "the covered screen's video never stopped, so nothing below has a starting state");
        return;
    }
    {
        const nlohmann::json entry = statusOf();
        check(entry.value("has_frame", false), "and the monitor keeps the picture it stopped on");
        check(!entry.value("screen_visible", true), "and GetStatus reports the screen as not being drawn");
        check(!entry.value("watched", true), "and the monitor as no longer watched");
    }

    // GOAL: S13.156 -- a covered screen cannot be restarted by FETCHING. Collection is only half of
    //       what makes a monitor watched; a consumer that goes on asking for frames -- which the
    //       plugin does, because a screen that is merely occluded still has an item asking -- must
    //       not be able to override what the screen itself said. False for a daemon whose only
    //       evidence is the hand-over: the decoder would run for the whole time a screen was
    //       locked, which is the cost this stage exists to remove.
    // WHY-THIS-SHAPE: the hand-overs are asserted to have been SERVED, so "nothing restarted"
    //       cannot be satisfied by a fetch that failed for an unrelated reason. The window is
    //       kQuietWindow, six of this file's picture intervals; a correct daemon shows nothing in it
    //       however long it is.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, noteHandoverServed() given a loop that
    //       also erases this monitor's screen from m_outputsNotDrawn -- "the hand-over is evidence
    //       the screen is being drawn", which is the shape this check exists to forbid -> "FAIL: a
    //       covered screen's video is not restarted by a consumer that keeps fetching", printing
    //       7 new pictures across 12 served hand-overs in the 600 ms window. Observed 2026-09-21.
    //       Reverted.
    //       Deleting watchedLocked()'s `if (!screenDrawnLocked(monitorId)) return false;` is NOT a
    //       usable control here: the video then never stops, so the phase abandons at the check
    //       above and this line is never reached.
    int servedWhileCovered = 0;
    const Clock::time_point coveredUntil = Clock::now() + kQuietWindow;
    while (Clock::now() < coveredUntil) {
        try {
            const wallpaperx::testing::Frame frame = getFrameFd(proxy, kMonitorVisible);
            wallpaperx::testing::releaseBuffer(proxy, kMonitorVisible, frame.bufferId);
            ++servedWhileCovered;
        } catch (const sdbus::Error&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(servedWhileCovered > 0, "a covered screen still serves the picture it stopped on");
    {
        const uint64_t afterFetching = latestGeneration(pictures, kMonitorVisible);
        reportUnless(afterFetching == stoppedAt,
                     "a covered screen's video is not restarted by a consumer that keeps fetching",
                     std::to_string(afterFetching - stoppedAt) + " new picture(s) across " +
                         std::to_string(servedWhileCovered) + " served hand-over(s)");
    }

    // GOAL: S13.157 -- the screen reporting that it is being drawn again starts the video, with
    //       nothing else asked for: no render posted, no size reported, no fetch first. The report
    //       is the only event, so the daemon has to credit the monitor with having been seen, run a
    //       round at once, and -- since the session has been freed by then -- open the file again.
    //       False for a daemon that records the answer and waits for something else to happen: the
    //       wallpaper would start moving whenever the user next changed something, which on a
    //       desktop reads as never.
    // WHY-THIS-SHAPE: THE STATE THE RESUME IS DRIVEN FROM IS THE WHOLE CHECK, and each half of it
    //       was measured to be necessary:
    //         * the last picture must be UNCOLLECTED and stale, so the monitor is genuinely
    //           unwatched when the report arrives. Driven by uncovering, letting the video publish
    //           pictures that this phase does not fetch, and waiting for it to stop on its own.
    //           With the screen re-covered straight after the fetching window above -- where every
    //           picture HAD been collected, leaving nothing outstanding -- the monitor reads as
    //           watched already and deleting the covered-to-drawn credit leaves this green.
    //         * the session must already be RELEASED, so the worker has no deadline of its own
    //           left. While a session is open the worker wakes at the release whether or not
    //           anything woke it, and deleting the wake leaves this green too.
    //       Both were observed green before the drive was built this way.
    //       The wait is for a generation past the one it stopped on, which nothing already
    //       announced can satisfy.
    // CONTROL: in backend/daemon/dbus_service.cpp, the `m_worker.wake();` line deleted from
    //       setOutputVisible() -> "FAIL: a screen reporting that it is drawn again starts its video,
    //       with nothing else asked for", after the 30 s limit. In
    //       backend/daemon/wallpaper_manager.cpp, the `if (!visible || wasDrawn) return;` guard in
    //       setOutputVisible() changed to `return;` -- so the covered-to-drawn edge credits nothing
    //       -- reddens the same line. Both observed 2026-09-20. Reverted.
    // COVERAGE-ASSERT: both preconditions are asserted rather than assumed -- the monitor is
    //       required to read as unwatched with its screen drawn, and the session is required to
    //       have been closed. A run that reached neither could not fail this check.
    setOutputVisible(proxy, screen, true);
    const bool playedAgain = settlesWithin(std::chrono::seconds(30),
                                           [&] { return latestGeneration(pictures, kMonitorVisible) > stoppedAt; });
    check(playedAgain, "the screen coming back starts the video while its session is still open");
    if (!playedAgain) {
        notVerified(kGoals, "the video never played again after the screen came back, so the stale-and-released "
                            "state the resume check needs could not be built");
        return;
    }
    // Nothing is fetched from here on, so the pictures it published pile up uncollected and the
    // monitor goes stale -- which is what a locked screen leaves behind.
    const bool stoppedUncollected =
        quiescedByGeneration(pictures, kMonitorVisible, kQuietWindow, std::chrono::seconds(25));
    check(stoppedUncollected, "and stops again once nothing is collecting from it");
    const uint64_t staleAt = latestGeneration(pictures, kMonitorVisible);
    {
        const nlohmann::json entry = statusOf();
        check(entry.value("screen_visible", false) && !entry.value("watched", true),
              "leaving a drawn screen whose newest picture nobody has taken");
    }
    setOutputVisible(proxy, screen, false);
    g_files.resetCounts();
    const bool sessionReleased =
        settlesWithin(std::chrono::seconds(20), [&clip] { return g_files.closes(clip) >= 1; });
    check(sessionReleased, "covering it again releases the session, leaving the worker nothing to wake for");
    if (!stoppedUncollected || !sessionReleased) {
        std::fprintf(stderr, "  open files after the wait: %s\n", describeFiles(g_files.live()).c_str());
        notVerified(kGoals, "the stale-and-released state was not reached, so the resume below could not fail");
        return;
    }
    setOutputVisible(proxy, screen, true);
    const bool resumed = settlesWithin(std::chrono::seconds(30),
                                       [&] { return latestGeneration(pictures, kMonitorVisible) > staleAt; });
    check(resumed, "a screen reporting that it is drawn again starts its video, with nothing else asked for");
    if (!resumed) {
        std::fprintf(stderr, "  it stayed at generation %llu after the screen said it was being drawn again\n",
                     static_cast<unsigned long long>(latestGeneration(pictures, kMonitorVisible)));
    }
    check(statusOf().value("screen_visible", false), "and GetStatus reports the screen as drawn again");

    // GOAL: S13.173 -- a screen saying it is being drawn again is worth ONE PICTURE, not continuous
    //       playback. Nothing collects anything here: a client that only calls
    //       SetOutputVisible(name,false) then (name,true) must not be able to keep a decoder
    //       running at the file's full rate. False for the code before this fix, where every
    //       hidden->shown pair cleared the "nothing has collected this" stamp outright, so 0.7 bus
    //       calls a second kept a 60 fps video decoding for a consumer collecting nothing (measured
    //       by the QA pass: 1269 pictures in 21 s).
    // WHY-THIS-SHAPE: a count of pictures against a bound COMPUTED from the unwatchedAfter this
    //       phase's manager was built with, not a rate and not a hand-written number. The credit is
    //       worth one picture and is granted at most once per that interval, so the most this window
    //       can legitimately produce is one per interval plus one already in flight -- while the
    //       defect produces one per picture of the file, ten a second here. With the phase's 750 ms
    //       interval that is about 5 against 39, doubled into an allowance of 13, so machine load
    //       cannot move a correct daemon across it. Edges come every quarter interval, so each
    //       credit is reported again several times before it comes due -- which is the situation the
    //       re-dating half below is about, and at one edge per interval that half could not fail.
    //       Nothing is fetched anywhere in the window -- that is the whole point, since a fetch is
    //       the evidence being forged -- and the starting generation is read after the monitor has
    //       been quiesced, so pictures the resume above was still publishing are not charged here.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, setOutputVisible()'s credit replaced by the
    //       pre-fix line `it->second.uncollectedSince.reset();` -> "FAIL: saying a screen is hidden
    //       and then shown again does not keep its video playing for a consumer that collects
    //       nothing", printing "40 picture(s) over 20 edges, where 13 is the most the one-picture
    //       credit allows" against 5-6 with the fix. Reverted.
    // COVERAGE-ASSERT: the number of edges actually driven is asserted, so a loop that stopped
    //       early -- which would make "few pictures appeared" true for the wrong reason -- fails
    //       here instead of reading as a bound that held.
    {
        const bool settled = quiescedByGeneration(pictures, kMonitorVisible, kQuietWindow, std::chrono::seconds(25));
        check(settled, "the video stops again after the resume, giving the forged-edge window a still starting point");
        if (!settled) {
            notVerified("S13.173", "the video never settled, so a count of pictures across the forged edges would "
                                   "include pictures the earlier resume was still publishing");
            return;
        }
        const uint64_t forgeFrom = latestGeneration(pictures, kMonitorVisible);

        const auto edgeEvery = config.unwatchedAfter / 4;
        constexpr int kEdges = 20;
        int edgesDriven = 0;
        const Clock::time_point forgeStart = Clock::now();
        for (int i = 0; i < kEdges; ++i) {
            setOutputVisible(proxy, screen, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            setOutputVisible(proxy, screen, true);
            ++edgesDriven;
            std::this_thread::sleep_for(edgeEvery);
        }
        const auto forgeSpan = Clock::now() - forgeStart;
        const uint64_t forged = latestGeneration(pictures, kMonitorVisible) - forgeFrom;

        // One credit per unwatchedAfter, each worth one picture, plus one for a round already
        // under way when the window opened. Doubled, so a correct daemon has room and the defect
        // -- a picture per FILE interval -- still cannot fit.
        const uint64_t allowed = 2 * (static_cast<uint64_t>(forgeSpan / config.unwatchedAfter) + 1) + 1;
        std::printf("video_wallpaper_test: %d forged hidden->shown edges over %.1f s, nothing collected: "
                    "+%llu picture(s), against an allowance of %llu\n",
                    edgesDriven, std::chrono::duration<double>(forgeSpan).count(),
                    static_cast<unsigned long long>(forged), static_cast<unsigned long long>(allowed));
        std::fflush(stdout);
        check(edgesDriven == kEdges, "every forged edge this check is written for was driven");
        reportUnless(forged <= allowed,
                     "saying a screen is hidden and then shown again does not keep its video playing for a "
                     "consumer that collects nothing",
                     std::to_string(forged) + " picture(s) over " + std::to_string(edgesDriven) +
                         " edges, where " + std::to_string(allowed) + " is the most the one-picture credit allows");

        // GOAL: S13.178 -- a credit reported again before it is spent is DATED FORWARD, not pushed
        //       out by every report: a screen that keeps saying hidden-then-shown still gets its
        //       picture about once per unwatchedAfter, not one picture and then none. False for a
        //       rule that re-dates a pending credit on each report -- reports every quarter interval
        //       then push it a whole interval further away each time, and it never comes due.
        // WHY-THIS-SHAPE: the same window the upper bound above is read from, so the two bounds
        //       are about one behaviour and cannot both be met by different ones. The floor is HALF
        //       what the interval allows, so a slow machine cannot cross it; the defect gives one.
        // CONTROL: backend/daemon/wallpaper_manager.cpp, setOutputVisible()'s early return for a
        //       pending or unspent credit deleted, so every report re-dates it -> "FAIL: a screen that
        //       keeps reporting it is drawn again goes on getting a picture about once per interval",
        //       printing "+1 picture(s), where at least 2 were due over 3.96 s"; the genuine-resume
        //       line below went red with it, its credit pushed out of reach. Reverted.
        const uint64_t expectedAtLeast = static_cast<uint64_t>(forgeSpan / config.unwatchedAfter) / 2;
        reportUnless(expectedAtLeast >= 2 && forged >= expectedAtLeast,
                     "a screen that keeps reporting it is drawn again goes on getting a picture about once per "
                     "interval, rather than one picture and then none",
                     "+" + std::to_string(forged) + " picture(s), where at least " + std::to_string(expectedAtLeast) +
                         " were due over " + std::to_string(std::chrono::duration<double>(forgeSpan).count()) + " s");

        // GOAL: S13.173's other half -- and the genuine case still works. A screen that comes back
        //       AND whose consumer then collects the picture that credit bought keeps playing, so
        //       the bound above is a bound on forgery and not on the feature.
        // WHY-THIS-SHAPE: the same screen, the same edge, in the same phase, differing only in
        //       whether anything is collected -- which is the exact distinction the fix draws. A
        //       separate monitor would leave open that the one above had stopped for some other
        //       reason. The bound it must beat is the forged allowance recomputed over ITS OWN
        //       span, so the two halves cannot both be satisfied by the same behaviour.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, setOutputVisible()'s
        //       `if (!visible || wasDrawn) return;` changed to `if (true) return;`, so no edge ever
        //       grants a credit -> "FAIL: while a screen that comes back AND has its picture
        //       collected goes on playing", printing "0 picture(s), 0 collected", with S13.157's
        //       own resume line red above it. The forged half stays green at +0, which is why both
        //       halves are needed. Reverted.
        // A credit is granted at most once per unwatchedAfter, and the forged loop above left one
        // a fraction of an interval old. Waiting past the interval is what makes the edge below a
        // grantable one, so this half grades the feature rather than the rate limit above.
        std::this_thread::sleep_for(2 * config.unwatchedAfter);
        const uint64_t genuineFrom = latestGeneration(pictures, kMonitorVisible);
        setOutputVisible(proxy, screen, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        setOutputVisible(proxy, screen, true);
        const Clock::time_point genuineStart = Clock::now();
        // Three seconds of collecting: about 30 pictures from a playing video, against the 11 a
        // forged edge could be worth over the same span.
        const Watched genuine = collectUntilQuiet(kQuietWindow, std::chrono::seconds(3));
        const auto genuineSpan = Clock::now() - genuineStart;
        const uint64_t played = latestGeneration(pictures, kMonitorVisible) - genuineFrom;
        const uint64_t forgedAllowance = 2 * (static_cast<uint64_t>(genuineSpan / config.unwatchedAfter) + 1) + 1;
        std::printf("video_wallpaper_test: the same edge with a consumer collecting: +%llu picture(s) over %.1f s, "
                    "%d collected\n",
                    static_cast<unsigned long long>(played), std::chrono::duration<double>(genuineSpan).count(),
                    genuine.collected);
        std::fflush(stdout);
        reportUnless(played > forgedAllowance && genuine.collected > 0,
                     "while a screen that comes back AND has its picture collected goes on playing",
                     std::to_string(played) + " picture(s), " + std::to_string(genuine.collected) +
                         " collected, against the " + std::to_string(forgedAllowance) +
                         " a forged edge alone would be worth");
    }
}

constexpr int kMonitorEvidence = 0;
constexpr int kMonitorRebound = 1;

// The evidence rules, on a manager with NO render worker: every one of these is a question about
// what the daemon counts as "something is collecting", which is pure state, and without a worker
// this thread owns the GPU and every hand-over is made where it can be read at a known moment.
void runWatchEvidenceChecks(const WallpaperManager::Config& config, const std::string& clip,
                            const std::string& still) {
    const char* kGoals = "S13.149-S13.152 (what counts as a monitor being collected from, and what does not)";
    PhaseGrade grade("the watch-evidence phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    wallpaperx::testing::nameScreen(manager, kMonitorEvidence, screenOf(kMonitorEvidence));
    manager.requestWallpaper("video", clip, kMonitorEvidence);
    if (!manager.renderPending(kMonitorEvidence).rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the video render on this host produced no frame to collect");
        return;
    }
    const auto watched = [&manager](int monitorId) {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, monitorId);
        return status && status->watched;
    };

    // Publishes another picture, which is the only thing that starts the wait again: the daemon
    // stamps at a commit and clears on collection, so after a hand-over there is nothing
    // outstanding and the monitor counts as watched until something new is published. Asking for
    // the same wallpaper again is what re-commits WITHOUT rebuilding the images -- a size change
    // would rebuild them, and that clears every consumer's hold, which is the state half of the
    // checks below are about.
    const auto republish = [&manager, &clip]() {
        manager.requestWallpaper("video", clip, kMonitorEvidence);
        return manager.renderPending(kMonitorEvidence).rendered;
    };
    // Republishes and waits for the monitor to go stale again, so the rule under test is asked in
    // a known state. False is reported by the caller rather than fallen through: a rule read off a
    // monitor that was already watched would be unfalsifiable.
    const auto rearmStale = [&]() {
        if (!republish()) return false;
        return settlesWithin(std::chrono::seconds(20), [&] { return !watched(kMonitorEvidence); });
    };

    // Images held open for the whole phase: a consumer that gives nothing back is the state the
    // refusals below need, and it is also the state the evidence rules are about.
    std::vector<int> open;
    struct CloseAll {
        std::vector<int>& fds;
        ~CloseAll() {
            for (const int fd : fds) {
                if (fd >= 0) close(fd);
            }
        }
    } closeAll{open};
    const auto takeAndKeep = [&](const std::string& consumer) {
        wallpaperx::FrameExport e = manager.exportCurrentFrame(kMonitorEvidence, consumer);
        if (e.handle) {
            open.push_back(e.handle->frame.fd);
            open.push_back(e.handle->acquireFd);
        }
        return e;
    };

    // GOAL: S13.149 -- a monitor whose published picture nothing has collected for the daemon's own
    //       interval stops counting as watched, and a SERVED hand-over to a named consumer makes it
    //       watched again. This is the state every other rule below is a qualification of.
    // WHY-THIS-SHAPE: `watched` is read out of the manager's own snapshot rather than inferred from
    //       pictures appearing, because that is one fact and the picture rate is several -- a video
    //       that stopped for a want of a session, or because a render was owed, would look the same
    //       from outside. This phase has no render worker at all, so nothing is racing the reading.
    //       INHERENTLY TEMPORAL: "has not been collected for two seconds" cannot be observed
    //       without waiting, and the wait is bounded and one-directional -- the flag only ever goes
    //       the wrong way early, never late.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, watchedLocked() made to `return true;`
    //       before the uncollected test -> "FAIL: a monitor nothing has collected from for the
    //       daemon's own interval stops counting as watched". Observed 2026-09-20. Reverted.
    check(watched(kMonitorEvidence), "a monitor is watched at the moment its picture is published");
    const bool wentUnwatched =
        settlesWithin(std::chrono::seconds(20), [&] { return !watched(kMonitorEvidence); });
    check(wentUnwatched, "a monitor nothing has collected from for the daemon's own interval stops counting as "
                         "watched");
    if (!wentUnwatched) {
        notVerified(kGoals, "the monitor never stopped counting as watched, so none of the rules below has a "
                            "starting state");
        return;
    }

    // GOAL: S13.150 -- a hand-over with NO bus name behind it is not collection. A diagnostic poll
    //       -- the daemon's own tooling, a status reader -- takes an image nobody can be charged
    //       with holding, so counting it would let a `busctl` call keep a screen's video decoding
    //       for nobody.
    // WHY-THIS-SHAPE: the unnamed hand-over is asserted to have been SERVED first, so "it did not
    //       count" cannot be satisfied by an export that failed for some other reason. The named
    //       one that follows is what proves the same call in the same state DOES count, which is
    //       the only way to tell "unnamed is ignored" from "nothing at all clears this".
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `if (!client.empty())` guard on
    //       noteHandoverServed() in exportCurrentFrame() dropped -> "FAIL: a hand-over with no bus
    //       name behind it does not count as collection". Observed 2026-09-20. Reverted.
    {
        wallpaperx::FrameExport anonymous = manager.exportCurrentFrame(kMonitorEvidence);
        check(anonymous.handle.has_value(), "an unnamed poll is served the monitor's picture");
        if (anonymous.handle) {
            const uint64_t bufferId = anonymous.handle->frame.bufferId;
            if (anonymous.handle->frame.fd >= 0) close(anonymous.handle->frame.fd);
            if (anonymous.handle->acquireFd >= 0) close(anonymous.handle->acquireFd);
            check(!watched(kMonitorEvidence), "a hand-over with no bus name behind it does not count as collection");
            manager.releaseBuffer(std::string(), kMonitorEvidence, bufferId);
        }
    }
    {
        wallpaperx::FrameExport named = manager.exportCurrentFrame(kMonitorEvidence, ":1.900");
        check(named.handle.has_value(), "and a named consumer is served the same picture");
        if (named.handle) {
            const uint64_t bufferId = named.handle->frame.bufferId;
            if (named.handle->frame.fd >= 0) close(named.handle->frame.fd);
            if (named.handle->acquireFd >= 0) close(named.handle->acquireFd);
            check(watched(kMonitorEvidence), "which does count: a served hand-over to a named consumer is collection");
            manager.releaseBuffer(":1.900", kMonitorEvidence, bufferId);
        }
    }

    // GOAL: S13.151 -- a REFUSAL tells the two kinds of consumer apart. One refused while holding
    //       none of this monitor's images is asking for a picture the daemon had none to give; that
    //       is the daemon's own back-pressure, so it counts as collecting and the video it would
    //       otherwise stop keeps playing for it. One refused while holding images is the opposite:
    //       those are the images it has not given back, which is exactly the state the off switch
    //       exists to catch. False either way round -- counting every refusal keeps a frozen screen
    //       decoding at display rate, counting none stops a video for a consumer the daemon itself
    //       is starving.
    // WHY-THIS-SHAPE: consumers are added until the daemon refuses one rather than a fixed number
    //       being assumed: how many images a monitor's ring holds is the driver's business. The
    //       holder's refusal is driven with a consumer that has been SERVED and kept its images, so
    //       the two arms differ in exactly the fact under test and in nothing else.
    // COVERAGE-ASSERT: the loop's own success is asserted -- a run where no consumer could be
    //       refused reports that rather than grading two arms it never reached.
    // The holder arm's consumer, taken to its own cap ACROSS PUBLISHED PICTURES. Asking twice for
    // the same picture hands the same image back and the hold does not grow -- measured, and it is
    // why a plain loop of hand-overs leaves a consumer holding one image and being served for ever.
    // A consumer at its cap is refused whatever the ring has free, which is what keeps this arm
    // drivable without saturating the ring: the daemon has to be able to publish another picture
    // between the arms, and a ring with nothing free would leave it deferring instead.
    const std::string holder = ":1.910";
    bool holderAtItsCap = false;
    for (int i = 0; i < 8 && !holderAtItsCap; ++i) {
        const wallpaperx::FrameExport e = takeAndKeep(holder);
        if (!e.handle) {
            holderAtItsCap = e.reason == wallpaperx::NoFrameReason::NoFreeImage;
            break;
        }
        if (!republish()) break;
    }
    // Then further consumers, each asked once for the picture that is up, until one is refused
    // holding nothing. Bounded rather than counted out: how many consumers a monitor will record
    // and how deep its ring is are the daemon's and the driver's business, not this file's.
    std::string starvedEmptyHanded;
    for (int i = 1; i < 16 && starvedEmptyHanded.empty(); ++i) {
        const std::string consumer = ":1.91" + std::to_string(i);
        const wallpaperx::FrameExport e = takeAndKeep(consumer);
        if (e.handle) continue;
        if (e.reason != wallpaperx::NoFrameReason::NoFreeImage) break;
        starvedEmptyHanded = consumer;
    }
    check(holderAtItsCap, "a consumer can be taken to the point where the daemon refuses it while it holds images");
    if (starvedEmptyHanded.empty() || !holderAtItsCap) {
        notVerified("S13.151", "no consumer could be refused an image on this host while another held one, so the "
                               "two kinds of refusal could not be told apart");
    } else {
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `std::none_of(...)` guard in
        //       exportCurrentFrame()'s refusal block dropped, so every refusal resets
        //       uncollectedSince -> "FAIL: and a consumer refused while holding this monitor's
        //       images does not count as collecting". Replacing the whole block with nothing --
        //       no refusal ever counts -> "FAIL: a consumer refused while holding none of this
        //       monitor's images counts as collecting". Both observed 2026-09-20. Reverted. The first
        //       re-run 2026-09-23 on the NVIDIA entry: this arm is reached there, and went red.
        const bool unwatchedAgain = rearmStale();
        check(unwatchedAgain, "the monitor goes unwatched again with its images out and nothing collecting");
        if (unwatchedAgain) {
            const wallpaperx::FrameExport emptyHanded =
                manager.exportCurrentFrame(kMonitorEvidence, starvedEmptyHanded);
            check(emptyHanded.reason == wallpaperx::NoFrameReason::NoFreeImage && !emptyHanded.handle,
                  "a consumer holding nothing is refused, which is the state this rule is about");
            check(watched(kMonitorEvidence),
                  "a consumer refused while holding none of this monitor's images counts as collecting");

            const bool unwatchedOnceMore = rearmStale();
            check(unwatchedOnceMore, "and the monitor goes unwatched again after it");
            if (unwatchedOnceMore) {
                const wallpaperx::FrameExport byHolder = manager.exportCurrentFrame(kMonitorEvidence, holder);
                if (byHolder.handle) {
                    std::fprintf(stderr, "  %s was SERVED image %llu; holders now:", holder.c_str(),
                                 static_cast<unsigned long long>(byHolder.handle->frame.bufferId));
                    const std::optional<wallpaperx::MonitorStatus> s = monitorStatus(manager, kMonitorEvidence);
                    if (s) {
                        for (const wallpaperx::BufferHold& h : s->holds) {
                            std::fprintf(stderr, " %s(%zu)", h.client.c_str(), h.buffers.size());
                        }
                    }
                    std::fprintf(stderr, "\n");
                }
                check(byHolder.reason == wallpaperx::NoFrameReason::NoFreeImage && !byHolder.handle,
                      "the consumer holding its fill is refused too");
                check(!watched(kMonitorEvidence),
                      "and a consumer refused while holding this monitor's images does not count as collecting");
            }
        }
    }

    // GOAL: S13.152 -- a video's FIRST PICTURE after a wallpaper change is never declared unwatched
    //       by the previous wallpaper's staleness. The wait is anchored to the oldest uncollected
    //       picture, and a monitor whose frame was thrown away has had nothing to collect since --
    //       so the wait has to start again at the new wallpaper's first picture. False for a daemon
    //       that keeps the old stamp: a screen whose video had been ignored for a minute would get
    //       one picture of its new wallpaper and stop, and on the user's desktop that reads as a
    //       video that refuses to play.
    // WHY-THIS-SHAPE: driven from a monitor that is PROVABLY stale -- the check above has just
    //       established it is unwatched -- so the new wallpaper's first picture is committed into
    //       exactly the state the defect needs. Reading `watched` straight after the render, with
    //       no worker running, means nothing can have collected in between.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, commitFrameLocked()'s
    //       `if (!hadFrame || !state.uncollectedSince)` narrowed to `if (!state.uncollectedSince)`
    //       -> "FAIL: a video's first picture after a wallpaper change starts the wait again".
    //       Observed 2026-09-20. Reverted.
    {
        const bool stale = rearmStale();
        check(stale, "the monitor is stale before its wallpaper is changed");
        if (stale) {
            manager.requestWallpaper("video", clip, kMonitorEvidence);
            const bool drew = manager.renderPending(kMonitorEvidence).rendered;
            check(drew, "the new wallpaper's first picture commits");
            if (drew) {
                check(watched(kMonitorEvidence),
                      "a video's first picture after a wallpaper change starts the wait again");
            }
        }
    }

    // GOAL: S13.153 -- a screen name rebound to another monitor does not carry its old not-drawn
    //       verdict. A report is filed under the NAME, because a screen can be named before a
    //       wallpaper is chosen for it; but no consumer has said anything about the monitor taking
    //       the name over, and an unreported screen is taken to be drawn. False for a daemon that
    //       leaves the verdict behind: a monitor that inherited a dark screen's name would have its
    //       video stopped with nothing able to correct it, because the plugin only sends a report
    //       when its own answer changes.
    // WHY-THIS-SHAPE: read through screenVisible on the monitor that took the name over, which is
    //       the whole claim; reading it on the monitor that lost the name would be satisfied by a
    //       daemon that simply forgot both. The still is there so the new monitor has a status
    //       entry to read at all -- GetStatus lists wallpapers.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the
    //       `m_outputsNotDrawn.erase(existing->first);` line deleted from bindOutputLocked() ->
    //       "FAIL: a screen name rebound to another monitor does not carry its old not-drawn
    //       verdict", printing screen_visible false. Observed 2026-09-20. Reverted.
    {
        const std::string name = screenOf(kMonitorEvidence);
        manager.setOutputVisible(name, false);
        const std::optional<wallpaperx::MonitorStatus> covered = monitorStatus(manager, kMonitorEvidence);
        check(covered && !covered->screenVisible, "a screen that says it is not being drawn is recorded as such");
        manager.requestWallpaper("static", still, kMonitorRebound);
        (void)manager.renderPending(kMonitorRebound);
        wallpaperx::testing::nameScreen(manager, kMonitorRebound, name);
        const std::optional<wallpaperx::MonitorStatus> rebound = monitorStatus(manager, kMonitorRebound);
        reportUnless(rebound && rebound->screenVisible,
                     "a screen name rebound to another monitor does not carry its old not-drawn verdict",
                     rebound ? "screen_visible: false" : std::string("no status for the monitor that took the name"));
    }
}

constexpr int kMonitorSlow = 0;

// How many of the slow fixture's pictures this phase waits for. Three, so the two-second gap is
// crossed twice.
constexpr uint64_t kSlowPictures = 3;

void runSlowPictureChecks(const WallpaperManager::Config& config, const std::string& slow) {
    const char* kGoal = "S13.158 (a consumer that keeps collecting keeps a video playing however far apart the "
                        "file's pictures are)";
    PhaseGrade grade("the slow-picture phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    Collector collector(manager, kMonitorSlow, ":1.750");
    RenderWorker worker(manager, [&signals, &pictures, &collector](int monitorId, uint64_t generation) {
        signals.record(monitorId, generation);
        pictures.record(monitorId, generation);
        collector.announced(monitorId, generation);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};

    wallpaperx::testing::nameScreen(manager, kMonitorSlow, screenOf(kMonitorSlow));
    manager.requestWallpaper("video", slow, kMonitorSlow);
    worker.post(kMonitorSlow);
    if (!signals.wait(kMonitorSlow, 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the slow video produced no first picture on this host");
        return;
    }

    // GOAL: S13.158 -- a video whose pictures are FARTHER APART than the daemon's own unwatched
    //       interval keeps playing for a consumer that collects every one of them. This is the case
    //       that froze on the user's desktop: with pictures farther apart than the interval, a rule
    //       measured from the last hand-over declares its own consumer gone in the gap and the
    //       wallpaper shows one picture and stops. The rule that survives it measures from the
    //       OLDEST picture nobody has collected -- and there is none, because the consumer took the
    //       last one.
    // WHY-THIS-SHAPE: a fixture whose pictures are two seconds apart -- more than twice the
    //       phase's 750 ms unwatchedAfter -- is the whole check; every other video fixture here runs
    //       at ten pictures a second, where the right rule and the wrong one agree and no
    //       arrangement of consumers can tell them apart. The consumer fetches only on an
    //       announcement, which is what the plugin does -- a consumer polling faster than the
    //       interval would keep the wrong rule satisfied too.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, noteHandoverServed() made to set
    //       `it->second.uncollectedSince = std::chrono::steady_clock::now();` instead of resetting
    //       it -- which is the rule measured from the last hand-over -> "FAIL: a video whose pictures
    //       are farther apart than the unwatched interval keeps playing for a consumer that collects
    //       them", with the picture count stuck at one. Reverted.
    // COVERAGE-ASSERT: the span the graded pictures took is asserted to be at least twice the
    //       phase's own unwatchedAfter, so a fixture regenerated at a fast rate -- which would make
    //       this check agree with the wrong rule and stop being able to fail -- fails here instead.
    const bool kept = signals.wait(kMonitorSlow, kSlowPictures);
    check(kept, "a video whose pictures are farther apart than the unwatched interval keeps playing for a consumer "
                "that collects them");
    if (!kept) {
        std::fprintf(stderr, "  it reached generation %llu; the consumer: %s\n",
                     static_cast<unsigned long long>(latestGeneration(pictures, kMonitorSlow)),
                     collector.describe().c_str());
        notVerified(kGoal, "the slow video never reached its third picture, so no span could be measured");
        return;
    }
    reportUnless(collector.threw().empty(), "the consumer's hand-overs did not throw", collector.threw());
    reportUnless(collector.served() > 0, "and the consumer really was collecting them", collector.describe());
    {
        const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorSlow);
        std::optional<Clock::time_point> first;
        std::optional<Clock::time_point> third;
        for (const PictureLog::Event& e : shown) {
            if (e.generation == 1) first = e.at;
            if (e.generation == kSlowPictures) third = e.at;
        }
        if (!first || !third) {
            notVerified(kGoal, "the first and third pictures were not both announced, so no span can be measured");
        } else {
            const double seconds = std::chrono::duration<double>(*third - *first).count();
            std::printf("video_wallpaper_test: the slow fixture's first %llu pictures took %.3f s\n",
                        static_cast<unsigned long long>(kSlowPictures), seconds);
            std::fflush(stdout);
            reportUnless(seconds >= 2 * std::chrono::duration<double>(config.unwatchedAfter).count(),
                         "and the gaps it crossed really are longer than the daemon's unwatched interval",
                         std::to_string(seconds) + " s for " + std::to_string(kSlowPictures) +
                             " pictures, which is not long enough for this check to be able to fail");
        }
    }
}

constexpr int kMonitorInnocent = 0;         // a video whose frames must keep being served throughout
constexpr int kMonitorIrregularStill = 1;   // a still whose path stops naming a regular file
constexpr int kMonitorReopen = 2;           // a video left to stop, so its session is released and reopened

// EXACTLY TWO MONITORS ARE GIVEN A BAD PATH, and never one whose reopen is not driven by a check.
// Under the control that reintroduces the wait (O_NONBLOCK removed), the daemon's one drawing
// thread parks inside such an open and only a writer on the pipe can release it -- so an ungraded
// monitor whose path is a pipe parks the worker at a moment no check is watching, with nothing to
// rescue it, and the phase's own teardown then waits for that thread for ever. Measured: a third
// monitor set up that way turned the control from four red lines into a run that had to be killed.

// What one of the three reopen paths ended in, read on the worker's own thread.
struct Refusal {
    bool answered = false;  // the worker ran the read at all, rather than being parked in the open
    bool recorded = false;
    std::string lastError;
    bool daemonFault = true;
    wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
};

std::string describe(const Refusal& r) {
    if (!r.answered) return "the worker never answered -- it is still inside the open";
    if (!r.recorded) return "no status for the monitor";
    return "last_error: " + (r.lastError.empty() ? std::string("(none)") : r.lastError) +
           ", daemon's fault: " + (r.daemonFault ? "yes" : "no") + ", reason " +
           std::to_string(static_cast<int>(r.reason));
}

// S13.170-S13.172 -- a wallpaper whose path stops naming a regular file, on each of the occasions
// the daemon opens it again without anyone having asked it to.
//
// WHY A PHASE OF ITS OWN, WITH A WORKER. The defect this grades is that the open happens on the one
// thread that draws and exports for every monitor, so the check that matters is what a SECOND
// monitor's frame request does while the first monitor's file is being opened. Without a worker
// there is no such thread and the question cannot be asked at all.
//
// A SYMLINK, NOT THE PATH ITSELF. SetWallpaper refuses a path that is not a readable regular file
// when it is set, so a pipe handed to it directly is rejected on the reply and never reaches any of
// these reopens. Repointing a symlink after the wallpaper has been accepted is how a real path
// stops being a real file -- a package update, a network mount going away, or any unprivileged
// process in the session -- and it is the only way to reach the code under test.
void runIrregularWallpaperChecks(const WallpaperManager::Config& config, const std::string& clip,
                                 const std::string& still) {
    const char* kGoals = "S13.170-S13.172 (a wallpaper path that stops naming a regular file is refused on every "
                         "path that opens it, without delaying any other monitor)";
    PhaseGrade grade("the irregular-wallpaper phase");

    const std::string dir = testAssetDir() + "/irregular";
    const std::string pipe = dir + "/a-pipe";
    const std::string stillLink = dir + "/still_link.png";
    const std::string reopenLink = dir + "/reopen_link.mp4";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    if (ec || ::mkfifo(pipe.c_str(), 0600) != 0) {
        wallpaperx::testing::harnessFailure(
            "the irregular-wallpaper phase could not make its scratch directory and FIFO, so no wallpaper path was "
            "made to stop being a regular file");
        return;
    }
    // Removed whatever happens: a FIFO left in the shared asset directory is one the next run's
    // directory walk can block on.
    struct RemoveScratch {
        const std::string& dir;
        ~RemoveScratch() {
            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        }
    } removeScratch{dir};

    const auto repoint = [](const std::string& link, const std::string& at) {
        std::error_code linkEc;
        std::filesystem::remove(link, linkEc);
        std::filesystem::create_symlink(at, link, linkEc);
        return !linkEc;
    };
    if (!repoint(stillLink, still) || !repoint(reopenLink, clip)) {
        wallpaperx::testing::harnessFailure(
            "the irregular-wallpaper phase could not make its symlinks, so no path could be repointed");
        return;
    }
    // The ledger records only paths it was told about, and these three are built here rather than
    // in main(). Without this the release the reopen check waits for is invisible and reads as a
    // session that was never released.
    for (const std::string& path : {stillLink, reopenLink}) g_files.watch(path);
    // Releases a daemon thread parked in open(2) on the FIFO, so a run under a control that
    // reintroduces the wait ends with a joined worker and a printed reason instead of a hung suite.
    // O_NONBLOCK on this side: with no reader parked there is nothing to release, and this gives
    // ENXIO rather than parking the rescue as well.
    const auto rescueParkedOpen = [&pipe] {
        const int writer = ::open(pipe.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
        if (writer >= 0) ::close(writer);
        return writer >= 0;
    };

    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    // Declared AFTER the worker so it runs BEFORE the worker is stopped, which is the whole point:
    // stopping a worker parked inside an open waits for that thread for ever. Every wallpaper path
    // is put back to a real file and a writer is attached to the pipe, so a run under a control
    // that reintroduces the wait ends with a joined worker and its failures printed.
    struct LeaveNothingParked {
        const std::string& stillLink;
        const std::string& reopenLink;
        const std::string& still;
        const std::string& clip;
        const std::string& pipe;
        ~LeaveNothingParked() {
            std::error_code ignored;
            for (const auto& [link, target] :
                 {std::pair<const std::string&, const std::string&>{stillLink, still},
                  std::pair<const std::string&, const std::string&>{reopenLink, clip}}) {
                std::filesystem::remove(link, ignored);
                std::filesystem::create_symlink(target, link, ignored);
            }
            const int writer = ::open(pipe.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
            if (writer >= 0) ::close(writer);
        }
    } leaveNothingParked{stillLink, reopenLink, still, clip, pipe};

    // Reads the monitor's verdict on the worker's own thread, so it is taken after the redraw has
    // run rather than racing it -- and so a worker parked inside the open never reaches it, which
    // is exactly what `answered` reports.
    const auto verdictOf = [&manager, &worker](int monitorId, std::chrono::seconds limit) {
        auto done = std::make_shared<std::promise<Refusal>>();
        std::future<Refusal> answer = done->get_future();
        worker.postTask([&manager, monitorId, done](RenderWorker::TaskState state) {
            Refusal out;
            if (state != RenderWorker::TaskState::Run) {
                done->set_value(out);
                return;
            }
            out.answered = true;
            if (const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, monitorId)) {
                out.recorded = true;
                out.lastError = status->lastError;
                out.daemonFault = status->lastErrorIsDaemonFault;
            }
            out.reason = manager.exportCurrentFrame(monitorId).reason;
            done->set_value(out);
        });
        if (answer.wait_for(limit) != std::future_status::ready) return Refusal{};
        return answer.get();
    };

    for (const int id : {kMonitorInnocent, kMonitorIrregularStill, kMonitorReopen}) {
        wallpaperx::testing::nameScreen(manager, id, screenOf(id));
    }
    manager.requestWallpaper("video", clip, kMonitorInnocent);
    manager.requestWallpaper("static", stillLink, kMonitorIrregularStill);
    manager.requestWallpaper("video", reopenLink, kMonitorReopen);
    for (const int id : {kMonitorInnocent, kMonitorIrregularStill, kMonitorReopen}) worker.post(id);
    const bool allDrew = signals.wait(kMonitorInnocent, 1) && signals.wait(kMonitorIrregularStill, 1) &&
                         signals.wait(kMonitorReopen, 1);
    if (!allDrew) {
        grade.expectNoDevice();
        notVerified(kGoals, "the three wallpapers this phase needs did not all draw on this host, so there was no "
                            "working wallpaper for a path to stop being a regular file under");
        return;
    }

    check(repoint(stillLink, pipe), "the still's path could be repointed at something that is not a regular file");

    // GOAL: S13.170's first path -- THE REDRAW A RESIZE FORCES reopens a STILL image's file, with
    //       no request and no validation in between, and a path that has stopped naming a regular
    //       file by then is refused rather than opened. This is the easier of the two paths to
    //       reach and the reason the still half of the finding mattered more: repointing a symlink
    //       and reporting a new screen size forces the reopen at once, where a video's reopen has
    //       to wait for the video to stop first.
    //       A VIDEO IS NOT DRIVEN HERE, and that is a measurement rather than an omission: a resize
    //       redraws a video from the session it already has and opens nothing (which is S13.135's
    //       "its file is neither reopened nor closed"). Driven this way the video monitor redrew
    //       successfully with its path a pipe and recorded no error -- correctly. The video's own
    //       reopen is graded below.
    // WHY-THIS-SHAPE: the error's WORDING is asserted, not merely that a render failed. Without the
    //       fix the still renderer also fails on a pipe -- eventually, if a writer ever appears --
    //       so "the render failed" is green either way and grades nothing. What distinguishes the
    //       fix is that the file was refused for what its descriptor said it was, before any read.
    //       The refusal must name the WALLPAPER'S path, not the pipe it now resolves to: the path
    //       the operator set is the one they can act on.
    // CONTROL: in renderer/wallpaper_file.cpp, `if (!S_ISREG(info.st_mode))` -> `if (false)` ->
    //       "FAIL: a still image is refused for what the descriptor says its path is, on the redraw
    //       a resize forces", printing "StaticRenderer: not a decodable image (unknown image type)"
    //       -- the pre-fix wording, reached because a pipe with no writer reads as end-of-file.
    //       Reverted.
    // COVERAGE-ASSERT: the wallpaper is required to have DRAWN first (the wait above) and the
    //       resize is required to have left its monitor owing a render, so a phase that refused a
    //       file the daemon had never accepted cannot be reported as having graded a reopen.
    Refusal stillResize;
    {
        std::vector<int> owed;
        manager.setOutputSize(screenOf(kMonitorIrregularStill), 96, 48, owed);
        check(std::find(owed.begin(), owed.end(), kMonitorIrregularStill) != owed.end(),
              "a new size leaves the still monitor owing a redraw, which is what reopens its path");
        for (const int id : owed) worker.post(id);

        // GOAL: S13.171's second half -- NO OTHER MONITOR IS DELAYED by the bad path. This is the
        //       whole severity of the finding: the open runs on the single thread that also answers
        //       every monitor's frame request, so one screen's wallpaper file froze every screen's
        //       while GetStatus went on calling the daemon healthy.
        // WHY-THIS-SHAPE: asked HERE, between posting the redraw and reading its verdict, and not
        //       after -- which is where it was first written, and where no control could redden it:
        //       the verdict read above detects the park and rescues it, so by the time it returned
        //       the worker was free again and this passed under the very mutation it exists to
        //       catch. Measured that way before it was moved.
        //       The hand-over is posted as a TASK ON THE SAME WORKER THREAD the open parks, which
        //       is the only thing that can answer this: a check made on this test's own thread
        //       would be served out of state the worker is not holding and would pass with the
        //       worker wedged. `answered` false is what FAILS the check; it is never fallen through
        //       into "refused", which is what a default-constructed Handover would look like.
        // CONTROL: in renderer/wallpaper_file.cpp, `O_NONBLOCK` removed from the open flags ->
        //       "FAIL: a monitor whose own wallpaper is fine is still served while another
        //       monitor's path has become a pipe -- one screen's file does not freeze every
        //       screen's", after the 20 s deadline, followed by "rescuing the parked open by
        //       attaching a writer to the pipe: done" and the rest of the phase's verdicts. The
        //       process ended in 1, not a hang. Reverted.
        Handover innocent = handOver(manager, worker, kMonitorInnocent, ":1.820", false, std::chrono::seconds(20));
        if (!innocent.answered) {
            check(false, "a monitor whose own wallpaper is fine is still served while another monitor's path has "
                         "become a pipe -- one screen's file does not freeze every screen's");
            std::fprintf(stderr, "  rescuing the parked open by attaching a writer to the pipe: %s\n",
                         rescueParkedOpen() ? "done" : "no reader was parked there");
            innocent = handOver(manager, worker, kMonitorInnocent, ":1.820", false, std::chrono::seconds(30));
        } else {
            reportUnless(innocent.served,
                         "a monitor whose own wallpaper is fine is still served while another monitor's path has "
                         "become a pipe",
                         describe(innocent));
        }
        const std::optional<wallpaperx::MonitorStatus> innocentStatus = monitorStatus(manager, kMonitorInnocent);
        check(innocentStatus && innocentStatus->hasFrame && innocentStatus->lastError.empty(),
              "and it keeps its own frame with no error of its own");

        stillResize = verdictOf(kMonitorIrregularStill, std::chrono::seconds(20));
        if (!stillResize.answered) {
            check(false, "the redraw of a still whose path became a pipe returns instead of parking the daemon's "
                         "one drawing thread in the kernel");
            std::fprintf(stderr, "  rescuing the parked open by attaching a writer to the pipe: %s\n",
                         rescueParkedOpen() ? "done" : "no reader was parked there");
            stillResize = verdictOf(kMonitorIrregularStill, std::chrono::seconds(30));
        }
        std::printf("video_wallpaper_test: a still whose path became a pipe, redrawn after a resize: %s\n",
                    describe(stillResize).c_str());
        std::fflush(stdout);
        if (!stillResize.answered) {
            notVerified(kGoals, "the worker never came back from redrawing the still, so nothing about the refusal "
                                "could be read");
        } else {
            reportUnless(stillResize.recorded &&
                             stillResize.lastError.find("is not a regular file") != std::string::npos,
                         "a still image is refused for what the descriptor says its path is, on the redraw a "
                         "resize forces",
                         describe(stillResize));
            reportUnless(stillResize.lastError.find(stillLink) != std::string::npos,
                         "and the refusal names the wallpaper path the operator set, not what it now resolves to",
                         describe(stillResize));
        }
    }

    // GOAL: S13.171's first half -- that refusal is THE FILE'S FAULT and reaches a client as such.
    //       The monitor records it, GetFrameFd answers RenderFailed rather than "still rendering,
    //       try again", and the daemon does not mark itself broken over a file the user could
    //       replace. False for a daemon that blames itself: /health would report a broken daemon
    //       and a watchdog would restart a process that is working perfectly. False too for the
    //       daemon the QA pass measured, which reported the affected monitor as having a frame and
    //       no error at all while every screen was frozen.
    // CONTROL: in renderer/wallpaper_file.cpp, the WallpaperContentError raised for a non-regular
    //       file swapped for GpuFailure -> "FAIL: the refusal is the file's fault, not the
    //       daemon's" and "FAIL: and the daemon does not report itself broken over one wallpaper's
    //       file", printing "daemon's fault: yes". Reverted.
    if (stillResize.answered) {
        check(!stillResize.daemonFault, "the refusal is the file's fault, not the daemon's");
        check(stillResize.reason == wallpaperx::NoFrameReason::RenderFailed,
              "and a client asking for that monitor's frame is told the render failed, not to keep retrying");
        check(manager.snapshot().health.gpu.status != wallpaperx::HealthStatus::Fail,
              "and the daemon does not report itself broken over one wallpaper's file");
    }

    // GOAL: S13.170's second path -- the REOPEN AFTER AN IDLE RELEASE. A video nothing is collecting
    //       stops, its decoder and file are released a few seconds later, and the next time the
    //       monitor is watched the daemon opens the path again from scratch. Nobody asks for this
    //       and nothing validates the path at that moment, which is precisely the reopen the QA
    //       pass reached: a lock screen, then a file that has gone.
    // WHY-THIS-SHAPE: the release is waited for through the file ledger rather than slept for, so
    //       the reopen this drives is provably a reopen and not a redraw from a session that was
    //       still resident. The path is repointed only AFTER the release is observed, so the
    //       session that played cannot have been opened on the pipe.
    // CONTROL: in renderer/wallpaper_file.cpp, `if (!S_ISREG(info.st_mode))` -> `if (false)` ->
    //       "FAIL: a video whose session was released and whose path has become a pipe is refused
    //       when the daemon reopens it, rather than opened", printing "VideoDecoder: cannot open
    //       ...: Invalid data found when processing input" -- ffmpeg's answer to an empty pipe,
    //       which is the pre-fix behaviour. Reverted.
    // COVERAGE-ASSERT: the release is asserted before the path is changed, so a run in which the
    //       session was never released -- where the hand-over would redraw from memory and open
    //       nothing -- fails here instead of grading a reopen that did not happen.
    {
        g_files.resetCounts();
        const bool released =
            settlesWithin(std::chrono::seconds(20), [&reopenLink] { return g_files.closes(reopenLink) >= 1; });
        check(released, "the video nothing is collecting from releases its file, so the next look at it must reopen");
        if (!released) {
            std::fprintf(stderr, "  open files after the wait: %s\n", describeFiles(g_files.live()).c_str());
            notVerified(kGoals, "the session was never released, so no reopen could be driven");
            return;
        }
        check(repoint(reopenLink, pipe), "the released video's path could be repointed at the pipe");
        g_files.resetCounts();
        Handover resume = handOver(manager, worker, kMonitorReopen, ":1.821", false, std::chrono::seconds(20));
        if (!resume.answered) {
            check(false, "the reopen of a released video whose path became a pipe returns instead of parking the "
                         "daemon's one drawing thread");
            std::fprintf(stderr, "  rescuing the parked open by attaching a writer to the pipe: %s\n",
                         rescueParkedOpen() ? "done" : "no reader was parked there");
            resume = handOver(manager, worker, kMonitorReopen, ":1.821", false, std::chrono::seconds(30));
        }
        // The hand-over itself is served -- the monitor still holds the picture it stopped on --
        // and it is the REOPEN it triggers that must fail, which is read from the monitor after.
        const bool refused = settlesWithin(std::chrono::seconds(20), [&manager] {
            const std::optional<wallpaperx::MonitorStatus> s = monitorStatus(manager, kMonitorReopen);
            return s && s->lastError.find("is not a regular file") != std::string::npos;
        });
        const std::optional<wallpaperx::MonitorStatus> reopened = monitorStatus(manager, kMonitorReopen);
        std::printf("video_wallpaper_test: a released video whose path became a pipe, on reopen: %s\n",
                    reopened ? (reopened->lastError.empty() ? "no error recorded" : reopened->lastError.c_str())
                             : "no status");
        std::fflush(stdout);
        check(refused,
              "a video whose session was released and whose path has become a pipe is refused when the daemon "
              "reopens it, rather than opened");
        check(reopened && !reopened->lastErrorIsDaemonFault,
              "and that refusal too is the file's fault rather than the daemon's");
    }

    // GOAL: S13.177 at the daemon -- a wallpaper whose CONTENT names a pipe (an ffconcat script
    //       listing one) is refused as the file's fault, and no other monitor's frame request waits
    //       while it is opened. Unlike the cases above this needs no symlink swap: the script is an
    //       ordinary regular file, so SetWallpaper accepts it as it is, and it was the cheapest way
    //       left to wedge every screen once a pipe given directly was refused.
    // WHY-THIS-SHAPE: the same shape as the resize case above, and for the same reason -- the
    //       innocent monitor's hand-over is asked between posting the render and reading its
    //       verdict, on the worker's own thread, so a park in the open is what makes it go unanswered.
    //       The render is posted, not run on this thread, because only a worker can be kept from
    //       hanging the test by a deadline and a writer on the pipe.
    // CONTROL: renderer/video/video_decoder.cpp, `m_format->protocol_whitelist = ...` deleted ->
    //       "FAIL: a monitor whose own wallpaper is fine is still served while another monitor opens
    //       a playlist that lists a pipe", then "rescuing the parked open ...: done" and the rest of
    //       the phase. The refusal line stayed green, correctly: once rescued the pipe reads as
    //       empty and the script still fails as the file's fault -- which is why the innocent
    //       monitor, and not the refusal, is what grades the wedge. Reverted.
    {
        constexpr int kMonitorPlaylist = 3;
        const std::string playlist = dir + "/playlist.mp4";
        std::ofstream(playlist) << "ffconcat version 1.0\nfile a-pipe\n";
        wallpaperx::testing::nameScreen(manager, kMonitorPlaylist, screenOf(kMonitorPlaylist));
        manager.requestWallpaper("video", playlist, kMonitorPlaylist);
        worker.post(kMonitorPlaylist);
        Handover innocent = handOver(manager, worker, kMonitorInnocent, ":1.822", false, std::chrono::seconds(20));
        if (!innocent.answered) {
            check(false, "a monitor whose own wallpaper is fine is still served while another monitor opens a playlist "
                         "that lists a pipe");
            std::fprintf(stderr, "  rescuing the parked open by attaching a writer to the pipe: %s\n",
                         rescueParkedOpen() ? "done" : "no reader was parked there");
        } else {
            reportUnless(innocent.served,
                         "a monitor whose own wallpaper is fine is still served while another monitor opens a "
                         "playlist that lists a pipe",
                         describe(innocent));
        }
        Refusal r = verdictOf(kMonitorPlaylist, std::chrono::seconds(20));
        if (!r.answered) {
            rescueParkedOpen();
            r = verdictOf(kMonitorPlaylist, std::chrono::seconds(30));
        }
        std::printf("video_wallpaper_test: a playlist listing a pipe: %s\n", describe(r).c_str());
        std::fflush(stdout);
        reportUnless(r.answered && r.recorded && !r.lastError.empty() && !r.daemonFault &&
                         r.lastError.rfind("VideoDecoder: ", 0) == 0 && r.lastError.find(playlist) != std::string::npos,
                     "a wallpaper whose content lists a pipe is refused as the file's fault, naming the wallpaper",
                     describe(r));
    }
}

constexpr int kMonitorDeparture = 0;

// S13.180's other half -- a consumer leaving the bus is dealt with on the thread that hears of it,
// without waiting for the render worker. That thread is the bus's own, which also has to answer
// every monitor's frame request, so a departure that waited behind a slow video open would stall
// the whole bus for as long as the open took.
void runDepartureChecks(const WallpaperManager::Config& config, const std::string& onePicture) {
    const char* kGoal = "S13.180 (a departure never waits on the render worker's GPU lock)";
    PhaseGrade grade("the departure phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    // GOAL: forgetHeldBuffers() returns while the render worker is inside a video open holding the
    //       GPU lock. False for a departure that prunes a video session's served list -- which is
    //       worker state -- and so has to take that lock, or post a task and wait: measured by the
    //       QA pass as 39 of a consumer's frame requests refused as Busy across 900 departures
    //       during video opens, where the fix gives none.
    // WHY-THIS-SHAPE: the worker is HELD inside the decode of a video it is opening -- a render
    //       pass, which takes the GPU lock before it opens anything -- so the lock is provably held
    //       for the whole time the departure is asked to complete. A departure timed against a
    //       worker that merely happens to be busy could land between two passes and pass by luck.
    //       The departure runs on a thread of its own with a deadline, so a regression reds instead
    //       of hanging, and the gate is released afterwards either way so the thread always joins.
    // CONTROL: backend/daemon/wallpaper_manager.cpp, `std::lock_guard<std::mutex>
    //       gpuLock(m_gpuMutex);` added at the top of forgetHeldBuffers() -> "FAIL: a consumer
    //       leaving the bus is dealt with at once, without waiting for the render worker to finish a
    //       video open", after the five-second deadline; the gate was then released and the phase
    //       finished normally. Reverted.
    wallpaperx::testing::nameScreen(manager, kMonitorDeparture, screenOf(kMonitorDeparture));
    manager.requestWallpaper("video", onePicture, kMonitorDeparture);
    g_decodeGate.arm(CallGate::Mode::Hold);
    worker.post(kMonitorDeparture);
    struct ReleaseGate {
        ~ReleaseGate() { g_decodeGate.release(); }
    } releaseGate;
    if (!g_decodeGate.waitFired(kGateLimit)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the video open never reached its first decode on this host, so the GPU lock could not "
                           "be held at a known moment");
        return;
    }
    auto departure = std::async(std::launch::async,
                                [&manager] { return manager.forgetHeldBuffers(":1.990", nullptr); });
    const bool returned = departure.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    g_decodeGate.release();
    const std::vector<int> owed = departure.get();
    check(returned, "a consumer leaving the bus is dealt with at once, without waiting for the render worker to "
                    "finish a video open");
    reportUnless(owed.empty(), "and a consumer that held nothing leaves the worker nothing to do",
                 std::to_string(owed.size()) + " monitor(s) posted");
}

constexpr int kMonitorCreditStill = 0;
constexpr int kMonitorCreditOnePicture = 1;
constexpr int kMonitorCreditDelayed = 2;

// S13.181 -- what the resume credit may stand in for, and for how long.
void runResumeCreditChecks(const WallpaperManager::Config& config, const std::string& clip,
                           const std::string& still, const std::string& onePicture) {
    const char* kGoals = "S13.181 (a resume credit counts only while a picture can spend it, and until one does)";
    PhaseGrade grade("the resume-credit phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    const auto edge = [&manager, &worker](int monitorId) {
        manager.setOutputVisible(screenOf(monitorId), false);
        manager.setOutputVisible(screenOf(monitorId), true);
        // What the bus method does after the manager call, and the only thing that runs a round.
        worker.wake();
    };
    const auto watched = [&manager](int monitorId) {
        const std::optional<wallpaperx::MonitorStatus> s = monitorStatus(manager, monitorId);
        return s && s->watched;
    };

    for (const int id : {kMonitorCreditStill, kMonitorCreditOnePicture, kMonitorCreditDelayed}) {
        wallpaperx::testing::nameScreen(manager, id, screenOf(id));
    }
    manager.requestWallpaper("static", still, kMonitorCreditStill);
    manager.requestWallpaper("video", onePicture, kMonitorCreditOnePicture);
    manager.requestWallpaper("video", clip, kMonitorCreditDelayed);
    for (const int id : {kMonitorCreditStill, kMonitorCreditOnePicture, kMonitorCreditDelayed}) worker.post(id);
    if (!signals.wait(kMonitorCreditStill, 1) || !signals.wait(kMonitorCreditOnePicture, 1) ||
        !signals.wait(kMonitorCreditDelayed, 1)) {
        grade.expectNoDevice();
        notVerified(kGoals, "the three wallpapers this phase needs did not all draw on this host");
        return;
    }

    // GOAL: S13.181's first half -- a credit COUNTS ONLY WHILE THE MONITOR CAN PUBLISH THE PICTURE
    //       THAT SPENDS IT. A still, and a video whose file holds one picture, never publish another,
    //       so a screen coming back must not make them read as watched -- and must not for ever,
    //       since only a picture would ever take the credit away. False for a credit that counts
    //       whatever the monitor holds: GetStatus then says `watched: true` for a still nothing has
    //       collected in hours.
    // WHY-THIS-SHAPE: each monitor is first required to read as UNWATCHED, having published a
    //       picture nothing collected, so "not watched after the edge" cannot be satisfied by a
    //       monitor that was never in the state the credit could change. And it is read twice, the
    //       second time past the credit's interval, because the defect is "for ever".
    // CONTROL: backend/daemon/wallpaper_manager.cpp, resumeCreditStands(), in separate runs, each
    //       reverted. Its `drawableType(state.type) == Drawable::Clip` term deleted -> red on the
    //       still only, "watched true at once, true an interval later". Its `parkedWallpaperSeq !=
    //       wallpaperSeq` term deleted -> red on the one-picture video only, "watched true at once".
    // Half an interval past the phase's unwatchedAfter, so the uncollected picture is provably stale.
    const auto pastInterval = config.unwatchedAfter + config.unwatchedAfter / 2;
    std::this_thread::sleep_for(pastInterval);
    for (const auto& [id, what] : {std::pair<int, const char*>{kMonitorCreditStill, "a still wallpaper"},
                                   std::pair<int, const char*>{kMonitorCreditOnePicture, "a one-picture video"}}) {
        const bool before = watched(id);
        check(!before, (std::string(what) + " nothing collected from reads as unwatched before its screen comes "
                                            "back, so the credit has something to change")
                           .c_str());
        if (before) continue;
        edge(id);
        const bool atOnce = watched(id);
        std::this_thread::sleep_for(pastInterval);
        const bool later = watched(id);
        std::printf("video_wallpaper_test: %s after its screen came back: watched %s at once, %s an interval "
                    "later\n",
                    what, atOnce ? "true" : "false", later ? "true" : "false");
        std::fflush(stdout);
        reportUnless(!atOnce && !later,
                     "a screen coming back does not make a monitor that can never publish another picture read as "
                     "watched",
                     std::string(what) + ": watched " + (atOnce ? "true" : "false") + " at once, " +
                         (later ? "true" : "false") + " an interval later");
    }

    // GOAL: S13.181's second half -- a credit that could not be spent at once STILL STANDS when it
    //       can be: it has no deadline of its own. A screen comes back while nothing can open its
    //       video -- every decode slot taken, the GPU just failed, or the worker simply busy -- and
    //       when that clears, the video starts. False for a credit that lapses after an interval:
    //       nothing else would ever start that video, because the screen, already drawn, sends no
    //       further report.
    // WHY-THIS-SHAPE: the delay is made by HOLDING THE WORKER for twice the phase's unwatchedAfter
    //       -- a whole interval past any deadline a credit could be given -- which stands in for the
    //       full slot table and the failed GPU the QA drivers used: all
    //       three are "nothing may open the file yet", and a held worker is the one of them this
    //       file can hold for a known time. The session is RELEASED first, so the picture that
    //       follows needs a reopen and can only come from the credit -- a resident session would
    //       leave the clock able to draw on its own. The wait for that picture is for a generation
    //       past the one it stopped on, which nothing already announced can satisfy.
    // CONTROL: backend/daemon/wallpaper_manager.cpp, watchedLocked()'s credit test given a
    //       deadline -- `&& now - *state.resumeCredit.at < m_config.unwatchedAfter` -> "FAIL: a
    //       screen that came back while nothing could open its video still gets its picture once
    //       something can", after the signal limit. Reverted.
    // COVERAGE-ASSERT: the release is asserted before the edge, and so is the worker being held.
    {
        // "No longer open" rather than "a close since now": the release is due unwatchedAfter plus
        // idleSessionRelease after the first picture, which the still and one-picture checks above
        // can already have outlived -- measured, a wait for a fresh close then timed out.
        const bool released = settlesWithin(std::chrono::seconds(20), [&clip] { return g_files.live().count(clip) == 0; });
        check(released, "the uncollected video's session is released, so resuming it needs a reopen");
        if (!released) {
            notVerified("S13.181's second half", "the session was never released, so no reopen could be owed");
            return;
        }
        const uint64_t stoppedAt = static_cast<uint64_t>(signals.countFor(kMonitorCreditDelayed));
        {
            const HeldWorker hold(worker);
            check(hold.held(), "the render worker can be held across the credit's interval");
            if (!hold.held()) return;
            edge(kMonitorCreditDelayed);
            std::this_thread::sleep_for(2 * config.unwatchedAfter);
        }
        const bool resumed = signals.waitAtLeast(kMonitorCreditDelayed, stoppedAt + 1);
        check(resumed, "a screen that came back while nothing could open its video still gets its picture once "
                       "something can -- the credit waits for it");
    }
}

constexpr int kMonitorClamp = 0;

// S13.175 and S13.176 -- the two bounds the daemon puts on the gap between one picture and the
// next, each driven by a file whose own timing sits outside it. Its own phase, with one monitor and
// one collecting consumer, because both halves are measurements of how often that monitor commits
// and anything else drawing on the same GPU thread is noise in them.
void runIntervalClampChecks(const WallpaperManager::Config& config, const std::string& fast,
                            const std::string& gap) {
    const char* kGoals = "S13.175-S13.176 (a file asking for more pictures a second than a screen can show is "
                         "played at the daemon's own ceiling, and a gap longer than five seconds is not waited out)";
    PhaseGrade grade("the interval-clamp phase");

    // GOAL: S13.175 -- a file declaring a thousand pictures a second is committed AT MOST 120 times
    //       a second, and STILL PLAYS EVERY PICTURE: it is slowed down, not thinned out. False for
    //       the code before this fix, whose floor was four milliseconds -- 250 a second -- which
    //       the QA pass measured as 688 renders a second across four screens, the daemon at 43% of
    //       a core and every other client's frame fetch going from 0.8 ms to 12 ms. Also false for
    //       a fix that capped the rate by dropping pictures, which would play a two-second clip in
    //       a quarter of a second and show a quarter of its content.
    // WHY-THIS-SHAPE: two oracles, because either alone is satisfied by a wrong daemon. The rate
    //       alone is satisfied by one that renders 120 times a second and skips four pictures in
    //       five. "Every picture" alone is satisfied by the unfixed daemon, which plays all of them
    //       at a thousand a second. The second oracle is the number of PACKETS the decoder was
    //       given against the number of pictures committed: a daemon thinning the file decodes far
    //       more than it shows, and no status field can say that.
    //       The rate is measured across a fixed number of pictures rather than counting pictures in
    //       a fixed time, and the bound is twice the daemon's own floor, so a loaded machine cannot
    //       cross it -- the unfixed daemon is eight times over.
    // CONTROL: two, both in backend/daemon/wallpaper_manager.cpp, both reverted. (1)
    //       kMinPictureInterval put back to `std::chrono::milliseconds(4)` -> "FAIL: a file
    //       declaring a thousand pictures a second is committed no faster than the daemon's own
    //       ceiling ...", printing 248.3 a second against 120.0 with the fix in (observed
    //       2026-09-21). (2) a second
    //       `(void)session.pipeline->decodeNext();` after the first in advanceSessionLocked(), so
    //       every other picture is thrown away -- which is what a rate cap implemented by thinning
    //       would do -> "FAIL: and it plays every picture rather than thinning the file out to keep
    //       up", 249 packets for 125 pictures against 124 with the fix in, while the RATE check
    //       stayed green. That is why both oracles are here.
    // COVERAGE-ASSERT: the packet count is asserted to be non-zero, so a run in which the decode
    //       hook saw nothing -- which would make "it decoded no more than it showed" trivially true
    //       -- fails instead of reading as the rate having been established.
    {
        FrameReadyLog signals;
        PictureLog pictures;
        WallpaperManager manager(config);
        CaptureGradeOnExit captureGrade{grade, manager};
        Collector collector(manager, kMonitorClamp, ":1.770");
        RenderWorker worker(manager, [&signals, &pictures, &collector](int monitorId, uint64_t generation) {
            signals.record(monitorId, generation);
            pictures.record(monitorId, generation);
            collector.announced(monitorId, generation);
        });
        collector.attach(worker);
        WorkerUnblock unblock{worker};

        wallpaperx::testing::nameScreen(manager, kMonitorClamp, screenOf(kMonitorClamp));
        manager.requestWallpaper("video", fast, kMonitorClamp);
        worker.post(kMonitorClamp);
        if (!signals.wait(kMonitorClamp, 1)) {
            grade.expectNoDevice();
            notVerified(kGoals, "the fast video produced no first picture on this host");
            return;
        }
        // From here on, everything sent to the decoder is this file's: one monitor, one session.
        g_decodeCalls.store(0);

        constexpr uint64_t kFrom = 5;
        constexpr uint64_t kTo = 125;
        const bool reached = signals.wait(kMonitorClamp, kTo);
        check(reached, "a video declaring a thousand pictures a second keeps committing them");
        if (!reached) {
            std::fprintf(stderr, "  it reached generation %llu; the consumer: %s\n",
                         static_cast<unsigned long long>(latestGeneration(pictures, kMonitorClamp)),
                         collector.describe().c_str());
            notVerified(kGoals, "the fast video did not reach the graded generation, so no rate can be measured -- "
                                "a stalled clock must not be graded as a slow one");
            return;
        }
        const int packets = g_decodeCalls.load();
        const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorClamp);
        std::optional<Clock::time_point> from;
        std::optional<Clock::time_point> to;
        for (const PictureLog::Event& e : shown) {
            if (e.generation == kFrom) from = e.at;
            if (e.generation == kTo) to = e.at;
        }
        if (!from || !to) {
            notVerified("S13.175", "the graded generations were not both announced, so no span can be measured");
        } else {
            const double seconds = std::chrono::duration<double>(*to - *from).count();
            const double perSecond = static_cast<double>(kTo - kFrom) / seconds;
            const double floorRate = 1.0 / kMinPictureIntervalSeconds;
            std::printf("video_wallpaper_test: the %g fps fixture committed %llu pictures in %.3f s (%.1f a second; "
                        "the daemon's floor allows %.1f), sending %d packet(s) to the decoder\n",
                        kFastClipDeclaredFps, static_cast<unsigned long long>(kTo - kFrom), seconds, perSecond,
                        floorRate, packets);
            std::fflush(stdout);
            reportUnless(perSecond <= 2 * floorRate,
                         "a file declaring a thousand pictures a second is committed no faster than the daemon's "
                         "own ceiling on how often a wallpaper may be drawn",
                         std::to_string(perSecond) + " a second, where the floor on the gap between pictures "
                                                     "allows " + std::to_string(floorRate));
            reportUnless(packets > 0, "the decoder was actually asked for the pictures that were shown",
                         std::to_string(packets) + " packet(s)");
            reportUnless(packets > 0 && static_cast<double>(packets) <= 1.5 * static_cast<double>(kTo),
                         "and it plays every picture rather than thinning the file out to keep up",
                         std::to_string(packets) + " packet(s) decoded for " + std::to_string(kTo) +
                             " picture(s) committed");
        }
        reportUnless(collector.threw().empty(), "the consumer's hand-overs did not throw", collector.threw());
        reportUnless(static_cast<uint64_t>(collector.served()) * 2 >= kTo,
                     "the consumer collected most of the pictures, so the rate is measured on a watched monitor",
                     collector.describe());
    }

    // GOAL: S13.176 -- the FIVE-SECOND CEILING on one picture's interval is unchanged by the change
    //       to the floor. A file whose next picture is eight seconds away is advanced after five,
    //       because a gap that long is a discontinuity in the timestamps rather than a pause the
    //       author asked for -- which is what keeps a timestamp jump from freezing a wallpaper for
    //       as long as the jump says. False for a daemon with no ceiling, which waits the full
    //       eight, and false for one whose ceiling moved with the floor.
    // WHY-THIS-SHAPE: INHERENTLY TEMPORAL -- a bound on a wait cannot be read from anything but
    //       time. Bounded on BOTH sides: above by six seconds, which the unclamped eight cannot
    //       meet, and below by two, which a daemon that ignored the file's timing altogether and
    //       free-ran would fail. One-sided, the check would pass for a daemon that drew this file
    //       as fast as the GPU allows, which is the opposite defect.
    //       A consumer collects every picture, because the gap is longer than the daemon's own
    //       unwatched interval and an uncollected video would stop inside the first gap and be
    //       graded as a very long one.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, kMaxPictureInterval raised to
    //       `std::chrono::seconds(30)` -> "FAIL: a gap longer than five seconds is read as a
    //       discontinuity ...", printing "advanced after 8.000 s" against 5.000 s with the ceiling
    //       in place, and nothing else red. Reverted.
    {
        FrameReadyLog signals;
        PictureLog pictures;
        WallpaperManager manager(config);
        CaptureGradeOnExit captureGrade{grade, manager};
        Collector collector(manager, kMonitorClamp, ":1.771");
        RenderWorker worker(manager, [&signals, &pictures, &collector](int monitorId, uint64_t generation) {
            signals.record(monitorId, generation);
            pictures.record(monitorId, generation);
            collector.announced(monitorId, generation);
        });
        collector.attach(worker);
        WorkerUnblock unblock{worker};

        wallpaperx::testing::nameScreen(manager, kMonitorClamp, screenOf(kMonitorClamp));
        manager.requestWallpaper("video", gap, kMonitorClamp);
        worker.post(kMonitorClamp);
        if (!signals.wait(kMonitorClamp, 1)) {
            grade.expectNoDevice();
            notVerified("S13.176", "the eight-second-gap video produced no first picture on this host");
            return;
        }
        const bool advanced = signals.wait(kMonitorClamp, 2);
        check(advanced, "a video whose next picture is eight seconds away advances at all");
        if (!advanced) {
            std::fprintf(stderr, "  the consumer: %s\n", collector.describe().c_str());
            notVerified("S13.176", "the second picture never arrived, so the wait it took cannot be measured");
            return;
        }
        const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorClamp);
        std::optional<Clock::time_point> first;
        std::optional<Clock::time_point> second;
        for (const PictureLog::Event& e : shown) {
            if (e.generation == 1) first = e.at;
            if (e.generation == 2) second = e.at;
        }
        if (!first || !second) {
            notVerified("S13.176", "the first two pictures were not both announced, so no wait can be measured");
            return;
        }
        const double seconds = std::chrono::duration<double>(*second - *first).count();
        std::printf("video_wallpaper_test: the fixture declaring a %g s gap advanced after %.3f s (the daemon's "
                    "ceiling is %g s)\n",
                    kGapClipDeclaredGapSeconds, seconds, kMaxPictureIntervalSeconds);
        std::fflush(stdout);
        reportUnless(seconds < kMaxPictureIntervalSeconds + 1.0,
                     "a gap longer than five seconds is read as a discontinuity, so the wallpaper is not frozen for "
                     "as long as the file's timestamps ask",
                     std::to_string(seconds) + " s, where the file declares " +
                         std::to_string(kGapClipDeclaredGapSeconds) + " s and the ceiling is " +
                         std::to_string(kMaxPictureIntervalSeconds) + " s");
        reportUnless(seconds > 2.0,
                     "and the wait is still the daemon's ceiling rather than no wait at all",
                     std::to_string(seconds) + " s between the first two pictures");
    }
}

// S13.183 -- the 120 a second cap is held between the times successive pictures are DUE, not on
// each gap in the file on its own.
void runCapBudgetChecks(const WallpaperManager::Config& config, const std::string& rate120) {
    const char* kGoal = "S13.183 (a 120 a second file with millisecond timestamps plays at 120, not slower)";
    PhaseGrade grade("the cap-budget phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    Collector collector(manager, kMonitorClamp, ":1.772");
    RenderWorker worker(manager, [&signals, &pictures, &collector](int monitorId, uint64_t generation) {
        signals.record(monitorId, generation);
        pictures.record(monitorId, generation);
        collector.announced(monitorId, generation);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};

    wallpaperx::testing::nameScreen(manager, kMonitorClamp, screenOf(kMonitorClamp));
    manager.requestWallpaper("video", rate120, kMonitorClamp);
    worker.post(kMonitorClamp);
    if (!signals.wait(kMonitorClamp, 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the 120 a second video produced no first picture on this host");
        return;
    }

    // GOAL: S13.183 -- a file authored at exactly 120 pictures a second, whose container rounds its
    //       timestamps to whole milliseconds (gaps of 8, 9, 8 ms), plays at 120 a second. False for
    //       a floor applied to each gap alone: every 8 is raised to 8.33 and every 9 is left, and
    //       the file plays at 116 a second -- a video at the cap slowed by a rule meant only for
    //       files above it.
    // WHY-THIS-SHAPE: the MEDIAN one-second window, not the average and not the best. The two
    //       rates differ by 3%, which a loaded machine can easily cost an average: a round waking
    //       more than one interval late rebases the schedule, so a stall is lost time for good. The
    //       best window fails the other way -- one window whose first picture was announced late
    //       reads fast, enough to lift the defect's 116 over any bound. The defect moves EVERY
    //       window, while a stall or a late announcement moves only the windows that contain it,
    //       so the median separates them. Counted across a fixed number of pictures, never pictures
    //       in a fixed time.
    //       Deliberately NOT asserted: an upper bound on the fastest window. Right after a late
    //       picture a correct daemon may draw two pictures closer than 1/120 s (coder's stated
    //       risk), so such a bound would be asserting on the scheduler, not on the budget.
    // CONTROL: backend/daemon/wallpaper_manager.cpp, gapBetween() returning
    //       `std::max(gap, kMinPictureInterval)` -- the floor held per gap, as before -> "FAIL: a 120
    //       a second file with millisecond timestamps plays at 120 a second, not slower", median
    //       116.88 (fastest window 116.91) against 119.99 with the fix. Reverted.
    // COVERAGE-ASSERT: the number of windows compared is asserted.
    constexpr uint64_t kWindow = 120;
    // Three seconds of pictures: 240 one-second windows, which is plenty for a median.
    constexpr uint64_t kPictures = 360;
    const bool reached = signals.wait(kMonitorClamp, kPictures + 5);
    if (!reached) {
        notVerified(kGoal, "the 120 a second video did not reach its graded picture on this host, so no window can be "
                           "measured -- a stalled clock must not be graded as a slow one");
        return;
    }
    const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorClamp);
    std::vector<double> rates;
    for (size_t i = 5; i + kWindow < shown.size() && i + kWindow <= kPictures + 5; ++i) {
        if (shown[i + kWindow].generation != shown[i].generation + kWindow) continue;
        const double seconds = std::chrono::duration<double>(shown[i + kWindow].at - shown[i].at).count();
        if (seconds > 0) rates.push_back(static_cast<double>(kWindow) / seconds);
    }
    std::sort(rates.begin(), rates.end());
    const double median = rates.empty() ? 0.0 : rates[rates.size() / 2];
    std::printf("video_wallpaper_test: the 120 a second file with millisecond timestamps: median one-second window "
                "%.2f a second over %zu windows (slowest %.2f, fastest %.2f)\n",
                median, rates.size(), rates.empty() ? 0.0 : rates.front(), rates.empty() ? 0.0 : rates.back());
    std::fflush(stdout);
    reportUnless(rates.size() >= 100, "enough one-second windows were compared for a median to mean something",
                 std::to_string(rates.size()) + " window(s)");
    reportUnless(median >= 119.0, "a 120 a second file with millisecond timestamps plays at 120 a second, not slower",
                 std::to_string(median) + " a second in the median one-second window");
}

constexpr int kEvictionWatchMonitors[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
static_assert(std::size(kEvictionWatchMonitors) == WallpaperManager::kMaxVideoSessions + 1,
              "this phase needs exactly one monitor past the session ceiling");

// S13.159, on a manager with NO render worker: the clock never runs here, so every session stays
// exactly as the pass that opened it left it and which one eviction picks depends on nothing but
// this function.
void runEvictionWatchChecks(const WallpaperManager::Config& config, const std::vector<std::string>& clips) {
    const char* kGoal = "S13.159 (eviction asks current-and-drawn, not watched: a screen merely behind on fetching "
                        "keeps its session, a covered one does not)";
    PhaseGrade grade("the eviction-narrowing phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    for (size_t i = 0; i + 1 < std::size(kEvictionWatchMonitors); ++i) {
        wallpaperx::testing::nameScreen(manager, kEvictionWatchMonitors[i], screenOf(kEvictionWatchMonitors[i]));
        manager.requestWallpaper("video", clips[i], kEvictionWatchMonitors[i]);
        if (!manager.renderPending(kEvictionWatchMonitors[i]).rendered) {
            if (i == 0) grade.expectNoDevice();
            notVerified(kGoal, "only " + std::to_string(i) + " of the " +
                                   std::to_string(WallpaperManager::kMaxVideoSessions) +
                                   " videos this phase needs could be drawn on this host");
            return;
        }
    }

    // GOAL: S13.159 -- the question eviction asks is NARROWER than the clock's. A screen whose
    //       consumer has merely not fetched for a couple of seconds is still showing a current
    //       picture, and must not lose its decoder to another screen: the moment anything collects
    //       from it again it would have to reopen the file and start the video over. A screen that
    //       has SAID it is not being drawn is evidence rather than a guess, and is taken at once.
    //       False for a daemon that evicts whatever the clock has stopped -- every screen a user
    //       is not actively fetching from becomes a spare slot, so setting a ninth video steals a
    //       decoder from a screen that is visibly playing.
    // WHY-THIS-SHAPE: the eight monitors are deliberately left uncollected past the daemon's own
    //       interval before the ninth is asked for, and that staleness is ASSERTED through
    //       GetStatus's `watched` -- otherwise "the ninth was refused" would be equally true of a
    //       run where the interval had not elapsed and nothing was stale at all. The two
    //       directions are then driven from the same eight sessions, so the only thing that differs
    //       between "refused" and "evicted" is the screen report.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, evictIdleVideoSessionLocked()'s
    //       `if (currentSeqLocked(...) && screenDrawnLocked(monitorId)) continue;` changed back to
    //       `if (playingSeqLocked(monitorId, session.wallpaperSeq)) continue;` -- the clock's own
    //       test -> "FAIL: a ninth video is refused while every screen is showing a current
    //       picture", because a stale-but-drawn session is evicted instead. Observed 2026-09-20.
    //       Reverted.
    // COVERAGE-ASSERT: every one of the eight is required to read as unwatched before the ninth is
    //       asked for, so a run that had not gone stale cannot report the refusal as evidence that
    //       eviction ignores staleness.
    size_t stale = 0;
    const bool allStale = settlesWithin(std::chrono::seconds(20), [&] {
        stale = 0;
        for (size_t i = 0; i + 1 < std::size(kEvictionWatchMonitors); ++i) {
            const std::optional<wallpaperx::MonitorStatus> s = monitorStatus(manager, kEvictionWatchMonitors[i]);
            if (s && !s->watched && s->screenVisible && s->hasFrame) ++stale;
        }
        return stale + 1 == std::size(kEvictionWatchMonitors);
    });
    check(allStale, "every one of the eight screens is showing a current picture that nothing is collecting");
    if (!allStale) {
        std::fprintf(stderr, "  %zu of %zu reached that state\n", stale, std::size(kEvictionWatchMonitors) - 1);
        notVerified(kGoal, "the eight sessions never reached the stale-but-drawn state the two directions differ in");
        return;
    }

    const size_t ninth = std::size(kEvictionWatchMonitors) - 1;
    wallpaperx::testing::nameScreen(manager, kEvictionWatchMonitors[ninth], screenOf(kEvictionWatchMonitors[ninth]));
    manager.requestWallpaper("video", clips[ninth], kEvictionWatchMonitors[ninth]);
    g_files.resetCounts();
    const bool refused = !manager.renderPending(kEvictionWatchMonitors[ninth]).rendered;
    check(refused, "a ninth video is refused while every screen is showing a current picture");
    {
        const std::optional<wallpaperx::MonitorStatus> ninthStatus =
            monitorStatus(manager, kEvictionWatchMonitors[ninth]);
        reportUnless(ninthStatus && ninthStatus->lastError.find("at most") != std::string::npos &&
                         ninthStatus->lastErrorIsDaemonFault,
                     "and it is refused with the daemon's own limit named, not the file's fault",
                     ninthStatus ? "last_error: " + ninthStatus->lastError : std::string("no status"));
        reportUnless(g_files.totalCloses() == 0, "and no screen loses the session it is showing from",
                     std::to_string(g_files.totalCloses()) + " session(s) closed by the refused request");
    }

    // The other direction, from the same eight sessions: one screen says it is not being drawn, and
    // that one is taken at once.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `&& screenDrawnLocked(monitorId)` term
    //       deleted from evictIdleVideoSessionLocked() -> "FAIL: a screen that says it is not being
    //       drawn gives its session up for the ninth video", because nothing is then evictable and
    //       the ninth is refused again. Observed 2026-09-20. Reverted.
    manager.setOutputVisible(screenOf(kEvictionWatchMonitors[2]), false);
    // Asked for again, not merely re-rendered: a pass that failed marks the request serviced, so
    // renderPending() on its own would answer "nothing to do" and this check would read a no-op.
    manager.requestWallpaper("video", clips[ninth], kEvictionWatchMonitors[ninth]);
    g_files.resetCounts();
    const bool drewNinth = manager.renderPending(kEvictionWatchMonitors[ninth]).rendered;
    check(drewNinth, "a screen that says it is not being drawn gives its session up for the ninth video");
    reportUnless(g_files.closes(clips[2]) == 1, "and it is the covered screen's own file that is closed",
                 "closed " + std::to_string(g_files.totalCloses()) + " file(s), " +
                     std::to_string(g_files.closes(clips[2])) + " of them the covered screen's");
}

constexpr int kMonitorHandover = 0;
constexpr int kMonitorHandoverCycle = 1;
constexpr int kMonitorHandoverMany = 2;

void runHandoverLogChecks(const WallpaperManager::Config& config, const std::string& clip) {
    const char* kGoals = "S13.144 (a hand-over of a playing video is announced once per consumer, and so is a "
                         "refusal)";
    PhaseGrade grade("the hand-over phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    wallpaperx::testing::nameScreen(manager, kMonitorHandover, screenOf(kMonitorHandover));
    manager.requestWallpaper("video", clip, kMonitorHandover);
    if (!manager.renderPending(kMonitorHandover).rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the video render on this host produced no frame to hand over");
        return;
    }

    // Closed at the end, not at each export: the refusals below are produced by consumers HOLDING
    // their images, which is the state a refusal exists for.
    std::vector<int> open;
    struct CloseAll {
        std::vector<int>& fds;
        ~CloseAll() {
            for (const int fd : fds) {
                if (fd >= 0) close(fd);
            }
        }
    } closeAll{open};
    const auto take = [&](const std::string& consumer) {
        wallpaperx::FrameExport e = manager.exportCurrentFrame(kMonitorHandover, consumer);
        if (e.handle) {
            open.push_back(e.handle->frame.fd);
            open.push_back(e.handle->acquireFd);
        }
        return e;
    };

    // GOAL: S13.144's first half -- a consumer this monitor's video has already served is marked
    //       routine, so the daemon logs one line when a consumer starts reading and not one per
    //       picture; a consumer it has NOT served is not, whatever the other consumers did. False
    //       for a per-monitor mark, where one consumer starting to read silences the next one's
    //       first hand-over entirely -- and which of two consumers is being served is the diagnosis
    //       the line exists to carry.
    // WHY-THIS-SHAPE: two consumers, because "once per consumer" and "once per monitor" agree on one.
    //       The mark is read off the manager rather than off the daemon's stdout: what dbus_service
    //       prints is one branch on exactly this flag, and capturing a process's stdout to grade a
    //       branch makes every other diagnostic in the phase disappear with it.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, exportCurrentFrame()'s `served.push_back`
    //       moved above the `std::find` that sets `handle.routine` -> "FAIL: the first hand-over to a
    //       consumer is not routine, so it is logged". Setting `handle.routine = false` outright
    //       reddens the second clause instead. Observed 2026-09-18. Reverted.
    const wallpaperx::FrameExport firstA = take(":1.301");
    const wallpaperx::FrameExport againA = take(":1.301");
    const wallpaperx::FrameExport firstB = take(":1.302");
    if (!firstA.handle || !againA.handle || !firstB.handle) {
        notVerified(kGoals, "the three hand-overs this check needs were not all served");
        return;
    }
    check(!firstA.handle->routine, "the first hand-over to a consumer is not routine, so it is logged");
    check(againA.handle->routine, "the next hand-over to the same consumer is routine, so it is not logged again");
    check(!firstB.handle->routine, "and a second consumer's first hand-over is its own line, not silenced by the first");

    // GOAL: S13.144's second half -- a refusal is reported once per consumer per episode: the same
    //       consumer refused again is not reported again, and a different consumer being refused is.
    //       False for a per-monitor flag, which would report only whichever consumer was refused
    //       first and leave a second consumer's starvation invisible.
    // WHY-THIS-SHAPE: consumers are added until the daemon refuses one, rather than a fixed number
    //       being assumed: how many images a monitor's ring holds is the driver's business, and a
    //       count written here would silently stop reaching the refusal on a driver that allocates
    //       one more. The loop's own success is asserted.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, exportCurrentFrame()'s `out.firstRefusal =
    //       ...` replaced with `out.firstRefusal = true;` -> "FAIL: the same consumer refused again
    //       is not reported again". Replacing it with `false` reddens the other two clauses.
    //       Observed 2026-09-18. Reverted.
    std::string starved;
    for (int i = 0; i < 16 && starved.empty(); ++i) {
        const std::string consumer = ":1.4" + std::to_string(i);
        const wallpaperx::FrameExport e = take(consumer);
        if (e.handle) continue;
        if (e.reason != wallpaperx::NoFrameReason::NoFreeImage) break;
        starved = consumer;
        check(e.firstRefusal, "the first refusal of a consumer is reported");
    }
    if (starved.empty()) {
        notVerified("S13.144's refusal half", "no consumer could be refused an image on this host");
        return;
    }
    const wallpaperx::FrameExport refusedAgain = manager.exportCurrentFrame(kMonitorHandover, starved);
    check(refusedAgain.reason == wallpaperx::NoFrameReason::NoFreeImage && !refusedAgain.firstRefusal,
          "the same consumer refused again is not reported again");
    const wallpaperx::FrameExport refusedOther = manager.exportCurrentFrame(kMonitorHandover, ":1.499");
    check(refusedOther.reason == wallpaperx::NoFrameReason::NoFreeImage && refusedOther.firstRefusal,
          "but a different consumer being refused is reported in its own right");

    // GOAL: S13.144's refusal half, RESTATED for Stage E4 -- the line is rate-limited per consumer,
    //       NOT re-armed by a hand-over. A consumer holding its fill of a playing video is served
    //       and refused alternately, many times a second: under the Stage E3 rule, where being
    //       served ended the episode, that is a log line per picture for as long as the video
    //       plays. False for a daemon that re-arms on a hand-over, and false for one that reports
    //       nothing at all, which the checks above already cover.
    // WHY-THIS-SHAPE: driven by taking ONE consumer to its own cap across published pictures, which
    //       is the only way to make the same consumer be refused, then served, then refused again:
    //       a consumer asked twice for one picture is handed the same image back, so the second ask
    //       is neither a new hand-over nor a refusal. Every other shape either cannot refuse the
    //       consumer a second time or cannot serve it in between.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the Stage E3 line
    //       `std::erase_if(it->second.refusalsReported, [&client](const ConsumerReport& r) { return
    //       r.client == client; });` added back after `it->second.refusedHandovers = 0;` in
    //       exportCurrentFrame() -> "FAIL: a consumer served in between is not reported again when
    //       it is refused once more", and nothing else red. Re-measured 2026-09-20 after
    //       RefusalReport became the shared ConsumerReport. Reverted.
    // COVERAGE-ASSERT: the drive asserts each of its three states -- refused, then served, then
    //       refused -- so a run that never reached one of them fails there rather than reporting
    //       the rate limit as established.
    // A monitor of its own, because the checks above deliberately leave this one's ring fully spoken
    // for: publishing another picture needs an image nobody holds, and that is exactly what the
    // starvation above removed.
    {
        const std::string cycling = ":1.480";
        wallpaperx::testing::nameScreen(manager, kMonitorHandoverCycle, screenOf(kMonitorHandoverCycle));
        const auto republish = [&manager, &clip]() {
            manager.requestWallpaper("video", clip, kMonitorHandoverCycle);
            return manager.renderPending(kMonitorHandoverCycle).rendered;
        };
        const auto take = [&](const std::string& consumer) {
            wallpaperx::FrameExport e = manager.exportCurrentFrame(kMonitorHandoverCycle, consumer);
            if (e.handle) {
                open.push_back(e.handle->frame.fd);
                open.push_back(e.handle->acquireFd);
            }
            return e;
        };
        bool atCap = false;
        bool firstRefusalReported = false;
        uint64_t lastHeld = 0;
        const bool firstPicture = republish();
        check(firstPicture, "the second monitor's video draws its first picture");
        for (int i = 0; firstPicture && i < 8 && !atCap; ++i) {
            const wallpaperx::FrameExport e = take(cycling);
            if (!e.handle) {
                atCap = e.reason == wallpaperx::NoFrameReason::NoFreeImage;
                // The loop's own refusal IS this consumer's first, so it is read here rather than
                // asked for again: a second call would be the one being rate-limited.
                firstRefusalReported = e.firstRefusal;
                break;
            }
            lastHeld = e.handle->frame.bufferId;
            if (!republish()) break;
        }
        check(atCap && lastHeld != 0, "a consumer can be taken to the point where it is refused while holding images");
        if (atCap && lastHeld != 0) {
            check(firstRefusalReported, "its first refusal is reported");
            manager.releaseBuffer(cycling, kMonitorHandoverCycle, lastHeld);
            const bool republished = republish();
            check(republished, "another picture is published for it to be served again");
            const wallpaperx::FrameExport servedAgain =
                republished ? manager.exportCurrentFrame(kMonitorHandoverCycle, cycling) : wallpaperx::FrameExport{};
            check(servedAgain.handle.has_value(), "and it is served again after giving an image back");
            if (servedAgain.handle) {
                open.push_back(servedAgain.handle->frame.fd);
                open.push_back(servedAgain.handle->acquireFd);
                const bool ready = republish();
                check(ready, "and one more picture is published, so it can be refused once more");
                if (ready) {
                    const wallpaperx::FrameExport refusedAfterService =
                        manager.exportCurrentFrame(kMonitorHandoverCycle, cycling);
                    check(refusedAfterService.reason == wallpaperx::NoFrameReason::NoFreeImage &&
                              !refusedAfterService.handle,
                          "and refused once more after it");
                    check(!refusedAfterService.firstRefusal,
                          "a consumer served in between is not reported again when it is refused once more");
                }
            }
        }
    }

    // GOAL: S13.174 -- MORE CONSUMERS THAN SLOTS on one playing video produce a BOUNDED number of
    //       reported hand-overs, not one per picture. A consumer that is reading keeps its slot,
    //       and a consumer that cannot be given one is silent rather than reported every time.
    //       False for the code before this fix, where the list held four names and dropped the
    //       oldest to make room: with five consumers taking turns, every hand-over evicted the name
    //       of the consumer that would ask next, so none was ever "already seen". Measured by the
    //       QA pass at the cliff: four consumers, 4 log lines in ten seconds; five consumers, 1453.
    // WHY-THIS-SHAPE: the bound asserted is EXACTLY kMaxHoldersPerMonitor, not "small". The policy
    //       makes it exact and this phase is single-threaded with no render worker, so there is no
    //       interleaving for it to depend on: the first four consumers take the four slots, hold
    //       them by reading, and the fifth and sixth can never be given one and are therefore never
    //       reported. "Fewer than the hand-overs" would be satisfied by a daemon reporting every
    //       other picture, which on a 60 fps video is still half a gigabyte of journal a day.
    //       Reports are read off the manager's `routine` flag rather than off the daemon's stdout:
    //       what the service prints is one branch on exactly that flag, and capturing a process's
    //       output to grade a branch takes every other diagnostic in the phase with it.
    //       No new picture is published between rounds, though the flood happens over successive
    //       ones: the report is decided per hand-over and not per picture, and the only way this
    //       phase (which has no render worker) can publish another is another requestWallpaper,
    //       which opens a NEW session whose served list is empty -- measured, that reports all four
    //       slot-holders once per round and grades the session churn instead of the policy.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, reportConsumer()'s full-list branch made to
    //       evict unconditionally -- the `if (now - quietest->heard < interval) return
    //       false;` line deleted -> all three checks red, printing "240 report(s) across 240
    //       hand-over(s): :1.90=40, :1.91=40, ... :1.95=40" against 4 reports with the fix in.
    //       That is the QA finding's cliff exactly: one line per hand-over. Observed 2026-09-21.
    //       Reverted.
    // COVERAGE-ASSERT: the number of hand-overs actually served is asserted, so a loop that was cut
    //       short -- which would make "few were reported" true for the wrong reason -- fails here
    //       rather than reporting a bound that was never tested.
    {
        constexpr int kRounds = 40;
        const size_t kConsumers = WallpaperManager::kMaxHoldersPerMonitor + 2;
        wallpaperx::testing::nameScreen(manager, kMonitorHandoverMany, screenOf(kMonitorHandoverMany));
        manager.requestWallpaper("video", clip, kMonitorHandoverMany);
        const bool drew = manager.renderPending(kMonitorHandoverMany).rendered;
        check(drew, "a third monitor's video draws a picture for many consumers to take turns on");
        if (drew) {
            std::vector<std::string> names;
            for (size_t i = 0; i < kConsumers; ++i) names.push_back(":1.9" + std::to_string(i));

            int served = 0;
            int reported = 0;
            std::map<std::string, int> reportsPer;
            for (int round = 0; round < kRounds; ++round) {
                for (const std::string& consumer : names) {
                    const wallpaperx::FrameExport e = manager.exportCurrentFrame(kMonitorHandoverMany, consumer);
                    if (!e.handle) continue;
                    ++served;
                    if (!e.handle->routine) {
                        ++reported;
                        ++reportsPer[consumer];
                    }
                    // Given straight back, so the ring never runs out and every consumer keeps
                    // getting a turn: what is being measured is the reporting, not the holding.
                    manager.releaseBuffer(consumer, kMonitorHandoverMany, e.handle->frame.bufferId);
                    close(e.handle->frame.fd);
                    close(e.handle->acquireFd);
                }
            }

            std::string perConsumer;
            for (const std::string& consumer : names) {
                perConsumer += (perConsumer.empty() ? "" : ", ") + consumer + "=" + std::to_string(reportsPer[consumer]);
            }
            std::printf("video_wallpaper_test: %zu consumers took %d hand-overs in turn from one video; %d were "
                        "reported (%s)\n",
                        kConsumers, served, reported, perConsumer.c_str());
            std::fflush(stdout);

            reportUnless(served >= kRounds * static_cast<int>(kConsumers) / 2,
                         "enough hand-overs were served to tell a bounded report count from a per-picture one",
                         std::to_string(served) + " hand-over(s)");
            reportUnless(reported <= static_cast<int>(WallpaperManager::kMaxHoldersPerMonitor),
                         "more consumers than slots on one playing video are reported no more often than there "
                         "are slots -- not once per picture",
                         std::to_string(reported) + " report(s) across " + std::to_string(served) +
                             " hand-over(s): " + perConsumer);
            // The two halves the bound is made of, asserted apart so a failure says which broke.
            int keptItsSlot = 0;
            int neverReported = 0;
            for (const std::string& consumer : names) {
                if (reportsPer[consumer] == 1) ++keptItsSlot;
                if (reportsPer[consumer] == 0) ++neverReported;
            }
            reportUnless(keptItsSlot == static_cast<int>(WallpaperManager::kMaxHoldersPerMonitor),
                         "a consumer that keeps reading keeps its slot, so it is reported once and never again",
                         std::to_string(keptItsSlot) + " consumer(s) reported exactly once: " + perConsumer);
            reportUnless(neverReported == static_cast<int>(kConsumers - WallpaperManager::kMaxHoldersPerMonitor),
                         "and a consumer that cannot be given a slot is not reported at all, rather than every time",
                         std::to_string(neverReported) + " consumer(s) never reported: " + perConsumer);

            // One hand-over, given straight back; whether it was reported.
            const auto reportedNow = [&manager](const std::string& consumer) -> std::optional<bool> {
                const wallpaperx::FrameExport e = manager.exportCurrentFrame(kMonitorHandoverMany, consumer);
                if (!e.handle) return std::nullopt;
                manager.releaseBuffer(consumer, kMonitorHandoverMany, e.handle->frame.bufferId);
                close(e.handle->frame.fd);
                close(e.handle->acquireFd);
                return !e.handle->routine;
            };

            // GOAL: S13.179 -- a consumer that STILL READS keeps its slot past the interval: it is
            //       not reported again every interval, and a newcomer does not take its slot from it.
            //       What frees a slot is its holder going quiet for a whole interval, not the time
            //       since its line was written. False for a list that ages a slot by when it was
            //       LOGGED: every interval the holders look stale, a slotless reader evicts one, and
            //       the evicted one is reported again on its next picture -- in production a line per
            //       reader per minute, for as long as they read.
            // WHY-THIS-SHAPE: INHERENTLY A WAIT PAST the phase's refusalLogInterval (5 s here, a
            //       minute in production), because nothing else can make a slot old. The holders keep
            //       reading through it -- one hand-over each every fifth of an interval, so each is
            //       always heard well within one -- and a slotless consumer asks once the interval has
            //       passed, which is the moment a logged-age rule gives its slot away. The holders are
            //       then asked again, because the defect shows as THEIR re-report as much as the
            //       newcomer's.
            // CONTROL: backend/daemon/wallpaper_manager.cpp, reportConsumer()'s eviction measured on
            //       `logged` instead of `heard` (both the min_element comparison and the age test) ->
            //       "FAIL: a consumer that keeps reading keeps its slot past the interval", printing "3
            //       holder(s) reported again; the newcomer was reported". Reverted.
            {
                const auto readEvery = config.refusalLogInterval / 5;
                const Clock::time_point until = Clock::now() + config.refusalLogInterval + readEvery;
                int heldReads = 0;
                bool holderReportedDuringWait = false;
                while (Clock::now() < until) {
                    for (size_t i = 0; i < WallpaperManager::kMaxHoldersPerMonitor; ++i) {
                        const std::optional<bool> r = reportedNow(names[i]);
                        if (r) ++heldReads;
                        if (r && *r) holderReportedDuringWait = true;
                    }
                    std::this_thread::sleep_for(readEvery);
                }
                const std::optional<bool> newcomer = reportedNow(names[WallpaperManager::kMaxHoldersPerMonitor]);
                int holdersReportedAfter = 0;
                for (size_t i = 0; i < WallpaperManager::kMaxHoldersPerMonitor; ++i) {
                    const std::optional<bool> r = reportedNow(names[i]);
                    if (r && *r) ++holdersReportedAfter;
                }
                std::printf("video_wallpaper_test: after %d reads over an interval by the %zu slot holders, a slotless "
                            "consumer was %s and %d holder(s) were reported again\n",
                            heldReads, WallpaperManager::kMaxHoldersPerMonitor,
                            newcomer ? (*newcomer ? "REPORTED" : "silent") : "not served", holdersReportedAfter);
                std::fflush(stdout);
                reportUnless(heldReads > 0 && newcomer.has_value(),
                             "the slot holders and the newcomer were all served, so the interval was really read through",
                             std::to_string(heldReads) + " holder read(s)");
                reportUnless(!holderReportedDuringWait && holdersReportedAfter == 0 && newcomer && !*newcomer,
                             "a consumer that keeps reading keeps its slot past the interval: it is not reported "
                             "again and a slotless newcomer does not take the slot from it",
                             std::to_string(holdersReportedAfter) + " holder(s) reported again; the newcomer " +
                                 (newcomer && *newcomer ? "was reported" : "was silent"));
            }

            // GOAL: S13.180 -- a consumer that LEAVES THE BUS frees its slot at once, so the next
            //       consumer is named on its first picture rather than an interval later. And the
            //       departure is handled on the calling thread alone: nothing about it is owed to the
            //       render worker for a consumer that held no image.
            // WHY-THIS-SHAPE: the newcomer is one that was just refused a slot by the check above, so
            //       "it is now reported" can only be the departure's doing -- no interval has passed
            //       since it last asked.
            // CONTROL: backend/daemon/wallpaper_manager.cpp, `std::erase_if(state.served, departed);`
            //       deleted from forgetHeldBuffers() -> "FAIL: a consumer leaving the bus frees its
            //       slot at once, so the next consumer is named on its first picture", printing "the
            //       newcomer was silent". Reverted.
            {
                const std::vector<int> owed = manager.forgetHeldBuffers(names[0], nullptr);
                reportUnless(owed.empty(),
                             "a departing consumer that held no image leaves the render worker nothing to do",
                             std::to_string(owed.size()) + " monitor(s) posted for a departure that freed no image");
                const std::optional<bool> newcomer = reportedNow(names[WallpaperManager::kMaxHoldersPerMonitor]);
                reportUnless(newcomer && *newcomer,
                             "a consumer leaving the bus frees its slot at once, so the next consumer is named on its "
                             "first picture",
                             newcomer ? "the newcomer was silent" : "the newcomer was not served");
            }
        }
    }
}

constexpr int kMemoryMonitors[] = {0, 1, 2, 3, 4, 5};

void runMemoryRetryChecks(const WallpaperManager::Config& config, const std::vector<std::string>& clips) {
    const char* kGoals = "S13.139 (an open that runs out of memory frees an idle session and tries once more)";
    PhaseGrade grade("the memory-retry phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    struct Disarm {
        ~Disarm() { g_failImageCreates.store(0); }
    } disarm;

    const auto draw = [&](size_t i) {
        wallpaperx::testing::nameScreen(manager, kMemoryMonitors[i], screenOf(kMemoryMonitors[i]));
        manager.requestWallpaper("video", clips[i], kMemoryMonitors[i]);
        return renderCounted(manager, kMemoryMonitors[i]);
    };
    if (!draw(0).outcome.rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }
    reportUnless(draw(1).outcome.rendered && draw(2).outcome.rendered && draw(3).outcome.rendered,
                 "four video monitors are drawn first", "one of the four did not commit");
    // THREE sessions to free, not two. The bound below is "the retry is tried once": telling a
    // daemon that stops after one retry from one that keeps freeing sessions needs a third idle
    // session for the third attempt to take -- with only two, both behave identically and the check
    // could not fail. This phase runs no render worker, so a monitor is idle exactly while it owes a
    // render, and setOutputSize owes one without changing the wallpaper.
    std::vector<int> owed;
    for (size_t i = 1; i <= 3; ++i) {
        manager.setOutputSize(screenOf(kMemoryMonitors[i]), kResizedWidth, kResizedHeight, owed);
    }
    reportUnless(owed.size() == 3, "three of them are left owing a render, so their sessions are free to close",
                 "setOutputSize named " + std::to_string(owed.size()) + " monitor(s)");

    // GOAL: S13.139 -- what the card can hold is settled by trying. An open that runs out of memory
    //       closes a session nothing is showing and opens again, and that second attempt commits.
    //       False for a daemon that reports the shortage to the user, which would refuse a wallpaper
    //       the machine has room for as soon as another screen's decoder had taken the last of it.
    // WHY-THIS-SHAPE: the shortage is injected into the daemon's own image creation, which on this
    //       path is the picture converter's image inside the decode that opens the session. The
    //       signature graded is that the FILE WAS OPENED TWICE: "it committed" alone is also true of
    //       an attempt that never ran out, and the injection being spent is asserted separately.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, retryAfterEvictionLocked()'s condition
    //       `!classifyFailure().exhausted` replaced by `true`, so no shortage is ever retried ->
    //       "FAIL: an open that runs out of memory frees an idle session and commits on the retry",
    //       printing "did not commit, 1 open(s), 1 close(s), 1 packet(s)", and the three checks after
    //       it; the eviction staging in runPlaybackBlameChecks then reports its death never came.
    //       Re-observed 2026-09-21 on the NVIDIA entry. Reverted.
    // COVERAGE-ASSERT: the armed shortage is asserted to have been spent, so a run where the
    //       injection never landed fails instead of grading an ordinary open.
    g_failImageCreates.store(1);
    const PassCost retried = draw(4);
    check(g_failImageCreates.load() == 0, "the injected memory shortage was spent on the daemon's own image");
    reportUnless(retried.outcome.rendered && retried.opens == 2,
                 "an open that runs out of memory frees an idle session and commits on the retry", describe(retried));
    const std::multiset<std::string> afterRetry = g_files.live();
    reportUnless(afterRetry.count(clips[1]) == 0 && afterRetry.count(clips[0]) == 1,
                 "what it freed is a session whose monitor owes a render, not one that is showing its video",
                 "open: " + describeFiles(afterRetry));

    // GOAL: S13.139's bound -- the retry is tried ONCE. A second shortage in a row is reported, not
    //       answered by freeing session after session. False for a loop that keeps evicting while
    //       anything is idle, which turns one bad allocation into every other screen going dark.
    // WHY-THIS-SHAPE: the count graded is OPENS of the new file -- two, the attempt and its one
    //       retry -- rather than closes, which also carry each failed attempt tearing its own
    //       half-built pipeline down and so say nothing about how many sessions were freed. How many
    //       were freed is asserted separately, by name. THREE idle sessions are set up above for this
    //       one check: with two, a daemon that retries once and a daemon that retries for ever both
    //       run out of sessions on the same attempt and produce the same counts, so the check could
    //       not have failed at all.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `retried ||` term deleted from
    //       retryAfterEvictionLocked()'s condition -> "FAIL: a second shortage in a row is reported
    //       rather than answered by freeing another session", printing "committed, 3 open(s), 4
    //       close(s), 3 packet(s), 0 shortage(s) left unspent", and the two checks after it.
    //       Re-observed 2026-09-21 on the NVIDIA entry. Reverted.
    // COVERAGE-ASSERT: the shortages left unspent are printed beside the failure, so a run where
    //       only one of the two landed is visible rather than graded as a bound that held.
    g_failImageCreates.store(2);
    const PassCost twice = draw(5);
    reportUnless(!twice.outcome.rendered && twice.opens == 2,
                 "a second shortage in a row is reported rather than answered by freeing another session",
                 describe(twice) + ", " + std::to_string(g_failImageCreates.load()) + " shortage(s) left unspent");
    reportUnless(g_files.closes(clips[2]) == 1 && g_files.closes(clips[3]) == 0 &&
                     g_files.live().count(clips[0]) == 1 && g_files.live().count(clips[4]) == 1,
                 "exactly one session was freed for it, and neither of the two that are showing a video",
                 "open: " + describeFiles(g_files.live()));
    const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMemoryMonitors[5]);
    reportUnless(status && status->lastErrorIsDaemonFault && !status->lastError.empty(),
                 "and the monitor records it as the daemon's own failure, not the file's",
                 status ? "last_error: " + status->lastError : std::string("no status"));
}

constexpr int kMonitorRetry = 0;

// The end state a spent retry budget leaves: no frame, an error standing, and no render owed, so
// nothing will run again until the user acts.
bool settledOnFailure(const WallpaperManager& manager, int monitorId) {
    const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, monitorId);
    return status && !status->hasFrame && !status->lastError.empty() && !status->renderOutstanding;
}

// For a phase whose subject is not watching at all but which needs its video to keep playing with no
// consumer: an unwatched interval longer than any phase runs. Without it such a phase leans on the
// grace period before an uncollected video stops -- two seconds in production, far less here -- and
// passes only if it finishes inside that, which a loaded machine does not promise.
WallpaperManager::Config playsUnwatched(const WallpaperManager::Config& base) {
    WallpaperManager::Config config = base;
    config.unwatchedAfter = std::chrono::hours(1);
    return config;
}

void runPlaybackRetryChecks(const WallpaperManager::Config& config, const std::vector<std::string>& clips) {
    const char* kGoals = "S13.140 (playback that keeps failing is reopened three times and then left alone; a "
                         "picture shown restores the budget; a new wallpaper starts fresh)";
    PhaseGrade grade("the playback-retry phase");
    FrameReadyLog signals;
    // Its reopens happen only while the video counts as watched, and nothing here collects from it.
    WallpaperManager manager(playsUnwatched(config));
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    const auto play = [&](const std::string& path) {
        manager.requestWallpaper("video", path, kMonitorRetry);
        worker.post(kMonitorRetry);
        const std::optional<wallpaperx::MonitorStatus> before = monitorStatus(manager, kMonitorRetry);
        const uint64_t next = (before ? before->generation : 0) + 1;
        return signals.waitAtLeast(kMonitorRetry, next);
    };
    wallpaperx::testing::nameScreen(manager, kMonitorRetry, screenOf(kMonitorRetry));
    if (!play(clips[0])) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }

    // GOAL: S13.140 -- playback that fails again after every reopen is retried three times and then
    //       left alone: the file is opened exactly kPlaybackRetries more times, the monitor ends with
    //       no frame and its error standing, a frame request answers RenderFailed, and no render is
    //       owed, so nothing reopens it until the user sets a wallpaper again. False for an unbounded
    //       retry, which reopens a decoder tens of times a second for ever.
    // WHY-THIS-SHAPE: the injected failure is the picture AFTER the one a decoder delivered, so each
    //       reopen SUCCEEDS and fails again on its first clock picture -- which is the only shape
    //       that reaches the bound at all. A failure that also broke the open would end the chain
    //       after one try and the check would pass over a daemon with no bound whatsoever. The count
    //       graded is opens, not failures: an open is what the bound is spending.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, MonitorState::ReopenBudget::spend()'s limit
    //       made to apply to shortages only (`if (shortage && spent >= kShortageRetries)`), leaving
    //       the file's reopens unbounded -> "FAIL: a video whose playback keeps failing stops on its
    //       own rather than reopening for ever" after the 30-second settle bound, and this phase
    //       reported not verified. Re-observed 2026-09-21 on the NVIDIA entry. Reverted.
    // GOAL: S13.192 -- a video that fails for good shows nothing and its error: its last picture is
    //       not held. The same settled state -- no frame, the error standing, a frame request
    //       answering RenderFailed -- is what is read below.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, both `state.hasFrame = false;` in
    //       recordFailureLocked() and `if (failed) releaseTarget(monitorId);` in advanceSessionLocked()
    //       deleted, so a failed video keeps its last picture -> "FAIL: a video whose playback keeps
    //       failing stops on its own ...", S13.142's "and the monitor is left with no frame", and the
    //       shortage phase's settle checks. Observed 2026-09-21 on the NVIDIA entry. Reverted.
    // COVERAGE-ASSERT: the number of failures actually injected is asserted to be at least one per
    //       reopen plus the one that spent the budget, so a run where the injection stopped landing
    //       fails instead of grading a daemon that simply never failed.
    g_files.resetCounts();
    const int injectedBefore = g_playbackFailure.injected();
    g_playbackFailure.arm();
    const bool settled = settlesWithin(std::chrono::seconds(30),
                                       [&manager] { return settledOnFailure(manager, kMonitorRetry); });
    g_playbackFailure.disarm();
    check(settled, "a video whose playback keeps failing stops on its own rather than reopening for ever");
    if (!settled) {
        notVerified(kGoals, "the monitor never settled after its playback was made to fail");
        return;
    }
    const int reopens = g_files.opens(clips[0]);
    reportUnless(reopens == static_cast<int>(WallpaperManager::kPlaybackRetries),
                 "playback that keeps failing is reopened three times and then left alone",
                 std::to_string(reopens) + " reopen(s) of the file");
    reportUnless(g_playbackFailure.injected() - injectedBefore >= reopens + 1,
                 "every one of those reopens was answered by another injected failure",
                 std::to_string(g_playbackFailure.injected() - injectedBefore) + " failure(s) for " +
                     std::to_string(reopens) + " reopen(s)");
    reportUnless(manager.exportCurrentFrame(kMonitorRetry).reason == wallpaperx::NoFrameReason::RenderFailed,
                 "and a frame request for that monitor answers RenderFailed", "a different no-frame reason");
    {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, kMonitorRetry);
        // What /health reads to name a monitor in its wallpapers check: an error recorded with no
        // render outstanding. The wording of that check is graded in http_server_test.
        reportUnless(status && !status->lastError.empty() && !status->renderOutstanding,
                     "the monitor is left with its error standing and no render owed, which is what /health names",
                     status ? "last_error: " + status->lastError : std::string("no status"));
        std::printf("video_wallpaper_test: a spent playback budget leaves -- %s\n",
                    status ? status->lastError.c_str() : "no status");
        std::fflush(stdout);
    }
    const int quietOpens = g_files.opens(clips[0]);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    reportUnless(g_files.opens(clips[0]) == quietOpens, "and nothing reopens it afterwards",
                 std::to_string(g_files.opens(clips[0]) - quietOpens) + " further open(s) in a quiet half second");

    // GOAL: S13.140's second clause -- a NEW wallpaper starts with a full budget, and a picture shown
    //       since the last failure restores it. So a file that hiccups once an hour keeps recovering
    //       instead of spending a retry it never gets back. False for a count kept per monitor and
    //       never reset, which would leave a screen that failed three times in its first hour unable
    //       to recover for the rest of the session.
    // WHY-THIS-SHAPE: the budget spent before the video is allowed to play again is MEASURED rather
    //       than assumed to be one: disarming the injection cannot stop a failure already in flight,
    //       so how many were spent first is a race. What is asserted is the difference -- that the
    //       full three are available AFTER a picture is shown, whatever was spent before it -- and
    //       that at least one and fewer than three had been spent, so the restoration is not being
    //       read off a budget that was never touched.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, `playbackBudgetLocked(it->second) =
    //       {it->second.wallpaperSeq};` deleted from advanceSessionLocked()'s commit -> exactly "FAIL:
    //       a picture shown since the last failure restores the whole retry budget", printing "2
    //       reopen(s) after the picture, where 3 were owed". Re-observed 2026-09-21 on the NVIDIA
    //       entry. Reverted.
    if (!play(clips[1])) {
        check(false, "a new video wallpaper on that monitor draws again after the budget was spent");
        return;
    }
    g_files.resetCounts();
    g_playbackFailure.arm();
    const bool failedOnce = settlesWithin(std::chrono::seconds(30),
                                          [&clips] { return g_files.opens(clips[1]) >= 1; });
    g_playbackFailure.disarm();
    check(failedOnce, "the new wallpaper's playback fails and is reopened, so the budget is partly spent");
    if (!failedOnce) return;

    // Let it play again: a committed clock picture is what restores the budget.
    const std::optional<wallpaperx::MonitorStatus> playing = monitorStatus(manager, kMonitorRetry);
    const uint64_t resumedBy = (playing ? playing->generation : 0) + 5;
    if (!signals.waitAtLeast(kMonitorRetry, resumedBy)) {
        check(false, "the reopened wallpaper plays again once the injected failure is withdrawn");
        return;
    }
    const int spentFirst = g_files.opens(clips[1]);
    reportUnless(spentFirst >= 1 && spentFirst < static_cast<int>(WallpaperManager::kPlaybackRetries),
                 "part of the budget -- not all of it -- was spent before the video played again",
                 std::to_string(spentFirst) + " reopen(s) spent");

    g_playbackFailure.arm();
    const bool settledAgain = settlesWithin(std::chrono::seconds(30),
                                            [&manager] { return settledOnFailure(manager, kMonitorRetry); });
    g_playbackFailure.disarm();
    check(settledAgain, "the wallpaper settles again once its restored budget is spent");
    if (!settledAgain) return;
    reportUnless(g_files.opens(clips[1]) == spentFirst + static_cast<int>(WallpaperManager::kPlaybackRetries),
                 "a picture shown since the last failure restores the whole retry budget",
                 std::to_string(g_files.opens(clips[1]) - spentFirst) + " reopen(s) after the picture, where " +
                     std::to_string(WallpaperManager::kPlaybackRetries) + " were owed");
}

constexpr int kMonitorFaulting = 0;
constexpr int kMonitorFaultStill = 1;
// A size nothing else in this phase draws at, so the injected device fault lands on this monitor's
// rebuild and not on a picture of the video playing beside it.
constexpr uint32_t kFaultWidth = 72;
constexpr uint32_t kFaultHeight = 36;

void runGpuFaultChecks(const WallpaperManager::Config& config, const std::vector<std::string>& clips,
                       const std::string& still) {
    const char* kGoals = "S13.141 (a video's pictures do not clear recorded GPU health) and S13.142 (a GPU fault "
                         "during playback is never retried)";
    PhaseGrade grade("the gpu-fault phase");
    FrameReadyLog signals;
    // Both halves need the video to go on submitting pictures with nothing collecting them: S13.141
    // reads health across a window it asserts the video kept playing in, and S13.142 injects its
    // fault into a picture's submission.
    WallpaperManager manager(playsUnwatched(config));
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager,
                        [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};

    wallpaperx::testing::nameScreen(manager, kMonitorFaulting, screenOf(kMonitorFaulting));
    manager.requestWallpaper("video", clips[0], kMonitorFaulting);
    worker.post(kMonitorFaulting);
    wallpaperx::testing::nameScreen(manager, kMonitorFaultStill, screenOf(kMonitorFaultStill));
    manager.requestWallpaper("static", still, kMonitorFaultStill);
    worker.post(kMonitorFaultStill);
    if (!signals.wait(kMonitorFaulting, 1) || !signals.wait(kMonitorFaultStill, 1)) {
        grade.expectNoDevice();
        notVerified(kGoals, "the video and the still did not both draw on this host");
        return;
    }

    // GOAL: S13.141 -- GPU health recorded as failed is cleared by a render somebody asked for, and
    //       NOT by the pictures a video goes on showing. False for a daemon that clears it on every
    //       commit: a video playing on one screen would wipe a fault recorded for another before an
    //       operator or a watchdog could read it, and /health would report a broken device as ok.
    // WHY-THIS-SHAPE: the fault is injected into a framebuffer of a size only the still monitor's
    //       rebuild asks for, because the video beside it is submitting several times a second and an
    //       unfiltered injection would be spent on one of its pictures instead. The video is then
    //       asserted to have kept playing across the window in which health is re-read, so "still
    //       failed" is never read off a daemon that had stopped committing anything.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the recordHealthLocked(gpu, Ok, ...) call
    //       moved from renderPassLocked()'s commit into commitFrameLocked(), which the clock also
    //       calls -> "FAIL: the pictures a video keeps showing do not clear a recorded GPU fault",
    //       printing gpu health ok. Observed 2026-09-18. Reverted.
    // COVERAGE-ASSERT: the recorded fault is asserted before the window as well as after it, so the
    //       check cannot pass because nothing was ever recorded.
    g_framebufferLostGate.arm(CallGate::Mode::Fail, extentOf(kFaultWidth, kFaultHeight));
    std::vector<int> owed;
    manager.setOutputSize(screenOf(kMonitorFaultStill), kFaultWidth, kFaultHeight, owed);
    for (const int id : owed) worker.post(id);
    const bool faulted = settlesWithin(std::chrono::seconds(30), [&manager] {
        return manager.snapshot().health.gpu.status == wallpaperx::HealthStatus::Fail;
    });
    g_framebufferLostGate.release();
    check(faulted, "a render that loses the device records GPU health as failed");
    if (!faulted) {
        notVerified(kGoals, "no GPU fault could be recorded on this host, so there is nothing to keep recorded");
        return;
    }
    const size_t playedBefore = signals.countFor(kMonitorFaulting);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const size_t playedAfter = signals.countFor(kMonitorFaulting);
    reportUnless(playedAfter >= playedBefore + 5, "the video kept playing across the window health is judged in",
                 std::to_string(playedAfter - playedBefore) + " picture(s) in a second");
    reportUnless(manager.snapshot().health.gpu.status == wallpaperx::HealthStatus::Fail,
                 "the pictures a video keeps showing do not clear a recorded GPU fault",
                 "gpu health: " + manager.snapshot().health.gpu.detail);

    // The other half of the same rule: a render somebody asked for does clear it.
    manager.requestWallpaper("static", still, kMonitorFaultStill);
    worker.post(kMonitorFaultStill);
    const bool cleared = signals.waitAtLeast(kMonitorFaultStill, 2) &&
                         settlesWithin(std::chrono::seconds(10), [&manager] {
                             return manager.snapshot().health.gpu.status == wallpaperx::HealthStatus::Ok;
                         });
    reportUnless(cleared, "a render somebody asked for does clear it",
                 "gpu health: " + manager.snapshot().health.gpu.detail);

    // GOAL: S13.142 -- a GPU fault during playback is never retried: the session is dropped, the
    //       fault is recorded, and the file is NOT reopened, because rendering again on a device that
    //       has just stopped answering is worse than stopping. False for a retry that treats every
    //       playback failure alike, which would reopen a decoder against a lost device three more
    //       times before giving up.
    // WHY-THIS-SHAPE: the injection is a lost device on the next queue submission, armed while the
    //       only thing submitting is the clock -- no job is posted and no other monitor is playing,
    //       so the submission it lands on is a picture's. The count graded is reopens of this
    //       monitor's own copy of the fixture, which only the retry path could produce.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `!failure.gpuFault` term deleted from
    //       advanceSessionLocked()'s retry condition -> "FAIL: a GPU fault during playback never
    //       reopens the file", printing "1 reopen(s) after the fault", with the two clauses after it.
    //       Observed 2026-09-18. Reverted.
    // COVERAGE-ASSERT: the injected fault is asserted to have been spent before anything is read, so
    //       "no reopen" cannot be read off an injection that never fired.
    g_files.resetCounts();
    g_submitDeviceLost.store(1);
    const bool injected = settlesWithin(std::chrono::seconds(30), [] { return g_submitDeviceLost.load() == 0; });
    check(injected, "the injected device loss was spent on a picture's submission");
    if (!injected) {
        notVerified("S13.142", "nothing submitted a picture, so no device loss could be injected into one");
        return;
    }
    // The reopen is read after a quiet window rather than after waiting for the monitor to settle:
    // a daemon that DOES retry a GPU fault never settles, so a settle-first shape would spend its
    // deadline on the check above and leave the reopen count -- the property -- ungraded.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    reportUnless(g_files.opens(clips[0]) == 0, "a GPU fault during playback never reopens the file",
                 std::to_string(g_files.opens(clips[0])) + " reopen(s) after the fault");
    reportUnless(manager.snapshot().health.gpu.status == wallpaperx::HealthStatus::Fail,
                 "and the fault is recorded against the GPU", "gpu health: " + manager.snapshot().health.gpu.detail);
    reportUnless(settledOnFailure(manager, kMonitorFaulting),
                 "and the monitor is left with no frame and the failure standing",
                 "the monitor did not settle on the failure");
}

// --------------------------------------------------------------------------- //
// Stage E5: which death a video answers for, its count, and the daemon's own shortages
// --------------------------------------------------------------------------- //

void check(bool condition, const std::string& what) { wallpaperx::testing::check(condition, what.c_str()); }

// A config file as the daemon writes it, for a child or a start to restore from.
void writeConfig(const std::filesystem::path& file, const std::vector<std::tuple<int, std::string, std::string>>& rows) {
    nlohmann::json monitors = nlohmann::json::array();
    for (const auto& [id, type, path] : rows) monitors.push_back({{"monitor_id", id}, {"type", type}, {"path", path}});
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::trunc) << nlohmann::json{{"version", 1}, {"monitors", std::move(monitors)}}.dump();
}

// A child of this binary that starts as the daemon does -- restoreSaved(), recordStartingVideos(),
// then every restored monitor posted to a render worker -- and dies of a real memory fault on the
// render thread at the point `scenario` stages. The parent reads what the death left.
//
//   decode        in the clock's decode of <path>'s next picture (its first was delivered)
//   replace       in freeing <path>'s decoder after its monitor was given the video <extra>
//   still         the same, given the still <extra>: the stale-session drop
//   idle          the same, when the idle release frees it (short waits, nothing collecting)
//   evict         the same, when an open of <extra> for monitor <extraMonitor> runs out of
//                 descriptors and frees <path>'s session, hidden, to try again
//   file-failure  the same, after the clock's next picture fails as the file's fault
//   gpu-failure   the same, after the clock's draw loses the device
constexpr const char* kBlameChildFlag = "--blame-child";
constexpr int kBlameChildNeverDied = 3;
constexpr int kBlameChildSetupFailed = 4;

[[noreturn]] void runBlameChild(int argc, char** argv) {
    wallpaperx::testing::suppressCoreDump();
    wallpaperx::installFatalSignalHandlers();
    if (argc < 7) std::_Exit(kBlameChildSetupFailed);
    const std::string scenario = argv[4];
    const int monitorId = std::atoi(argv[5]);
    const std::string path = argv[6];
    const std::string extra = argc > 7 ? argv[7] : "";
    const int extraMonitor = argc > 8 ? std::atoi(argv[8]) : -1;

    WallpaperManager::Config config;
    config.width = kTargetWidth;
    config.height = kTargetHeight;
    config.configFile = argv[2];
    config.stateDir = argv[3];
    config.unwatchedAfter = std::chrono::hours(1);
    config.idleSessionRelease = std::chrono::hours(1);
    if (scenario == "idle") {
        config.unwatchedAfter = std::chrono::milliseconds(300);
        config.idleSessionRelease = std::chrono::milliseconds(300);
    }
    g_files.watch(path);
    if (!extra.empty()) g_files.watch(extra);
    try {
        WallpaperManager manager(config);
        const std::vector<int> restored = manager.restoreSaved();
        manager.recordStartingVideos();
        FrameReadyLog signals;
        RenderWorker worker(manager, [&signals](int id, uint64_t generation) { signals.record(id, generation); });
        {
            std::promise<void> known;
            std::future<void> recorded = known.get_future();
            worker.postTask([&known](RenderWorker::TaskState) {
                g_crashThread.store(::gettid());
                known.set_value();
            });
            if (recorded.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
                std::_Exit(kBlameChildSetupFailed);
            }
        }
        for (const int id : restored) worker.post(id);
        if (scenario == "decode") {
            armCrash(CrashOn::Decode, path);
        } else {
            if (!signals.wait(monitorId, 1)) std::_Exit(kBlameChildSetupFailed);
            armCrash(CrashOn::Free, path);
            if (scenario == "replace" || scenario == "still") {
                manager.requestWallpaper(scenario == "replace" ? "video" : "static", extra, monitorId);
                worker.post(monitorId);
            } else if (scenario == "evict") {
                wallpaperx::testing::nameScreen(manager, monitorId, "DP-BLAME");
                manager.setOutputVisible("DP-BLAME", false);
                // Two: the request's own open is a shortage it accepts, the render's is the one
                // that frees a session to try again.
                g_openFailure.arm(extra, EMFILE, 2);
                manager.requestWallpaper("video", extra, extraMonitor);
                worker.post(extraMonitor);
            } else if (scenario == "file-failure") {
                g_playbackFailure.arm();
            } else if (scenario == "gpu-failure") {
                g_submitDeviceLost.store(1);
            } else if (scenario != "idle") {
                std::_Exit(kBlameChildSetupFailed);
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(30));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "video_wallpaper_test blame child: %s\n", e.what());
        std::_Exit(kBlameChildSetupFailed);
    }
    std::_Exit(kBlameChildNeverDied);
}

// What one staged death left behind. `died` is set only when the child really died of SIGSEGV;
// anything else is reported here, as a harness fault or as a death that never came.
struct StagedDeath {
    bool died = false;
    std::optional<wallpaperx::PlayingHistory> playing;
};

StagedDeath stageDeath(const char* goal, const std::filesystem::path& configFile, const std::filesystem::path& stateDir,
                       const std::vector<std::string>& scenario) {
    std::vector<std::string> args{kBlameChildFlag, configFile.string(), stateDir.string()};
    args.insert(args.end(), scenario.begin(), scenario.end());
    const wallpaperx::testing::SelfExecResult r = wallpaperx::testing::runSelf(args, std::chrono::seconds(90));
    StagedDeath out;
    if (!r.fault.empty() || !r.reaped) {
        wallpaperx::testing::harnessFailure("a '" + scenario.front() + "' child " +
                                            (r.fault.empty() ? r.how() : r.fault));
        return out;
    }
    if (r.exitedWith(kBlameChildNeverDied)) {
        notVerified(goal, "the '" + scenario.front() + "' child never reached the point its death was staged at");
        return out;
    }
    if (!r.diedOf(SIGSEGV)) {
        wallpaperx::testing::harnessFailure("a '" + scenario.front() + "' child " + r.how() +
                                            " rather than dying of the staged fault");
        return out;
    }
    out.died = true;
    out.playing = wallpaperx::readPlayingRecord(stateDir);
    return out;
}

std::string describeDeath(const StagedDeath& d) {
    if (!d.playing) return "no readable playing record";
    if (!d.playing->died) return "the record names no video";
    return "the record names monitor " + std::to_string(d.playing->died->monitorId) + " '" + d.playing->died->path +
           "'";
}

constexpr int kMonitorBlamed = 0;
constexpr int kMonitorBystander = 1;

// S13.184 and S13.190 through a real render thread, and S13.185's consequence.
void runPlaybackBlameChecks(const WallpaperManager::Config& base, const std::vector<std::string>& clips,
                            const std::string& still, const std::string& onePicture) {
    const char* kGoals = "S13.184, S13.185 and S13.190 (which death a video answers for, through the real render thread)";
    const wallpaperx::testing::ScratchDir scratch("videoblame");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch directory could be created");
        return;
    }
    const std::string& blamed = clips[0];
    const std::string& bystander = clips[1];

    // GOAL: S13.184 through the daemon's own render thread -- a fatal signal while the clock is
    //       decoding one video's next picture names THAT video, its monitor and path, and not the
    //       other video playing beside it. False if the render thread holds no name while it works
    //       on a video -- the crash loop the record exists to stop goes on -- or holds the wrong one.
    // WHY-THIS-SHAPE: a child of this binary that starts as the daemon does and dies of a real
    //       memory fault inside the decoder call, staged on the one file's context after its first
    //       picture, so the death is the clock's and never the open's (which the applying record
    //       covers). Two videos play, so a record naming the wrong one is told apart from the right.
    //       The fault fires only on the render worker's thread: without that, a child unwinding after
    //       a failed setup died in ~WallpaperManager's teardown and was graded as the staged death.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, `const PlayingBlame working(*session.blame);`
    //       deleted from advanceSessionLocked() -> "FAIL: a death in the clock's decode of one video
    //       names that video", printing "the record names no video", "FAIL: after two deaths in its
    //       playback, the next start refuses that video, saying so" and "FAIL: and the refused file
    //       is never opened". Observed 2026-09-21 on the NVIDIA entry. Reverted.
    {
        const std::filesystem::path configFile = scratch.path() / "decode" / "config.json";
        const std::filesystem::path stateDir = scratch.path() / "decode" / "state";
        writeConfig(configFile, {{kMonitorBlamed, "video", blamed}, {kMonitorBystander, "video", bystander}});
        const StagedDeath first =
            stageDeath("S13.184", configFile, stateDir, {"decode", std::to_string(kMonitorBlamed), blamed});
        if (!first.died) return;
        const bool named = first.playing && first.playing->died && first.playing->died->monitorId == kMonitorBlamed &&
                           first.playing->died->path == blamed && first.playing->signal == SIGSEGV;
        reportUnless(named, "a death in the clock's decode of one video names that video", describeDeath(first));

        // GOAL: S13.185 end to end -- after a second such death the next start refuses that video with
        //       the reason and leaves the other one playing: it is drawn, and it goes on to show more
        //       pictures, while the refused file is never opened.
        // WHY-THIS-SHAPE: the start is this process's own manager on the state the two children left,
        //       with a worker, so "still plays" is pictures committed rather than a status field.
        // CONTROL: the same deletion as above -> "FAIL: after two deaths in its playback, the next
        //       start refuses that video, saying so" and "FAIL: and the refused file is never opened";
        //       the bystander still played. Observed 2026-09-21 on the NVIDIA entry. Reverted.
        const StagedDeath second =
            stageDeath("S13.185", configFile, stateDir, {"decode", std::to_string(kMonitorBlamed), blamed});
        if (!second.died) return;
        WallpaperManager::Config config = playsUnwatched(base);
        config.configFile = configFile;
        config.stateDir = stateDir;
        PhaseGrade grade("the start after two deaths");
        WallpaperManager manager(config);
        CaptureGradeOnExit captureGrade{grade, manager};
        g_files.resetCounts();
        const std::vector<int> restored = manager.restoreSaved();
        FrameReadyLog signals;
        RenderWorker worker(manager, [&signals](int id, uint64_t g) { signals.record(id, g); });
        WorkerUnblock unblock{worker};
        for (const int id : restored) worker.post(id);
        const std::optional<wallpaperx::MonitorStatus> refusedStatus = monitorStatus(manager, kMonitorBlamed);
        reportUnless(refusedStatus && !refusedStatus->lastError.empty() && !refusedStatus->renderOutstanding &&
                         refusedStatus->lastError.find("while playing this video, the 2nd time") != std::string::npos,
                     "after two deaths in its playback, the next start refuses that video, saying so",
                     refusedStatus ? "last_error: " + refusedStatus->lastError : std::string("no status"));
        const bool plays = signals.wait(kMonitorBystander, 1) && signals.waitAtLeast(kMonitorBystander, 3);
        check(plays, "while the other video is drawn and keeps playing");
        reportUnless(drainWorker(worker) && g_files.opens(blamed) == 0, "and the refused file is never opened",
                     std::to_string(g_files.opens(blamed)) + " open(s)");
    }

    // GOAL: S13.190 -- a teardown that is not a failure blames nothing. A death while freeing a
    //       video's decoder because its monitor was given another video, because it was given a still
    //       (the stale-session drop), because nothing could see it (the idle release), or because
    //       another monitor's open ran short and evicted it, names NO video -- and leaves no applying
    //       record either. A driver crash during two ordinary screen locks would otherwise refuse a
    //       good video.
    // WHY-THIS-SHAPE: the same child, dying inside the free of the one file's decoder, reached four
    //       ways. The positive reading that makes each absence evidence is the file-failure case
    //       below, the same death in the same free, which IS named.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, closeVideoSessionLocked() made to free the
    //       pipeline under `const PlayingBlame freeing(*session.blame);`, as closeFailedVideoSessionLocked()
    //       does for the file's own failure -> "FAIL: a death freeing a video's decoder when its
    //       monitor is given another video names nothing" and the same for a still, the idle release
    //       and an eviction, each printing the record naming video_session_0.mp4. The applying-record
    //       half: the same function made to free under an `ApplyingRecord` naming the file -> the four
    //       "-- and leaves no applying record either" lines, beside S13.119's and S13.130's. Both
    //       observed 2026-09-21 on the NVIDIA entry. Reverted.
    struct Teardown {
        const char* scenario;
        const char* what;
        std::string extra;
        int extraMonitor;
    };
    const Teardown routine[] = {
        {"replace", "a death freeing a video's decoder when its monitor is given another video names nothing",
         clips[2], -1},
        {"still", "a death freeing a video's decoder when its monitor is given a still names nothing", still, -1},
        {"idle", "a death freeing a video's decoder in the idle release names nothing", "", -1},
        {"evict", "a death freeing a video's decoder evicted for another monitor's short open names nothing",
         clips[3], kMonitorBystander},
    };
    int graded = 0;
    for (const Teardown& t : routine) {
        const std::filesystem::path dir = scratch.path() / t.scenario;
        writeConfig(dir / "config.json", {{kMonitorBlamed, "video", blamed}});
        std::vector<std::string> scenario{t.scenario, std::to_string(kMonitorBlamed), blamed};
        if (!t.extra.empty()) scenario.push_back(t.extra);
        if (t.extraMonitor >= 0) scenario.push_back(std::to_string(t.extraMonitor));
        const StagedDeath death = stageDeath("S13.190", dir / "config.json", dir / "state", scenario);
        if (!death.died) continue;
        ++graded;
        reportUnless(death.playing && !death.playing->died, t.what, describeDeath(death));
        const bool noRecord = !wallpaperx::readCrashRecord(dir / "state", kMonitorBlamed) &&
                              (t.extraMonitor < 0 || !wallpaperx::readCrashRecord(dir / "state", t.extraMonitor));
        check(noRecord, std::string(t.what) + " -- and leaves no applying record either");
    }
    std::printf("video_wallpaper_test: %d of 4 routine teardowns were reached and died as staged\n", graded);

    // GOAL: S13.190's failure half -- the teardown after a failure names the video only when the
    //       FILE failed: a death freeing it after its own picture failed to decode names it, and a
    //       death freeing it after the device was lost at its draw names nothing (the S13.129
    //       restatement, for the clock's path).
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, closeFailedVideoSessionLocked(): (a)
    //       `const PlayingBlame freeing(*session.blame);` deleted -> "FAIL: a death freeing a video's
    //       decoder after its own picture failed names that video", printing "the record names no
    //       video"; (b) `if (failure != Fault::File) return;` deleted -> "FAIL: a death freeing a
    //       video's decoder after the device was lost names nothing", printing the record naming
    //       video_session_0.mp4. Each alone; observed 2026-09-21 on the NVIDIA entry. Reverted.
    {
        const std::filesystem::path dir = scratch.path() / "file-failure";
        writeConfig(dir / "config.json", {{kMonitorBlamed, "video", blamed}});
        const StagedDeath death =
            stageDeath("S13.190", dir / "config.json", dir / "state", {"file-failure", std::to_string(kMonitorBlamed), blamed});
        if (death.died) {
            reportUnless(death.playing && death.playing->died && death.playing->died->path == blamed,
                         "a death freeing a video's decoder after its own picture failed names that video",
                         describeDeath(death));
        }
    }
    {
        const std::filesystem::path dir = scratch.path() / "gpu-failure";
        writeConfig(dir / "config.json", {{kMonitorBlamed, "video", blamed}});
        const StagedDeath death =
            stageDeath("S13.190", dir / "config.json", dir / "state", {"gpu-failure", std::to_string(kMonitorBlamed), blamed});
        if (death.died) {
            reportUnless(death.playing && !death.playing->died,
                         "a death freeing a video's decoder after the device was lost names nothing",
                         describeDeath(death));
        }
    }
    (void)onePicture;
}

constexpr int kMonitorClean = 0;

// S13.189: the count a video carries is cleared by play, and only by play. Run twice, once per hide
// length; `gradeClear` asks for the clearing half as well, which consumes the count.
//
// GOAL: S13.189's second clause -- time with nothing shown does not count toward clearing, however
//       SHORT the stop is. A screen hidden for three seconds -- well inside the five seconds the
//       rule before the fix tolerated between two pictures -- adds nothing, so two visible stretches
//       that together fall short of cleanPlayAfter leave the count standing; the same for a
//       six-second hide, across which the idle release also frees the session. False for a clock
//       credited by wall time (every hide would count), and false for one credited whenever two
//       pictures are within five seconds of each other -- under which the three-second hide counts
//       and the six-second one does not, which is why both are driven.
// WHY-THIS-SHAPE: two runs of the same drive, each with a state directory and a manager of its own,
//       because the clearing half consumes the count. The visible stretches are timed by this
//       thread, and a run whose visible time reached cleanPlayAfter reports NOT VERIFIED rather than
//       reading a clear as a defect. The three-second case is the one this suite could not fail
//       before: under the old rule that hide was credited as play, measured on unmodified production
//       at the time.
// CONTROL: in backend/daemon/wallpaper_manager.cpp, the three `cleanPlay.lastShown.reset()` drops
//       this drive crosses deleted -- setOutputVisible()'s, videoSessionLocked()'s (the session is
//       released and reopened inside both hides) and advanceSessionLocked()'s stopped branch -> both
//       "FAIL: a 6 s hide is not counted as play" and "FAIL: a 3 s hide is not counted as play",
//       each printing "the count is 0". The same three deleted AND creditCleanPlayLocked()'s
//       `onTime` term replaced by the rule before the fix, `now - *play.lastShown <=
//       kMaxPictureInterval` -> the 3 s run red and the 6 s run GREEN, which is exactly the defect
//       the short hide was added for. Measured and NOT usable as controls: deleting
//       setOutputVisible()'s drop alone leaves both green (the clock's stopped branch drops the
//       anchor a moment later), and the `onTime` term alone defends neither hide -- a session
//       reopened after the release draws its first picture on time. Observed 2026-09-22 on the
//       NVIDIA entry. Reverted.
// COVERAGE-ASSERT: the session is required to have been released during the hide, so the "released"
//       part of the goal is exercised rather than assumed, and the count is read once before the
//       hide so the run is known to have started carrying one.
void runCleanPlayHideChecks(const WallpaperManager::Config& base, const std::string& clip,
                            std::chrono::seconds hide, bool gradeClear) {
    const std::string what = "a " + std::to_string(hide.count()) + " s hide";
    const char* kGoal = "S13.189 (a video's death count is cleared by play, and only by play)";
    const wallpaperx::testing::ScratchDir scratch("videoclean");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created");
        return;
    }
    WallpaperManager::Config config = base;
    config.configFile = scratch.path() / "config.json";
    config.stateDir = scratch.path() / "state";
    // Three seconds: above the two visible stretches below with a second to spare, and far below the
    // six-second hide between them, so which of the two rules is in force decides the answer.
    config.cleanPlayAfter = std::chrono::seconds(3);
    writeConfig(config.configFile, {{kMonitorClean, "video", clip}});
    {
        // One death carried in, written by the daemon's own record, as a start after it would carry it.
        wallpaperx::PlayingRecord record(config.stateDir);
        record.start({{kMonitorClean, clip, 1}});
    }
    const auto countNow = [&]() {
        const std::optional<wallpaperx::PlayingHistory> h = wallpaperx::readPlayingRecord(config.stateDir);
        if (!h) return -1;
        for (const wallpaperx::PlayingVideo& v : h->videos) {
            if (v.monitorId == kMonitorClean) return v.deaths;
        }
        return 0;
    };

    PhaseGrade grade("the clean-play phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    Collector collector(manager, kMonitorClean, ":1.clean");
    RenderWorker worker(manager, [&](int id, uint64_t g) {
        signals.record(id, g);
        collector.announced(id, g);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};
    const std::vector<int> restored = manager.restoreSaved();
    manager.recordStartingVideos();
    check(countNow() == 1, "the video starts this run carrying one death (the harness's premise)");
    const std::string screen = "DP-CLEAN";
    wallpaperx::testing::nameScreen(manager, kMonitorClean, screen);
    for (const int id : restored) worker.post(id);
    if (!signals.wait(kMonitorClean, 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the video did not draw on this host");
        return;
    }

    const Clock::time_point firstShown = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    manager.setOutputVisible(screen, false);
    const double visibleBefore = std::chrono::duration<double>(Clock::now() - firstShown).count();
    g_files.resetCounts();
    std::this_thread::sleep_for(hide);
    const int releasedDuringHide = g_files.closes(clip);
    const Clock::time_point shownAgain = Clock::now();
    manager.setOutputVisible(screen, true);
    worker.wake();
    const size_t before = signals.countFor(kMonitorClean);
    const bool resumed = settlesWithin(std::chrono::seconds(10),
                                       [&] { return signals.countFor(kMonitorClean) >= before + 2; });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int afterHide = countNow();
    const double visibleAfter = std::chrono::duration<double>(Clock::now() - shownAgain).count();
    check(resumed, what + ": the video plays again when the screen is shown");
    check(releasedDuringHide >= 1,
          what + ": the session was released during it, so a release is inside what is graded");
    if (visibleBefore + visibleAfter >= 3.0) {
        // A loaded host that took long to resume showed enough for the count to clear honestly.
        const std::string clause = "S13.189's hidden-time clause (" + what + ")";
        notVerified(clause.c_str(), "the visible stretches took " + std::to_string(visibleBefore + visibleAfter) +
                                        " s, not less than cleanPlayAfter, so a clear proves nothing");
    } else {
        const std::string says = what + " is not counted as play";
        reportUnless(afterHide == 1, says.c_str(), "the count is " + std::to_string(afterHide));
    }
    if (!gradeClear) return;

    // GOAL: S13.189 -- a video that goes on playing normally for cleanPlayAfter has its count
    //       cleared, from the next start on. False for a count that stands for ever: two unrelated
    //       driver crashes months apart would then refuse a working video.
    // WHY-THIS-SHAPE: read through readPlayingRecord(), which is what the next start reads, with a
    //       bound of cleanPlayAfter plus the longest picture interval and a margin, reported rather
    //       than waited out.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, `if (playedClean) m_playing.clear(monitorId);`
    //       deleted from advanceSessionLocked() -> "FAIL: a video that plays normally for cleanPlayAfter
    //       has its count cleared", printing "the count is still 1, 138 collected, 0 refused". Observed
    //       2026-09-21 on the NVIDIA entry. Reverted.
    const bool cleared = settlesWithin(std::chrono::seconds(12), [&] { return countNow() == 0; });
    reportUnless(cleared, "a video that plays normally for cleanPlayAfter has its count cleared",
                 "the count is still " + std::to_string(countNow()) + ", " + collector.describe());
}

constexpr int kMonitorShort = 0;

// S13.193 at the manager: a shortage is the daemon's, has a budget of its own, and a request made
// during one is accepted.
void runShortageChecks(const WallpaperManager::Config& base, const std::vector<std::string>& clips) {
    const char* kGoals = "S13.193 (running out of descriptors or memory is the daemon's fault, with a budget of its own)";
    PhaseGrade grade("the shortage phase");
    FrameReadyLog signals;
    WallpaperManager manager(playsUnwatched(base));
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager, [&signals](int id, uint64_t g) { signals.record(id, g); });
    WorkerUnblock unblock{worker};
    struct Disarm {
        ~Disarm() { g_openFailure.disarm(); }
    } disarm;
    wallpaperx::testing::nameScreen(manager, kMonitorShort, screenOf(kMonitorShort));

    // GOAL: S13.193's request clause -- a wallpaper asked for while the daemon has run out of file
    //       descriptors is ACCEPTED: the shortage is the daemon's, gives no verdict on the file, and
    //       the render's own open checks again. False for a request that refuses a working file
    //       because the daemon was short at that instant.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, requestWallpaper()'s
    //       `catch (const ResourceExhausted&) {}` made to rethrow as InvalidRequest -> "FAIL: a
    //       wallpaper asked for while the daemon is out of descriptors is accepted", printing
    //       "refused: WallpaperManager: the daemon has run out of file descriptors", and the eviction
    //       staging in runPlaybackBlameChecks reports a harness fault, since it goes through the same
    //       request. Observed 2026-09-21 on the NVIDIA entry. Reverted.
    g_openFailure.arm(clips[4], EMFILE, 1);
    std::string refusal;
    try {
        manager.requestWallpaper("video", clips[4], kMonitorShort);
    } catch (const std::exception& e) {
        refusal = e.what();
    }
    check(g_openFailure.left() == 0, "the injected shortage was spent on the request's own open");
    reportUnless(refusal.empty(), "a wallpaper asked for while the daemon is out of descriptors is accepted",
                 "refused: " + refusal);
    if (!refusal.empty()) {
        // Set it without a shortage, so the render clause below still has a wallpaper to run.
        g_openFailure.disarm();
        manager.requestWallpaper("video", clips[4], kMonitorShort);
    }

    // GOAL: S13.193's render clause -- a render whose open runs out of descriptors records a failure
    //       that is the DAEMON's (not the file's), saying so, with no frame.
    // CONTROL: in renderer/wallpaper_file.cpp, openWallpaper()'s `if (isShortage(failed))
    //       throwShortage(...)` after the open deleted, so EMFILE is the open's own answer -> "FAIL: a
    //       render whose open runs out of descriptors is the daemon's failure, not the file's",
    //       printing "VideoDecoder: cannot open the wallpaper (Too many open files) ... (the file's)",
    //       beside the request clause above and the eviction staging's harness fault (both reach the
    //       same open). Observed 2026-09-21 on the NVIDIA entry. Reverted.
    g_openFailure.arm(clips[4], EMFILE, 1);
    worker.post(kMonitorShort);
    const bool settled = settlesWithin(std::chrono::seconds(30), [&] {
        const std::optional<wallpaperx::MonitorStatus> s = monitorStatus(manager, kMonitorShort);
        return s && !s->renderOutstanding && !s->lastError.empty();
    });
    {
        const std::optional<wallpaperx::MonitorStatus> s = monitorStatus(manager, kMonitorShort);
        check(settled && g_openFailure.left() == 0, "the render's open ran out of descriptors (the harness's premise)");
        reportUnless(s && s->lastErrorIsDaemonFault && !s->hasFrame &&
                         s->lastError.find("run out of file descriptors") != std::string::npos,
                     "a render whose open runs out of descriptors is the daemon's failure, not the file's",
                     s ? "last_error: " + s->lastError + (s->lastErrorIsDaemonFault ? " (daemon's)" : " (the file's)")
                       : std::string("no status"));
    }

    const auto play = [&](const std::string& path) {
        manager.requestWallpaper("video", path, kMonitorShort);
        worker.post(kMonitorShort);
        const std::optional<wallpaperx::MonitorStatus> now = monitorStatus(manager, kMonitorShort);
        return signals.waitAtLeast(kMonitorShort, (now ? now->generation : 0) + 1);
    };
    if (!play(clips[5])) {
        grade.expectNoDevice();
        notVerified(kGoals, "the video did not draw on this host");
        return;
    }

    // GOAL: S13.193's budget -- playback that keeps failing because the daemon runs short is reopened
    //       kShortageRetries (10) times, not the file's kPlaybackRetries (3), and the monitor settles
    //       on a failure that is the daemon's. False for a shortage spending the file's budget, which
    //       is what gave up on a good file after three tries while another screen held the memory.
    // WHY-THIS-SHAPE: the injected failure is the picture after each reopen's first, for S13.140's
    //       reason -- each reopen succeeds and fails again -- and it is ENOMEM, which the decoder
    //       reports as a shortage. The count graded is opens of this file.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, ReopenBudget::spend()'s
    //       `kShortageRetries` replaced by `kPlaybackRetries` -> "FAIL: playback that keeps running
    //       short is reopened kShortageRetries times", printing "3 reopen(s)", and the separation
    //       check below printing "5 reopen(s)". Observed 2026-09-21 on the NVIDIA entry. Reverted.
    // COVERAGE-ASSERT: at least one shortage per reopen, plus the one that spent the budget, is
    //       asserted to have been injected.
    g_files.resetCounts();
    const int shortagesBefore = g_playbackFailure.shortagesInjected();
    g_playbackFailure.arm(0);
    const bool spent = settlesWithin(std::chrono::seconds(60), [&] { return settledOnFailure(manager, kMonitorShort); });
    g_playbackFailure.disarm();
    check(spent, "playback that keeps running short stops on its own");
    const int reopens = g_files.opens(clips[5]);
    reportUnless(reopens == static_cast<int>(WallpaperManager::kShortageRetries),
                 "playback that keeps running short is reopened kShortageRetries times",
                 std::to_string(reopens) + " reopen(s)");
    check(g_playbackFailure.shortagesInjected() - shortagesBefore >= reopens + 1,
          "every one of those reopens was answered by another injected shortage");
    {
        const std::optional<wallpaperx::MonitorStatus> s = monitorStatus(manager, kMonitorShort);
        check(s && s->lastErrorIsDaemonFault, "and the monitor settles on a failure that is the daemon's");
    }

    // GOAL: S13.193's separation -- the two budgets are separate: two failures that are the file's,
    //       then shortages, reopen the file 2 + 10 times. False for one shared budget of either size
    //       (10 or 3 in total).
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, ReopenBudget::spend()'s
    //       `uint32_t& spent = shortage ? shortages : failures;` made `uint32_t& spent = failures;` ->
    //       exactly "FAIL: the file's failures and the daemon's shortages spend separate budgets",
    //       printing "10 reopen(s)". Observed 2026-09-21 on the NVIDIA entry. Reverted.
    if (!play(clips[6])) {
        check(false, "a new video draws on the monitor after the shortage budget was spent");
        return;
    }
    g_files.resetCounts();
    g_playbackFailure.arm(2);
    const bool spentBoth = settlesWithin(std::chrono::seconds(60), [&] { return settledOnFailure(manager, kMonitorShort); });
    g_playbackFailure.disarm();
    check(spentBoth, "playback that fails twice as the file's fault and then runs short stops on its own");
    reportUnless(g_files.opens(clips[6]) == 2 + static_cast<int>(WallpaperManager::kShortageRetries),
                 "the file's failures and the daemon's shortages spend separate budgets",
                 std::to_string(g_files.opens(clips[6])) + " reopen(s)");
}

constexpr int kMonitorAsked = 0;

// A mount table that puts `dir` on a filesystem of type `type`, for the open hook to serve.
std::string writeFakeMountTable(const std::filesystem::path& file, const std::string& dir, const std::string& type) {
    std::ofstream out(file, std::ios::trunc);
    out << "1 0 0:1 / / rw - ext4 /dev/root rw\n2 1 0:999 / " << dir << " rw - " << type << " share rw\n";
    return out ? file.string() : std::string();
}

struct MountTableOverride {
    explicit MountTableOverride(const std::string& table) {
        std::lock_guard<std::mutex> lock(g_mountTableMutex);
        g_mountTable = table;
        g_mountTableSet.store(true);
    }
    ~MountTableOverride() {
        g_mountTableSet.store(false);
        std::lock_guard<std::mutex> lock(g_mountTableMutex);
        g_mountTable.clear();
    }
};

// S13.195: picking a wallpaper opens it, with the checks the draw uses.
void runOpenAtRequestChecks(const WallpaperManager::Config& config, const std::string& still) {
    const char* kGoals = "S13.195 (picking a wallpaper opens the file on the spot, with the draw's checks)";
    const wallpaperx::testing::ScratchDir scratch("videoasked");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch directory could be created");
        return;
    }
    std::error_code ec;
    const std::filesystem::path shareDir = std::filesystem::canonical(scratch.path(), ec) / "share";
    std::filesystem::create_directories(shareDir, ec);
    const std::string onShare = (shareDir / "still.png").string();
    std::filesystem::copy_file(still, onShare, std::filesystem::copy_options::overwrite_existing, ec);
    const std::string denied = (scratch.path() / "denied.png").string();
    std::filesystem::copy_file(still, denied, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        notVerified(kGoals, "the fixture copies could not be made: " + ec.message());
        return;
    }
    g_files.watch(onShare);
    g_files.watch(denied);

    PhaseGrade grade("the open-at-request phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    wallpaperx::testing::nameScreen(manager, kMonitorAsked, screenOf(kMonitorAsked));
    struct Disarm {
        ~Disarm() { g_openFailure.disarm(); }
    } disarm;
    const auto ask = [&](const std::string& type, const std::string& path) -> std::string {
        try {
            manager.requestWallpaper(type, path, kMonitorAsked);
            return {};
        } catch (const wallpaperx::InvalidRequest& e) {
            return e.what();
        } catch (const std::exception& e) {
            return std::string("(not InvalidRequest) ") + e.what();
        }
    };

    // GOAL: S13.195 -- the request OPENS the file, on the calling thread, and closes it again: an
    //       unreadable file is refused at the request rather than accepted and then failing. And a
    //       file that stat() calls readable but whose open is refused is refused -- which a stat-only
    //       check cannot tell.
    // WHY-THIS-SHAPE: the open is observed at the kernel call, and the refused open is injected at
    //       the same call with access(2) still answering yes, so the only thing that can refuse it is
    //       an open. No render runs before these readings.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, requestWallpaper()'s openWallpaper() block
    //       replaced by the check it superseded (`!std::filesystem::is_regular_file(path, ec) ||
    //       ::access(path.c_str(), R_OK) != 0` -> refused) -> "FAIL: a wallpaper request opens the
    //       file and closes it again" ("0 open(s), 0 close(s)") and "FAIL: a file whose open is refused
    //       is refused at the request, though stat calls it readable" ("accepted"), with the two
    //       filesystem refusals below and the shortage phase's "the injected shortage was spent on the
    //       request's own open". Observed 2026-09-21 on the NVIDIA entry. Reverted.
    g_files.resetCounts();
    const std::string accepted = ask("static", denied);
    reportUnless(accepted.empty() && g_files.opens(denied) == 1 && g_files.closes(denied) == 1,
                 "a wallpaper request opens the file and closes it again",
                 std::to_string(g_files.opens(denied)) + " open(s), " + std::to_string(g_files.closes(denied)) +
                     " close(s)" + (accepted.empty() ? "" : ", refused: " + accepted));
    check(::access(denied.c_str(), R_OK) == 0, "the file below is readable as far as access(2) can tell (the premise)");
    g_openFailure.arm(denied, EACCES, 1);
    const std::string refusal = ask("static", denied);
    reportUnless(!refusal.empty() && refusal.rfind("(not", 0) != 0 && refusal.find("not a readable file") != std::string::npos,
                 "a file whose open is refused is refused at the request, though stat calls it readable",
                 refusal.empty() ? std::string("accepted") : refusal);

    // GOAL: S13.195 with S13.194 -- a file on a network filesystem, or on a FUSE one not on the
    //       allow-list, is refused at the request with the reason; one on an allow-listed FUSE
    //       filesystem is accepted. And because the check runs on EVERY open, a wallpaper accepted
    //       from a local directory is refused at a later render once that directory is a network
    //       mount, as the file's own failure.
    // WHY-THIS-SHAPE: the mount table is substituted at the open hook, the same substitution
    //       wallpaper_file_probe grades exhaustively; here what is graded is that the daemon's request
    //       and its render both consult it.
    // CONTROL: in renderer/wallpaper_file.cpp, openWallpaper()'s leading refusedFilesystemOf() block
    //       deleted -> "FAIL: a wallpaper on a network filesystem is refused at the request", the FUSE
    //       one, and "FAIL: a wallpaper whose directory has since become a network mount is refused
    //       at its next render", printing an empty last_error (the still redrew); the fake mounts
    //       carry a device no file has, so the check after the open cannot stand in. Observed
    //       2026-09-21 on the NVIDIA entry. Reverted.
    const std::string nfsTable = writeFakeMountTable(scratch.path() / "mountinfo-nfs", shareDir.string(), "nfs4");
    const std::string sshTable = writeFakeMountTable(scratch.path() / "mountinfo-ssh", shareDir.string(), "fuse.sshfs");
    const std::string ntfsTable = writeFakeMountTable(scratch.path() / "mountinfo-ntfs", shareDir.string(), "fuse.ntfs-3g");
    {
        const MountTableOverride table(nfsTable);
        const std::string r = ask("static", onShare);
        reportUnless(r.find("network filesystem (nfs4)") != std::string::npos && r.rfind("(not", 0) != 0,
                     "a wallpaper on a network filesystem is refused at the request, saying so",
                     r.empty() ? std::string("accepted") : r);
    }
    {
        const MountTableOverride table(sshTable);
        const std::string r = ask("static", onShare);
        reportUnless(r.find("FUSE filesystem (fuse.sshfs)") != std::string::npos,
                     "a wallpaper on a FUSE filesystem not on the allow-list is refused at the request, saying so",
                     r.empty() ? std::string("accepted") : r);
    }
    {
        const MountTableOverride table(ntfsTable);
        const std::string r = ask("static", onShare);
        reportUnless(r.empty(), "a wallpaper on an allow-listed FUSE filesystem is accepted", "refused: " + r);
    }
    const std::string local = ask("static", onShare);
    const wallpaperx::RenderOutcome drawn = manager.renderPending(kMonitorAsked);
    if (!local.empty() || !drawn.rendered) {
        grade.expectNoDevice();
        notVerified("S13.195's later-open clause", "the local wallpaper did not draw on this host: " + local);
        return;
    }
    {
        const MountTableOverride table(nfsTable);
        std::vector<int> owed;
        manager.setOutputSize(screenOf(kMonitorAsked), kResizedWidth, kResizedHeight, owed);
        check(std::find(owed.begin(), owed.end(), kMonitorAsked) != owed.end(),
              "a new size owes the still a render, which opens its file again");
        manager.renderPending(kMonitorAsked);
    }
    const std::optional<wallpaperx::MonitorStatus> s = monitorStatus(manager, kMonitorAsked);
    reportUnless(s && !s->hasFrame && !s->lastErrorIsDaemonFault &&
                     s->lastError.find("network filesystem (nfs4)") != std::string::npos,
                 "a wallpaper whose directory has since become a network mount is refused at its next render, "
                 "as the file's failure",
                 s ? "last_error: " + s->lastError : std::string("no status"));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], kBlameChildFlag) == 0) runBlameChild(argc, argv);
    wallpaperx::testing::g_testName = "video_wallpaper_test";

    const wallpaperx::testing::ScratchStateHome stateHome;
    if (!stateHome.ok()) {
        notVerified("every goal in this file",
                    "no scratch state directory could be created, and running without one would write this "
                    "test's crash records into the developer's own state directory");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    // Before any Device is built: the layer reads its settings at instance creation.
    const DecoderMessageExcuse excuse(stateHome.path());
    if (!excuse.applied()) {
        // Not fatal and not a skip: without the filter the decoder's own messages are counted, so
        // the phases below go red with a reason that is visible rather than passing quietly.
        std::fprintf(stderr,
                     "FAIL (harness): the validation settings file that excuses %s could not be written to %s, so "
                     "ffmpeg's own decoder messages will be counted against this daemon\n",
                     wallpaperx::testing::kExcusedDecoderMessageId, excuse.file().c_str());
        ++wallpaperx::testing::g_harnessFaults;
    } else {
        std::printf("video_wallpaper_test: WARNING -- %s is excused for this process, by id, through the validation "
                    "layer's own filter (%s). It is ffmpeg's Vulkan decoder binding one reference slot per DPB "
                    "picture, reproducible with the stock ffmpeg command line. Every other validation message is "
                    "still graded, here and everywhere else.\n",
                    wallpaperx::testing::kExcusedDecoderMessageId, excuse.file().c_str());
    }
    std::fflush(stdout);

    const std::string clip = clipPath();
    const std::string onePicture = onePicturePath();
    const std::string slow = slowClipPath();
    const std::string fast = fastClipPath();
    const std::string gap = gapClipPath();
    const std::string rate120 = rate120ClipPath();
    if (!std::filesystem::exists(clip) || !std::filesystem::exists(onePicture)) {
        notVerified("every goal in this file",
                    "the video fixtures " + clip + " and " + onePicture +
                        " were not both generated, which means this build found no ffmpeg command line");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    try {
        std::filesystem::create_directories(testAssetDir());
        const std::string still = wallpaperx::testing::writeGradientPng(testAssetDir() + "/video_test_still.png", 32, 32);
        const std::string junk = writeUndecodable("video_test_not_a_clip.mp4");
        // Copies, one per monitor the session phases draw, so the set of open files names which
        // monitors hold a session. Nine of each, one more than the session ceiling, so a phase can
        // ask for a session past it. The parked copies hold a single picture: on the render
        // worker's thread they leave the pass under test as the only thing that can open a file or
        // send a packet while a phase counts (see runResizeChecks).
        std::vector<std::string> clips;
        std::vector<std::string> parked;
        for (int i = 0; i < 9; ++i) {
            clips.push_back(testAssetDir() + "/video_session_" + std::to_string(i) + ".mp4");
            std::filesystem::copy_file(clip, clips.back(), std::filesystem::copy_options::overwrite_existing);
            parked.push_back(testAssetDir() + "/video_parked_" + std::to_string(i) + ".mp4");
            std::filesystem::copy_file(onePicture, parked.back(), std::filesystem::copy_options::overwrite_existing);
        }
        // A type this build records and cannot draw; requestWallpaper wants a readable file.
        const std::string shaderRow = writeUndecodable("video_test_undrawable.frag");

        // Told to the ledger before any manager exists. Only watched paths are recorded, so a
        // fixture missing from this list is one whose opens read as zero -- which is the answer
        // several checks are looking for. That the ledger sees anything at all is asserted once,
        // after the first phase that draws a video.
        for (const std::string& path : clips) g_files.watch(path);
        for (const std::string& path : parked) g_files.watch(path);
        g_files.watch(clip);
        g_files.watch(onePicture);
        g_files.watch(slow);
        g_files.watch(fast);
        g_files.watch(gap);
        g_files.watch(junk);

        WallpaperManager::Config config;
        config.width = kTargetWidth;
        config.height = kTargetHeight;
        // The GPU-side oracle for everything the render and export path does, with exactly one
        // message id excused above.
        config.enableValidation = !validationWaived();
        // The daemon's three waits, shortened so this entry runs in seconds. Every sleep and bound
        // below reads these from the config its phase was built with -- never the production
        // constants -- or a short wait and a long bound drift apart unnoticed.
        //   unwatchedAfter 750 ms: seven and a half of the clip's picture intervals, and far above
        //     the tens of milliseconds a hand-over takes on the software driver under a parallel run.
        //   idleSessionRelease 2 s: the production ratio to unwatchedAfter (5 s to 2 s) kept, so a
        //     stopped session is still resident when the phases read it before its release.
        //   refusalLogInterval 5 s: a refuse-serve-refuse cycle of several renders must fit inside
        //     one interval even on a loaded machine.
        config.unwatchedAfter = std::chrono::milliseconds(750);
        config.idleSessionRelease = std::chrono::seconds(2);
        config.refusalLogInterval = std::chrono::seconds(5);

        {
            PhaseGrade grade("the bus phase");
            FrameReadyLog signals;
            WallpaperManager manager(config);
            CaptureGradeOnExit captureGrade{grade, manager};

            auto serviceConn = sdbus::createSessionBusConnection(sdbus::ServiceName{DbusService::kBusName});
            DbusService service(*serviceConn, manager);
            serviceConn->enterEventLoopAsync();
            auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                            sdbus::ObjectPath{DbusService::kObjectPath});
            proxy->uponSignal("FrameReady")
                .onInterface(DbusService::kInterface)
                .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

            runBusChecks(*proxy, signals, clip, still);
        }

        // COVERAGE-ASSERT for the whole file ledger, placed after the first phase that is known to
        // have drawn a video. Everything below reads "how many times was this file opened", and a
        // ledger watching the wrong call answers zero to all of them -- which is the expected
        // answer for several, so a blind ledger would read as a clean run. This is the one place
        // that asserts it can see anything at all.
        // CONTROL: the `open` hook's `g_files.opened(fd, path)` deleted -> this line red first,
        //       followed by 30-odd others across the session, failed-pass, eviction and retry
        //       phases ("a monitor's first video pass opens its file and decodes a picture (the
        //       counters work)", "eight video monitors hold eight sessions", ...). Restored.
        reportUnless(g_files.totalOpens() >= 1,
                     "the file ledger saw the daemon open a wallpaper file at all, so the open counts "
                     "every check below reads are measurements rather than a hook watching a call "
                     "nobody makes",
                     std::to_string(g_files.totalOpens()) + " open(s) after a video was drawn over the bus");

        runUndecodableChecks(config, junk);
        runResizeChecks(config, onePicture);
        runCrashRecordSpanChecks(config, onePicture);
        runSessionChecks(config, clips, still, shaderRow);
        runFailedPassChecks(config, parked, still);
        runEvictionRecordCheck(config, parked);
        runShutdownChecks(config, parked[0]);
        runClockChecks(config, clip, still);
        runOnePictureChecks(config, onePicture);
        runParkedReleaseChecks(config, onePicture);
        runOffSwitchChecks(config, clip);
        runWatchEvidenceChecks(config, clip, still);

        // A bus of its own, and a second phase that owns the daemon's well-known name: the
        // visibility goals are about the whole chain -- the method, the manager, and the worker
        // being woken -- and a manager-level phase would have to stand in for the last of those.
        {
            PhaseGrade grade("the visibility phase");
            PictureLog pictures;
            WallpaperManager manager(config);
            CaptureGradeOnExit captureGrade{grade, manager};

            auto serviceConn = sdbus::createSessionBusConnection(sdbus::ServiceName{DbusService::kBusName});
            DbusService service(*serviceConn, manager);
            serviceConn->enterEventLoopAsync();
            auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                            sdbus::ObjectPath{DbusService::kObjectPath});
            proxy->uponSignal("FrameReady")
                .onInterface(DbusService::kInterface)
                .call([&pictures](int32_t monitorId, uint64_t generation) { pictures.record(monitorId, generation); });

            runVisibilityChecks(*proxy, pictures, clip, config);
        }

        runEvictionWatchChecks(config, clips);
        if (std::filesystem::exists(slow)) {
            runSlowPictureChecks(config, slow);
        } else {
            notVerified("S13.158", "the slow-picture fixture " + slow +
                                       " was not generated, so the case that froze on a real desktop -- a video "
                                       "whose pictures are farther apart than the daemon's unwatched interval -- "
                                       "was not exercised at all");
        }
        runIrregularWallpaperChecks(config, clip, still);
        if (std::filesystem::exists(fast) && std::filesystem::exists(gap)) {
            runIntervalClampChecks(config, fast, gap);
            if (std::filesystem::exists(rate120)) {
                runCapBudgetChecks(config, rate120);
            } else {
                notVerified("S13.183", "the fixture " + rate120 + " was not generated, so the cap's budget was not "
                                       "exercised");
            }
        } else {
            notVerified("S13.175-S13.176",
                        "the interval-clamp fixtures " + fast + " and " + gap +
                            " were not both generated, so neither bound on the gap between two pictures was "
                            "exercised -- a file declaring a thousand pictures a second would go ungraded");
        }
        runHandoverLogChecks(config, clip);
        runDepartureChecks(config, onePicture);
        runResumeCreditChecks(config, clip, still, onePicture);
        runMemoryRetryChecks(config, clips);
        runPlaybackRetryChecks(config, clips);
        runGpuFaultChecks(config, clips, still);
        runShortageChecks(config, clips);
        runOpenAtRequestChecks(config, still);
        // Two hides: one past the five seconds the rule before the fix tolerated between pictures,
        // one well inside it. The second consumes nothing, so the clearing half rides on the first.
        runCleanPlayHideChecks(config, clip, std::chrono::seconds(6), /*gradeClear=*/true);
        runCleanPlayHideChecks(config, clip, std::chrono::seconds(3), /*gradeClear=*/false);
        runPlaybackBlameChecks(config, clips, still, onePicture);
    } catch (const std::exception& e) {
        // Without this an escaping sdbus::Error -- what a host with no session bus produces --
        // would abort the process, so ctest would report an opaque SIGABRT instead of the named
        // failures above.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++wallpaperx::testing::g_failures;
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
